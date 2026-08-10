//
// (c) 2026 Eduardo Doria.
//

#include "WindowWin.h"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace doriax;

namespace {

    HINSTANCE gInstance = nullptr;
    HWND gWindow = nullptr;
    const wchar_t* gClassName = nullptr;
    bool gClassRegistered = false;

    std::function<void(int, int)> gClientSizeCallback;
    int gClientWidth = 0;
    int gClientHeight = 0;

    HCURSOR gCursor = nullptr;
    bool gCursorHidden = false;
    bool gRelativeMouse = false;
    bool gCursorConfined = false;
    POINT gSavedCursorPosition{};
    bool gHasSavedCursorPosition = false;

    // Windowed placement kept across a fullscreen round trip
    WINDOWPLACEMENT gSavedPlacement{};
    DWORD gSavedStyle = 0;
    bool gFullscreen = false;

    DWORD windowStyle(bool resizable, bool maximized, bool clipChildren) {
        DWORD style = WS_OVERLAPPEDWINDOW;
        if (!resizable) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        if (maximized) style |= WS_MAXIMIZE;
        if (clipChildren) style |= WS_CLIPCHILDREN;
        return style;
    }

}

std::wstring doriax::winUtf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (count <= 0) {
        codePage = CP_ACP;
        flags = 0;
        count = MultiByteToWideChar(
            codePage, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
    }
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()),
                        result.data(), count);
    return result;
}

bool WindowWin::create(const WindowWinConfig& config) {
    gInstance = GetModuleHandleW(nullptr);
    gClassName = config.className;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = config.classStyle;
    windowClass.lpfnWndProc = config.windowProc;
    windowClass.hInstance = gInstance;
    windowClass.hIcon = config.icon;
    windowClass.hIconSm = config.icon;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = gClassName;
    if (!RegisterClassExW(&windowClass)) {
        std::fprintf(stderr, "Error: Could not register the window class (%lu).\n",
                     GetLastError());
        return false;
    }
    gClassRegistered = true;
    gCursor = windowClass.hCursor;

    const DWORD style = windowStyle(config.resizable, config.maximized,
                                    config.clipChildren);
    constexpr DWORD exStyle = WS_EX_APPWINDOW;

    RECT rect{0, 0, std::max(config.width, 1), std::max(config.height, 1)};
    AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    const int totalWidth = rect.right - rect.left;
    const int totalHeight = rect.bottom - rect.top;

    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int x = workArea.left + static_cast<int>(std::max<LONG>(
        0, (workArea.right - workArea.left - totalWidth) / 2));
    const int y = workArea.top + static_cast<int>(std::max<LONG>(
        0, (workArea.bottom - workArea.top - totalHeight) / 2));

    gWindow = CreateWindowExW(
        exStyle, gClassName, winUtf8ToWide(config.title).c_str(), style,
        x, y, totalWidth, totalHeight, nullptr, nullptr, gInstance, nullptr);
    if (!gWindow) {
        std::fprintf(stderr, "Error: Could not create the window (%lu).\n",
                     GetLastError());
        return false;
    }

    // Relative mouse motion for CAPTURED mode
    RAWINPUTDEVICE rawInput{};
    rawInput.usUsagePage = 0x01;
    rawInput.usUsage = 0x02;
    rawInput.hwndTarget = gWindow;
    RegisterRawInputDevices(&rawInput, 1, sizeof(rawInput));

    refreshClientSize();
    return true;
}

void WindowWin::destroy() {
    releaseCursorClip();
    if (gWindow) {
        DestroyWindow(gWindow);
        gWindow = nullptr;
    }
    if (gClassRegistered) {
        UnregisterClassW(gClassName, gInstance);
        gClassRegistered = false;
    }
    gClassName = nullptr;
    gClientSizeCallback = nullptr;
    gClientWidth = gClientHeight = 0;
    gCursorHidden = gRelativeMouse = gCursorConfined = false;
    gHasSavedCursorPosition = false;
    gFullscreen = false;
}

HWND WindowWin::handle() {
    return gWindow;
}

HINSTANCE WindowWin::instance() {
    return gInstance;
}

