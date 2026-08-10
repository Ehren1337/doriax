//
// (c) 2026 Eduardo Doria.
//

#include "SystemWin.h"

#include "WinInputRouter.h"
#include "WindowWin.h"

using namespace doriax;

SystemWin::SystemWin(WinInputRouter* router) : router(router) {
}

int SystemWin::getScreenWidth() {
    return WindowWin::getClientWidth();
}

int SystemWin::getScreenHeight() {
    return WindowWin::getClientHeight();
}

int SystemWin::getSampleCount() {
    return 1;
}

bool SystemWin::isFullscreen() {
    return WindowWin::isFullscreen();
}

void SystemWin::requestFullscreen() {
    WindowWin::requestFullscreen();
}

void SystemWin::exitFullscreen() {
    WindowWin::exitFullscreen();
}

bool SystemWin::isWindowMaximized() {
    return WindowWin::isMaximized();
}

void SystemWin::maximizeWindow() {
    WindowWin::maximize();
}

void SystemWin::restoreWindow() {
    WindowWin::restore();
}

void SystemWin::setWindowSize(int width, int height) {
    WindowWin::setSize(width, height);
}

bool SystemWin::isWindowResizable() {
    return WindowWin::isResizable();
}

void SystemWin::setWindowResizable(bool resizable) {
    WindowWin::setResizable(resizable);
}

void SystemWin::setWindowTitle(const std::string& title) {
    WindowWin::setTitle(title);
}

void SystemWin::quit() {
    WindowWin::quit();
}

void SystemWin::setMouseCursor(CursorType type) {
    WindowWin::setMouseCursor(type);
}

void SystemWin::setMouseMode(MouseMode mode) {
    WindowWin::setMouseMode(mode);
}

void SystemWin::setMousePosition(float x, float y) {
    // The window warps the pointer; the router owns the position the engine
    // sees, which reports no motion of its own while captured.
    if (router) router->setMousePosition(x, y);
    WindowWin::setMousePosition(x, y);
}

// An exported game ships its assets next to the executable, so the relative
// default is right there. Running a project from outside the editor leaves the
// assets in the project directory instead, so the editor bakes absolute paths
// in through these macros (see Generator::getPlatformCMakeConfig).
std::string SystemWin::getAssetPath() {
#ifdef DORIAX_ASSET_PATH
    return DORIAX_ASSET_PATH;
#else
    return "assets";
#endif
}

std::string SystemWin::getUserDataPath() {
    return ".";
}

std::string SystemWin::getLuaPath() {
#ifdef DORIAX_LUA_PATH
    return DORIAX_LUA_PATH;
#else
    return "lua";
#endif
}
