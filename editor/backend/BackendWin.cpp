#include "Backend.h"
#include "EditorHost.h"

#include "Engine.h"
#include "SystemRender.h"

#include "imgui_impl_vulkan.h"
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

#include <algorithm>
#include <array>
#include <chrono>
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
constexpr uint32_t IMGUI_RENDER_BUFFER_COUNT = 15;

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

    VkInstance instanceVk = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = UINT32_MAX;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    ImGui_ImplVulkanH_Window swapchain;
    std::vector<VkSemaphore> sokolFinishedSemaphores;
    std::unordered_map<uint32_t, VkDescriptorSet> imguiTextures;
    bool swapchainRebuild = false;
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

double monotonicSeconds() {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
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
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
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

int createViewportSurface(ImGuiViewport* viewport, ImU64 instance,
                          const void* allocator, ImU64* surface) {
    VkWin32SurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = backend->instance;
    info.hwnd = static_cast<HWND>(viewport->PlatformHandleRaw);
    return static_cast<int>(vkCreateWin32SurfaceKHR(
        reinterpret_cast<VkInstance>(instance), &info,
        static_cast<const VkAllocationCallbacks*>(allocator),
        reinterpret_cast<VkSurfaceKHR*>(surface)));
}

bool hasDeviceExtension(VkPhysicalDevice device, const char* name) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS)
        return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(
            device, nullptr, &count, extensions.data()) != VK_SUCCESS)
        return false;
    for (const VkExtensionProperties& extension : extensions)
        if (std::strcmp(extension.extensionName, name) == 0) return true;
    return false;
}

bool supportsVulkanDevice(VkPhysicalDevice device, uint32_t& queueFamily) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_3 ||
        !hasDeviceExtension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME) ||
        !hasDeviceExtension(device, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME))
        return false;

    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptorBuffer{};
    descriptorBuffer.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT extendedDynamicState{};
    extendedDynamicState.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;
    extendedDynamicState.pNext = &descriptorBuffer;
    VkPhysicalDeviceVulkan12Features vulkan12{};
    vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12.pNext = &extendedDynamicState;
    VkPhysicalDeviceVulkan13Features vulkan13{};
    vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan13.pNext = &vulkan12;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &vulkan13;
    vkGetPhysicalDeviceFeatures2(device, &features);
    if (!features.features.samplerAnisotropy || !features.features.dualSrcBlend ||
        !vulkan12.bufferDeviceAddress || !vulkan13.dynamicRendering ||
        !vulkan13.synchronization2 || !extendedDynamicState.extendedDynamicState ||
        !descriptorBuffer.descriptorBuffer)
        return false;

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    const VkQueueFlags required =
        VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    for (uint32_t index = 0; index < count; ++index) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, index, backend->surface, &present);
        if ((families[index].queueFlags & required) == required && present) {
            queueFamily = index;
            return true;
        }
    }
    return false;
}

