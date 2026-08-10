//
// (c) 2026 Eduardo Doria.
//

#include "SystemMac.h"
#include "WindowMac.h"

using namespace doriax;

int SystemMac::getScreenWidth() {
    return WindowMac::getDrawableWidth();
}

int SystemMac::getScreenHeight() {
    return WindowMac::getDrawableHeight();
}

int SystemMac::getSampleCount() {
    return 1;
}

bool SystemMac::isFullscreen() {
    return WindowMac::isFullscreen();
}

void SystemMac::requestFullscreen() {
    WindowMac::requestFullscreen();
}

void SystemMac::exitFullscreen() {
    WindowMac::exitFullscreen();
}

bool SystemMac::isWindowMaximized() {
    return WindowMac::isMaximized();
}

void SystemMac::maximizeWindow() {
    WindowMac::maximize();
}

void SystemMac::restoreWindow() {
    WindowMac::restore();
}

void SystemMac::setWindowSize(int width, int height) {
    WindowMac::setSize(width, height);
}

bool SystemMac::isWindowResizable() {
    return WindowMac::isResizable();
}

void SystemMac::setWindowResizable(bool resizable) {
    WindowMac::setResizable(resizable);
}

void SystemMac::setWindowTitle(const std::string& title) {
    WindowMac::setTitle(title);
}

void SystemMac::quit() {
    WindowMac::quit();
}

void SystemMac::setMouseCursor(CursorType type) {
    WindowMac::setMouseCursor(type);
}

void SystemMac::setMouseMode(MouseMode mode) {
    WindowMac::setMouseMode(mode);
}

void SystemMac::setMousePosition(float x, float y) {
    WindowMac::setMousePosition(x, y);
}

// An exported game ships its assets next to the executable, so the relative
// default is right there. Running a project from outside the editor leaves the
// assets in the project directory instead, so the editor bakes absolute paths
// in through these macros (see Generator::getPlatformCMakeConfig).
std::string SystemMac::getAssetPath() {
#ifdef DORIAX_ASSET_PATH
    return DORIAX_ASSET_PATH;
#else
    return "assets";
#endif
}

std::string SystemMac::getUserDataPath() {
    return ".";
}

std::string SystemMac::getLuaPath() {
#ifdef DORIAX_LUA_PATH
    return DORIAX_LUA_PATH;
#else
    return "lua";
#endif
}
