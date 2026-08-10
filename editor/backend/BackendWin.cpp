#include "Backend.h"
#include "EditorHost.h"
#include "backend/EditorFrame.h"
#include "backend/renderer/Renderer.h"

#include "Engine.h"

#include "imgui_impl_win32.h"

#include "nfd.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <xinput.h>

#if defined(SOKOL_VULKAN)
#include <vulkan/vulkan.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam);

using namespace doriax;
using doriax::editor::PlatformMenuCallback;
using doriax::editor::PlatformMenuCommand;
using doriax::editor::PlatformMenuItem;
using doriax::editor::PlatformMenuItemType;
using doriax::editor::PlatformMenuModel;

namespace {

constexpr wchar_t WINDOW_CLASS_NAME[] = L"DoriaxEditorNativeWindow";
constexpr UINT WM_DORIAX_WAKE = WM_APP + 1;
constexpr UINT FIRST_MENU_COMMAND = 0x1000;
constexpr WORD ARROW_CURSOR_ID = 32512; // IDC_ARROW
constexpr UINT_PTR LIVE_RESIZE_TIMER_ID = 0xD04A;
constexpr UINT LIVE_RESIZE_INTERVAL_MS = 16;
constexpr int GAMEPAD_COUNT = XUSER_MAX_COUNT;
constexpr int GAMEPAD_BUTTON_COUNT = 15;
constexpr int GAMEPAD_AXIS_COUNT = 6;
#if defined(SOKOL_VULKAN)
constexpr UINT WINDOW_CLASS_STYLE = CS_HREDRAW | CS_VREDRAW;
#else
// OpenGL keeps a device context per window, which needs a private DC
constexpr UINT WINDOW_CLASS_STYLE = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
#endif

struct Gamepad {
    bool connected = false;
    std::array<unsigned char, GAMEPAD_BUTTON_COUNT> buttons{};
    std::array<float, GAMEPAD_AXIS_COUNT> axes{};
};

struct NativeMenu {
    HMENU handle = nullptr;
    PlatformMenuModel model;
    std::unordered_map<UINT, PlatformMenuCommand> commands;
    std::vector<PlatformMenuCommand> pendingCommands;
};

using XInputGetStateProc = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);

struct WinBackendData {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    bool classRegistered = false;
    bool shouldClose = false;
    bool redrawRequested = false;
    bool inSizeMove = false;
    bool relativeMouse = false;
    bool cursorClipped = false;
    bool gameCursorInSceneRect = false;
    bool gameCursorHidden = false;
    bool mouseControlSuspended = false;
    bool hasSavedCursorPosition = false;
    MouseMode gameMouseMode = MouseMode::NORMAL;
    POINT savedCursorPosition{};
    double virtualMouseX = 0.0;
    double virtualMouseY = 0.0;
    LONG rawMouseX = 0;
    LONG rawMouseY = 0;
    double framePeriod = 1.0 / 60.0;
    // Win32 blocks the outer frame loop while a sizing border is being
    // dragged. This callback renders from that modal message loop instead.
    std::function<void()> liveResizeFrame;
    void (*createImGuiWindow)(ImGuiViewport*) = nullptr;
    NativeMenu menu;

    HMODULE xinputLibrary = nullptr;
    XInputGetStateProc xinputGetState = nullptr;
    std::array<Gamepad, GAMEPAD_COUNT> gamepads;
    std::unique_ptr<editor::Renderer> renderer;
    editor::EditorFrame editorFrame;
};

WinBackendData* backend = nullptr;
nfdwindowhandle_t nativeWindowHandle{};

void applyDarkWindowTheme(HWND window) {
    if (!window) return;

    const BOOL enabled = TRUE;
    // Windows 10 versions before 20H1 used attribute 19. Newer Windows 10
    // releases and Windows 11 use attribute 20.
    constexpr DWORD immersiveDarkMode = 20;
    constexpr DWORD immersiveDarkModeLegacy = 19;
    if (FAILED(DwmSetWindowAttribute(
            window, immersiveDarkMode, &enabled, sizeof(enabled)))) {
        DwmSetWindowAttribute(
            window, immersiveDarkModeLegacy, &enabled, sizeof(enabled));
    }

    // Windows 11 supports explicit non-client colors. Calls fail harmlessly
    // on older releases, where the immersive-dark attributes above apply.
    constexpr DWORD borderColorAttribute = 34;
    constexpr DWORD captionColorAttribute = 35;
    constexpr DWORD textColorAttribute = 36;
    const COLORREF borderColor = RGB(45, 45, 48);
    const COLORREF captionColor = RGB(31, 31, 31);
    const COLORREF textColor = RGB(240, 240, 240);
    DwmSetWindowAttribute(window, borderColorAttribute,
                          &borderColor, sizeof(borderColor));
    DwmSetWindowAttribute(window, captionColorAttribute,
                          &captionColor, sizeof(captionColor));
    DwmSetWindowAttribute(window, textColorAttribute,
                          &textColor, sizeof(textColor));
}

