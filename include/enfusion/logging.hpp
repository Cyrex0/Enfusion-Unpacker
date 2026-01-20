/**
 * Enfusion Unpacker - Logging System
 * 
 * Provides structured logging with configurable levels.
 * Thread-safe, supports file and console output.
 */

#pragma once

#include <string>
#include <string_view>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <filesystem>
#include <functional>

namespace enfusion {

/**
 * Log severity levels
 */
enum class LogLevel {
    Debug = 0,   // Detailed debugging information
    Info = 1,    // General operational messages
    Warning = 2, // Non-critical issues
    Error = 3,   // Critical failures
    None = 4     // Disable all logging
};

/**
 * Convert LogLevel to string representation
 */
constexpr const char* log_level_string(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:   return "DEBUG";
        case LogLevel::Info:    return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error:   return "ERROR";
        default:                return "UNKNOWN";
    }
}

/**
 * Thread-safe logger with level filtering
 */
class Logger {
public:
    static Logger& instance() {
        static Logger instance;
        return instance;
    }
    
    // Non-copyable
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    
    /**
     * Set minimum log level (messages below this level are ignored)
     */
    void set_level(LogLevel level) {
        std::lock_guard<std::mutex> lock(mutex_);
        min_level_ = level;
    }
    
    LogLevel get_level() const {
        return min_level_;
    }
    
    /**
     * Enable/disable console output
     */
    void set_console_output(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        console_enabled_ = enabled;
    }
    
    /**
     * Set log file path (opens file for appending)
     */
    bool set_file(const std::filesystem::path& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (file_.is_open()) {
            file_.close();
        }
        
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }
        
        file_.open(path, std::ios::app);
        return file_.is_open();
    }
    
    /**
     * Close log file
     */
    void close_file() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            file_.close();
        }
    }
    
    /**
     * Set callback for UI log display
     */
    void set_callback(std::function<void(LogLevel, const std::string&)> callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(callback);
    }
    
    /**
     * Log a message at the specified level
     */
    template<typename... Args>
    void log(LogLevel level, std::string_view tag, std::string_view format, Args&&... args) {
        if (level < min_level_) return;
        
        std::string message = format_message(level, tag, format, std::forward<Args>(args)...);
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (console_enabled_) {
            // Use cerr for errors, cout for others
            auto& stream = (level == LogLevel::Error) ? std::cerr : std::cout;
            stream << message << std::endl;
        }
        
        if (file_.is_open()) {
            file_ << message << std::endl;
            file_.flush();
        }
        
        if (callback_) {
            callback_(level, message);
        }
    }
    
    // Convenience methods
    template<typename... Args>
    void debug(std::string_view tag, std::string_view format, Args&&... args) {
        log(LogLevel::Debug, tag, format, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    void info(std::string_view tag, std::string_view format, Args&&... args) {
        log(LogLevel::Info, tag, format, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    void warn(std::string_view tag, std::string_view format, Args&&... args) {
        log(LogLevel::Warning, tag, format, std::forward<Args>(args)...);
    }
    
    template<typename... Args>
    void error(std::string_view tag, std::string_view format, Args&&... args) {
        log(LogLevel::Error, tag, format, std::forward<Args>(args)...);
    }

private:
    Logger() = default;
    ~Logger() {
        if (file_.is_open()) {
            file_.close();
        }
    }
    
    template<typename... Args>
    std::string format_message(LogLevel level, std::string_view tag, 
                               std::string_view format, Args&&... args) {
        std::ostringstream ss;
        
        // Timestamp
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        
        std::tm tm_buf;
#ifdef _WIN32
        localtime_s(&tm_buf, &time);
#else
        localtime_r(&time, &tm_buf);
#endif
        
        ss << std::put_time(&tm_buf, "%H:%M:%S") << '.'
           << std::setfill('0') << std::setw(3) << ms.count() << ' ';
        
        // Level
        ss << '[' << log_level_string(level) << "] ";
        
        // Tag
        if (!tag.empty()) {
            ss << '[' << tag << "] ";
        }
        
        // Format message with variadic args (simple concatenation for now)
        ss << format;
        ((ss << args), ...);
        
        return ss.str();
    }
    
    std::mutex mutex_;
    LogLevel min_level_ = LogLevel::Info;  // Default to Info level
    bool console_enabled_ = true;
    std::ofstream file_;
    std::function<void(LogLevel, const std::string&)> callback_;
};

// Convenience macros for logging with automatic tag from function name
#define LOG_DEBUG(tag, ...) \
    enfusion::Logger::instance().debug(tag, __VA_ARGS__)

#define LOG_INFO(tag, ...) \
    enfusion::Logger::instance().info(tag, __VA_ARGS__)

#define LOG_WARN(tag, ...) \
    enfusion::Logger::instance().warn(tag, __VA_ARGS__)

#define LOG_ERROR(tag, ...) \
    enfusion::Logger::instance().error(tag, __VA_ARGS__)

} // namespace enfusion