bool createVulkanDevice() {
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Doriax Engine Editor";
    application.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    application.pEngineName = "Doriax";
    application.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    application.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME
    };
    uint32_t extensionCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateInstanceExtensionProperties(
        nullptr, &extensionCount, availableExtensions.data());
    for (const VkExtensionProperties& extension : availableExtensions) {
        if (std::strcmp(extension.extensionName,
                        VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            break;
        }
    }

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &application;
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    VkResult result = vkCreateInstance(&instanceInfo, nullptr, &backend->instanceVk);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "Error: Could not create Vulkan instance (%d).\n", result);
        return false;
    }

    VkWin32SurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.hinstance = backend->instance;
    surfaceInfo.hwnd = backend->window;
    result = vkCreateWin32SurfaceKHR(
        backend->instanceVk, &surfaceInfo, nullptr, &backend->surface);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "Error: Could not create Vulkan Win32 surface (%d).\n",
                     result);
        return false;
    }

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(backend->instanceVk, &deviceCount, nullptr);
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(
        backend->instanceVk, &deviceCount, devices.data());
    for (VkPhysicalDevice device : devices) {
        uint32_t family = UINT32_MAX;
        if (!supportsVulkanDevice(device, family)) continue;
        backend->physicalDevice = device;
        backend->queueFamily = family;
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) break;
    }
    if (backend->physicalDevice == VK_NULL_HANDLE) {
        std::fprintf(stderr,
            "Error: Vulkan 1.3 with swapchain and descriptor-buffer support is required.\n");
        return false;
    }

    VkPhysicalDeviceFeatures2 supported{};
    supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    vkGetPhysicalDeviceFeatures2(backend->physicalDevice, &supported);

    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptorBuffer{};
    descriptorBuffer.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
    descriptorBuffer.descriptorBuffer = VK_TRUE;
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT extendedDynamicState{};
    extendedDynamicState.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;
    extendedDynamicState.pNext = &descriptorBuffer;
    extendedDynamicState.extendedDynamicState = VK_TRUE;
    VkPhysicalDeviceVulkan12Features vulkan12{};
    vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12.pNext = &extendedDynamicState;
    vulkan12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceVulkan13Features vulkan13{};
    vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan13.pNext = &vulkan12;
    vulkan13.dynamicRendering = VK_TRUE;
    vulkan13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceFeatures2 requiredFeatures{};
    requiredFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    requiredFeatures.pNext = &vulkan13;
    requiredFeatures.features.samplerAnisotropy = VK_TRUE;
    requiredFeatures.features.dualSrcBlend = VK_TRUE;
    requiredFeatures.features.textureCompressionBC =
        supported.features.textureCompressionBC;
    requiredFeatures.features.textureCompressionETC2 =
        supported.features.textureCompressionETC2;
    requiredFeatures.features.textureCompressionASTC_LDR =
        supported.features.textureCompressionASTC_LDR;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = backend->queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const char* deviceExtensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME
    };
    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &requiredFeatures;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 2;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    result = vkCreateDevice(
        backend->physicalDevice, &deviceInfo, nullptr, &backend->device);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "Error: Could not create Vulkan device (%d).\n", result);
        return false;
    }
    vkGetDeviceQueue(backend->device, backend->queueFamily, 0, &backend->queue);
    return true;
}

VkPresentModeKHR choosePresentMode(bool synchronized) {
    if (synchronized) {
        const VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        return ImGui_ImplVulkanH_SelectPresentMode(
            backend->physicalDevice, backend->surface, &mode, 1);
    }
    const VkPresentModeKHR modes[] = {
        VK_PRESENT_MODE_IMMEDIATE_KHR,
        VK_PRESENT_MODE_MAILBOX_KHR,
        VK_PRESENT_MODE_FIFO_KHR
    };
    return ImGui_ImplVulkanH_SelectPresentMode(
        backend->physicalDevice, backend->surface, modes, 3);
}

void checkVkResult(VkResult result) {
    if (result < 0) std::fprintf(stderr, "Vulkan error: %d\n", result);
}

void destroySokolSemaphores() {
    for (VkSemaphore semaphore : backend->sokolFinishedSemaphores)
        vkDestroySemaphore(backend->device, semaphore, nullptr);
    backend->sokolFinishedSemaphores.clear();
}

void rebuildImGuiPipeline() {
    VkFormat format = backend->swapchain.SurfaceFormat.format;
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &format;

    ImGui_ImplVulkan_PipelineInfo pipelineInfo{};
    pipelineInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    pipelineInfo.PipelineRenderingCreateInfo = renderingInfo;
    ImGui_ImplVulkan_CreateMainPipeline(&pipelineInfo);
}

