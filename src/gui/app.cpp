/**
 * Enfusion Unpacker - Main Application Implementation
 */

#include "gui/app.hpp"
#include "gui/main_window.hpp"
#include "gui/theme.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <fstream>
#include <nlohmann/json.hpp>

namespace enfusion {

App& App::instance() {
    static App instance;
    return instance;
}

bool App::init() {
    if (!init_glfw()) {
        return false;
    }
    
    if (!init_imgui()) {
        return false;
    }
    
    load_settings();
    apply_theme(static_cast<Theme>(settings_.theme));
    
    main_window_ = std::make_unique<MainWindow>();
    
    running_ = true;
    set_status("Ready");
    
    return true;
}

bool App::init_glfw() {
    if (!glfwInit()) {
        return false;
    }
    
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);  // Remove Windows title bar
    
    window_ = glfwCreateWindow(width_, height_, "Enfusion Unpacker", nullptr, nullptr);
    if (!window_) {
        glfwTerminate();
        return false;
    }
    
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);  // VSync
    
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        return false;
    }
    
    return true;
}

bool App::init_imgui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.IniFilename = "enfusion_unpacker.ini";
    
    // Setup fonts
    setup_fonts(settings_.ui_scale);
    
    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");
    
    return true;
}

void App::run() {
    while (running_ && !glfwWindowShouldClose(window_)) {
        process_events();
        render();
    }
}

void App::process_events() {
    glfwPollEvents();
    glfwGetWindowSize(window_, &width_, &height_);
}

void App::render() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    
    // Render main window
    main_window_->render();
    
    ImGui::Render();
    
    glViewport(0, 0, width_, height_);
    glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    
    // Handle multi-viewport
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        GLFWwindow* backup_context = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backup_context);
    }
    
    glfwSwapBuffers(window_);
}

void App::shutdown() {
    save_settings();
    
    main_window_.reset();
    
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    
    glfwDestroyWindow(window_);
    glfwTerminate();
}

void App::set_status(const std::string& msg) {
    status_ = msg;
}

void App::load_settings() {
    try {
        std::ifstream file("settings.json");
        if (file) {
            nlohmann::json j;
            file >> j;
            
            if (j.contains("last_addon_path")) settings_.last_addon_path = j["last_addon_path"].get<std::string>();
            if (j.contains("last_export_path")) settings_.last_export_path = j["last_export_path"].get<std::string>();
            if (j.contains("arma_addons_path")) settings_.arma_addons_path = j["arma_addons_path"].get<std::string>();
            if (j.contains("theme")) settings_.theme = j["theme"].get<int>();
            if (j.contains("ui_scale")) settings_.ui_scale = j["ui_scale"].get<float>();
            if (j.contains("convert_textures_to_png")) settings_.convert_textures_to_png = j["convert_textures_to_png"].get<bool>();
            if (j.contains("convert_meshes_to_obj")) settings_.convert_meshes_to_obj = j["convert_meshes_to_obj"].get<bool>();
        }
    } catch (...) {
        // Use defaults
    }
}

void App::save_settings() {
    try {
        nlohmann::json j;
        j["last_addon_path"] = settings_.last_addon_path.string();
        j["last_export_path"] = settings_.last_export_path.string();
        j["arma_addons_path"] = settings_.arma_addons_path.string();
        j["theme"] = settings_.theme;
        j["ui_scale"] = settings_.ui_scale;
        j["convert_textures_to_png"] = settings_.convert_textures_to_png;
        j["convert_meshes_to_obj"] = settings_.convert_meshes_to_obj;
        
        std::ofstream file("settings.json");
        file << j.dump(2);
    } catch (...) {
        // Ignore
    }
}

} // namespace enfusion