void createThemedImGuiWindow(ImGuiViewport* viewport) {
    if (backend && backend->createImGuiWindow)
        backend->createImGuiWindow(viewport);
    applyDarkWindowTheme(static_cast<HWND>(viewport->PlatformHandleRaw));
}

std::wstring utf8ToWide(const std::string& text) {
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

std::string wideToUtf8(const wchar_t* text, int length) {
    if (!text || length <= 0) return {};
    const int count = WideCharToMultiByte(
        CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, result.data(), count,
                        nullptr, nullptr);
    return result;
}

std::wstring menuLabel(const PlatformMenuItem& item) {
    std::string label;
    label.reserve(item.label.size() + item.shortcut.size() + 2);
    for (char ch : item.label) {
        label.push_back(ch);
        if (ch == '&') label.push_back('&');
    }
    if (!item.shortcut.empty()) {
        label.push_back('\t');
        label += item.shortcut;
    }
    return utf8ToWide(label);
}

bool appendMenuItems(
    HMENU menu, const std::vector<PlatformMenuItem>& items, UINT& nextCommand,
    std::unordered_map<UINT, PlatformMenuCommand>& commands) {
    for (const PlatformMenuItem& item : items) {
        if (item.type == PlatformMenuItemType::Separator) {
            if (!AppendMenuW(menu, MF_SEPARATOR, 0, nullptr)) return false;
            continue;
        }

        UINT flags = MF_STRING;
        if (!item.enabled) flags |= MF_GRAYED;
        if (item.checked) flags |= MF_CHECKED;
        const std::wstring label = menuLabel(item);
        if (item.type == PlatformMenuItemType::Submenu) {
            HMENU submenu = CreatePopupMenu();
            if (!submenu) return false;
            if (!appendMenuItems(submenu, item.children, nextCommand, commands) ||
                !AppendMenuW(menu, flags | MF_POPUP,
                             reinterpret_cast<UINT_PTR>(submenu), label.c_str())) {
                DestroyMenu(submenu);
                return false;
            }
            continue;
        }

        if (nextCommand >= 0xF000) return false;
        const UINT commandId = nextCommand++;
        if (!AppendMenuW(menu, flags, commandId, label.c_str())) return false;
        commands.emplace(commandId, item.command);
    }
    return true;
}

bool rebuildNativeMenu(const PlatformMenuModel& model) {
    HMENU newMenu = CreateMenu();
    if (!newMenu) return false;
    std::unordered_map<UINT, PlatformMenuCommand> commands;
    UINT nextCommand = FIRST_MENU_COMMAND;
    if (!appendMenuItems(newMenu, model.menus, nextCommand, commands)) {
        DestroyMenu(newMenu);
        return false;
    }

    RECT oldClient{};
    RECT oldWindow{};
    GetClientRect(backend->window, &oldClient);
    GetWindowRect(backend->window, &oldWindow);
    HMENU oldMenu = backend->menu.handle;
    if (!SetMenu(backend->window, newMenu)) {
        DestroyMenu(newMenu);
        return false;
    }
    backend->menu.handle = newMenu;
    backend->menu.model = model;
    backend->menu.commands = std::move(commands);
    DrawMenuBar(backend->window);

    // A menu can change the non-client height when it is attached or wraps.
    // Keep the editor's saved client dimensions stable.
    if (!IsZoomed(backend->window)) {
        RECT newClient{};
        GetClientRect(backend->window, &newClient);
        const LONG oldHeight = oldClient.bottom - oldClient.top;
        const LONG newHeight = newClient.bottom - newClient.top;
        if (oldHeight != newHeight) {
            SetWindowPos(backend->window, nullptr, 0, 0,
                         oldWindow.right - oldWindow.left,
                         oldWindow.bottom - oldWindow.top + oldHeight - newHeight,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    if (oldMenu) DestroyMenu(oldMenu);
    backend->redrawRequested = true;
    return true;
}

bool appHasFocus() {
    HWND focused = GetForegroundWindow();
    if (!focused) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(focused, &processId);
    return processId == GetCurrentProcessId();
}

void updateCursorClip() {
    if (!backend || !backend->window || !appHasFocus()) {
        ClipCursor(nullptr);
        if (backend) backend->cursorClipped = false;
        return;
    }
    if (!backend->relativeMouse && backend->gameMouseMode != MouseMode::CONFINED) {
        ClipCursor(nullptr);
        backend->cursorClipped = false;
        return;
    }
    RECT rect{};
    GetClientRect(backend->window, &rect);
    POINT topLeft{rect.left, rect.top};
    POINT bottomRight{rect.right, rect.bottom};
    ClientToScreen(backend->window, &topLeft);
    ClientToScreen(backend->window, &bottomRight);
    rect = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    ClipCursor(&rect);
    backend->cursorClipped = true;
}

void releaseRelativeMouse() {
    const bool restorePosition = backend->relativeMouse &&
        backend->hasSavedCursorPosition;
    backend->relativeMouse = false;
    backend->rawMouseX = backend->rawMouseY = 0;
    ClipCursor(nullptr);
    backend->cursorClipped = false;
    if (restorePosition)
        SetCursorPos(backend->savedCursorPosition.x,
                     backend->savedCursorPosition.y);
    backend->hasSavedCursorPosition = false;
}

void showEditorCursor() {
    if (!backend) return;
    releaseRelativeMouse();
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = false;
    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(ARROW_CURSOR_ID)));
    backend->gameCursorHidden = false;
}

void hideEditorCursor() {
    if (!backend) return;
    releaseRelativeMouse();
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = false;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    SetCursor(nullptr);
    backend->gameCursorHidden = true;
}

void confineEditorCursor() {
    if (!backend) return;
    releaseRelativeMouse();
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = false;
    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(ARROW_CURSOR_ID)));
    updateCursorClip();
    backend->gameCursorHidden = false;
}

