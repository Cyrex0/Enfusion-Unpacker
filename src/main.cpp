/**
 * Enfusion Unpacker - Entry Point
 * 
 * Supports both GUI and CLI modes:
 *   GUI:  EnfusionUnpacker.exe
 *   CLI:  EnfusionUnpacker.exe --extract <pak_path> --output <output_dir> [--filter <pattern>]
 *         EnfusionUnpacker.exe --list <pak_path>
 *         EnfusionUnpacker.exe --help
 */

#include "gui/app.hpp"
#include "enfusion/pak_reader.hpp"
#include "enfusion/addon_extractor.hpp"
#include "enfusion/logging.hpp"
#include "enfusion/xob_parser.hpp"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <fstream>
#include <iostream>
#include <ctime>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>

// Global log file stream
static std::ofstream g_log_file;

// Custom stream buffer that writes to file
class LogStreamBuf : public std::streambuf {
public:
    explicit LogStreamBuf(std::ofstream& file) : file_(file) {}
protected:
    int overflow(int c) override {
        if (c != EOF) {
            file_.put(static_cast<char>(c));
            file_.flush();
        }
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        file_.write(s, n);
        file_.flush();
        return n;
    }
private:
    std::ofstream& file_;
};

static std::unique_ptr<LogStreamBuf> g_log_buf;
static std::streambuf* g_original_cerr_buf = nullptr;

void init_logging() {
    // Logger auto-initializes with Debug level to enfusion_unpacker.log
    // Just trigger the singleton to ensure it's created
    auto& logger = enfusion::Logger::instance();
    static_cast<void>(logger);  // Suppress unused warning (proper C++ cast)
    
    // Redirect std::cerr to log file for legacy output
    g_log_file.open("enfusion_unpacker.log", std::ios::out | std::ios::app);
    if (g_log_file.is_open()) {
        g_log_buf = std::make_unique<LogStreamBuf>(g_log_file);
        g_original_cerr_buf = std::cerr.rdbuf(g_log_buf.get());
    }
}

void shutdown_logging() {
    // Restore original cerr buffer before releasing our custom buffer
    if (g_original_cerr_buf) {
        std::cerr.rdbuf(g_original_cerr_buf);
        g_original_cerr_buf = nullptr;
    }
    
    g_log_buf.reset();
    
    if (g_log_file.is_open()) {
        g_log_file.close();
    }
}

// CLI argument parsing
struct CliArgs {
    bool show_help = false;
    bool cli_mode = false;
    bool list_mode = false;
    bool extract_mode = false;
    bool xob_test_mode = false;
    std::string pak_path;
    std::string output_dir;
    std::string filter_pattern;
    std::string xob_path;
    bool verbose = false;
    bool debug_logging = false;
};

void print_help() {
    std::cout << R"(
Enfusion Unpacker - Arma Reforger PAK Extraction Tool

Usage:
  EnfusionUnpacker.exe                              Launch GUI
  EnfusionUnpacker.exe --help                       Show this help
  EnfusionUnpacker.exe --list <pak_path>            List files in PAK
  EnfusionUnpacker.exe --extract <pak_path> --output <dir> [options]
    EnfusionUnpacker.exe --xob-test <pak_path> --xob <path_in_pak>

Options:
  --help, -h           Show this help message
  --list, -l           List files in the specified PAK
  --extract, -e        Extract files from PAK
  --output, -o <dir>   Output directory for extraction
  --filter, -f <pat>   Only extract files matching pattern (glob-style)
    --xob-test           Parse a single XOB from a PAK (headless diagnostics)
    --xob <path>         XOB path inside the PAK (use with --xob-test)
  --verbose, -v        Verbose output
  --debug, -d          Enable debug logging for troubleshooting

Examples:
  EnfusionUnpacker.exe --list "C:\Games\ArmaReforger\addons\data.pak"
  EnfusionUnpacker.exe --extract "data.pak" --output "C:\Extracted" --filter "*.edds"
  EnfusionUnpacker.exe -e addon.pak -o ./output -f "Textures/*"
  EnfusionUnpacker.exe --debug -e addon.pak -o ./output   # With debug logs
    EnfusionUnpacker.exe --xob-test "data.pak" --xob "Assets/Vehicles/Wheeled/Cougar/CougarH_Base.xob"

)" << std::endl;
}

CliArgs parse_args(int argc, char* argv[]) {
    CliArgs args;
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "--help" || arg == "-h") {
            args.show_help = true;
            args.cli_mode = true;
        }
        else if (arg == "--list" || arg == "-l") {
            args.list_mode = true;
            args.cli_mode = true;
            if (i + 1 < argc) {
                args.pak_path = argv[++i];
            }
        }
        else if (arg == "--extract" || arg == "-e") {
            args.extract_mode = true;
            args.cli_mode = true;
            if (i + 1 < argc) {
                args.pak_path = argv[++i];
            }
        }
        else if (arg == "--xob-test" || arg == "-x") {
            args.xob_test_mode = true;
            args.cli_mode = true;
            if (i + 1 < argc) {
                args.pak_path = argv[++i];
            }
        }
        else if (arg == "--output" || arg == "-o") {
            if (i + 1 < argc) {
                args.output_dir = argv[++i];
            }
        }
        else if (arg == "--filter" || arg == "-f") {
            if (i + 1 < argc) {
                args.filter_pattern = argv[++i];
            }
        }
        else if (arg == "--xob") {
            if (i + 1 < argc) {
                args.xob_path = argv[++i];
            }
        }
        else if (arg == "--verbose" || arg == "-v") {
            args.verbose = true;
        }
        else if (arg == "--debug" || arg == "-d") {
            args.debug_logging = true;
        }
    }
    
    return args;
}

