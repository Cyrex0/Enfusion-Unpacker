/**
 * Enfusion Unpacker - Entry Point
 */

#include "gui/app.hpp"

#ifdef _WIN32
#include <Windows.h>
#include <io.h>
#include <fcntl.h>
#include <cstdio>
#include <iostream>
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // Enable high DPI awareness
    SetProcessDPIAware();
    
    // Allocate console for debug output
    if (AllocConsole()) {
        FILE* fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        freopen_s(&fp, "CONIN$", "r", stdin);
        std::cout.clear();
        std::cerr.clear();
        std::cin.clear();
    }
#endif
    
    auto& app = enfusion::App::instance();
    
    if (!app.init()) {
        return 1;
    }
    
    app.run();
    app.shutdown();
    
    return 0;
}

#ifdef _WIN32
// Windows entry point (hide console)
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    return main(__argc, __argv);
}
#endif
