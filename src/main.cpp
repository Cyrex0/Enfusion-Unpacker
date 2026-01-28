/**
 * Enfusion Unpacker - Entry Point
 */

#include "gui/app.hpp"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <fstream>
#include <iostream>
#include <ctime>

// Global log file stream
static std::ofstream g_log_file;

// Custom stream buffer that writes to file
class LogStreamBuf : public std::streambuf {
public:
    LogStreamBuf(std::ofstream& file) : file_(file) {}
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

static LogStreamBuf* g_log_buf = nullptr;

void init_logging() {
    // Open log file with timestamp
    g_log_file.open("enfusion_unpacker.log", std::ios::out | std::ios::trunc);
    if (g_log_file.is_open()) {
        // Write header
        std::time_t now = std::time(nullptr);
        char time_buf[64];
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        g_log_file << "=== Enfusion Unpacker Log - " << time_buf << " ===\n\n";
        g_log_file.flush();
        
        // Redirect cerr to log file
        g_log_buf = new LogStreamBuf(g_log_file);
        std::cerr.rdbuf(g_log_buf);
    }
}

void shutdown_logging() {
    if (g_log_file.is_open()) {
        g_log_file << "\n=== Log End ===\n";
        g_log_file.close();
    }
    delete g_log_buf;
    g_log_buf = nullptr;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // Enable high DPI awareness
    SetProcessDPIAware();
#endif
    
    // Initialize file logging
    init_logging();
    
    auto& app = enfusion::App::instance();
    
    if (!app.init()) {
        shutdown_logging();
        return 1;
    }
    
    app.run();
    app.shutdown();
    
    shutdown_logging();
    return 0;
}

#ifdef _WIN32
// Windows entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    return main(__argc, __argv);
}
#endif