void WindowWin::show(bool maximized) {
    if (!gWindow) return;
    ShowWindow(gWindow, maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    UpdateWindow(gWindow);
}

bool WindowWin::hasFocus() {
    HWND focused = GetForegroundWindow();
    if (!focused) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(focused, &processId);
    return processId == GetCurrentProcessId();
}

void WindowWin::getClientSize(int& width, int& height) {
    RECT rect{};
    if (gWindow) GetClientRect(gWindow, &rect);
    width = static_cast<int>(std::max<LONG>(rect.right - rect.left, 0));
    height = static_cast<int>(std::max<LONG>(rect.bottom - rect.top, 0));
}

int WindowWin::getClientWidth() {
    return gClientWidth;
}

int WindowWin::getClientHeight() {
    return gClientHeight;
}

void WindowWin::refreshClientSize() {
    int width = 0;
    int height = 0;
    getClientSize(width, height);
    if (width == gClientWidth && height == gClientHeight) return;

    gClientWidth = width;
    gClientHeight = height;
    if (gClientSizeCallback) gClientSizeCallback(width, height);
}

void WindowWin::setClientSizeCallback(std::function<void(int, int)> callback) {
    gClientSizeCallback = std::move(callback);
}

bool WindowWin::isFullscreen() {
    return gFullscreen;
}

void WindowWin::requestFullscreen() {
    if (gFullscreen || !gWindow) return;

    gSavedPlacement.length = sizeof(gSavedPlacement);
    GetWindowPlacement(gWindow, &gSavedPlacement);
    gSavedStyle = static_cast<DWORD>(GetWindowLongPtrW(gWindow, GWL_STYLE));

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(MonitorFromWindow(gWindow, MONITOR_DEFAULTTONEAREST),
                         &monitorInfo))
        return;

    SetWindowLongPtrW(gWindow, GWL_STYLE,
                      static_cast<LONG_PTR>(gSavedStyle & ~WS_OVERLAPPEDWINDOW));
    SetWindowPos(gWindow, HWND_TOP,
                 monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top,
                 monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                 monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    gFullscreen = true;
}

void WindowWin::exitFullscreen() {
    if (!gFullscreen || !gWindow) return;

    SetWindowLongPtrW(gWindow, GWL_STYLE, static_cast<LONG_PTR>(gSavedStyle));
    SetWindowPlacement(gWindow, &gSavedPlacement);
    SetWindowPos(gWindow, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    gFullscreen = false;
}

bool WindowWin::isMaximized() {
    return gWindow && IsZoomed(gWindow) != FALSE;
}

void WindowWin::maximize() {
    if (gWindow && !gFullscreen) ShowWindow(gWindow, SW_SHOWMAXIMIZED);
}

void WindowWin::restore() {
    if (gWindow && !gFullscreen) ShowWindow(gWindow, SW_RESTORE);
}

void WindowWin::setSize(int width, int height) {
    if (width < 1 || height < 1 || !gWindow) return;

    if (gFullscreen) {
        // Keep the display mode; resize the window that exitFullscreen restores
        gSavedPlacement.rcNormalPosition.right =
            gSavedPlacement.rcNormalPosition.left + width;
        gSavedPlacement.rcNormalPosition.bottom =
            gSavedPlacement.rcNormalPosition.top + height;
        return;
    }

    RECT rect{0, 0, width, height};
    AdjustWindowRect(&rect,
                     static_cast<DWORD>(GetWindowLongPtrW(gWindow, GWL_STYLE)),
                     GetMenu(gWindow) != nullptr);
    SetWindowPos(gWindow, nullptr, 0, 0,
                 rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool WindowWin::isResizable() {
    return gWindow && (GetWindowLongPtrW(gWindow, GWL_STYLE) & WS_THICKFRAME) != 0;
}

void WindowWin::setResizable(bool resizable) {
    if (!gWindow) return;
    LONG_PTR style = GetWindowLongPtrW(gWindow, GWL_STYLE);
    if (resizable) style |= (WS_THICKFRAME | WS_MAXIMIZEBOX);
    else style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    SetWindowLongPtrW(gWindow, GWL_STYLE, style);
    SetWindowPos(gWindow, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

void WindowWin::setTitle(const std::string& title) {
    if (gWindow) SetWindowTextW(gWindow, winUtf8ToWide(title).c_str());
}

void WindowWin::quit() {
    if (gWindow) PostMessageW(gWindow, WM_CLOSE, 0, 0);
}

void WindowWin::setMouseCursor(CursorType type) {
    const wchar_t* name = IDC_ARROW;
    switch (type) {
        case CursorType::ARROW:         name = IDC_ARROW; break;
        case CursorType::IBEAM:         name = IDC_IBEAM; break;
        case CursorType::CROSSHAIR:     name = IDC_CROSS; break;
        case CursorType::POINTING_HAND: name = IDC_HAND; break;
        case CursorType::RESIZE_EW:     name = IDC_SIZEWE; break;
        case CursorType::RESIZE_NS:     name = IDC_SIZENS; break;
        case CursorType::RESIZE_NWSE:   name = IDC_SIZENWSE; break;
        case CursorType::RESIZE_NESW:   name = IDC_SIZENESW; break;
        case CursorType::RESIZE_ALL:    name = IDC_SIZEALL; break;
        case CursorType::NOT_ALLOWED:   name = IDC_NO; break;
    }

    HCURSOR cursor = LoadCursorW(nullptr, name);
    if (!cursor) return;
    gCursor = cursor;
    if (gWindow)
        SetClassLongPtrW(gWindow, GCLP_HCURSOR,
                         reinterpret_cast<LONG_PTR>(cursor));
    // A hidden cursor would be made visible again by setting a shape
    if (!gCursorHidden) SetCursor(cursor);
}

void WindowWin::setMouseMode(MouseMode mode) {
    switch (mode) {
        case MouseMode::NORMAL:
            setRelativeMouse(false, true);
            setCursorConfined(false);
            setCursorHidden(false);
            break;
        case MouseMode::HIDDEN:
            setRelativeMouse(false, true);
            setCursorConfined(false);
            setCursorHidden(true);
            break;
        case MouseMode::CAPTURED:
            setCursorConfined(false);
            setCursorHidden(true);
            setRelativeMouse(true, false);
            break;
        case MouseMode::CONFINED:
            setRelativeMouse(false, true);
            setCursorHidden(false);
            setCursorConfined(true);
            break;
    }
}

void WindowWin::setMousePosition(float x, float y) {
    if (!gWindow) return;
    POINT point{static_cast<LONG>(x), static_cast<LONG>(y)};
    ClientToScreen(gWindow, &point);
    SetCursorPos(point.x, point.y);
}

void WindowWin::setCursorHidden(bool hidden) {
    gCursorHidden = hidden;
    // Win32 has no hide counter to unbalance: the cursor is whatever the last
    // SetCursor said, and WM_SETCURSOR reapplies it as the pointer moves.
    SetCursor(hidden ? nullptr : (gCursor ? gCursor : LoadCursorW(nullptr, IDC_ARROW)));
}

bool WindowWin::isCursorHidden() {
    return gCursorHidden;
}

void WindowWin::setRelativeMouse(bool enabled, bool restorePosition) {
    if (enabled == gRelativeMouse) {
        if (enabled) updateCursorClip();
        return;
    }

    if (enabled) {
        gHasSavedCursorPosition = GetCursorPos(&gSavedCursorPosition) != FALSE;
        gRelativeMouse = true;
        updateCursorClip();
        return;
    }

    const bool restore = restorePosition && gHasSavedCursorPosition;
    gRelativeMouse = false;
    updateCursorClip();
    if (restore) SetCursorPos(gSavedCursorPosition.x, gSavedCursorPosition.y);
    gHasSavedCursorPosition = false;
}

bool WindowWin::isRelativeMouse() {
    return gRelativeMouse;
}

void WindowWin::setCursorConfined(bool confined) {
    if (confined == gCursorConfined) return;
    gCursorConfined = confined;
    updateCursorClip();
}

void WindowWin::updateCursorClip() {
    if (!gWindow || !hasFocus() || !(gRelativeMouse || gCursorConfined)) {
        releaseCursorClip();
        return;
    }

    RECT rect{};
    GetClientRect(gWindow, &rect);
    POINT topLeft{rect.left, rect.top};
    POINT bottomRight{rect.right, rect.bottom};
    ClientToScreen(gWindow, &topLeft);
    ClientToScreen(gWindow, &bottomRight);
    rect = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    ClipCursor(&rect);
}

void WindowWin::releaseCursorClip() {
    ClipCursor(nullptr);
}

void WindowWin::handleFocusLost() {
    releaseCursorClip();
    gHasSavedCursorPosition = false;
}

bool WindowWin::readRawMouseDelta(HRAWINPUT handle, LONG& deltaX, LONG& deltaY) {
    UINT size = 0;
    if (GetRawInputData(handle, RID_INPUT, nullptr, &size,
                        sizeof(RAWINPUTHEADER)) != 0 || size == 0)
        return false;
    std::vector<unsigned char> storage(size);
    if (GetRawInputData(handle, RID_INPUT, storage.data(), &size,
                        sizeof(RAWINPUTHEADER)) != size)
        return false;
    const RAWINPUT* input = reinterpret_cast<const RAWINPUT*>(storage.data());
    if (input->header.dwType != RIM_TYPEMOUSE) return false;
    if (input->data.mouse.lLastX == 0 && input->data.mouse.lLastY == 0) return false;

    deltaX = input->data.mouse.lLastX;
    deltaY = input->data.mouse.lLastY;
    return true;
}
