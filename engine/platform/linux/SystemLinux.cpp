//
// (c) 2026 Eduardo Doria.
//

#include "SystemLinux.h"

#include "LinuxInputRouter.h"
#include "WindowLinux.h"

using namespace doriax;

SystemLinux::SystemLinux(LinuxInputRouter* router) : router(router) {
}

int SystemLinux::getScreenWidth() {
    return WindowLinux::getWidth();
}

int SystemLinux::getScreenHeight() {
    return WindowLinux::getHeight();
}

int SystemLinux::getSampleCount() {
    return 1;
}

bool SystemLinux::isFullscreen() {
    return WindowLinux::isFullscreen();
}

void SystemLinux::requestFullscreen() {
    WindowLinux::requestFullscreen();
}

void SystemLinux::exitFullscreen() {
    WindowLinux::exitFullscreen();
}

bool SystemLinux::isWindowMaximized() {
    return WindowLinux::isMaximized();
}

void SystemLinux::maximizeWindow() {
    WindowLinux::maximize();
}

void SystemLinux::restoreWindow() {
    WindowLinux::restore();
}

void SystemLinux::setWindowSize(int width, int height) {
    WindowLinux::setSize(width, height);
}

bool SystemLinux::isWindowResizable() {
    return WindowLinux::isResizable();
}

void SystemLinux::setWindowResizable(bool resizable) {
    WindowLinux::setResizable(resizable);
}

void SystemLinux::setWindowTitle(const std::string& title) {
    WindowLinux::setTitle(title);
}

void SystemLinux::quit() {
    WindowLinux::quit();
}

void SystemLinux::setMouseCursor(CursorType type) {
    WindowLinux::setMouseCursor(type);
}

void SystemLinux::setMouseMode(MouseMode mode) {
    WindowLinux::setMouseMode(mode);
}

void SystemLinux::setMousePosition(float x, float y) {
    // The window warps the pointer; the router owns the position the engine
    // sees, which reports no motion of its own while captured.
    if (router) router->setMousePosition(x, y);
    WindowLinux::setMousePosition(x, y);
}

// An exported game ships its assets next to the executable, so the relative
// default is right there. Running a project from outside the editor leaves the
// assets in the project directory instead, so the editor bakes absolute paths
// in through these macros (see Generator::getPlatformCMakeConfig).
std::string SystemLinux::getAssetPath() {
#ifdef DORIAX_ASSET_PATH
    return DORIAX_ASSET_PATH;
#else
    return "assets";
#endif
}

std::string SystemLinux::getUserDataPath() {
    return ".";
}

std::string SystemLinux::getLuaPath() {
#ifdef DORIAX_LUA_PATH
    return DORIAX_LUA_PATH;
#else
    return "lua";
#endif
}