void applyHoverVisibility(bool force = false) {
    if (!backend || backend->mouseControlSuspended ||
        backend->gameMouseMode != MouseMode::HIDDEN)
        return;
    const bool shouldHide = backend->gameCursorInSceneRect;
    if (!force && shouldHide == backend->gameCursorHidden) return;
    if (shouldHide) hideEditorCursor();
    else showEditorCursor();
}

void applyRelativeMouseData() {
    if (!backend->relativeMouse || !appHasFocus()) {
        backend->rawMouseX = backend->rawMouseY = 0;
        return;
    }
    backend->virtualMouseX += backend->rawMouseX;
    backend->virtualMouseY += backend->rawMouseY;
    backend->rawMouseX = backend->rawMouseY = 0;
    ImGui::GetIO().AddMousePosEvent(
        static_cast<float>(backend->virtualMouseX),
        static_cast<float>(backend->virtualMouseY));
}

float normalizeThumb(SHORT value) {
    return value >= 0 ? static_cast<float>(value) / 32767.0f
                      : static_cast<float>(value) / 32768.0f;
}

void disconnectGamepad(int id) {
    Gamepad& gamepad = backend->gamepads[id];
    if (!gamepad.connected) return;
    gamepad = {};
    Engine::systemGamepadDisconnect(id);
}

void pollGamepads() {
    if (!backend->xinputGetState) return;
    constexpr std::array<WORD, GAMEPAD_BUTTON_COUNT> buttonMasks = {
        XINPUT_GAMEPAD_A,
        XINPUT_GAMEPAD_B,
        XINPUT_GAMEPAD_X,
        XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_LEFT_SHOULDER,
        XINPUT_GAMEPAD_RIGHT_SHOULDER,
        XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_START,
        0,
        XINPUT_GAMEPAD_LEFT_THUMB,
        XINPUT_GAMEPAD_RIGHT_THUMB,
        XINPUT_GAMEPAD_DPAD_UP,
        XINPUT_GAMEPAD_DPAD_RIGHT,
        XINPUT_GAMEPAD_DPAD_DOWN,
        XINPUT_GAMEPAD_DPAD_LEFT
    };

    for (DWORD id = 0; id < GAMEPAD_COUNT; ++id) {
        XINPUT_STATE state{};
        const bool connected = backend->xinputGetState(id, &state) == ERROR_SUCCESS;
        Gamepad& gamepad = backend->gamepads[id];
        if (!connected) {
            disconnectGamepad(static_cast<int>(id));
            continue;
        }
        if (!gamepad.connected) {
            gamepad = {};
            gamepad.connected = true;
            gamepad.axes[4] = gamepad.axes[5] = -1.0f;
            Engine::systemGamepadConnect(static_cast<int>(id), "XInput Controller");
        }

        for (int button = 0; button < GAMEPAD_BUTTON_COUNT; ++button) {
            const unsigned char pressed = buttonMasks[button] != 0 &&
                (state.Gamepad.wButtons & buttonMasks[button]) != 0;
            if (pressed == gamepad.buttons[button]) continue;
            gamepad.buttons[button] = pressed;
            if (pressed) Engine::systemGamepadButtonDown(id, button);
            else Engine::systemGamepadButtonUp(id, button);
        }

        const std::array<float, GAMEPAD_AXIS_COUNT> axes = {
            normalizeThumb(state.Gamepad.sThumbLX),
            -normalizeThumb(state.Gamepad.sThumbLY),
            normalizeThumb(state.Gamepad.sThumbRX),
            -normalizeThumb(state.Gamepad.sThumbRY),
            static_cast<float>(state.Gamepad.bLeftTrigger) / 127.5f - 1.0f,
            static_cast<float>(state.Gamepad.bRightTrigger) / 127.5f - 1.0f
        };
        for (int axis = 0; axis < GAMEPAD_AXIS_COUNT; ++axis) {
            if (std::fabs(axes[axis] - gamepad.axes[axis]) <= 0.001f) continue;
            gamepad.axes[axis] = axes[axis];
            Engine::systemGamepadAxisMove(id, axis, axes[axis]);
        }
    }
}