bool createMainSwapchain(int width, int height, bool synchronized) {
    if (width <= 0 || height <= 0) return false;
    if (backend->device != VK_NULL_HANDLE) {
        const VkResult result = vkDeviceWaitIdle(backend->device);
        if (result != VK_SUCCESS) {
            checkVkResult(result);
            return false;
        }
    }
    destroySokolSemaphores();

    ImGui_ImplVulkanH_Window& window = backend->swapchain;
    const VkFormat previousFormat = window.SurfaceFormat.format;
    window.UseDynamicRendering = true;
    window.Surface = backend->surface;
    const VkFormat formats[] = {
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM
    };
    window.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
        backend->physicalDevice, window.Surface, formats, 2,
        VK_COLORSPACE_SRGB_NONLINEAR_KHR);
    window.PresentMode = choosePresentMode(synchronized);
    ImGui_ImplVulkanH_CreateOrResizeWindow(
        backend->instanceVk, backend->physicalDevice, backend->device,
        &window, backend->queueFamily, nullptr, width, height, 2, 0);
    if (ImGui::GetIO().BackendRendererUserData != nullptr &&
        previousFormat != window.SurfaceFormat.format)
        rebuildImGuiPipeline();

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    backend->sokolFinishedSemaphores.resize(window.SemaphoreCount);
    for (VkSemaphore& semaphore : backend->sokolFinishedSemaphores) {
        if (vkCreateSemaphore(
                backend->device, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS) {
            std::fprintf(stderr,
                         "Error: Could not create Vulkan frame semaphore.\n");
            return false;
        }
    }
    backend->swapchainRebuild = false;
    backend->redrawRequested = true;
    return true;
}

bool failMainFrame(VkResult result) {
    checkVkResult(result);
    backend->swapchainRebuild = true;
    return false;
}

void removeInvalidImGuiTextures() {
    if (!backend || backend->device == VK_NULL_HANDLE) return;
    bool deviceIdle = false;
    for (auto texture = backend->imguiTextures.begin();
         texture != backend->imguiTextures.end();) {
        if (TextureRender::isViewValid(texture->first)) {
            ++texture;
            continue;
        }
        if (!deviceIdle) {
            vkDeviceWaitIdle(backend->device);
            deviceIdle = true;
        }
        ImGui_ImplVulkan_RemoveTexture(texture->second);
        texture = backend->imguiTextures.erase(texture);
    }
}

bool initializeImGuiVulkan() {
    VkFormat format = backend->swapchain.SurfaceFormat.format;
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &format;

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = backend->instanceVk;
    info.PhysicalDevice = backend->physicalDevice;
    info.Device = backend->device;
    info.QueueFamily = backend->queueFamily;
    info.Queue = backend->queue;
    info.DescriptorPoolSize = 8192;
    info.MinImageCount = 2;
    // ImGui's swapchain helper accepts at most 15 images. Allocating the full
    // render-buffer ring keeps it valid across present-mode changes.
    info.ImageCount = IMGUI_RENDER_BUFFER_COUNT;
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = renderingInfo;
    info.PipelineInfoForViewports.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.CheckVkResultFn = checkVkResult;
    return ImGui_ImplVulkan_Init(&info);
}

bool acquireMainFrame() {
    ImGui_ImplVulkanH_Window& window = backend->swapchain;
    ImGui_ImplVulkanH_FrameSemaphores& semaphores =
        window.FrameSemaphores[window.SemaphoreIndex];
    VkResult result = vkAcquireNextImageKHR(
        backend->device, window.Swapchain, UINT64_MAX,
        semaphores.ImageAcquiredSemaphore, VK_NULL_HANDLE, &window.FrameIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        backend->swapchainRebuild = true;
        return false;
    }
    if (result == VK_SUBOPTIMAL_KHR) backend->swapchainRebuild = true;
    else if (result != VK_SUCCESS) return failMainFrame(result);

    ImGui_ImplVulkanH_Frame& frame = window.Frames[window.FrameIndex];
    result = vkWaitForFences(
        backend->device, 1, &frame.Fence, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) return failMainFrame(result);
    return true;
}

