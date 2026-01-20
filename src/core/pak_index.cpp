/**
 * Enfusion Unpacker - PAK Index Implementation
 * 
 * SQLite-based persistent file index with multi-threaded indexing.
 */

#include "enfusion/pak_index.hpp"
#include "enfusion/pak_reader.hpp"

#include <sqlite3.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <future>
#include <chrono>
#include <set>

namespace enfusion {

PakIndex& PakIndex::instance() {
    static PakIndex instance;
    return instance;
}

PakIndex::PakIndex() {
    // Use available hardware threads, leave 1 for UI
    thread_count_ = std::max(1u, std::thread::hardware_concurrency() - 1);
    if (thread_count_ > 8) thread_count_ = 8;  // Cap at 8 threads
    
    // Default database path
    db_path_ = std::filesystem::temp_directory_path() / "enfusion_unpacker_index.db";
}

PakIndex::~PakIndex() {
    close_database();
}

void PakIndex::set_game_path(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    game_path_ = path;
}

void PakIndex::set_mods_path(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    mods_path_ = path;
}

void PakIndex::set_db_path(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    db_path_ = path;
}

bool PakIndex::open_database() {
    std::lock_guard<std::mutex> lock(db_mutex_);
    
    if (db_) return true;  // Already open
    
    // Ensure directory exists
    if (db_path_.has_parent_path()) {
        std::filesystem::create_directories(db_path_.parent_path());
    }
    
    int rc = sqlite3_open(db_path_.string().c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::cerr << "[PakIndex] Failed to open database: " << sqlite3_errmsg(db_) << "\n";
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    
    // Enable WAL mode for better concurrent access
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA cache_size=10000;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA temp_store=MEMORY;", nullptr, nullptr, nullptr);
    
    // Create schema
    if (!create_schema()) {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    
    std::cerr << "[PakIndex] Database opened: " << db_path_.string() << "\n";
    return true;
}

void PakIndex::close_database() {
    std::lock_guard<std::mutex> lock(db_mutex_);
    
    // Clear prepared statement cache first
    clear_stmt_cache();
    
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

sqlite3_stmt* PakIndex::get_or_prepare_stmt(const std::string& name, const char* sql) const {
    // Check cache first
    auto it = stmt_cache_.find(name);
    if (it != stmt_cache_.end()) {
        sqlite3_reset(it->second);  // Reset for reuse
        return it->second;
    }
    
    // Prepare new statement
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[PakIndex] Failed to prepare statement '" << name << "': " << sqlite3_errmsg(db_) << "\n";
        return nullptr;
    }
    
    // Cache it
    stmt_cache_[name] = stmt;
    return stmt;
}

void PakIndex::clear_stmt_cache() {
    for (auto& [name, stmt] : stmt_cache_) {
        if (stmt) {
            sqlite3_finalize(stmt);
        }
    }
    stmt_cache_.clear();
}

bool PakIndex::create_schema() {
    // SQL Schema with optimized indexes for common query patterns
    // - Composite index on (pak_id, path_lower) for efficient JOINs
    // - Covering index on path_lower includes pak_id for fast lookups
    const char* schema = R"(
        CREATE TABLE IF NOT EXISTS paks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            path TEXT UNIQUE NOT NULL,
            file_size INTEGER NOT NULL,
            last_modified INTEGER NOT NULL,
            indexed_at INTEGER NOT NULL
        );
        
        CREATE TABLE IF NOT EXISTS files (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            pak_id INTEGER NOT NULL,
            path TEXT NOT NULL,
            path_lower TEXT NOT NULL,
            FOREIGN KEY (pak_id) REFERENCES paks(id) ON DELETE CASCADE
        );
        
        -- Single-column indexes for basic lookups
        CREATE INDEX IF NOT EXISTS idx_files_path_lower ON files(path_lower);
        CREATE INDEX IF NOT EXISTS idx_files_pak_id ON files(pak_id);
        CREATE INDEX IF NOT EXISTS idx_paks_path ON paks(path);
        
        -- Composite index for common JOIN pattern: find pak_path for file
        -- Covers queries like: SELECT p.path FROM paks p JOIN files f ON f.pak_id = p.id WHERE f.path_lower = ?
        CREATE INDEX IF NOT EXISTS idx_files_path_pak ON files(path_lower, pak_id);
        
        -- Composite index for pattern search within specific PAK
        CREATE INDEX IF NOT EXISTS idx_files_pak_path ON files(pak_id, path_lower);
    )";
    
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, schema, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "[PakIndex] Failed to create schema: " << errMsg << "\n";
        sqlite3_free(errMsg);
        return false;
    }
    
