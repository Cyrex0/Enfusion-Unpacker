cmake_minimum_required(VERSION 3.16)
project(EnfusionUnpacker VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Platform-specific settings
if(WIN32)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    add_definitions(-DNOMINMAX)
    add_definitions(-DWIN32_LEAN_AND_MEAN)
endif()

# Release optimizations
if(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /GL")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG")
    # Hide console window in Release
    set(CMAKE_WIN32_EXECUTABLE ON)
endif()

# Dependencies
find_package(ZLIB REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(imguizmo CONFIG QUIET)
find_package(Stb REQUIRED)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${Stb_INCLUDE_DIR}
)

# Source files - Core
set(CORE_SOURCES
    src/core/pak_reader.cpp
    src/core/rdb_parser.cpp
    src/core/manifest.cpp
    src/core/pak_manager.cpp
)

# Source files - Formats
set(FORMAT_SOURCES
    src/formats/xob_parser.cpp
    src/formats/edds_converter.cpp
    src/formats/dds_loader.cpp
)

# Source files - Converters
set(CONVERTER_SOURCES
    src/converters/mesh_converter.cpp
    src/converters/addon_extractor.cpp
)

# Source files - Utils
set(UTIL_SOURCES
    src/utils/compression.cpp
    src/utils/files.cpp
)

# Source files - GUI
set(GUI_SOURCES
    src/gui/app.cpp
    src/gui/main_window.cpp
    src/gui/addon_browser.cpp
    src/gui/file_browser.cpp
    src/gui/texture_viewer.cpp
    src/gui/model_viewer.cpp
    src/gui/text_viewer.cpp
    src/gui/export_dialog.cpp
    src/gui/settings_dialog.cpp
    src/gui/theme.cpp
    src/gui/widgets.cpp
)

# Source files - Renderer
set(RENDERER_SOURCES
    src/renderer/mesh_renderer.cpp
    src/renderer/texture_renderer.cpp
    src/renderer/camera.cpp
    src/renderer/shader.cpp
)

# All sources
set(SOURCES
    src/main.cpp
    ${CORE_SOURCES}
    ${FORMAT_SOURCES}
    ${CONVERTER_SOURCES}
    ${UTIL_SOURCES}
    ${GUI_SOURCES}
    ${RENDERER_SOURCES}
)

# Application icon (Windows)
if(WIN32)
    set(APP_ICON_RESOURCE "${CMAKE_SOURCE_DIR}/resources/app.rc")
    list(APPEND SOURCES ${APP_ICON_RESOURCE})
endif()

# Create executable
add_executable(EnfusionUnpacker WIN32 ${SOURCES})

# Link libraries
target_link_libraries(EnfusionUnpacker PRIVATE
    ZLIB::ZLIB
    lz4::lz4
    nlohmann_json::nlohmann_json
    glfw
    glad::glad
    imgui::imgui
    opengl32
)

# Copy resources if they exist
if(EXISTS ${CMAKE_SOURCE_DIR}/resources/fonts)
    file(COPY ${CMAKE_SOURCE_DIR}/resources/fonts DESTINATION ${CMAKE_BINARY_DIR})
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/resources/shaders)
    file(COPY ${CMAKE_SOURCE_DIR}/resources/shaders DESTINATION ${CMAKE_BINARY_DIR})
endif()

# Install
install(TARGETS EnfusionUnpacker RUNTIME DESTINATION bin)

message(STATUS "EnfusionUnpacker v${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
