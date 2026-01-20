/**
 * Enfusion Unpacker - Main Application
 */

#pragma once

#include "enfusion/types.hpp"
#include <memory>
#include <string>

struct GLFWwindow;

namespace enfusion {

class MainWindow;

/**
 * Main application class.
 * Handles window creation, rendering loop, and global state.
 */
class App {
public:
    static App& instance();

    bool init();
    void run();
    void shutdown();
    void quit() { running_ = false; }

    // Settings
    AppSettings& settings() { return settings_; }
    const AppSettings& settings() const { return settings_; }
    void save_settings();
    void load_settings();

    // Window
    GLFWwindow* window() { return window_; }
    int width() const { return width_; }
    int height() const { return height_; }

    // Status
    void set_status(const std::string& msg);
    const std::string& status() const { return status_; }

private:
    App() = default;
    ~App() = default;

    bool init_glfw();
    bool init_imgui();
    void process_events();
    void render();

    GLFWwindow* window_ = nullptr;
    std::unique_ptr<MainWindow> main_window_;

    AppSettings settings_;
    std::string status_;
    
    // Loading state
    bool is_loading_ = false;
    std::string loading_message_;
    
public:
    void set_loading(bool loading, const std::string& msg = "") { 
        is_loading_ = loading; 
        loading_message_ = msg;
    }
    bool is_loading() const { return is_loading_; }
    const std::string& loading_message() const { return loading_message_; }

private:
    int width_ = 1600;
    int height_ = 900;
    bool running_ = false;
};

} // namespace enfusion