// Simple glob pattern matching
bool matches_pattern(const std::string& str, const std::string& pattern) {
    if (pattern.empty()) return true;
    
    std::string str_lower = str;
    std::string pat_lower = pattern;
    std::transform(str_lower.begin(), str_lower.end(), str_lower.begin(), ::tolower);
    std::transform(pat_lower.begin(), pat_lower.end(), pat_lower.begin(), ::tolower);
    
    // Simple pattern: * matches anything
    if (pat_lower == "*") return true;
    
    // Check if pattern contains *
    size_t star_pos = pat_lower.find('*');
    if (star_pos == std::string::npos) {
        // No wildcard - exact substring match
        return str_lower.find(pat_lower) != std::string::npos;
    }
    
    // Pattern like "*.edds" - suffix match
    if (star_pos == 0) {
        std::string suffix = pat_lower.substr(1);
        return str_lower.length() >= suffix.length() &&
               str_lower.compare(str_lower.length() - suffix.length(), suffix.length(), suffix) == 0;
    }
    
    // Pattern like "Textures/*" - prefix match
    if (star_pos == pat_lower.length() - 1) {
        std::string prefix = pat_lower.substr(0, star_pos);
        return str_lower.compare(0, prefix.length(), prefix) == 0;
    }
    
    // Pattern like "path/*.edds" - prefix and suffix
    std::string prefix = pat_lower.substr(0, star_pos);
    std::string suffix = pat_lower.substr(star_pos + 1);
    return str_lower.compare(0, prefix.length(), prefix) == 0 &&
           str_lower.length() >= suffix.length() &&
           str_lower.compare(str_lower.length() - suffix.length(), suffix.length(), suffix) == 0;
}