    return true;
}

std::vector<std::filesystem::path> PakIndex::scan_for_paks(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> paks;
    
    if (!std::filesystem::exists(dir)) {
        return paks;
    }
    
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                dir, std::filesystem::directory_options::skip_permission_denied)) {
            if (cancel_requested_) break;
            
            if (entry.is_regular_file()) {
                auto ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".pak") {
                    paks.push_back(entry.path());
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[PakIndex] Error scanning " << dir.string() << ": " << e.what() << "\n";
    }
    
    return paks;
}

bool PakIndex::pak_needs_update(const std::filesystem::path& pak_path) const {
    if (!db_) return true;
    
    try {
        uint64_t current_size = std::filesystem::file_size(pak_path);
        uint64_t current_mtime = std::filesystem::last_write_time(pak_path).time_since_epoch().count();
        
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT file_size, last_modified FROM paks WHERE path = ?";
        
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return true;
        }
        
        sqlite3_bind_text(stmt, 1, pak_path.string().c_str(), -1, SQLITE_TRANSIENT);
        
        bool needs_update = true;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            uint64_t cached_size = sqlite3_column_int64(stmt, 0);
            uint64_t cached_mtime = sqlite3_column_int64(stmt, 1);
            
            needs_update = (current_size != cached_size || current_mtime != cached_mtime);
        }
        
        sqlite3_finalize(stmt);
        return needs_update;
        
    } catch (...) {
        return true;
    }
}

