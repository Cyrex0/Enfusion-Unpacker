/**
 * Enfusion Unpacker - Result Type
 * 
 * Provides a Result<T, E> type for consistent error handling.
 * Similar to Rust's Result or C++23's std::expected.
 */

#pragma once

#include <variant>
#include <string>
#include <optional>
#include <stdexcept>

namespace enfusion {

/**
 * Error information with code and message
 */
struct Error {
    enum class Code {
        None = 0,
        FileNotFound,
        InvalidFormat,
        CompressionError,
        DatabaseError,
        IoError,
        ParseError,
        OutOfMemory,
        InvalidArgument,
        NotSupported,
        Unknown
    };
    
    Code code = Code::None;
    std::string message;
    std::string context;  // Additional context (file path, etc.)
    
    Error() = default;
    Error(Code c, std::string msg) : code(c), message(std::move(msg)) {}
    Error(Code c, std::string msg, std::string ctx) 
        : code(c), message(std::move(msg)), context(std::move(ctx)) {}
    
    bool ok() const { return code == Code::None; }
    
    std::string full_message() const {
        if (context.empty()) {
            return message;
        }
        return message + " [" + context + "]";
    }
    
    // Common error constructors
    static Error file_not_found(const std::string& path) {
        return Error(Code::FileNotFound, "File not found", path);
    }
    
    static Error invalid_format(const std::string& msg, const std::string& path = "") {
        return Error(Code::InvalidFormat, msg, path);
    }
    
    static Error compression_error(const std::string& msg) {
        return Error(Code::CompressionError, msg);
    }
    
    static Error database_error(const std::string& msg) {
        return Error(Code::DatabaseError, msg);
    }
    
    static Error io_error(const std::string& msg, const std::string& path = "") {
        return Error(Code::IoError, msg, path);
    }
    
    static Error parse_error(const std::string& msg, const std::string& ctx = "") {
        return Error(Code::ParseError, msg, ctx);
    }
};

/**
 * Result type that holds either a value T or an Error
 * 
 * Usage:
 *   Result<std::vector<uint8_t>> read_file(const std::string& path);
 *   
 *   auto result = read_file("test.bin");
 *   if (result) {
 *       auto& data = result.value();
 *       // use data...
 *   } else {
 *       std::cerr << "Error: " << result.error().message << "\n";
 *   }
 */
template<typename T>
class Result {
public:
    // Success construction
    Result(T value) : data_(std::move(value)) {}
    
    // Error construction
    Result(Error error) : data_(std::move(error)) {}
    
    // Check if result is successful
    bool ok() const { return std::holds_alternative<T>(data_); }
    bool has_value() const { return ok(); }
    explicit operator bool() const { return ok(); }
    
    // Access value (throws if error)
    T& value() {
        if (!ok()) {
            throw std::runtime_error("Result contains error: " + error().message);
        }
        return std::get<T>(data_);
    }
    
    const T& value() const {
        if (!ok()) {
            throw std::runtime_error("Result contains error: " + error().message);
        }
        return std::get<T>(data_);
    }
    
    // Access value with default
    T value_or(T default_value) const {
        if (ok()) {
            return std::get<T>(data_);
        }
        return default_value;
    }
    
    // Access error
    const Error& error() const {
        if (ok()) {
            static Error no_error;
            return no_error;
        }
        return std::get<Error>(data_);
    }
    
    // Pointer-like access
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    T& operator*() { return value(); }
    const T& operator*() const { return value(); }
    
    // Transform the value if present
    template<typename F>
    auto map(F&& func) -> Result<decltype(func(std::declval<T>()))> {
        using U = decltype(func(std::declval<T>()));
        if (ok()) {
            return Result<U>(func(value()));
        }
        return Result<U>(error());
    }
    
    // Convert to optional (discards error info)
    std::optional<T> to_optional() const {
        if (ok()) {
            return std::get<T>(data_);
        }
        return std::nullopt;
    }

private:
    std::variant<T, Error> data_;
};

/**
 * Specialization for void results (just success/failure)
 */
template<>
class Result<void> {
public:
    Result() : error_(std::nullopt) {}
    Result(Error error) : error_(std::move(error)) {}
    
    bool ok() const { return !error_.has_value(); }
    explicit operator bool() const { return ok(); }
    
    const Error& error() const {
        static Error no_error;
        return error_.value_or(no_error);
    }
    
    static Result success() { return Result(); }
    static Result failure(Error err) { return Result(std::move(err)); }

private:
    std::optional<Error> error_;
};

// Helper macros for early return on error
#define TRY(expr) \
    do { \
        auto _result = (expr); \
        if (!_result.ok()) { \
            return _result.error(); \
        } \
    } while(0)

#define TRY_ASSIGN(var, expr) \
    auto _result_##var = (expr); \
    if (!_result_##var.ok()) { \
        return _result_##var.error(); \
    } \
    auto var = std::move(_result_##var.value())

} // namespace enfusion