int run_cli(const CliArgs& args) {
    if (args.show_help) {
        print_help();
        return 0;
    }
    
    if (args.pak_path.empty()) {
        std::cerr << "Error: No PAK file specified\n";
        print_help();
        return 1;
    }
    
    if (!std::filesystem::exists(args.pak_path)) {
        std::cerr << "Error: PAK file not found: " << args.pak_path << "\n";
        return 1;
    }
    
    // Open PAK file
    enfusion::PakReader reader;
    if (!reader.open(args.pak_path)) {
        std::cerr << "Error: Failed to open PAK file: " << args.pak_path << "\n";
        return 1;
    }
    
    auto files = reader.list_files();
    std::cout << "PAK: " << args.pak_path << "\n";
    std::cout << "Files: " << files.size() << "\n\n";

    // XOB test mode (headless diagnostics)
    if (args.xob_test_mode) {
        std::string xob_path = args.xob_path;

        if (xob_path.empty()) {
            // Auto-pick a Cougar XOB if available, otherwise first .xob
            std::string first_xob;
            std::string cougar_xob;
            for (const auto& entry : files) {
                std::string p = entry.path;
                std::string lower = p;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                if (lower.find(".xob") == std::string::npos) continue;
                if (first_xob.empty()) first_xob = p;
                if (lower.find("cougar") != std::string::npos) {
                    cougar_xob = p;
                    break;
                }
            }

            if (!cougar_xob.empty()) {
                xob_path = cougar_xob;
            } else if (!first_xob.empty()) {
                xob_path = first_xob;
            } else {
                std::cerr << "Error: No XOB files found in PAK\n";
                LOG_ERROR("XobTest", "No XOB files found in PAK");
                return 1;
            }

            LOG_INFO("XobTest", "Auto-selected XOB: " << xob_path);
        }

        auto data = reader.read_file(xob_path);
        if (data.empty()) {
            std::cerr << "Error: Failed to read XOB from PAK: " << xob_path << "\n";
            LOG_ERROR("XobTest", "Failed to read XOB from PAK: " << xob_path);
            return 1;
        }

        LOG_INFO("XobTest", "XOB: " << xob_path << " (" << data.size() << " bytes)");

        enfusion::XobParser parser(std::span<const uint8_t>(data.data(), data.size()));
        auto mesh_opt = parser.parse(0);
        if (!mesh_opt) {
            std::cerr << "Error: Failed to parse XOB\n";
            LOG_ERROR("XobTest", "Failed to parse XOB");
            return 1;
        }

        const auto& mesh = *mesh_opt;
        uint32_t total_tris = static_cast<uint32_t>(mesh.indices.size() / 3);
        LOG_INFO("XobTest", "Materials: " << mesh.materials.size() 
             << " Triangles: " << total_tris 
             << " Ranges: " << mesh.material_ranges.size());

        // UV diagnostics (flat-color detection)
        if (!mesh.vertices.empty()) {
            float u_min = std::numeric_limits<float>::max();
            float v_min = std::numeric_limits<float>::max();
            float u_max = std::numeric_limits<float>::lowest();
            float v_max = std::numeric_limits<float>::lowest();
            size_t near_zero = 0;
            size_t sample_count = 0;
            const size_t stride = std::max<size_t>(1, mesh.vertices.size() / 2000);
            for (size_t i = 0; i < mesh.vertices.size(); i += stride) {
                const auto& uv = mesh.vertices[i].uv;
                u_min = std::min(u_min, uv.x);
                v_min = std::min(v_min, uv.y);
                u_max = std::max(u_max, uv.x);
                v_max = std::max(v_max, uv.y);
                if (std::abs(uv.x) < 1e-4f && std::abs(uv.y) < 1e-4f) {
                    near_zero++;
                }
                sample_count++;
            }
            float u_range = u_max - u_min;
            float v_range = v_max - v_min;
            float zero_pct = (sample_count > 0) ? (100.0f * near_zero / static_cast<float>(sample_count)) : 0.0f;
            LOG_INFO("XobTest", "UV range: u=[" << u_min << "," << u_max << "] v=[" << v_min << "," << v_max 
                     << "] (du=" << u_range << " dv=" << v_range << ") zero%=" << zero_pct);
        }

        uint32_t covered = 0;
        for (const auto& r : mesh.material_ranges) {
            covered += r.triangle_count;
            LOG_INFO("XobTest", "mat=" << r.material_index
                     << " tris=" << r.triangle_start << "-" << r.triangle_end
                     << " count=" << r.triangle_count);
        }
        LOG_INFO("XobTest", "Coverage: " << covered << "/" << total_tris << " tris");
        return (covered == total_tris) ? 0 : 2;
    }
    
    // List mode
    if (args.list_mode) {
        size_t matched = 0;
        for (const auto& entry : files) {
            if (matches_pattern(entry.path, args.filter_pattern)) {
                std::cout << entry.path;
                if (args.verbose) {
                    std::cout << " (" << entry.size << " bytes compressed, "
                              << entry.original_size << " bytes uncompressed)";
                }
                std::cout << "\n";
                matched++;
            }
        }
        std::cout << "\nMatched: " << matched << " / " << files.size() << " files\n";
        return 0;
    }
    
    // Extract mode
    if (args.extract_mode) {
        if (args.output_dir.empty()) {
            std::cerr << "Error: No output directory specified (use --output)\n";
            return 1;
        }
        
        std::filesystem::path out_path(args.output_dir);
        std::filesystem::create_directories(out_path);
        
        size_t extracted = 0;
        size_t failed = 0;
        
        for (const auto& entry : files) {
            if (!matches_pattern(entry.path, args.filter_pattern)) {
                continue;
            }
            
            if (args.verbose) {
                std::cout << "Extracting: " << entry.path << "..." << std::flush;
            }
            
            auto data = reader.read_file(entry.path);
            if (data.empty() && entry.original_size > 0) {
                if (args.verbose) std::cout << " FAILED\n";
                failed++;
                continue;
            }
            
            // Create output path
            std::filesystem::path file_out = out_path / entry.path;
            std::filesystem::create_directories(file_out.parent_path());
            
            // Write file
            std::ofstream ofs(file_out, std::ios::binary);
            if (ofs && !data.empty()) {
                ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
                extracted++;
                if (args.verbose) std::cout << " OK\n";
            } else {
                if (args.verbose) std::cout << " WRITE FAILED\n";
                failed++;
            }
        }
        
        std::cout << "\nExtracted: " << extracted << " files";
        if (failed > 0) std::cout << " (" << failed << " failed)";
        std::cout << "\n";
        
        return failed > 0 ? 1 : 0;
    }
    
    return 0;
}

int run_gui() {
    auto& app = enfusion::App::instance();
    
    if (!app.init()) {
        shutdown_logging();
        return 1;
    }
    
    app.run();
    app.shutdown();
    
    return 0;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // Enable high DPI awareness
    SetProcessDPIAware();
#endif
    
    // Parse command line arguments
    CliArgs args = parse_args(argc, argv);
    
    // Initialize logging
    if (!args.cli_mode || args.xob_test_mode || args.debug_logging) {
        init_logging();
    }
    
    // Enable debug logging if requested
    if (args.debug_logging) {
        enfusion::Logger::instance().set_level(enfusion::LogLevel::Debug);
        LOG_INFO("App", "Debug logging enabled");
    }
    
    int result;
    if (args.cli_mode) {
        result = run_cli(args);
    } else {
        result = run_gui();
    }
    
    if (!args.cli_mode || args.xob_test_mode || args.debug_logging) {
        shutdown_logging();
    }
    
    return result;
}

#ifdef _WIN32
// Windows entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    return main(__argc, __argv);
}
#endif