bool submitMainFrame(ImDrawData* drawData) {
    ImGui_ImplVulkanH_Window& window = backend->swapchain;
    ImGui_ImplVulkanH_Frame& frame = window.Frames[window.FrameIndex];
    const uint32_t semaphoreIndex = window.SemaphoreIndex;
    VkSemaphore waitSemaphore = backend->sokolFinishedSemaphores[semaphoreIndex];
    VkSemaphore signalSemaphore =
        window.FrameSemaphores[semaphoreIndex].RenderCompleteSemaphore;

    VkResult result = vkResetCommandPool(backend->device, frame.CommandPool, 0);
    if (result != VK_SUCCESS) return failMainFrame(result);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(frame.CommandBuffer, &beginInfo);
    if (result != VK_SUCCESS) return failMainFrame(result);

    VkImageMemoryBarrier beginBarrier{};
    beginBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    beginBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    beginBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    beginBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    beginBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    beginBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    beginBarrier.image = frame.Backbuffer;
    beginBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    beginBarrier.subresourceRange.levelCount = 1;
    beginBarrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(
        frame.CommandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
        0, nullptr, 0, nullptr, 1, &beginBarrier);

    VkRenderingAttachmentInfo attachment{};
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView = frame.BackbufferView;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea.extent.width = window.Width;
    renderingInfo.renderArea.extent.height = window.Height;
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &attachment;
    vkCmdBeginRendering(frame.CommandBuffer, &renderingInfo);
    ImGui_ImplVulkan_RenderDrawData(drawData, frame.CommandBuffer);
    vkCmdEndRendering(frame.CommandBuffer);

    VkImageMemoryBarrier endBarrier{};
    endBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    endBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    endBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    endBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    endBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    endBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    endBarrier.image = frame.Backbuffer;
    endBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    endBarrier.subresourceRange.levelCount = 1;
    endBarrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(
        frame.CommandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
        0, nullptr, 0, nullptr, 1, &endBarrier);

    result = vkEndCommandBuffer(frame.CommandBuffer);
    if (result != VK_SUCCESS) return failMainFrame(result);
    result = vkResetFences(backend->device, 1, &frame.Fence);
    if (result != VK_SUCCESS) return failMainFrame(result);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &waitSemaphore;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frame.CommandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &signalSemaphore;
    result = vkQueueSubmit(backend->queue, 1, &submitInfo, frame.Fence);
    if (result != VK_SUCCESS) return failMainFrame(result);
    return true;
}