bool PakIndex::index_pak_to_db(const std::filesystem::path& pak_path) {
    // Read PAK file list
    PakReader reader;
    if (!reader.open(pak_path)) {
        std::cerr << "[PakIndex] Failed to open PAK for indexing: " << pak_path.string() << "\n";
        return false;
    }
    
    auto files = reader.list_files();
    reader.close();
    
    // Get file metadata
    uint64_t file_size = 0;
    uint64_t last_modified = 0;
    try {
        file_size = std::filesystem::file_size(pak_path);
        last_modified = std::filesystem::last_write_time(pak_path).time_since_epoch().count();
    } catch (const std::exception& e) {
        std::cerr << "[PakIndex] Failed to get file metadata: " << e.what() << "\n";
        return false;
    }
    
    // Database operations with lock
    std::lock_guard<std::mutex> lock(db_mutex_);
    
    // RAII transaction helper - automatically rolls back on scope exit unless committed
    bool transaction_committed = false;
    char* errMsg = nullptr;
    
    // Start transaction with IMMEDIATE to avoid lock contention
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::cerr << "[PakIndex] Failed to begin transaction: " << (errMsg ? errMsg : "unknown error") << "\n";
        sqlite3_free(errMsg);
        return false;
    }
    
    // Auto-rollback guard - rolls back if we exit scope without committing
    auto rollback_guard = [&]() {
        if (!transaction_committed) {
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    };
    
    // Use unique_ptr with custom deleter for automatic cleanup
    struct ScopeGuard {
        std::function<void()> cleanup;
        ~ScopeGuard() { if (cleanup) cleanup(); }
    } guard{rollback_guard};
    
    // Delete existing entries for this PAK (cascade-like behavior)
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "DELETE FROM files WHERE pak_id IN (SELECT id FROM paks WHERE path = ?)";
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "[PakIndex] Failed to prepare delete files stmt: " << sqlite3_errmsg(db_) << "\n";
            return false;
        }
        
        sqlite3_bind_text(stmt, 1, pak_path.string().c_str(), -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            std::cerr << "[PakIndex] Failed to delete existing files: " << sqlite3_errmsg(db_) << "\n";
            return false;
        }
    }
    
    // Delete existing PAK entry
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "DELETE FROM paks WHERE path = ?";
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "[PakIndex] Failed to prepare delete pak stmt: " << sqlite3_errmsg(db_) << "\n";
            return false;
        }
        
        sqlite3_bind_text(stmt, 1, pak_path.string().c_str(), -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            std::cerr << "[PakIndex] Failed to delete existing pak: " << sqlite3_errmsg(db_) << "\n";
            return false;
        }
    }
    
    // Insert PAK entry
    int64_t pak_id = 0;
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "INSERT INTO paks (path, file_size, last_modified, indexed_at) VALUES (?, ?, ?, ?)";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::cerr << "[PakIndex] Failed to prepare pak insert: " << sqlite3_errmsg(db_) << "\n";
            return false;
        }
        
        sqlite3_bind_text(stmt, 1, pak_path.string().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, file_size);
        sqlite3_bind_int64(stmt, 3, last_modified);
        sqlite3_bind_int64(stmt, 4, std::chrono::system_clock::now().time_since_epoch().count());
        
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::cerr << "[PakIndex] Failed to insert pak entry: " << sqlite3_errmsg(db_) << "\n";
            sqlite3_finalize(stmt);
            return false;
        }
        
        pak_id = sqlite3_last_insert_rowid(db_);
        sqlite3_finalize(stmt);
    }
    
    // Prepare file insert statement (reused for all files)
    sqlite3_stmt* file_stmt = nullptr;
    const char* file_sql = "INSERT INTO files (pak_id, path, path_lower) VALUES (?, ?, ?)";
    if (sqlite3_prepare_v2(db_, file_sql, -1, &file_stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[PakIndex] Failed to prepare file insert: " << sqlite3_errmsg(db_) << "\n";
        return false;
    }
    
    // Insert all files
    for (const auto& file : files) {
        std::string path_lower = file.path;
        std::replace(path_lower.begin(), path_lower.end(), '\\', '/');
        std::transform(path_lower.begin(), path_lower.end(), path_lower.begin(), ::tolower);
        
        sqlite3_reset(file_stmt);
        sqlite3_bind_int64(file_stmt, 1, pak_id);
        sqlite3_bind_text(file_stmt, 2, file.path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(file_stmt, 3, path_lower.c_str(), -1, SQLITE_TRANSIENT);
        
        if (sqlite3_step(file_stmt) != SQLITE_DONE) {
            // Log but continue - some files might have weird paths
            std::cerr << "[PakIndex] Warning: Failed to insert file '" << file.path 
                      << "': " << sqlite3_errmsg(db_) << "\n";
        }
    }
    
    sqlite3_finalize(file_stmt);
    
    // Commit transaction - mark as committed so guard doesn't rollback
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::cerr << "[PakIndex] Failed to commit transaction: " << (errMsg ? errMsg : "unknown") << "\n";
        sqlite3_free(errMsg);
        return false;
    }
    transaction_committed = true;
    
    return true;
}