void initializeXInput() {
    constexpr const wchar_t* libraries[] = {
        L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"
    };
    for (const wchar_t* library : libraries) {
        backend->xinputLibrary = LoadLibraryW(library);
        if (!backend->xinputLibrary) continue;
        backend->xinputGetState = reinterpret_cast<XInputGetStateProc>(
            GetProcAddress(backend->xinputLibrary, "XInputGetState"));
        if (backend->xinputGetState) return;
        FreeLibrary(backend->xinputLibrary);
        backend->xinputLibrary = nullptr;
    }
}

void handleRawInput(HRAWINPUT handle) {
    if (!backend->relativeMouse) return;
    UINT size = 0;
    if (GetRawInputData(handle, RID_INPUT, nullptr, &size,
                        sizeof(RAWINPUTHEADER)) != 0 || size == 0)
        return;
    std::vector<unsigned char> storage(size);
    if (GetRawInputData(handle, RID_INPUT, storage.data(), &size,
                        sizeof(RAWINPUTHEADER)) != size)
        return;
    const RAWINPUT* input = reinterpret_cast<const RAWINPUT*>(storage.data());
    if (input->header.dwType != RIM_TYPEMOUSE) return;
    backend->rawMouseX += input->data.mouse.lLastX;
    backend->rawMouseY += input->data.mouse.lLastY;
    backend->redrawRequested = true;
}

void handleDrop(HDROP drop) {
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::string> paths;
    paths.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        std::wstring path(static_cast<size_t>(length) + 1, L'\0');
        DragQueryFileW(drop, index, path.data(), length + 1);
        path.resize(length);
        paths.push_back(wideToUtf8(path.c_str(), static_cast<int>(path.size())));
    }
    DragFinish(drop);
    if (!paths.empty()) editor::Backend::getApp().handleExternalDrop(paths);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (backend) {
        switch (message) {
            case WM_INPUT:
                handleRawInput(reinterpret_cast<HRAWINPUT>(lParam));
                return DefWindowProcW(window, message, wParam, lParam);
            case WM_COMMAND:
                if (HIWORD(wParam) == 0 && lParam == 0) {
                    const auto command = backend->menu.commands.find(LOWORD(wParam));
                    if (command != backend->menu.commands.end()) {
                        backend->menu.pendingCommands.push_back(command->second);
                        backend->redrawRequested = true;
                        return 0;
                    }
                }
                break;
            case WM_DROPFILES:
                handleDrop(reinterpret_cast<HDROP>(wParam));
                backend->redrawRequested = true;
                return 0;
            case WM_CLOSE:
                editor::Backend::getApp().exit();
                backend->redrawRequested = true;
                return 0;
            case WM_DORIAX_WAKE:
                backend->redrawRequested = true;
                return 0;
            case WM_SETFOCUS:
                updateCursorClip();
                backend->redrawRequested = true;
                break;
            case WM_KILLFOCUS:
                ClipCursor(nullptr);
                backend->cursorClipped = false;
                backend->redrawRequested = true;
                break;
            case WM_MOVE:
            case WM_DISPLAYCHANGE:
                updateCursorClip();
                backend->redrawRequested = true;
                break;
            case WM_SIZE:
                updateCursorClip();
                backend->redrawRequested = true;
                break;
            case WM_ENTERSIZEMOVE:
                backend->inSizeMove = true;
                SetTimer(window, LIVE_RESIZE_TIMER_ID,
                         LIVE_RESIZE_INTERVAL_MS, nullptr);
                backend->redrawRequested = true;
                return 0;
            case WM_EXITSIZEMOVE:
                KillTimer(window, LIVE_RESIZE_TIMER_ID);
                backend->inSizeMove = false;
                backend->redrawRequested = true;
                return 0;
            case WM_TIMER:
                if (wParam == LIVE_RESIZE_TIMER_ID && backend->inSizeMove) {
                    if (backend->liveResizeFrame) backend->liveResizeFrame();
                    return 0;
                }
                break;
            case WM_SETTINGCHANGE:
            case WM_THEMECHANGED:
                applyDarkWindowTheme(window);
                RedrawWindow(window, nullptr, nullptr,
                             RDW_FRAME | RDW_INVALIDATE);
                backend->redrawRequested = true;
                break;
            case WM_MOUSEMOVE:
            case WM_NCMOUSEMOVE:
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
            case WM_CHAR:
                backend->redrawRequested = true;
                break;
            case WM_SETCURSOR:
                if (LOWORD(lParam) == HTCLIENT &&
                    (backend->relativeMouse || backend->gameCursorHidden)) {
                    SetCursor(nullptr);
                    return TRUE;
                }
                break;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                BeginPaint(window, &paint);
                EndPaint(window, &paint);
                backend->redrawRequested = true;
                return 0;
            }
            default:
                break;
        }
    }

    if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam))
        return TRUE;

    if (message == WM_DPICHANGED) {
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void updateFramePeriod() {
    HMONITOR monitor = MonitorFromWindow(backend->window, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) return;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
        mode.dmDisplayFrequency > 1)
        backend->framePeriod = 1.0 / static_cast<double>(mode.dmDisplayFrequency);
}

