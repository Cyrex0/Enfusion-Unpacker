/**
 * Enfusion Unpacker - PAK Index
 * 
 * SQLite-based persistent file index for fast file lookups.
 * Uses multi-threaded indexing with proper database storage.
 */

#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>
#include <thread>
#include <unordered_map>

// Forward declare SQLite
struct sqlite3;
struct sqlite3_stmt;

namespace enfusion {

/**
 * SQLite-based persistent PAK file index
 * 
 * Stores file-to-PAK mappings in a SQLite database for:
 * - Fast startup (no need to re-scan all PAKs)
 * - Efficient lookups (indexed queries)
 * - Small disk footprint (~10-20MB vs 600MB+ JSON)
 * - Thread-safe access
 * - Prepared statement caching for better query performance
 */
class PakIndex {
public:
    static PakIndex& instance();
    
    PakIndex();
    ~PakIndex();
    
    // Non-copyable
    PakIndex(const PakIndex&) = delete;
    PakIndex& operator=(const PakIndex&) = delete;
    
    // Set paths for scanning
    void set_game_path(const std::filesystem::path& path);
    void set_mods_path(const std::filesystem::path& path);
    void set_db_path(const std::filesystem::path& path);
    
    // Open/create the database
    bool open_database();
    void close_database();
    
    // Scan and index all PAKs (multi-threaded)
    // Returns true if index was updated
    bool build_index(std::function<void(const std::string&, int, int)> progress_callback = nullptr);
    
    // Find which PAK contains a file (fast lookup)
    std::filesystem::path find_pak_for_file(const std::string& virtual_path) const;
    
    // Find all PAKs that might contain files matching a pattern
    std::vector<std::filesystem::path> find_paks_for_pattern(const std::string& pattern) const;
    
    // Get stats
    size_t total_files() const;
    size_t total_paks() const;
    
    // Check if index is loaded/ready
    bool is_ready() const { return ready_.load(); }
    
    // Get indexing progress (0-100)
    int progress() const { return progress_.load(); }
    
    // Cancel ongoing indexing
    void cancel_indexing() { cancel_requested_ = true; }
    
private:
    // Scan a directory for PAK files
    std::vector<std::filesystem::path> scan_for_paks(const std::filesystem::path& dir);
    
    // Check if a PAK needs re-indexing (by size/mtime)
    bool pak_needs_update(const std::filesystem::path& pak_path) const;
    
    // Index a single PAK file and insert into database
    bool index_pak_to_db(const std::filesystem::path& pak_path);
    
    // Create database schema
    bool create_schema();
    
    // Prepared statement cache management
    sqlite3_stmt* get_or_prepare_stmt(const std::string& name, const char* sql) const;
    void clear_stmt_cache();
    
    mutable std::mutex mutex_;
    mutable std::mutex db_mutex_;  // Separate mutex for DB operations
    
    // Database
    sqlite3* db_ = nullptr;
    std::filesystem::path db_path_;
    
    // Prepared statement cache (name -> statement)
    mutable std::unordered_map<std::string, sqlite3_stmt*> stmt_cache_;
    
    // Paths
    std::filesystem::path game_path_;
    std::filesystem::path mods_path_;
    
    // State
    std::atomic<bool> ready_{false};
    std::atomic<int> progress_{0};
    std::atomic<bool> cancel_requested_{false};
    unsigned int thread_count_ = 4;
};

} // namespace enfusion