bool PakIndex::build_index(std::function<void(const std::string&, int, int)> progress_callback) {
    ready_ = false;
    cancel_requested_ = false;
    progress_ = 0;
    
    if (!open_database()) {
        std::cerr << "[PakIndex] Failed to open database\n";
        return false;
    }
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Collect all PAK paths
    std::vector<std::filesystem::path> all_paks;
    
    if (!game_path_.empty()) {
        auto paks = scan_for_paks(game_path_);
        all_paks.insert(all_paks.end(), paks.begin(), paks.end());
    }
    
    if (!mods_path_.empty()) {
        auto paks = scan_for_paks(mods_path_);
        all_paks.insert(all_paks.end(), paks.begin(), paks.end());
    }
    
    if (all_paks.empty()) {
        std::cerr << "[PakIndex] No PAKs found to index\n";
        ready_ = true;
        return false;
    }
    
    std::cerr << "[PakIndex] Found " << all_paks.size() << " PAKs to check\n";
    
    // Check which PAKs need updating
    std::vector<std::filesystem::path> paks_to_update;
    for (const auto& pak : all_paks) {
        if (pak_needs_update(pak)) {
            paks_to_update.push_back(pak);
        }
    }
    
    std::cerr << "[PakIndex] " << paks_to_update.size() << " PAKs need indexing\n";
    
    if (progress_callback) {
        progress_callback("Indexing PAKs...", 0, static_cast<int>(paks_to_update.size()));
    }
    
    // Process PAKs that need updating
    std::atomic<int> completed{0};
    std::atomic<int> success_count{0};
    
    // Use thread pool approach
    size_t chunk_size = (paks_to_update.size() + thread_count_ - 1) / thread_count_;
    std::vector<std::future<void>> futures;
    
    for (unsigned int t = 0; t < thread_count_; t++) {
        size_t start_idx = t * chunk_size;
        size_t end_idx = std::min(start_idx + chunk_size, paks_to_update.size());
        
        if (start_idx >= paks_to_update.size()) break;
        
        futures.push_back(std::async(std::launch::async, [&, start_idx, end_idx]() {
            for (size_t i = start_idx; i < end_idx && !cancel_requested_; i++) {
                const auto& pak_path = paks_to_update[i];
                
                if (index_pak_to_db(pak_path)) {
                    success_count++;
                }
                
                completed++;
                progress_ = paks_to_update.empty() ? 100 : 
                    (completed * 100) / static_cast<int>(paks_to_update.size());
                
                if (progress_callback && (completed % 10 == 0 || completed == paks_to_update.size())) {
                    progress_callback(pak_path.filename().string(),
                                    completed.load(),
                                    static_cast<int>(paks_to_update.size()));
                }
            }
        }));
    }
    
    // Wait for all threads
    for (auto& f : futures) {
        f.get();
    }
    
    if (cancel_requested_) {
        std::cerr << "[PakIndex] Indexing cancelled\n";
        return false;
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    std::cerr << "[PakIndex] Indexing complete: " << total_paks() << " PAKs, "
              << total_files() << " files in " << duration.count() << "ms ("
              << success_count << " updated)\n";
    
    ready_ = true;
    return success_count > 0;
}

std::filesystem::path PakIndex::find_pak_for_file(const std::string& virtual_path) const {
    if (!db_ || !ready_) {
        return {};
    }
    
    // Normalize path
    std::string normalized = virtual_path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
    
    std::lock_guard<std::mutex> lock(db_mutex_);
    
    // Use cached prepared statement for better performance
    static const char* sql = R"(
        SELECT p.path FROM paks p
        JOIN files f ON f.pak_id = p.id
        WHERE f.path_lower = ?
        LIMIT 1
    )";
    
    sqlite3_stmt* stmt = get_or_prepare_stmt("find_pak_for_file", sql);
    if (!stmt) {
        return {};
    }
    
    sqlite3_bind_text(stmt, 1, normalized.c_str(), -1, SQLITE_TRANSIENT);
    
    std::filesystem::path result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (path) {
            result = path;
        }
    }
    
    // Don't finalize - it's cached. Just reset happens in get_or_prepare_stmt
    return result;
}

std::vector<std::filesystem::path> PakIndex::find_paks_for_pattern(const std::string& pattern) const {
    std::vector<std::filesystem::path> results;
    
    if (!db_ || !ready_) {
        return results;
    }
    
    std::string pattern_lower = pattern;
    std::transform(pattern_lower.begin(), pattern_lower.end(), pattern_lower.begin(), ::tolower);
    
    std::lock_guard<std::mutex> lock(db_mutex_);
    
    // Use cached prepared statement
    static const char* sql = R"(
        SELECT DISTINCT p.path FROM paks p
        JOIN files f ON f.pak_id = p.id
        WHERE f.path_lower LIKE ?
    )";
    
    sqlite3_stmt* stmt = get_or_prepare_stmt("find_paks_for_pattern", sql);
    if (!stmt) {
        return results;
    }
    
    std::string like_pattern = "%" + pattern_lower + "%";
    sqlite3_bind_text(stmt, 1, like_pattern.c_str(), -1, SQLITE_TRANSIENT);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (path) {
            results.emplace_back(path);
        }
    }
    
    // Don't finalize cached statement
    return results;
}

size_t PakIndex::total_files() const {
    if (!db_) return 0;
    
    std::lock_guard<std::mutex> lock(db_mutex_);
    
    static const char* sql = "SELECT COUNT(*) FROM files";
    sqlite3_stmt* stmt = get_or_prepare_stmt("total_files", sql);
    if (!stmt) return 0;
    
    size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    
    return count;
}

size_t PakIndex::total_paks() const {
    if (!db_) return 0;
    
    std::lock_guard<std::mutex> lock(db_mutex_);
    
    static const char* sql = "SELECT COUNT(*) FROM paks";
    sqlite3_stmt* stmt = get_or_prepare_stmt("total_paks", sql);
    if (!stmt) return 0;
    
    size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    
    return count;
}

} // namespace enfusion