bool initializeWindow(int width, int height, bool maximized) {
    backend->instance = GetModuleHandleW(nullptr);
    ImGui_ImplWin32_EnableDpiAwareness();

    HICON icon = static_cast<HICON>(LoadImageW(
        backend->instance, L"GLFW_ICON", IMAGE_ICON, 0, 0,
        LR_DEFAULTSIZE | LR_SHARED));
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = WINDOW_CLASS_STYLE;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = backend->instance;
    windowClass.hIcon = icon;
    windowClass.hIconSm = icon;
    windowClass.hCursor = LoadCursorW(
        nullptr, MAKEINTRESOURCEW(ARROW_CURSOR_ID));
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = WINDOW_CLASS_NAME;
    if (!RegisterClassExW(&windowClass)) {
        std::fprintf(stderr, "Error: Could not register the Win32 window class (%lu).\n",
                     GetLastError());
        return false;
    }
    backend->classRegistered = true;

    constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    constexpr DWORD exStyle = WS_EX_APPWINDOW;
    RECT windowRect{0, 0, std::max(width, 1), std::max(height, 1)};
    AdjustWindowRectEx(&windowRect, style, FALSE, exStyle);
    const int windowWidth = windowRect.right - windowRect.left;
    const int windowHeight = windowRect.bottom - windowRect.top;
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int x = workArea.left + std::max<LONG>(
        0, (workArea.right - workArea.left - windowWidth) / 2);
    const int y = workArea.top + std::max<LONG>(
        0, (workArea.bottom - workArea.top - windowHeight) / 2);
    backend->window = CreateWindowExW(
        exStyle, WINDOW_CLASS_NAME, L"Doriax Engine", style,
        x, y, windowWidth, windowHeight, nullptr, nullptr,
        backend->instance, nullptr);
    if (!backend->window) {
        std::fprintf(stderr, "Error: Could not create the Win32 window (%lu).\n",
                     GetLastError());
        return false;
    }
    applyDarkWindowTheme(backend->window);
    DragAcceptFiles(backend->window, TRUE);

    RAWINPUTDEVICE rawInput{};
    rawInput.usUsagePage = 0x01;
    rawInput.usUsage = 0x02;
    rawInput.hwndTarget = backend->window;
    RegisterRawInputDevices(&rawInput, 1, sizeof(rawInput));

    nativeWindowHandle.type = NFD_WINDOW_HANDLE_TYPE_WINDOWS;
    nativeWindowHandle.handle = backend->window;
    initializeXInput();
    updateFramePeriod();
    ShowWindow(backend->window, maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    UpdateWindow(backend->window);
    return true;
}

void shutdownWindow() {
    if (!backend) return;
    ClipCursor(nullptr);
    for (int id = 0; id < GAMEPAD_COUNT; ++id) disconnectGamepad(id);
    if (backend->xinputLibrary) FreeLibrary(backend->xinputLibrary);
    backend->xinputLibrary = nullptr;
    backend->xinputGetState = nullptr;
    if (backend->window) {
        KillTimer(backend->window, LIVE_RESIZE_TIMER_ID);
        DragAcceptFiles(backend->window, FALSE);
        if (backend->menu.handle) {
            SetMenu(backend->window, nullptr);
            DestroyMenu(backend->menu.handle);
            backend->menu.handle = nullptr;
        }
        DestroyWindow(backend->window);
        backend->window = nullptr;
    }
    if (backend->classRegistered)
        UnregisterClassW(WINDOW_CLASS_NAME, backend->instance);
    delete backend;
    backend = nullptr;
    nativeWindowHandle = {};
}

// Window-system half of the renderer, see renderer/Renderer.h
#if defined(SOKOL_VULKAN)

int createSurface(ImGuiViewport* viewport, ImU64 instance, const void* allocator,
                  ImU64* surface) {
    VkWin32SurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = backend->instance;
    info.hwnd = static_cast<HWND>(viewport->PlatformHandleRaw);
    return static_cast<int>(vkCreateWin32SurfaceKHR(
        reinterpret_cast<VkInstance>(instance), &info,
        static_cast<const VkAllocationCallbacks*>(allocator),
        reinterpret_cast<VkSurfaceKHR*>(surface)));
}

bool initImGuiPlatform() {
    return ImGui_ImplWin32_Init(backend->window);
}

void installViewportHooks(ImGuiPlatformIO&) {}

editor::RendererPlatform rendererPlatform() {
    editor::RendererPlatform platform;
    platform.surfaceExtension = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
    platform.createSurface = createSurface;
    platform.requestRedraw = []() { backend->redrawRequested = true; };
    return platform;
}

#else // SOKOL_GLCORE

using CreateContextAttribsProc = HGLRC (WINAPI*)(HDC, HGLRC, const int*);
using SwapIntervalProc = BOOL (WINAPI*)(int);

constexpr int WGL_CONTEXT_MAJOR_VERSION = 0x2091;
constexpr int WGL_CONTEXT_MINOR_VERSION = 0x2092;
constexpr int WGL_CONTEXT_PROFILE_MASK = 0x9126;
constexpr int WGL_CONTEXT_CORE_PROFILE_BIT = 0x00000001;

HGLRC glContext = nullptr;
int pixelFormat = 0;
SwapIntervalProc swapIntervalEXT = nullptr;
void (*imguiDestroyWindow)(ImGuiViewport*) = nullptr;
std::unordered_map<HWND, HDC> windowContexts;

template <typename T>
T wglProc(const char* name) {
    PROC address = wglGetProcAddress(name);
    if (!address || address == reinterpret_cast<PROC>(1) ||
        address == reinterpret_cast<PROC>(2) ||
        address == reinterpret_cast<PROC>(3) ||
        address == reinterpret_cast<PROC>(-1))
        return nullptr;
    return reinterpret_cast<T>(address);
}

PIXELFORMATDESCRIPTOR pixelFormatDescriptor() {
    PIXELFORMATDESCRIPTOR descriptor{};
    descriptor.nSize = sizeof(descriptor);
    descriptor.nVersion = 1;
    descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    descriptor.iPixelType = PFD_TYPE_RGBA;
    descriptor.cColorBits = 32;
    descriptor.cDepthBits = 24;
    descriptor.cStencilBits = 8;
    return descriptor;
}

// Every window takes the main window's pixel format and keeps its own DC
HDC deviceContextOf(ImGuiViewport* viewport) {
    HWND window = viewport ? static_cast<HWND>(viewport->PlatformHandleRaw)
                           : backend->window;
    if (!window) return nullptr;
    auto existing = windowContexts.find(window);
    if (existing != windowContexts.end()) return existing->second;

    HDC deviceContext = GetDC(window);
    if (!deviceContext) return nullptr;
    const int existingFormat = GetPixelFormat(deviceContext);
    if (existingFormat == 0) {
        PIXELFORMATDESCRIPTOR descriptor = pixelFormatDescriptor();
        if (!SetPixelFormat(deviceContext, pixelFormat, &descriptor)) {
            ReleaseDC(window, deviceContext);
            return nullptr;
        }
    } else if (existingFormat != pixelFormat) {
        ReleaseDC(window, deviceContext);
        return nullptr;
    }
    windowContexts.emplace(window, deviceContext);
    return deviceContext;
}

void destroyViewportWindow(ImGuiViewport* viewport) {
    HWND window = static_cast<HWND>(viewport->PlatformHandleRaw);
    auto existing = windowContexts.find(window);
    if (existing != windowContexts.end()) {
        ReleaseDC(window, existing->second);
        windowContexts.erase(existing);
    }
    if (imguiDestroyWindow) imguiDestroyWindow(viewport);
}

// InitForOpenGL gives viewport windows the private DC their context needs
bool initImGuiPlatform() {
    return ImGui_ImplWin32_InitForOpenGL(backend->window);
}

void installViewportHooks(ImGuiPlatformIO& platformIo) {
    imguiDestroyWindow = platformIo.Platform_DestroyWindow;
    platformIo.Platform_DestroyWindow = destroyViewportWindow;
}

editor::RendererPlatform rendererPlatform() {
    editor::RendererPlatform platform;
    platform.createContext = []() {
        HDC deviceContext = GetDC(backend->window);
        if (!deviceContext) {
            std::fprintf(stderr, "Error: Could not acquire the OpenGL device context.\n");
            return false;
        }
        PIXELFORMATDESCRIPTOR descriptor = pixelFormatDescriptor();
        pixelFormat = ChoosePixelFormat(deviceContext, &descriptor);
        if (!pixelFormat || !SetPixelFormat(deviceContext, pixelFormat, &descriptor)) {
            std::fprintf(stderr, "Error: No OpenGL pixel format available.\n");
            ReleaseDC(backend->window, deviceContext);
            return false;
        }
        windowContexts.emplace(backend->window, deviceContext);

        // wglCreateContextAttribsARB is only reachable through a current context
        HGLRC legacy = wglCreateContext(deviceContext);
        if (!legacy) {
            std::fprintf(stderr, "Error: Could not create an OpenGL context.\n");
            return false;
        }
        if (!wglMakeCurrent(deviceContext, legacy)) {
            wglDeleteContext(legacy);
            std::fprintf(stderr, "Error: Could not activate the OpenGL context.\n");
            return false;
        }
        auto createContext = wglProc<CreateContextAttribsProc>(
            "wglCreateContextAttribsARB");
        if (createContext) {
            const int attributes[] = {
                WGL_CONTEXT_MAJOR_VERSION, 4,
                WGL_CONTEXT_MINOR_VERSION, 1,
                WGL_CONTEXT_PROFILE_MASK, WGL_CONTEXT_CORE_PROFILE_BIT,
                0
            };
            glContext = createContext(deviceContext, nullptr, attributes);
        }
        wglMakeCurrent(nullptr, nullptr);
        if (!glContext) {
            wglDeleteContext(legacy);
            std::fprintf(stderr, "Error: Could not create an OpenGL 4.1 core context.\n");
            return false;
        }
        wglDeleteContext(legacy);
        if (!wglMakeCurrent(deviceContext, glContext)) {
            std::fprintf(stderr, "Error: Could not activate the OpenGL 4.1 context.\n");
            return false;
        }
        swapIntervalEXT = wglProc<SwapIntervalProc>("wglSwapIntervalEXT");
        return true;
    };
    platform.destroyContext = []() {
        if (glContext) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(glContext);
            glContext = nullptr;
        }
        for (const auto& entry : windowContexts) ReleaseDC(entry.first, entry.second);
        windowContexts.clear();
        swapIntervalEXT = nullptr;
        pixelFormat = 0;
    };
    platform.makeCurrent = [](ImGuiViewport* viewport) {
        HDC deviceContext = deviceContextOf(viewport);
        if (!deviceContext || !wglMakeCurrent(deviceContext, glContext))
            std::fprintf(stderr, "Error: Could not activate an OpenGL viewport.\n");
    };
    platform.swapBuffers = [](ImGuiViewport* viewport) {
        HDC deviceContext = deviceContextOf(viewport);
        if (!deviceContext || !wglMakeCurrent(deviceContext, glContext)) {
            std::fprintf(stderr, "Error: Could not activate an OpenGL viewport.\n");
            return;
        }
        if (viewport && swapIntervalEXT) swapIntervalEXT(0);
        SwapBuffers(deviceContext);
    };
    platform.setSwapInterval = [](int interval) {
        if (swapIntervalEXT) swapIntervalEXT(interval);
    };
    return platform;
}