void presentMainFrame() {
    ImGui_ImplVulkanH_Window& window = backend->swapchain;
    ImGui_ImplVulkanH_FrameSemaphores& semaphores =
        window.FrameSemaphores[window.SemaphoreIndex];
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &semaphores.RenderCompleteSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &window.Swapchain;
    presentInfo.pImageIndices = &window.FrameIndex;
    VkResult result = vkQueuePresentKHR(backend->queue, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        backend->swapchainRebuild = true;
    else if (result != VK_SUCCESS) {
        checkVkResult(result);
        backend->swapchainRebuild = true;
    }
    window.SemaphoreIndex = (window.SemaphoreIndex + 1) % window.SemaphoreCount;
}

void shutdownVulkan() {
    if (backend->device != VK_NULL_HANDLE) vkDeviceWaitIdle(backend->device);
    destroySokolSemaphores();
    if (backend->swapchain.Swapchain != VK_NULL_HANDLE)
        ImGui_ImplVulkanH_DestroyWindow(
            backend->instanceVk, backend->device, &backend->swapchain, nullptr);
    if (backend->surface != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(backend->instanceVk, backend->surface, nullptr);
    if (backend->device != VK_NULL_HANDLE)
        vkDestroyDevice(backend->device, nullptr);
    if (backend->instanceVk != VK_NULL_HANDLE)
        vkDestroyInstance(backend->instanceVk, nullptr);
    backend->surface = VK_NULL_HANDLE;
    backend->device = VK_NULL_HANDLE;
    backend->instanceVk = VK_NULL_HANDLE;
}

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

    CameraRender render;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    if (!ImGui_ImplWin32_Init(backend->window)) {
        ImGui::DestroyContext();
        NFD_Quit();
        shutdownWindow();
        return -1;
    }
    ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
    backend->createImGuiWindow = platformIO.Platform_CreateWindow;
    platformIO.Platform_CreateWindow = createThemedImGuiWindow;
    platformIO.Platform_CreateVkSurface = createViewportSurface;

    app.setup();
    int clientWidth = 0;
    int clientHeight = 0;
    getClientSize(clientWidth, clientHeight);
    if (!createVulkanDevice() ||
        !createMainSwapchain(clientWidth, clientHeight, true) ||
        !initializeImGuiVulkan()) {
        ImGui::GetPlatformIO().Platform_CreateVkSurface = nullptr;
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        NFD_Quit();
        shutdownVulkan();
        shutdownWindow();
        return -1;
    }

    app.engineInit(argc, argv);
    // Keep the initial hide soft so ImGui can restore the normal editor cursor.
    SetCursor(nullptr);
    app.engineViewLoaded();

    Project* activeProject = app.getProject();
    bool currentFrameSync = true;
    app.setWakeCallback([]() {
        if (backend && backend->window)
            PostMessageW(backend->window, WM_DORIAX_WAKE, 0, 0);
    });

    double lastActivityTime = monotonicSeconds();
    constexpr double IDLE_ENTER_DELAY = 0.5;
    bool frameInProgress = false;

    auto renderFrame = [&](bool forceRedraw) {
        if (frameInProgress || backend->shouldClose) return;
        frameInProgress = true;
        const double frameStart = monotonicSeconds();
        const bool idleFrame = !forceRedraw &&
            frameStart - lastActivityTime > IDLE_ENTER_DELAY;
        pollGamepads();

        const bool minimized = IsIconic(backend->window) != FALSE;
        const bool focused = appHasFocus();
        const bool playSessionActive = activeProject->isPlaySessionActive();
        const bool frameSync = !playSessionActive || activeProject->isVSyncEnabled();
        setMouseControlSuspended(playSessionActive &&
                                 !activeProject->isMainScenePlaying());
        updateCursorClip();

        int windowWidth = 0;
        int windowHeight = 0;
        getClientSize(windowWidth, windowHeight);
        const bool desiredFrameSync = focused && frameSync;
        if (!minimized && windowWidth > 0 && windowHeight > 0 &&
            (backend->swapchainRebuild ||
             windowWidth != backend->swapchain.Width ||
             windowHeight != backend->swapchain.Height ||
             desiredFrameSync != currentFrameSync)) {
            if (!createMainSwapchain(
                    windowWidth, windowHeight, desiredFrameSync)) {
                backend->shouldClose = true;
                frameInProgress = false;
                return;
            }
            currentFrameSync = desiredFrameSync;
        }

        const bool renderRequested = forceRedraw || playSessionActive ||
                                     !idleFrame || backend->redrawRequested ||
                                     app.hasPendingMainThreadTasks();
        const bool renderMainFrame = !minimized && renderRequested;
        const bool frameReady = renderMainFrame && acquireMainFrame();

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplWin32_NewFrame();
        applyRelativeMouseData();
        ImGui::NewFrame();

        if (frameReady) {
            app.engineRender();
        } else {
            app.processMainThreadTasks();
        }

        app.show();
        bool activityThisFrame = false;
        {
            ImGuiIO& io = ImGui::GetIO();
            bool typing = io.InputQueueCharacters.Size > 0;
            for (int key = ImGuiKey_Keyboard_BEGIN;
                 !typing && key < ImGuiKey_Keyboard_END; ++key)
                typing = ImGui::IsKeyDown(static_cast<ImGuiKey>(key));
            const bool activity =
                io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f ||
                io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f ||
                ImGui::IsAnyMouseDown() || typing || io.WantTextInput ||
                ImGui::IsAnyItemActive() || app.didRenderScene() ||
                app.hasPendingMainThreadTasks() || backend->redrawRequested;
            backend->redrawRequested = false;
            if (activity) lastActivityTime = monotonicSeconds();
            activityThisFrame = activity;
        }

        ImGui::Render();
        bool frameSubmitted = false;
        if (frameReady) {
            // App::show() may create preview framebuffers. Initialize those before
            // opening the swapchain pass so Sokol never sees nested passes.
            render.setClearColor(Vector4(0.45f, 0.55f, 0.60f, 1.00f));
            render.startRenderPass(windowWidth, windowHeight);
            render.endRenderPass();
            SystemRender::commit();
            frameSubmitted = submitMainFrame(ImGui::GetDrawData());
        }

        const bool submitFailed = frameReady && !frameSubmitted;
        if (!submitFailed &&
            (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)) {
            ImGui::UpdatePlatformWindows();
            if (renderRequested || activityThisFrame)
                ImGui::RenderPlatformWindowsDefault();
        }

        if (frameSubmitted) presentMainFrame();
        removeInvalidImGuiTextures();

        if (!focused || minimized) {
            const int sleepMs = static_cast<int>(
                (backend->framePeriod - (monotonicSeconds() - frameStart)) * 1000.0);
            if (sleepMs > 0) Sleep(static_cast<DWORD>(sleepMs));
        }
        frameInProgress = false;
    };

    backend->liveResizeFrame = [&]() { renderFrame(true); };
    while (!backend->shouldClose) {
        const bool idleFrame =
            monotonicSeconds() - lastActivityTime > IDLE_ENTER_DELAY;
        processMessages(idleFrame);
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

    vkDeviceWaitIdle(backend->device);
    ImGui_ImplVulkan_Shutdown();
    ImGui::GetPlatformIO().Platform_CreateVkSurface = nullptr;
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    app.engineViewDestroyed();
    NFD_Quit();
    shutdownVulkan();
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
    const uint32_t viewId = texture->getViewId();
    auto existing = backend->imguiTextures.find(viewId);
    if (existing != backend->imguiTextures.end())
        return (ImTextureID)existing->second;
    VkImageView imageView = reinterpret_cast<VkImageView>(
        const_cast<void*>(texture->getVulkanHandler()));
    if (imageView == VK_NULL_HANDLE) return ImTextureID{};
    VkDescriptorSet descriptor = ImGui_ImplVulkan_AddTexture(
        imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    backend->imguiTextures.emplace(viewId, descriptor);
    return (ImTextureID)descriptor;
}

sg_environment editor::Backend::getSokolEnvironment() {
    sg_environment environment{};
    environment.defaults.sample_count = 1;
    environment.defaults.color_format = SG_PIXELFORMAT_RGBA8;
    environment.defaults.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;
    environment.vulkan.instance = backend->instanceVk;
    environment.vulkan.physical_device = backend->physicalDevice;
    environment.vulkan.device = backend->device;
    environment.vulkan.queue = backend->queue;
    environment.vulkan.queue_family_index = backend->queueFamily;
    return environment;
}

sg_swapchain editor::Backend::getSokolSwapchain() {
    const ImGui_ImplVulkanH_Window& window = backend->swapchain;
    const ImGui_ImplVulkanH_Frame& frame = window.Frames[window.FrameIndex];
    const ImGui_ImplVulkanH_FrameSemaphores& semaphores =
        window.FrameSemaphores[window.SemaphoreIndex];
    sg_swapchain swapchain{};
    swapchain.width = window.Width;
    swapchain.height = window.Height;
    swapchain.sample_count = 1;
    swapchain.color_format = window.SurfaceFormat.format == VK_FORMAT_R8G8B8A8_UNORM
        ? SG_PIXELFORMAT_RGBA8 : SG_PIXELFORMAT_BGRA8;
    swapchain.depth_format = SG_PIXELFORMAT_NONE;
    swapchain.vulkan.render_image = frame.Backbuffer;
    swapchain.vulkan.render_view = frame.BackbufferView;
    swapchain.vulkan.present_complete_semaphore =
        semaphores.ImageAcquiredSemaphore;
    swapchain.vulkan.render_finished_semaphore =
        backend->sokolFinishedSemaphores[window.SemaphoreIndex];
    return swapchain;
}

void editor::Backend::updateWindowTitle(const std::string& projectName) {
    title = projectName.empty()
        ? "Empty project - Doriax Engine"
        : projectName + " - Doriax Engine";
    if (backend && backend->window) {
        const std::wstring wideTitle = utf8ToWide(title);
        SetWindowTextW(backend->window, wideTitle.c_str());
    }
}

void* editor::Backend::getNFDWindowHandle() {
    return &nativeWindowHandle;
}