#endif

void getClientSize(int& width, int& height) {
    RECT rect{};
    GetClientRect(backend->window, &rect);
    width = std::max<LONG>(rect.right - rect.left, 0);
    height = std::max<LONG>(rect.bottom - rect.top, 0);
}

void processMessages(bool waitForMessage) {
    if (waitForMessage)
        MsgWaitForMultipleObjectsEx(
            0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            backend->shouldClose = true;
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

} // namespace

editor::App editor::Backend::app;
std::string editor::Backend::title;

int editor::Backend::init(int argc, char* argv[]) {
    setEditorHost(&app);
    app.initializeSettings();

    backend = new WinBackendData();
    const int initialWidth = app.getInitialWindowWidth();
    const int initialHeight = app.getInitialWindowHeight();
    if (!initializeWindow(initialWidth, initialHeight,
                          app.getInitialWindowMaximized())) {
        shutdownWindow();
        return -1;
    }

    if (NFD_Init() != NFD_OKAY) {
        std::fprintf(stderr, "Error: NFD_Init failed: %s\n", NFD_GetError());
        shutdownWindow();
        return -1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    if (!initImGuiPlatform()) {
        ImGui::DestroyContext();
        NFD_Quit();
        shutdownWindow();
        return -1;
    }
    ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
    backend->createImGuiWindow = platformIO.Platform_CreateWindow;
    platformIO.Platform_CreateWindow = createThemedImGuiWindow;
    installViewportHooks(platformIO);

    app.setup();
    int clientWidth = 0;
    int clientHeight = 0;
    getClientSize(clientWidth, clientHeight);
    backend->renderer = std::make_unique<editor::Renderer>();
    if (!backend->renderer->init(
            rendererPlatform(), clientWidth, clientHeight, true)) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        NFD_Quit();
        backend->renderer.reset();
        shutdownWindow();
        return -1;
    }

    backend->editorFrame.init(*backend->renderer, app, []() {
        ImGui_ImplWin32_NewFrame();
        applyRelativeMouseData();
    });

    app.engineInit(argc, argv);
    // Keep the initial hide soft so ImGui can restore the normal editor cursor.
    SetCursor(nullptr);
    app.engineViewLoaded();

    app.setWakeCallback([]() {
        if (backend && backend->window)
            PostMessageW(backend->window, WM_DORIAX_WAKE, 0, 0);
    });

    bool frameInProgress = false;

    auto renderFrame = [&](bool forceRedraw) {
        if (frameInProgress || backend->shouldClose) return;
        frameInProgress = true;
        pollGamepads();
        updateCursorClip();

        editor::EditorFrameState state{backend->redrawRequested};
        state.framePeriod = backend->framePeriod;
        state.forceRedraw = forceRedraw;
        state.minimized = IsIconic(backend->window) != FALSE;
        state.focused = appHasFocus();
        getClientSize(state.width, state.height);
        if (!backend->editorFrame.run(state))
            backend->shouldClose = true;
        frameInProgress = false;
    };

    backend->liveResizeFrame = [&]() { renderFrame(true); };
    while (!backend->shouldClose) {
        processMessages(backend->editorFrame.isIdle());
        if (backend->shouldClose) break;
        renderFrame(false);
    }
    backend->liveResizeFrame = {};
    KillTimer(backend->window, LIVE_RESIZE_TIMER_ID);
    backend->inSizeMove = false;

    app.shutdownBackgroundWork();
    int width = 0;
    int height = 0;
    getClientSize(width, height);
    app.saveWindowSettings(width, height, IsZoomed(backend->window) != FALSE);

    backend->renderer->shutdownImGui();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    app.engineViewDestroyed();
    NFD_Quit();
    backend->renderer.reset();
    shutdownWindow();
    app.engineShutdown();
    return 0;
}

editor::App& editor::Backend::getApp() {
    return app;
}

void editor::Backend::disableMouseCursor() {
    if (!backend) return;
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.MouseDrawCursor = false;
    if (!backend->relativeMouse) {
        backend->hasSavedCursorPosition = GetCursorPos(
            &backend->savedCursorPosition) != FALSE;
        if (io.MousePos.x > -FLT_MAX && io.MousePos.y > -FLT_MAX) {
            backend->virtualMouseX = io.MousePos.x;
            backend->virtualMouseY = io.MousePos.y;
        } else if (backend->hasSavedCursorPosition) {
            POINT position = backend->savedCursorPosition;
            if (!(io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable))
                ScreenToClient(backend->window, &position);
            backend->virtualMouseX = position.x;
            backend->virtualMouseY = position.y;
        }
    }
    backend->relativeMouse = true;
    backend->rawMouseX = backend->rawMouseY = 0;
    SetCursor(nullptr);
    updateCursorClip();
    backend->gameCursorHidden = true;
}

void editor::Backend::enableMouseCursor() {
    if (backend->mouseControlSuspended) showEditorCursor();
    else setMouseMode(backend->gameMouseMode);
}

void editor::Backend::setMouseControlSuspended(bool suspended) {
    if (!backend || backend->mouseControlSuspended == suspended) return;
    backend->mouseControlSuspended = suspended;
    if (suspended) showEditorCursor();
    else setMouseMode(backend->gameMouseMode);
}

void editor::Backend::setMouseMode(MouseMode mode) {
    if (!backend) return;
    backend->gameMouseMode = mode;
    if (backend->mouseControlSuspended) return;
    switch (mode) {
        case MouseMode::CAPTURED: disableMouseCursor(); break;
        case MouseMode::CONFINED: confineEditorCursor(); break;
        case MouseMode::HIDDEN: applyHoverVisibility(true); break;
        case MouseMode::NORMAL: showEditorCursor(); break;
    }
}

void editor::Backend::setGameCursorInSceneRect(bool inSceneRect) {
    if (!backend) return;
    backend->gameCursorInSceneRect = inSceneRect;
    applyHoverVisibility();
}

void editor::Backend::closeWindow() {
    if (!backend) return;
    backend->shouldClose = true;
    if (backend->window) PostMessageW(backend->window, WM_DORIAX_WAKE, 0, 0);
}

bool editor::Backend::isRunningOnWayland() {
    return false;
}

float editor::Backend::setMainMenu(const PlatformMenuModel& model,
                                   PlatformMenuCallback callback) {
    if (!backend || !backend->window) return 0.0f;
    if (!backend->menu.handle || backend->menu.model.menus != model.menus) {
        if (!rebuildNativeMenu(model))
            return backend->menu.handle ? -1.0f : 0.0f;
    }
    std::vector<PlatformMenuCommand> pendingCommands;
    pendingCommands.swap(backend->menu.pendingCommands);
    for (const PlatformMenuCommand& command : pendingCommands)
        if (callback) callback(command);
    // HMENU is non-client UI, so ImGui must suppress its fallback without
    // reserving an in-client side bar.
    return -1.0f;
}

ImTextureID editor::Backend::getImGuiTexture(TextureRender* texture) {
    if (!texture || !texture->isCreated()) return ImTextureID{};
    return backend->renderer->getTexture(texture);
}

#if defined(SOKOL_VULKAN)

sg_environment editor::Backend::getSokolEnvironment() {
    return backend->renderer->getSokolEnvironment();
}

sg_swapchain editor::Backend::getSokolSwapchain() {
    return backend->renderer->getSokolSwapchain();
}

#endif

void editor::Backend::updateWindowTitle(const std::string& projectName) {
    title = editor::EditorFrame::formatWindowTitle(projectName);
    if (backend && backend->window) {
        const std::wstring wideTitle = utf8ToWide(title);
        SetWindowTextW(backend->window, wideTitle.c_str());
    }
}

void* editor::Backend::getNFDWindowHandle() {
    return &nativeWindowHandle;
}
