#include "Backend.h"
#include "EditorHost.h"

#include "Engine.h"

#include "imgui_internal.h"
#include "imgui_impl_opengl3.h"

#include "nfd.hpp"

#include <GL/glx.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xrandr.h>
#include <X11/keysym.h>

#include <linux/input-event-codes.h>
#include <linux/joystick.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <unistd.h>
#include <vector>

using namespace doriax;
using doriax::editor::PlatformMenuCallback;
using doriax::editor::PlatformMenuCommand;
using doriax::editor::PlatformMenuItem;
using doriax::editor::PlatformMenuItemType;
using doriax::editor::PlatformMenuModel;

namespace {

constexpr long WINDOW_EVENT_MASK =
    StructureNotifyMask | FocusChangeMask | EnterWindowMask | LeaveWindowMask |
    PointerMotionMask | ButtonPressMask | ButtonReleaseMask | KeyPressMask |
    KeyReleaseMask | PropertyChangeMask | ExposureMask;

constexpr int GAMEPAD_COUNT = 16;
constexpr int GAMEPAD_BUTTON_COUNT = 15;
constexpr int GAMEPAD_AXIS_COUNT = 6;
constexpr int MENU_BAR_HEIGHT = 27;
constexpr int MENU_ITEM_HEIGHT = 25;
constexpr int MENU_SEPARATOR_HEIGHT = 9;
constexpr int MENU_HORIZONTAL_PADDING = 12;

struct NativeWindow {
    Window handle = None;
    GLXContext context = nullptr;
    Colormap colormap = None;
    XIC inputContext = nullptr;
    bool owned = false;
    bool focused = false;
    int ignoreMoveFrame = -1;
    int ignoreResizeFrame = -1;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct Gamepad {
    int fd = -1;
    bool connected = false;
    std::string path;
    std::string name;
    std::array<unsigned char, ABS_CNT> axisMap{};
    std::array<unsigned short, KEY_MAX - BTN_MISC + 1> buttonMap{};
    std::array<unsigned char, GAMEPAD_BUTTON_COUNT> buttons{};
    std::array<float, GAMEPAD_AXIS_COUNT> axes{};
    int hatX = 0;
    int hatY = 0;
};

struct OutgoingSelection {
    Window requestor = None;
    Atom property = None;
    Atom target = None;
    size_t offset = 0;
    std::string data;
};

struct MenuRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct MenuPopup {
    Window handle = None;
    std::vector<int> path;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int hovered = -1;
    std::vector<int> itemOffsets;
    std::vector<int> itemHeights;
};

struct NativeMenu {
    Window bar = None;
    GC gc = nullptr;
    XFontSet fontSet = nullptr;
    PlatformMenuModel model;
    std::vector<MenuRect> topLevelRects;
    std::vector<MenuPopup> popups;
    std::vector<PlatformMenuCommand> pendingCommands;
    int activeTop = -1;
    int hoveredTop = -1;
    bool pointerGrabbed = false;
    bool restoreGamePointer = false;
    unsigned long background = 0;
    unsigned long popupBackground = 0;
    unsigned long foreground = 0;
    unsigned long disabled = 0;
    unsigned long highlight = 0;
    unsigned long border = 0;
};

struct LinuxBackendData {
    Display* display = nullptr;
    int screen = 0;
    Window root = None;
    GLXFBConfig fbConfig = nullptr;
    XVisualInfo* visual = nullptr;
    NativeWindow* mainWindow = nullptr;
    std::unordered_map<Window, NativeWindow*> windows;
    XIM inputMethod = nullptr;

    Atom wmDelete = None;
    Atom wmProtocols = None;
    Atom netWmName = None;
    Atom utf8String = None;
    Atom netWmState = None;
    Atom netWmStateMaxVert = None;
    Atom netWmStateMaxHorz = None;
    Atom netWmStateAbove = None;
    Atom netWmStateSkipTaskbar = None;
    Atom netWmWindowType = None;
    Atom netWmWindowTypeDialog = None;
    Atom netWmWindowOpacity = None;
    Atom motifWmHints = None;
    Atom clipboard = None;
    Atom targets = None;
    Atom text = None;
    Atom incr = None;
    Atom clipboardProperty = None;
    Atom xdndAware = None;
    Atom xdndEnter = None;
    Atom xdndPosition = None;
    Atom xdndStatus = None;
    Atom xdndDrop = None;
    Atom xdndFinished = None;
    Atom xdndSelection = None;
    Atom xdndTypeList = None;
    Atom xdndActionCopy = None;
    Atom uriList = None;

    Cursor mouseCursors[ImGuiMouseCursor_COUNT]{};
    Cursor invisibleCursor = None;
    ImGuiMouseCursor lastCursor = ImGuiMouseCursor_COUNT;

    std::string clipboardText;
    std::string clipboardReadBuffer;
    std::vector<OutgoingSelection> outgoingSelections;
    Window xdndSource = None;
    Atom xdndTarget = None;
    int xdndVersion = 0;

    bool shouldClose = false;
    bool redrawRequested = false;
    bool pointerGrabbed = false;
    bool relativeMouse = false;
    double virtualMouseX = 0.0;
    double virtualMouseY = 0.0;
    MouseMode gameMouseMode = MouseMode::NORMAL;
    bool gameCursorInSceneRect = false;
    bool gameCursorHidden = false;
    bool mouseControlSuspended = false;

    int wakePipe[2] = {-1, -1};
    int randrEventBase = -1;
    double time = 0.0;
    double framePeriod = 1.0 / 60.0;
    double nextGamepadScan = 0.0;
    std::array<Gamepad, GAMEPAD_COUNT> gamepads;
    NativeMenu menu;

    using SwapIntervalExt = void (*)(Display*, GLXDrawable, int);
    using SwapIntervalMesa = int (*)(unsigned int);
    using SwapIntervalSgi = int (*)(int);
    SwapIntervalExt swapIntervalExt = nullptr;
    SwapIntervalMesa swapIntervalMesa = nullptr;
    SwapIntervalSgi swapIntervalSgi = nullptr;
};

LinuxBackendData* backend = nullptr;
nfdwindowhandle_t nativeWindowHandle{};
bool contextCreationFailed = false;

int contextErrorHandler(Display*, XErrorEvent*) {
    contextCreationFailed = true;
    return 0;
}

double monotonicSeconds() {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

Atom atom(const char* name) {
    return XInternAtom(backend->display, name, False);
}

NativeWindow* findWindow(Window handle) {
    auto it = backend->windows.find(handle);
    return it == backend->windows.end() ? nullptr : it->second;
}

ImGuiViewport* findViewport(NativeWindow* window) {
    return window ? ImGui::FindViewportByPlatformHandle(window) : nullptr;
}

void getWindowSize(NativeWindow* window, int& width, int& height);

unsigned long allocateImGuiColor(const ImVec4& color, unsigned long fallback) {
    XColor xcolor{};
    xcolor.red = static_cast<unsigned short>(
        std::clamp(color.x, 0.0f, 1.0f) * 65535.0f + 0.5f);
    xcolor.green = static_cast<unsigned short>(
        std::clamp(color.y, 0.0f, 1.0f) * 65535.0f + 0.5f);
    xcolor.blue = static_cast<unsigned short>(
        std::clamp(color.z, 0.0f, 1.0f) * 65535.0f + 0.5f);
    xcolor.flags = DoRed | DoGreen | DoBlue;
    if (XAllocColor(backend->display, backend->mainWindow->colormap, &xcolor))
        return xcolor.pixel;
    return fallback;
}

ImVec4 opaqueStyleColor(ImGuiCol color, ImGuiCol fallback) {
    const ImVec4& value = ImGui::GetStyle().Colors[color];
    if (value.w > 0.001f) return value;
    return ImGui::GetStyle().Colors[fallback];
}

void initializeNativeMenuColors() {
    NativeMenu& menu = backend->menu;
    const ImGuiStyle& style = ImGui::GetStyle();
    // Theme::apply leaves MenuBarBg fully transparent so the ImGui bar shows the
    // window behind it. Native X11 needs an opaque fill — use WindowBg instead.
    const ImVec4 barBackground = opaqueStyleColor(ImGuiCol_MenuBarBg, ImGuiCol_WindowBg);
    const unsigned long black = BlackPixel(backend->display, backend->screen);
    const unsigned long white = WhitePixel(backend->display, backend->screen);
    menu.background = allocateImGuiColor(barBackground, black);
    menu.popupBackground = allocateImGuiColor(style.Colors[ImGuiCol_PopupBg], menu.background);
    menu.foreground = allocateImGuiColor(style.Colors[ImGuiCol_Text], white);
    menu.disabled = allocateImGuiColor(style.Colors[ImGuiCol_TextDisabled], menu.foreground);
    menu.highlight = allocateImGuiColor(style.Colors[ImGuiCol_HeaderHovered], menu.background);
    menu.border = allocateImGuiColor(style.Colors[ImGuiCol_Border], black);
}

int menuTextWidth(const std::string& text) {
    if (text.empty()) return 0;
    if (backend->menu.fontSet)
        return Xutf8TextEscapement(backend->menu.fontSet, text.c_str(),
                                   static_cast<int>(text.size()));
    return static_cast<int>(text.size()) * 8;
}

int menuTextBaseline(int y, int height) {
    if (backend->menu.fontSet) {
        XFontSetExtents* extents = XExtentsOfFontSet(backend->menu.fontSet);
        const XRectangle& logical = extents->max_logical_extent;
        return y + (height - logical.height) / 2 - logical.y;
    }
    return y + (height + 10) / 2;
}

void drawMenuText(Window window, const std::string& text, int x, int baseline,
                  unsigned long color) {
    XSetForeground(backend->display, backend->menu.gc, color);
    if (backend->menu.fontSet) {
        Xutf8DrawString(backend->display, window, backend->menu.fontSet,
                        backend->menu.gc, x, baseline, text.c_str(),
                        static_cast<int>(text.size()));
    } else {
        XDrawString(backend->display, window, backend->menu.gc, x, baseline,
                    text.c_str(), static_cast<int>(text.size()));
    }
}

const PlatformMenuItem* menuItemAtPath(const std::vector<int>& path) {
    if (path.empty() || path[0] < 0 ||
        path[0] >= static_cast<int>(backend->menu.model.menus.size()))
        return nullptr;
    const PlatformMenuItem* item = &backend->menu.model.menus[path[0]];
    for (size_t depth = 1; depth < path.size(); ++depth) {
        const int index = path[depth];
        if (index < 0 || index >= static_cast<int>(item->children.size())) return nullptr;
        item = &item->children[index];
    }
    return item;
}

const std::vector<PlatformMenuItem>* popupMenuItems(const MenuPopup& popup) {
    const PlatformMenuItem* parent = menuItemAtPath(popup.path);
    return parent ? &parent->children : nullptr;
}

void drawMenuBar() {
    NativeMenu& menu = backend->menu;
    if (menu.bar == None || !menu.gc) return;
    int width = 1;
    int height = 1;
    getWindowSize(backend->mainWindow, width, height);
    XSetForeground(backend->display, menu.gc, menu.background);
    XFillRectangle(backend->display, menu.bar, menu.gc, 0, 0,
                   static_cast<unsigned int>(std::max(width, 1)), MENU_BAR_HEIGHT);

    menu.topLevelRects.clear();
    int cursorX = 4;
    for (size_t i = 0; i < menu.model.menus.size(); ++i) {
        const PlatformMenuItem& item = menu.model.menus[i];
        const int itemWidth = menuTextWidth(item.label) + MENU_HORIZONTAL_PADDING * 2;
        MenuRect rect{cursorX, 0, itemWidth, MENU_BAR_HEIGHT};
        menu.topLevelRects.push_back(rect);
        if (static_cast<int>(i) == menu.activeTop ||
            (menu.activeTop < 0 && static_cast<int>(i) == menu.hoveredTop)) {
            XSetForeground(backend->display, menu.gc, menu.highlight);
            XFillRectangle(backend->display, menu.bar, menu.gc, rect.x, rect.y,
                           static_cast<unsigned int>(rect.width),
                           static_cast<unsigned int>(rect.height));
        }
        drawMenuText(menu.bar, item.label, rect.x + MENU_HORIZONTAL_PADDING,
                     menuTextBaseline(0, MENU_BAR_HEIGHT),
                     item.enabled ? menu.foreground : menu.disabled);
        cursorX += itemWidth;
    }
    XSetForeground(backend->display, menu.gc, menu.border);
    XDrawLine(backend->display, menu.bar, menu.gc, 0, MENU_BAR_HEIGHT - 1,
              std::max(width - 1, 0), MENU_BAR_HEIGHT - 1);
}

void drawMenuPopup(MenuPopup& popup) {
    const std::vector<PlatformMenuItem>* items = popupMenuItems(popup);
    if (!items || popup.handle == None) return;
    NativeMenu& menu = backend->menu;
    XSetForeground(backend->display, menu.gc, menu.popupBackground);
    XFillRectangle(backend->display, popup.handle, menu.gc, 0, 0,
                   static_cast<unsigned int>(popup.width),
                   static_cast<unsigned int>(popup.height));

    for (size_t i = 0; i < items->size(); ++i) {
        const PlatformMenuItem& item = (*items)[i];
        const int itemY = popup.itemOffsets[i];
        const int itemHeight = popup.itemHeights[i];
        if (item.type == PlatformMenuItemType::Separator) {
            XSetForeground(backend->display, menu.gc, menu.border);
            XDrawLine(backend->display, popup.handle, menu.gc, 7,
                      itemY + itemHeight / 2, popup.width - 7,
                      itemY + itemHeight / 2);
            continue;
        }
        if (popup.hovered == static_cast<int>(i) && item.enabled) {
            XSetForeground(backend->display, menu.gc, menu.highlight);
            XFillRectangle(backend->display, popup.handle, menu.gc, 1, itemY,
                           static_cast<unsigned int>(std::max(popup.width - 2, 1)),
                           static_cast<unsigned int>(itemHeight));
        }
        const unsigned long color = item.enabled ? menu.foreground : menu.disabled;
        const int baseline = menuTextBaseline(itemY, itemHeight);
        if (item.checked) {
            XSetForeground(backend->display, menu.gc, color);
            const int centerY = itemY + itemHeight / 2;
            XDrawLine(backend->display, popup.handle, menu.gc, 9, centerY,
                      12, centerY + 3);
            XDrawLine(backend->display, popup.handle, menu.gc, 12, centerY + 3,
                      18, centerY - 4);
        }
        drawMenuText(popup.handle, item.label, 29, baseline, color);
        if (!item.shortcut.empty()) {
            drawMenuText(popup.handle, item.shortcut,
                         popup.width - 18 - menuTextWidth(item.shortcut),
                         baseline, color);
        }
        if (item.type == PlatformMenuItemType::Submenu)
            drawMenuText(popup.handle, ">", popup.width - 14, baseline, color);
    }

    XSetForeground(backend->display, menu.gc, menu.border);
    XDrawRectangle(backend->display, popup.handle, menu.gc, 0, 0,
                   static_cast<unsigned int>(std::max(popup.width - 1, 0)),
                   static_cast<unsigned int>(std::max(popup.height - 1, 0)));
}

void destroyMenuPopupsFrom(size_t first) {
    NativeMenu& menu = backend->menu;
    while (menu.popups.size() > first) {
        if (menu.popups.back().handle != None)
            XDestroyWindow(backend->display, menu.popups.back().handle);
        menu.popups.pop_back();
    }
}

void closeNativeMenu() {
    NativeMenu& menu = backend->menu;
    const bool restoreGamePointer = menu.restoreGamePointer;
    menu.restoreGamePointer = false;
    destroyMenuPopupsFrom(0);
    if (menu.pointerGrabbed) {
        XUngrabPointer(backend->display, CurrentTime);
        menu.pointerGrabbed = false;
    }
    menu.activeTop = -1;
    menu.hoveredTop = -1;
    drawMenuBar();
    if (restoreGamePointer && backend->mainWindow->focused &&
        !backend->mouseControlSuspended)
        editor::Backend::setMouseMode(backend->gameMouseMode);
}

bool getMenuBarRootPosition(int& rootX, int& rootY) {
    if (backend->menu.bar == None) return false;
    Window child = None;
    return XTranslateCoordinates(backend->display, backend->menu.bar, backend->root,
                                 0, 0, &rootX, &rootY, &child) != 0;
}

bool createMenuPopup(const std::vector<int>& path, int requestedX, int requestedY) {
    const PlatformMenuItem* parent = menuItemAtPath(path);
    if (!parent || parent->children.empty()) return false;

    MenuPopup popup;
    popup.path = path;
    popup.width = 150;
    popup.height = 2;
    for (const PlatformMenuItem& item : parent->children) {
        const int rowHeight = item.type == PlatformMenuItemType::Separator
            ? MENU_SEPARATOR_HEIGHT : MENU_ITEM_HEIGHT;
        popup.itemOffsets.push_back(popup.height - 1);
        popup.itemHeights.push_back(rowHeight);
        popup.height += rowHeight;
        int rowWidth = 50 + menuTextWidth(item.label);
        if (!item.shortcut.empty()) rowWidth += 28 + menuTextWidth(item.shortcut);
        if (item.type == PlatformMenuItemType::Submenu) rowWidth += 16;
        popup.width = std::max(popup.width, rowWidth);
    }

    const int screenWidth = DisplayWidth(backend->display, backend->screen);
    const int screenHeight = DisplayHeight(backend->display, backend->screen);
    popup.width = std::min(popup.width, std::max(screenWidth - 8, 1));
    popup.height = std::min(popup.height, std::max(screenHeight - 8, 1));
    popup.x = std::clamp(requestedX, 0, std::max(screenWidth - popup.width, 0));
    popup.y = std::clamp(requestedY, 0, std::max(screenHeight - popup.height, 0));

    XSetWindowAttributes attributes{};
    attributes.colormap = backend->mainWindow->colormap;
    attributes.background_pixel = backend->menu.popupBackground;
    attributes.override_redirect = True;
    attributes.save_under = True;
    attributes.cursor = backend->mouseCursors[ImGuiMouseCursor_Arrow];
    attributes.event_mask = ExposureMask | EnterWindowMask | LeaveWindowMask |
        PointerMotionMask | ButtonPressMask | ButtonReleaseMask;
    popup.handle = XCreateWindow(
        backend->display, backend->root, popup.x, popup.y,
        static_cast<unsigned int>(popup.width), static_cast<unsigned int>(popup.height), 0,
        backend->visual->depth, InputOutput, backend->visual->visual,
        CWColormap | CWBackPixel | CWOverrideRedirect | CWSaveUnder | CWEventMask | CWCursor,
        &attributes);
    if (popup.handle == None) return false;
    backend->menu.popups.push_back(std::move(popup));
    XMapRaised(backend->display, backend->menu.popups.back().handle);
    drawMenuPopup(backend->menu.popups.back());
    return true;
}

void openTopLevelMenu(int index) {
    NativeMenu& menu = backend->menu;
    if (index < 0 || index >= static_cast<int>(menu.model.menus.size()) ||
        !menu.model.menus[index].enabled)
        return;
    destroyMenuPopupsFrom(0);
    menu.activeTop = index;
    menu.hoveredTop = index;
    drawMenuBar();

    int barX = 0;
    int barY = 0;
    if (!getMenuBarRootPosition(barX, barY) ||
        index >= static_cast<int>(menu.topLevelRects.size())) {
        closeNativeMenu();
        return;
    }
    const MenuRect& rect = menu.topLevelRects[index];
    if (!createMenuPopup({index}, barX + rect.x, barY + MENU_BAR_HEIGHT)) {
        closeNativeMenu();
        return;
    }

    if (!menu.pointerGrabbed) {
        if (backend->pointerGrabbed) {
            XUngrabPointer(backend->display, CurrentTime);
            backend->pointerGrabbed = false;
            backend->relativeMouse = false;
            menu.restoreGamePointer = true;
        }
        menu.pointerGrabbed = XGrabPointer(
            backend->display, menu.bar, False,
            PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
            GrabModeAsync, GrabModeAsync, None,
            backend->mouseCursors[ImGuiMouseCursor_Arrow], CurrentTime) == GrabSuccess;
    }
}

int menuTopLevelAt(int rootX, int rootY) {
    int barX = 0;
    int barY = 0;
    if (!getMenuBarRootPosition(barX, barY)) return -1;
    for (size_t i = 0; i < backend->menu.topLevelRects.size(); ++i) {
        const MenuRect& rect = backend->menu.topLevelRects[i];
        if (rootX >= barX + rect.x && rootX < barX + rect.x + rect.width &&
            rootY >= barY + rect.y && rootY < barY + rect.y + rect.height)
            return static_cast<int>(i);
    }
    return -1;
}

int menuPopupItemAt(const MenuPopup& popup, int rootX, int rootY) {
    if (rootX < popup.x || rootX >= popup.x + popup.width ||
        rootY < popup.y || rootY >= popup.y + popup.height)
        return -2;
    const int localY = rootY - popup.y;
    for (size_t i = 0; i < popup.itemOffsets.size(); ++i) {
        if (localY >= popup.itemOffsets[i] &&
            localY < popup.itemOffsets[i] + popup.itemHeights[i])
            return static_cast<int>(i);
    }
    return -1;
}

void openMenuChild(size_t popupIndex, int itemIndex) {
    if (popupIndex >= backend->menu.popups.size()) return;
    MenuPopup& parentPopup = backend->menu.popups[popupIndex];
    const std::vector<PlatformMenuItem>* items = popupMenuItems(parentPopup);
    if (!items || itemIndex < 0 || itemIndex >= static_cast<int>(items->size())) return;
    const PlatformMenuItem& item = (*items)[itemIndex];
    if (!item.enabled || item.type != PlatformMenuItemType::Submenu) return;

    std::vector<int> childPath = parentPopup.path;
    childPath.push_back(itemIndex);
    if (backend->menu.popups.size() > popupIndex + 1 &&
        backend->menu.popups[popupIndex + 1].path == childPath)
        return;
    const int requestedX = parentPopup.x + parentPopup.width - 2;
    const int requestedY = parentPopup.y + parentPopup.itemOffsets[itemIndex];
    destroyMenuPopupsFrom(popupIndex + 1);
    createMenuPopup(childPath, requestedX, requestedY);
}

void updateMenuPointer(int rootX, int rootY) {
    NativeMenu& menu = backend->menu;
    const int top = menuTopLevelAt(rootX, rootY);
    if (menu.activeTop < 0) {
        if (top != menu.hoveredTop) {
            menu.hoveredTop = top;
            drawMenuBar();
        }
        return;
    }
    if (top >= 0 && top != menu.activeTop) {
        openTopLevelMenu(top);
        return;
    }

    for (size_t reverse = menu.popups.size(); reverse > 0; --reverse) {
        const size_t popupIndex = reverse - 1;
        MenuPopup& popup = menu.popups[popupIndex];
        const int itemIndex = menuPopupItemAt(popup, rootX, rootY);
        if (itemIndex == -2) continue;
        if (popup.hovered != itemIndex) {
            popup.hovered = itemIndex;
            drawMenuPopup(popup);
        }
        const std::vector<PlatformMenuItem>* items = popupMenuItems(popup);
        if (items && itemIndex >= 0 && itemIndex < static_cast<int>(items->size()) &&
            (*items)[itemIndex].enabled &&
            (*items)[itemIndex].type == PlatformMenuItemType::Submenu) {
            openMenuChild(popupIndex, itemIndex);
        } else {
            destroyMenuPopupsFrom(popupIndex + 1);
        }
        return;
    }
}

void activateMenuItem(size_t popupIndex, int itemIndex) {
    if (popupIndex >= backend->menu.popups.size()) return;
    const std::vector<PlatformMenuItem>* items = popupMenuItems(backend->menu.popups[popupIndex]);
    if (!items || itemIndex < 0 || itemIndex >= static_cast<int>(items->size())) return;
    const PlatformMenuItem item = (*items)[itemIndex];
    if (!item.enabled || item.type == PlatformMenuItemType::Separator) return;
    if (item.type == PlatformMenuItemType::Submenu) {
        openMenuChild(popupIndex, itemIndex);
        return;
    }
    const PlatformMenuCommand command = item.command;
    closeNativeMenu();
    backend->redrawRequested = true;
    backend->menu.pendingCommands.push_back(command);
}

int selectableMenuItem(const MenuPopup& popup, int start, int direction) {
    const std::vector<PlatformMenuItem>* items = popupMenuItems(popup);
    if (!items || items->empty()) return -1;
    int index = start;
    for (size_t count = 0; count < items->size(); ++count) {
        index = (index + direction + static_cast<int>(items->size())) %
            static_cast<int>(items->size());
        if ((*items)[index].type != PlatformMenuItemType::Separator &&
            (*items)[index].enabled)
            return index;
    }
    return -1;
}

bool processMenuKey(XKeyEvent& event) {
    NativeMenu& menu = backend->menu;
    const KeySym key = XLookupKeysym(&event, 0);
    if (menu.activeTop < 0) {
        int target = -1;
        if (key == XK_F10 && !menu.model.menus.empty()) {
            target = 0;
        } else if ((event.state & Mod1Mask) != 0) {
            const int pressed = std::tolower(static_cast<unsigned char>(key));
            for (size_t i = 0; i < menu.model.menus.size(); ++i) {
                const std::string& label = menu.model.menus[i].label;
                if (!label.empty() &&
                    std::tolower(static_cast<unsigned char>(label[0])) == pressed) {
                    target = static_cast<int>(i);
                    break;
                }
            }
        }
        if (target >= 0) {
            openTopLevelMenu(target);
            if (!menu.popups.empty()) {
                menu.popups[0].hovered = selectableMenuItem(menu.popups[0], -1, 1);
                drawMenuPopup(menu.popups[0]);
            }
            return true;
        }
        return false;
    }

    if (key == XK_Escape) {
        closeNativeMenu();
        return true;
    }
    if (menu.popups.empty()) return false;
    if (key == XK_Down || key == XK_Up) {
        MenuPopup& popup = menu.popups.back();
        popup.hovered = selectableMenuItem(popup, popup.hovered,
                                           key == XK_Down ? 1 : -1);
        drawMenuPopup(popup);
        return true;
    }
    if (key == XK_Left) {
        if (menu.popups.size() > 1) {
            destroyMenuPopupsFrom(menu.popups.size() - 1);
        } else {
            const int count = static_cast<int>(menu.model.menus.size());
            openTopLevelMenu((menu.activeTop + count - 1) % count);
            if (!menu.popups.empty()) {
                menu.popups[0].hovered = selectableMenuItem(menu.popups[0], -1, 1);
                drawMenuPopup(menu.popups[0]);
            }
        }
        return true;
    }
    if (key == XK_Right) {
        MenuPopup& popup = menu.popups.back();
        const std::vector<PlatformMenuItem>* items = popupMenuItems(popup);
        if (items && popup.hovered >= 0 &&
            popup.hovered < static_cast<int>(items->size()) &&
            (*items)[popup.hovered].type == PlatformMenuItemType::Submenu) {
            openMenuChild(menu.popups.size() - 1, popup.hovered);
            if (menu.popups.size() > 1) {
                menu.popups.back().hovered = selectableMenuItem(menu.popups.back(), -1, 1);
                drawMenuPopup(menu.popups.back());
            }
        } else {
            openTopLevelMenu((menu.activeTop + 1) % static_cast<int>(menu.model.menus.size()));
            if (!menu.popups.empty()) {
                menu.popups[0].hovered = selectableMenuItem(menu.popups[0], -1, 1);
                drawMenuPopup(menu.popups[0]);
            }
        }
        return true;
    }
    if (key == XK_Return || key == XK_KP_Enter || key == XK_space) {
        const size_t popupIndex = menu.popups.size() - 1;
        activateMenuItem(popupIndex, menu.popups[popupIndex].hovered);
        return true;
    }
    return false;
}

bool processNativeMenuEvent(XEvent& event) {
    NativeMenu& menu = backend->menu;
    bool isMenuWindow = event.xany.window == menu.bar;
    if (!isMenuWindow) {
        for (const MenuPopup& popup : menu.popups)
            isMenuWindow |= event.xany.window == popup.handle;
    }

    if (event.type == Expose && isMenuWindow) {
        if (event.xexpose.window == menu.bar) drawMenuBar();
        else {
            for (MenuPopup& popup : menu.popups)
                if (popup.handle == event.xexpose.window) drawMenuPopup(popup);
        }
        return true;
    }
    if (event.type == KeyPress) {
        // F10 and Alt+mnemonic events arrive on the main window because the menu
        // bar does not take keyboard focus.
        if (processMenuKey(event.xkey)) return true;
        if (menu.activeTop >= 0 || isMenuWindow) return true;
    }
    if (event.type == MotionNotify && (isMenuWindow || menu.activeTop >= 0)) {
        updateMenuPointer(event.xmotion.x_root, event.xmotion.y_root);
        return true;
    }
    if (event.type == LeaveNotify && event.xcrossing.window == menu.bar &&
        menu.activeTop < 0 && event.xcrossing.mode == NotifyNormal) {
        menu.hoveredTop = -1;
        drawMenuBar();
        return true;
    }
    if (event.type == ButtonPress && (isMenuWindow || menu.activeTop >= 0)) {
        if (event.xbutton.button != Button1) return true;
        const int top = menuTopLevelAt(event.xbutton.x_root, event.xbutton.y_root);
        if (top >= 0) {
            if (top == menu.activeTop) closeNativeMenu();
            else openTopLevelMenu(top);
            return true;
        }
        for (const MenuPopup& popup : menu.popups)
            if (menuPopupItemAt(popup, event.xbutton.x_root, event.xbutton.y_root) != -2)
                return true;
        closeNativeMenu();
        return true;
    }
    if (event.type == ButtonRelease && menu.activeTop >= 0) {
        if (event.xbutton.button != Button1) return true;
        if (menuTopLevelAt(event.xbutton.x_root, event.xbutton.y_root) >= 0) return true;
        for (size_t reverse = menu.popups.size(); reverse > 0; --reverse) {
            const size_t popupIndex = reverse - 1;
            const int item = menuPopupItemAt(menu.popups[popupIndex],
                                             event.xbutton.x_root, event.xbutton.y_root);
            if (item != -2) {
                activateMenuItem(popupIndex, item);
                return true;
            }
        }
        return true;
    }
    return isMenuWindow;
}

bool initializeNativeMenu() {
    NativeMenu& menu = backend->menu;
    if (menu.bar != None) return true;
    initializeNativeMenuColors();

    XSetWindowAttributes attributes{};
    attributes.colormap = backend->mainWindow->colormap;
    attributes.background_pixel = menu.background;
    attributes.cursor = backend->mouseCursors[ImGuiMouseCursor_Arrow];
    attributes.event_mask = ExposureMask | EnterWindowMask | LeaveWindowMask |
        PointerMotionMask | ButtonPressMask | ButtonReleaseMask;
    int width = 1;
    int height = 1;
    getWindowSize(backend->mainWindow, width, height);
    menu.bar = XCreateWindow(
        backend->display, backend->mainWindow->handle, 0, 0,
        static_cast<unsigned int>(std::max(width, 1)), MENU_BAR_HEIGHT, 0,
        backend->visual->depth, InputOutput, backend->visual->visual,
        CWColormap | CWBackPixel | CWEventMask | CWCursor, &attributes);
    if (menu.bar == None) return false;
    menu.gc = XCreateGC(backend->display, menu.bar, 0, nullptr);
    if (!menu.gc) {
        XDestroyWindow(backend->display, menu.bar);
        menu.bar = None;
        return false;
    }

    char** missing = nullptr;
    int missingCount = 0;
    char* defaultString = nullptr;
    menu.fontSet = XCreateFontSet(
        backend->display, "-*-sans-medium-r-normal--14-*-*-*-*-*-*-*",
        &missing, &missingCount, &defaultString);
    if (missing) XFreeStringList(missing);
    if (!menu.fontSet) {
        missing = nullptr;
        menu.fontSet = XCreateFontSet(backend->display, "fixed",
                                      &missing, &missingCount, &defaultString);
        if (missing) XFreeStringList(missing);
    }
    XMapRaised(backend->display, menu.bar);
    drawMenuBar();
    return true;
}

void syncNativeMenuGeometry() {
    NativeMenu& menu = backend->menu;
    if (menu.bar == None) return;
    int width = 1;
    int height = 1;
    getWindowSize(backend->mainWindow, width, height);
    XMoveResizeWindow(backend->display, menu.bar, 0, 0,
                      static_cast<unsigned int>(std::max(width, 1)), MENU_BAR_HEIGHT);
    XRaiseWindow(backend->display, menu.bar);
    if (menu.activeTop >= 0) openTopLevelMenu(menu.activeTop);
    else drawMenuBar();
}

void shutdownNativeMenu() {
    NativeMenu& menu = backend->menu;
    if (menu.bar == None) return;
    menu.restoreGamePointer = false;
    closeNativeMenu();
    if (menu.fontSet) XFreeFontSet(backend->display, menu.fontSet);
    if (menu.gc) XFreeGC(backend->display, menu.gc);
    XDestroyWindow(backend->display, menu.bar);
    menu = NativeMenu{};
}

void setWindowTitle(NativeWindow* window, const char* title) {
    XStoreName(backend->display, window->handle, title);
    XChangeProperty(backend->display, window->handle, backend->netWmName,
                    backend->utf8String, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(title),
                    static_cast<int>(std::strlen(title)));
}

void sendWmState(NativeWindow* window, long action, Atom first, Atom second = None) {
    XEvent event{};
    event.type = ClientMessage;
    event.xclient.window = window->handle;
    event.xclient.message_type = backend->netWmState;
    event.xclient.format = 32;
    event.xclient.data.l[0] = action;
    event.xclient.data.l[1] = static_cast<long>(first);
    event.xclient.data.l[2] = static_cast<long>(second);
    event.xclient.data.l[3] = 1;
    XSendEvent(backend->display, backend->root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
}

void setDecorated(NativeWindow* window, bool decorated) {
    struct MotifHints {
        unsigned long flags;
        unsigned long functions;
        unsigned long decorations;
        long inputMode;
        unsigned long status;
    } hints{2, 0, decorated ? 1UL : 0UL, 0, 0};
    XChangeProperty(backend->display, window->handle, backend->motifWmHints,
                    backend->motifWmHints, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(&hints), 5);
}

GLXContext createContext(GLXContext shared) {
    using CreateContextAttribs = GLXContext (*)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
    auto createContextAttribs = reinterpret_cast<CreateContextAttribs>(
        glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXCreateContextAttribsARB")));
    if (createContextAttribs) {
        const int attributes[] = {
            GLX_CONTEXT_MAJOR_VERSION_ARB, 4,
            GLX_CONTEXT_MINOR_VERSION_ARB, 1,
            GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
            GLX_CONTEXT_FLAGS_ARB, GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
            None
        };
        contextCreationFailed = false;
        XErrorHandler oldHandler = XSetErrorHandler(contextErrorHandler);
        GLXContext context = createContextAttribs(
            backend->display, backend->fbConfig, shared, True, attributes);
        XSync(backend->display, False);
        XSetErrorHandler(oldHandler);
        if (context && !contextCreationFailed) return context;
        if (context) glXDestroyContext(backend->display, context);
    }
    contextCreationFailed = false;
    XErrorHandler oldHandler = XSetErrorHandler(contextErrorHandler);
    GLXContext context = glXCreateNewContext(
        backend->display, backend->fbConfig, GLX_RGBA_TYPE, shared, True);
    XSync(backend->display, False);
    XSetErrorHandler(oldHandler);
    if (contextCreationFailed) {
        if (context) glXDestroyContext(backend->display, context);
        return nullptr;
    }
    return context;
}

void configureXdnd(NativeWindow* window) {
    const unsigned long version = 5;
    XChangeProperty(backend->display, window->handle, backend->xdndAware,
                    XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&version), 1);
}

NativeWindow* createNativeWindow(int x, int y, int width, int height,
                                 bool decorated, bool topmost, bool skipTaskbar,
                                 GLXContext shared) {
    auto* result = new NativeWindow();
    result->owned = true;
    result->x = x;
    result->y = y;
    result->width = width;
    result->height = height;

    XSetWindowAttributes attributes{};
    attributes.colormap = XCreateColormap(
        backend->display, backend->root, backend->visual->visual, AllocNone);
    attributes.event_mask = WINDOW_EVENT_MASK;
    result->colormap = attributes.colormap;
    result->handle = XCreateWindow(
        backend->display, backend->root, x, y,
        static_cast<unsigned int>(std::max(width, 1)),
        static_cast<unsigned int>(std::max(height, 1)), 0,
        backend->visual->depth, InputOutput, backend->visual->visual,
        CWColormap | CWEventMask, &attributes);
    if (!result->handle) {
        XFreeColormap(backend->display, result->colormap);
        delete result;
        return nullptr;
    }

    XSetWMProtocols(backend->display, result->handle, &backend->wmDelete, 1);
    setDecorated(result, decorated);
    configureXdnd(result);

    XClassHint classHint{};
    classHint.res_name = const_cast<char*>("doriax-editor");
    classHint.res_class = const_cast<char*>("DoriaxEditor");
    XSetClassHint(backend->display, result->handle, &classHint);

    if (topmost || skipTaskbar) {
        Atom states[2];
        int count = 0;
        if (topmost) states[count++] = backend->netWmStateAbove;
        if (skipTaskbar) states[count++] = backend->netWmStateSkipTaskbar;
        XChangeProperty(backend->display, result->handle, backend->netWmState,
                        XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<unsigned char*>(states), count);
    }
    if (skipTaskbar) {
        XChangeProperty(backend->display, result->handle, backend->netWmWindowType,
                        XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<unsigned char*>(&backend->netWmWindowTypeDialog), 1);
    }

    result->context = createContext(shared);
    if (!result->context) {
        XDestroyWindow(backend->display, result->handle);
        XFreeColormap(backend->display, result->colormap);
        delete result;
        return nullptr;
    }

    if (backend->inputMethod) {
        result->inputContext = XCreateIC(
            backend->inputMethod,
            XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
            XNClientWindow, result->handle,
            XNFocusWindow, result->handle,
            nullptr);
    }
    backend->windows[result->handle] = result;
    return result;
}

void destroyNativeWindow(NativeWindow* window) {
    if (!window) return;
    backend->windows.erase(window->handle);
    if (window->inputContext) XDestroyIC(window->inputContext);
    if (window->context) glXDestroyContext(backend->display, window->context);
    if (window->handle) XDestroyWindow(backend->display, window->handle);
    if (window->colormap) XFreeColormap(backend->display, window->colormap);
    delete window;
}

ImGuiKey keySymToImGui(KeySym key) {
    if (key >= XK_0 && key <= XK_9) return static_cast<ImGuiKey>(ImGuiKey_0 + key - XK_0);
    if (key >= XK_a && key <= XK_z) return static_cast<ImGuiKey>(ImGuiKey_A + key - XK_a);
    if (key >= XK_A && key <= XK_Z) return static_cast<ImGuiKey>(ImGuiKey_A + key - XK_A);
    if (key >= XK_F1 && key <= XK_F24) return static_cast<ImGuiKey>(ImGuiKey_F1 + key - XK_F1);
    if (key >= XK_KP_0 && key <= XK_KP_9)
        return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + key - XK_KP_0);

    switch (key) {
        case XK_Tab: case XK_ISO_Left_Tab: return ImGuiKey_Tab;
        case XK_Left: return ImGuiKey_LeftArrow;
        case XK_Right: return ImGuiKey_RightArrow;
        case XK_Up: return ImGuiKey_UpArrow;
        case XK_Down: return ImGuiKey_DownArrow;
        case XK_Prior: return ImGuiKey_PageUp;
        case XK_Next: return ImGuiKey_PageDown;
        case XK_Home: return ImGuiKey_Home;
        case XK_End: return ImGuiKey_End;
        case XK_Insert: return ImGuiKey_Insert;
        case XK_Delete: return ImGuiKey_Delete;
        case XK_BackSpace: return ImGuiKey_Backspace;
        case XK_space: return ImGuiKey_Space;
        case XK_Return: return ImGuiKey_Enter;
        case XK_Escape: return ImGuiKey_Escape;
        case XK_apostrophe: return ImGuiKey_Apostrophe;
        case XK_comma: return ImGuiKey_Comma;
        case XK_minus: return ImGuiKey_Minus;
        case XK_period: return ImGuiKey_Period;
        case XK_slash: return ImGuiKey_Slash;
        case XK_semicolon: return ImGuiKey_Semicolon;
        case XK_equal: return ImGuiKey_Equal;
        case XK_bracketleft: return ImGuiKey_LeftBracket;
        case XK_backslash: return ImGuiKey_Backslash;
        case XK_bracketright: return ImGuiKey_RightBracket;
        case XK_grave: return ImGuiKey_GraveAccent;
        case XK_Caps_Lock: return ImGuiKey_CapsLock;
        case XK_Scroll_Lock: return ImGuiKey_ScrollLock;
        case XK_Num_Lock: return ImGuiKey_NumLock;
        case XK_Print: return ImGuiKey_PrintScreen;
        case XK_Pause: return ImGuiKey_Pause;
        case XK_KP_Decimal: case XK_KP_Delete: return ImGuiKey_KeypadDecimal;
        case XK_KP_Divide: return ImGuiKey_KeypadDivide;
        case XK_KP_Multiply: return ImGuiKey_KeypadMultiply;
        case XK_KP_Subtract: return ImGuiKey_KeypadSubtract;
        case XK_KP_Add: return ImGuiKey_KeypadAdd;
        case XK_KP_Enter: return ImGuiKey_KeypadEnter;
        case XK_KP_Equal: return ImGuiKey_KeypadEqual;
        case XK_Shift_L: return ImGuiKey_LeftShift;
        case XK_Control_L: return ImGuiKey_LeftCtrl;
        case XK_Alt_L: case XK_Meta_L: return ImGuiKey_LeftAlt;
        case XK_Super_L: return ImGuiKey_LeftSuper;
        case XK_Shift_R: return ImGuiKey_RightShift;
        case XK_Control_R: return ImGuiKey_RightCtrl;
        case XK_Alt_R: case XK_Meta_R: case XK_ISO_Level3_Shift: return ImGuiKey_RightAlt;
        case XK_Super_R: return ImGuiKey_RightSuper;
        case XK_Menu: return ImGuiKey_Menu;
        case XK_less: case XK_greater: return ImGuiKey_Oem102;
        default: return ImGuiKey_None;
    }
}

unsigned int modifierMaskForKey(KeySym key) {
    switch (key) {
        case XK_Shift_L: case XK_Shift_R: return ShiftMask;
        case XK_Control_L: case XK_Control_R: return ControlMask;
        case XK_Alt_L: case XK_Alt_R: case XK_Meta_L: case XK_Meta_R:
        case XK_ISO_Level3_Shift: return Mod1Mask;
        case XK_Super_L: case XK_Super_R: return Mod4Mask;
        default: return 0;
    }
}

void addModifiers(ImGuiIO& io, unsigned int state) {
    io.AddKeyEvent(ImGuiMod_Ctrl, (state & ControlMask) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (state & ShiftMask) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (state & Mod1Mask) != 0);
    io.AddKeyEvent(ImGuiMod_Super, (state & Mod4Mask) != 0);
}

int mouseButton(unsigned int button) {
    switch (button) {
        case Button1: return ImGuiMouseButton_Left;
        case Button2: return ImGuiMouseButton_Middle;
        case Button3: return ImGuiMouseButton_Right;
        case 8: return 3;
        case 9: return 4;
        default: return -1;
    }
}

void getWindowSize(NativeWindow* window, int& width, int& height) {
    XWindowAttributes attributes{};
    XGetWindowAttributes(backend->display, window->handle, &attributes);
    width = attributes.width;
    height = attributes.height;
}

ImVec2 getWindowPosition(NativeWindow* window) {
    int x = 0;
    int y = 0;
    Window child = None;
    XTranslateCoordinates(backend->display, window->handle, backend->root,
                          0, 0, &x, &y, &child);
    return ImVec2(static_cast<float>(x), static_cast<float>(y));
}

bool isWindowMinimized(NativeWindow* window) {
    XWindowAttributes attributes{};
    if (!XGetWindowAttributes(backend->display, window->handle, &attributes)) return true;
    return attributes.map_state != IsViewable;
}

void setSwapInterval(NativeWindow* window, int interval) {
    if (backend->swapIntervalExt) {
        backend->swapIntervalExt(backend->display, window->handle, interval);
        return;
    }

    const GLXDrawable oldDrawable = glXGetCurrentDrawable();
    GLXContext oldContext = glXGetCurrentContext();
    const bool changeContext = oldDrawable != window->handle || oldContext != window->context;
    if (changeContext && !glXMakeCurrent(backend->display, window->handle, window->context))
        return;
    if (backend->swapIntervalMesa) {
        backend->swapIntervalMesa(static_cast<unsigned int>(std::max(interval, 0)));
    } else if (backend->swapIntervalSgi && interval > 0) {
        backend->swapIntervalSgi(interval);
    }
    if (changeContext) glXMakeCurrent(backend->display, oldDrawable, oldContext);
}

void updateMonitors() {
    ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
    platformIo.Monitors.resize(0);

    XRRScreenResources* resources = XRRGetScreenResourcesCurrent(backend->display, backend->root);
    if (resources) {
        const RROutput primary = XRRGetOutputPrimary(backend->display, backend->root);
        bool refreshRateSet = false;
        for (int i = 0; i < resources->noutput; ++i) {
            XRROutputInfo* output = XRRGetOutputInfo(backend->display, resources, resources->outputs[i]);
            if (!output) continue;
            if (output->connection == RR_Connected && output->crtc != None) {
                XRRCrtcInfo* crtc = XRRGetCrtcInfo(backend->display, resources, output->crtc);
                if (crtc && crtc->width > 0 && crtc->height > 0) {
                    ImGuiPlatformMonitor monitor;
                    monitor.MainPos = monitor.WorkPos =
                        ImVec2(static_cast<float>(crtc->x), static_cast<float>(crtc->y));
                    monitor.MainSize = monitor.WorkSize =
                        ImVec2(static_cast<float>(crtc->width), static_cast<float>(crtc->height));
                    if (output->mm_width > 0) {
                        const float dpi = static_cast<float>(crtc->width) * 25.4f /
                                          static_cast<float>(output->mm_width);
                        monitor.DpiScale = std::clamp(dpi / 96.0f, 0.5f, 4.0f);
                    }
                    monitor.PlatformHandle = reinterpret_cast<void*>(
                        static_cast<uintptr_t>(resources->outputs[i]));
                    if (resources->outputs[i] == primary)
                        platformIo.Monitors.insert(platformIo.Monitors.begin(), monitor);
                    else
                        platformIo.Monitors.push_back(monitor);

                    if (!refreshRateSet && crtc->mode != None) {
                        for (int modeIndex = 0; modeIndex < resources->nmode; ++modeIndex) {
                            const XRRModeInfo& mode = resources->modes[modeIndex];
                            if (mode.id != crtc->mode || mode.hTotal == 0 || mode.vTotal == 0)
                                continue;
                            double refreshRate = static_cast<double>(mode.dotClock) /
                                (static_cast<double>(mode.hTotal) * mode.vTotal);
                            if (mode.modeFlags & RR_DoubleScan) refreshRate *= 0.5;
                            if (mode.modeFlags & RR_Interlace) refreshRate *= 2.0;
                            if (refreshRate > 1.0) {
                                backend->framePeriod = 1.0 / refreshRate;
                                refreshRateSet = true;
                            }
                            break;
                        }
                    }
                }
                if (crtc) XRRFreeCrtcInfo(crtc);
            }
            XRRFreeOutputInfo(output);
        }
        XRRFreeScreenResources(resources);
    }

    if (platformIo.Monitors.empty()) {
        ImGuiPlatformMonitor monitor;
        monitor.MainPos = monitor.WorkPos = ImVec2(0, 0);
        monitor.MainSize = monitor.WorkSize = ImVec2(
            static_cast<float>(DisplayWidth(backend->display, backend->screen)),
            static_cast<float>(DisplayHeight(backend->display, backend->screen)));
        platformIo.Monitors.push_back(monitor);
    }
}

void updateMouseCursor() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) return;

    const ImGuiMouseCursor cursor = ImGui::GetMouseCursor();
    if (cursor == backend->lastCursor && !io.MouseDrawCursor) return;
    backend->lastCursor = cursor;

    Cursor xcursor = backend->invisibleCursor;
    if (!io.MouseDrawCursor && cursor != ImGuiMouseCursor_None) {
        xcursor = backend->mouseCursors[cursor] != None
            ? backend->mouseCursors[cursor]
            : backend->mouseCursors[ImGuiMouseCursor_Arrow];
    }
    for (const auto& entry : backend->windows)
        XDefineCursor(backend->display, entry.first, xcursor);
}

void updateMouseData() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantSetMousePos && !backend->relativeMouse) {
        int x = static_cast<int>(io.MousePos.x);
        int y = static_cast<int>(io.MousePos.y);
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImVec2 pos = getWindowPosition(backend->mainWindow);
            x -= static_cast<int>(pos.x);
            y -= static_cast<int>(pos.y);
        }
        XWarpPointer(backend->display, None, backend->mainWindow->handle,
                     0, 0, 0, 0, x, y);
    }
}

void writeSelection(XSelectionRequestEvent& request) {
    XSelectionEvent response{};
    response.type = SelectionNotify;
    response.display = request.display;
    response.requestor = request.requestor;
    response.selection = request.selection;
    response.target = request.target;
    response.time = request.time;
    response.property = None;

    Atom property = request.property == None ? request.target : request.property;
    if (request.target == backend->targets) {
        Atom supported[] = {backend->targets, backend->utf8String, backend->text, XA_STRING};
        XChangeProperty(backend->display, request.requestor, property, XA_ATOM, 32,
                        PropModeReplace, reinterpret_cast<unsigned char*>(supported), 4);
        response.property = property;
    } else if (request.target == backend->utf8String || request.target == backend->text ||
               request.target == XA_STRING) {
        const Atom type = request.target == XA_STRING ? XA_STRING : backend->utf8String;
        const long extendedSize = XExtendedMaxRequestSize(backend->display);
        const long requestUnits = extendedSize > 0
            ? extendedSize : XMaxRequestSize(backend->display);
        const size_t maxPayload = static_cast<size_t>(std::max(1024L, requestUnits - 100)) * 4;
        if (backend->clipboardText.size() <= maxPayload) {
            XChangeProperty(backend->display, request.requestor, property, type, 8,
                            PropModeReplace,
                            reinterpret_cast<const unsigned char*>(backend->clipboardText.data()),
                            static_cast<int>(backend->clipboardText.size()));
        } else {
            const unsigned long total = backend->clipboardText.size();
            XChangeProperty(backend->display, request.requestor, property,
                            backend->incr, 32, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(&total), 1);
            XSelectInput(backend->display, request.requestor, PropertyChangeMask);
            backend->outgoingSelections.push_back(
                {request.requestor, property, type, 0, backend->clipboardText});
        }
        response.property = property;
    }
    XSendEvent(backend->display, request.requestor, False, 0,
               reinterpret_cast<XEvent*>(&response));
    XFlush(backend->display);
}

void advanceOutgoingSelection(const XPropertyEvent& event) {
    if (event.state != PropertyDelete) return;
    auto transfer = std::find_if(
        backend->outgoingSelections.begin(), backend->outgoingSelections.end(),
        [&](const OutgoingSelection& value) {
            return value.requestor == event.window && value.property == event.atom;
        });
    if (transfer == backend->outgoingSelections.end()) return;

    const long extendedSize = XExtendedMaxRequestSize(backend->display);
    const long requestUnits = extendedSize > 0
        ? extendedSize : XMaxRequestSize(backend->display);
    const size_t chunkSize = static_cast<size_t>(std::max(1024L, requestUnits - 100)) * 4;
    const size_t remaining = transfer->data.size() - transfer->offset;
    const size_t count = std::min(remaining, chunkSize);
    XChangeProperty(
        backend->display, transfer->requestor, transfer->property,
        transfer->target, 8, PropModeReplace,
        reinterpret_cast<const unsigned char*>(transfer->data.data() + transfer->offset),
        static_cast<int>(count));
    transfer->offset += count;
    if (count == 0) backend->outgoingSelections.erase(transfer);
    XFlush(backend->display);
}

bool readProperty(Window window, Atom property, std::string& result, bool remove) {
    unsigned long offset = 0;
    result.clear();
    for (;;) {
        Atom actualType = None;
        int actualFormat = 0;
        unsigned long count = 0;
        unsigned long remaining = 0;
        unsigned char* data = nullptr;
        int status = XGetWindowProperty(
            backend->display, window, property, static_cast<long>(offset), 65536,
            remove ? True : False, AnyPropertyType, &actualType, &actualFormat,
            &count, &remaining, &data);
        if (status != Success || actualType == None) {
            if (data) XFree(data);
            return false;
        }
        if (actualType == backend->incr) {
            if (data) XFree(data);
            return false;
        }
        const size_t bytes = count * static_cast<unsigned long>(actualFormat / 8);
        if (data && bytes) result.append(reinterpret_cast<char*>(data), bytes);
        if (data) XFree(data);
        if (remaining == 0) return true;
        offset += (bytes + 3) / 4;
    }
}

std::string decodeUri(std::string value) {
    if (value.rfind("file://", 0) == 0) {
        value.erase(0, 7);
        // file://localhost/path → /path (keep the root slash)
        if (value.rfind("localhost/", 0) == 0) value.replace(0, 9, "/");
    }
    std::string result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            auto digit = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int high = digit(value[i + 1]);
            const int low = digit(value[i + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        result.push_back(value[i]);
    }
    return result;
}

std::vector<std::string> parseDroppedUris(const std::string& value) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start < value.size()) {
        const size_t end = value.find_first_of("\r\n", start);
        std::string line = value.substr(start, end == std::string::npos
            ? std::string::npos : end - start);
        if (!line.empty() && line[0] != '#') result.push_back(decodeUri(line));
        if (end == std::string::npos) break;
        start = end + 1;
        while (start < value.size() && (value[start] == '\r' || value[start] == '\n')) ++start;
    }
    return result;
}

void replyXdndStatus(const XClientMessageEvent& message, bool accepted) {
    XEvent response{};
    response.type = ClientMessage;
    response.xclient.display = backend->display;
    response.xclient.window = backend->xdndSource;
    response.xclient.message_type = backend->xdndStatus;
    response.xclient.format = 32;
    response.xclient.data.l[0] = static_cast<long>(message.window);
    response.xclient.data.l[1] = accepted ? 1 : 0;
    response.xclient.data.l[4] = accepted ? static_cast<long>(backend->xdndActionCopy) : None;
    XSendEvent(backend->display, backend->xdndSource, False, NoEventMask, &response);
    XFlush(backend->display);
}

void finishXdnd(bool success) {
    if (backend->xdndSource == None) return;
    XEvent response{};
    response.type = ClientMessage;
    response.xclient.display = backend->display;
    response.xclient.window = backend->xdndSource;
    response.xclient.message_type = backend->xdndFinished;
    response.xclient.format = 32;
    response.xclient.data.l[0] = static_cast<long>(backend->mainWindow->handle);
    response.xclient.data.l[1] = success ? 1 : 0;
    response.xclient.data.l[2] = success ? static_cast<long>(backend->xdndActionCopy) : None;
    XSendEvent(backend->display, backend->xdndSource, False, NoEventMask, &response);
    XFlush(backend->display);
    backend->xdndSource = None;
    backend->xdndTarget = None;
}

void handleXdndEnter(const XClientMessageEvent& message) {
    backend->xdndSource = static_cast<Window>(message.data.l[0]);
    backend->xdndVersion = static_cast<int>((message.data.l[1] >> 24) & 0xff);
    backend->xdndTarget = None;

    std::vector<Atom> offered;
    if (message.data.l[1] & 1) {
        Atom actualType = None;
        int format = 0;
        unsigned long count = 0;
        unsigned long remaining = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(backend->display, backend->xdndSource,
                               backend->xdndTypeList, 0, 1024, False, XA_ATOM,
                               &actualType, &format, &count, &remaining, &data) == Success && data) {
            Atom* values = reinterpret_cast<Atom*>(data);
            offered.assign(values, values + count);
            XFree(data);
        }
    } else {
        for (int i = 2; i <= 4; ++i)
            if (message.data.l[i] != None) offered.push_back(static_cast<Atom>(message.data.l[i]));
    }
    if (std::find(offered.begin(), offered.end(), backend->uriList) != offered.end())
        backend->xdndTarget = backend->uriList;
}

void updateModStateForEvent(ImGuiIO& io, XKeyEvent& keyEvent, KeySym keySym, bool pressed) {
    unsigned int state = keyEvent.state;
    const unsigned int changed = modifierMaskForKey(keySym);
    if (pressed) state |= changed;
    else state &= ~changed;
    addModifiers(io, state);
}

void handleKeyEvent(NativeWindow* window, XKeyEvent& event, bool pressed) {
    ImGuiIO& io = ImGui::GetIO();
    KeySym keySym = XkbKeycodeToKeysym(
        backend->display, static_cast<KeyCode>(event.keycode), 0, 0);
    if (keySym == NoSymbol) keySym = XLookupKeysym(&event, 0);
    updateModStateForEvent(io, event, keySym, pressed);

    const ImGuiKey key = keySymToImGui(keySym);
    if (key != ImGuiKey_None) {
        io.AddKeyEvent(key, pressed);
        io.SetKeyEventNativeData(key, static_cast<int>(keySym), event.keycode, event.keycode);
    }

    if (pressed) {
        char buffer[128];
        KeySym composedSym = NoSymbol;
        Status status = 0;
        int length = window->inputContext
            ? Xutf8LookupString(window->inputContext, &event, buffer,
                                static_cast<int>(sizeof(buffer) - 1), &composedSym, &status)
            : XLookupString(&event, buffer, static_cast<int>(sizeof(buffer) - 1),
                            &composedSym, nullptr);
        if (length > 0 && length < static_cast<int>(sizeof(buffer))) {
            buffer[length] = '\0';
            io.AddInputCharactersUTF8(buffer);
        }
    }
}

void handleClientMessage(NativeWindow* window, XClientMessageEvent& message) {
    if (message.message_type == backend->wmProtocols &&
        static_cast<Atom>(message.data.l[0]) == backend->wmDelete) {
        if (window == backend->mainWindow) {
            editor::Backend::getApp().exit();
        } else if (ImGuiViewport* viewport = findViewport(window)) {
            viewport->PlatformRequestClose = true;
        }
        return;
    }
    if (message.message_type == backend->xdndEnter) {
        handleXdndEnter(message);
    } else if (message.message_type == backend->xdndPosition) {
        replyXdndStatus(message, backend->xdndTarget != None);
    } else if (message.message_type == backend->xdndDrop) {
        if (backend->xdndTarget == None) {
            finishXdnd(false);
        } else {
            const Time timestamp = backend->xdndVersion >= 1
                ? static_cast<Time>(message.data.l[2]) : CurrentTime;
            XConvertSelection(backend->display, backend->xdndSelection,
                              backend->xdndTarget, backend->xdndSelection,
                              backend->mainWindow->handle, timestamp);
        }
    }
}

void processEvent(XEvent& event) {
    if (backend->randrEventBase >= 0 &&
        (event.type == backend->randrEventBase + RRScreenChangeNotify ||
         event.type == backend->randrEventBase + RRNotify)) {
        if (event.type == backend->randrEventBase + RRScreenChangeNotify)
            XRRUpdateConfiguration(&event);
        updateMonitors();
        backend->redrawRequested = true;
        return;
    }
    if (event.type == SelectionRequest) {
        writeSelection(event.xselectionrequest);
        return;
    }
    if (event.type == SelectionClear) return;
    if (event.type == PropertyNotify && event.xproperty.state == PropertyDelete) {
        advanceOutgoingSelection(event.xproperty);
        return;
    }
    if (event.type == SelectionNotify && event.xselection.selection == backend->xdndSelection) {
        std::string data;
        const bool success = event.xselection.property != None &&
            readProperty(event.xselection.requestor, event.xselection.property, data, true);
        if (success) {
            auto paths = parseDroppedUris(data);
            if (!paths.empty()) editor::Backend::getApp().handleExternalDrop(paths);
            finishXdnd(!paths.empty());
        } else {
            finishXdnd(false);
        }
        return;
    }

    if (processNativeMenuEvent(event)) return;

    NativeWindow* window = findWindow(event.xany.window);
    if (!window) return;
    ImGuiIO& io = ImGui::GetIO();

    switch (event.type) {
        case ClientMessage:
            handleClientMessage(window, event.xclient);
            break;
        case ConfigureNotify:
            backend->redrawRequested = true;
            if (window == backend->mainWindow) syncNativeMenuGeometry();
            if (ImGuiViewport* viewport = findViewport(window)) {
                if (window != backend->mainWindow) {
                    const int frame = ImGui::GetFrameCount();
                    const ImVec2 position = getWindowPosition(window);
                    const int x = static_cast<int>(position.x);
                    const int y = static_cast<int>(position.y);
                    const bool moved = x != window->x || y != window->y;
                    const bool resized = event.xconfigure.width != window->width ||
                                         event.xconfigure.height != window->height;
                    window->x = x;
                    window->y = y;
                    window->width = event.xconfigure.width;
                    window->height = event.xconfigure.height;
                    if (moved && frame > window->ignoreMoveFrame + 1)
                        viewport->PlatformRequestMove = true;
                    if (resized && frame > window->ignoreResizeFrame + 1)
                        viewport->PlatformRequestResize = true;
                }
            }
            break;
        case MapNotify:
            backend->redrawRequested = true;
            break;
        case UnmapNotify:
            break;
        case FocusIn:
            window->focused = true;
            if (window->inputContext) XSetICFocus(window->inputContext);
            io.AddFocusEvent(true);
            if (window == backend->mainWindow && !backend->mouseControlSuspended &&
                (backend->gameMouseMode == MouseMode::CAPTURED ||
                 backend->gameMouseMode == MouseMode::CONFINED))
                editor::Backend::setMouseMode(backend->gameMouseMode);
            break;
        case FocusOut:
            window->focused = false;
            if (window->inputContext) XUnsetICFocus(window->inputContext);
            io.AddFocusEvent(false);
            if (window == backend->mainWindow && backend->pointerGrabbed) {
                XUngrabPointer(backend->display, CurrentTime);
                backend->pointerGrabbed = false;
                backend->relativeMouse = false;
            }
            if (window == backend->mainWindow && backend->menu.activeTop >= 0)
                closeNativeMenu();
            break;
        case EnterNotify:
            if (!backend->relativeMouse) {
                float x = static_cast<float>(event.xcrossing.x);
                float y = static_cast<float>(event.xcrossing.y);
                if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                    x = static_cast<float>(event.xcrossing.x_root);
                    y = static_cast<float>(event.xcrossing.y_root);
                }
                io.AddMousePosEvent(x, y);
            }
            break;
        case LeaveNotify:
            if (!backend->relativeMouse && event.xcrossing.mode == NotifyNormal)
                io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            break;
        case MotionNotify: {
            if (backend->relativeMouse && window == backend->mainWindow) {
                int width = 0;
                int height = 0;
                getWindowSize(window, width, height);
                const int centerX = width / 2;
                const int centerY = height / 2;
                // XWarpPointer emits a MotionNotify at the center. Always discard
                // zero-delta center events; multiple queued warps may produce more
                // than one and must not start a warp feedback loop.
                if (event.xmotion.x == centerX && event.xmotion.y == centerY) break;
                backend->virtualMouseX += event.xmotion.x - centerX;
                backend->virtualMouseY += event.xmotion.y - centerY;
                io.AddMousePosEvent(static_cast<float>(backend->virtualMouseX),
                                    static_cast<float>(backend->virtualMouseY));
                XWarpPointer(backend->display, None, window->handle,
                             0, 0, 0, 0, centerX, centerY);
            } else {
                float x = static_cast<float>(event.xmotion.x);
                float y = static_cast<float>(event.xmotion.y);
                if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                    x = static_cast<float>(event.xmotion.x_root);
                    y = static_cast<float>(event.xmotion.y_root);
                }
                io.AddMousePosEvent(x, y);
            }
            break;
        }
        case ButtonPress:
        case ButtonRelease: {
            const bool pressed = event.type == ButtonPress;
            addModifiers(io, event.xbutton.state);
            if (pressed && event.xbutton.button >= Button4 && event.xbutton.button <= 7) {
                float horizontal = 0.0f;
                float vertical = 0.0f;
                if (event.xbutton.button == Button4) vertical = 1.0f;
                if (event.xbutton.button == Button5) vertical = -1.0f;
                if (event.xbutton.button == 6) horizontal = -1.0f;
                if (event.xbutton.button == 7) horizontal = 1.0f;
                io.AddMouseWheelEvent(horizontal, vertical);
            } else {
                const int button = mouseButton(event.xbutton.button);
                if (button >= 0) io.AddMouseButtonEvent(button, pressed);
            }
            break;
        }
        case KeyPress:
            handleKeyEvent(window, event.xkey, true);
            break;
        case KeyRelease:
            handleKeyEvent(window, event.xkey, false);
            break;
        case Expose:
            backend->redrawRequested = true;
            break;
        default:
            break;
    }
}

void drainWakePipe() {
    if (backend->wakePipe[0] < 0) return;
    char buffer[128];
    while (read(backend->wakePipe[0], buffer, sizeof(buffer)) > 0) {}
}

void processEvents(double waitSeconds) {
    if (XPending(backend->display) == 0 && waitSeconds > 0.0) {
        pollfd fds[2] = {
            {ConnectionNumber(backend->display), POLLIN, 0},
            {backend->wakePipe[0], POLLIN, 0}
        };
        poll(fds, backend->wakePipe[0] >= 0 ? 2 : 1,
             static_cast<int>(waitSeconds * 1000.0));
        if (backend->wakePipe[0] >= 0 && (fds[1].revents & POLLIN)) drainWakePipe();
    }
    while (XPending(backend->display) > 0) {
        XEvent event;
        XNextEvent(backend->display, &event);
        if (!XFilterEvent(&event, event.xany.window)) processEvent(event);
    }
}

void setPointerMode(bool relative, bool confined, Cursor cursor) {
    if (backend->pointerGrabbed) {
        XUngrabPointer(backend->display, CurrentTime);
        backend->pointerGrabbed = false;
    }
    backend->relativeMouse = relative;
    if (relative || confined) {
        const int result = XGrabPointer(
            backend->display, backend->mainWindow->handle, True,
            PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
            GrabModeAsync, GrabModeAsync,
            confined ? backend->mainWindow->handle : None,
            cursor, CurrentTime);
        backend->pointerGrabbed = result == GrabSuccess;
    }
    XDefineCursor(backend->display, backend->mainWindow->handle, cursor);
    XFlush(backend->display);
}

void showEditorCursor() {
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = false;
    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    setPointerMode(false, false, backend->mouseCursors[ImGuiMouseCursor_Arrow]);
    backend->lastCursor = ImGuiMouseCursor_COUNT;
    backend->gameCursorHidden = false;
}

void hideEditorCursor() {
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = false;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    setPointerMode(false, false, backend->invisibleCursor);
    backend->gameCursorHidden = true;
}

void confineEditorCursor() {
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = false;
    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    setPointerMode(false, true, backend->mouseCursors[ImGuiMouseCursor_Arrow]);
    backend->lastCursor = ImGuiMouseCursor_COUNT;
    backend->gameCursorHidden = false;
}

void applyHoverVisibility(bool force = false) {
    if (backend->mouseControlSuspended || backend->gameMouseMode != MouseMode::HIDDEN) return;
    const bool hide = backend->gameCursorInSceneRect;
    if (!force && hide == backend->gameCursorHidden) return;
    if (hide) hideEditorCursor(); else showEditorCursor();
}

int gamepadButtonForCode(unsigned short code) {
    if (code == BTN_SOUTH) return 0;
    if (code == BTN_EAST) return 1;
    if (code == BTN_WEST) return 2;
    if (code == BTN_NORTH) return 3;
    if (code == BTN_TL) return 4;
    if (code == BTN_TR) return 5;
    if (code == BTN_SELECT) return 6;
    if (code == BTN_START) return 7;
    if (code == BTN_MODE) return 8;
    if (code == BTN_THUMBL) return 9;
    if (code == BTN_THUMBR) return 10;
    if (code == BTN_DPAD_UP) return 11;
    if (code == BTN_DPAD_RIGHT) return 12;
    if (code == BTN_DPAD_DOWN) return 13;
    if (code == BTN_DPAD_LEFT) return 14;
    return -1;
}

int gamepadAxisForCode(unsigned char code) {
    switch (code) {
        case ABS_X: return 0;
        case ABS_Y: return 1;
        case ABS_RX: return 2;
        case ABS_RY: return 3;
        case ABS_Z: case ABS_BRAKE: return 4;
        case ABS_RZ: case ABS_GAS: return 5;
        default: return -1;
    }
}

void setGamepadButton(int id, Gamepad& gamepad, int button, bool pressed) {
    if (button < 0 || button >= GAMEPAD_BUTTON_COUNT ||
        gamepad.buttons[button] == static_cast<unsigned char>(pressed)) return;
    gamepad.buttons[button] = static_cast<unsigned char>(pressed);
    if (pressed) Engine::systemGamepadButtonDown(id, button);
    else Engine::systemGamepadButtonUp(id, button);
}

void updateHat(int id, Gamepad& gamepad, bool horizontal, int value) {
    if (horizontal) gamepad.hatX = value;
    else gamepad.hatY = value;
    setGamepadButton(id, gamepad, 11, gamepad.hatY < 0);
    setGamepadButton(id, gamepad, 12, gamepad.hatX > 0);
    setGamepadButton(id, gamepad, 13, gamepad.hatY > 0);
    setGamepadButton(id, gamepad, 14, gamepad.hatX < 0);
}

void disconnectGamepad(int id) {
    Gamepad& gamepad = backend->gamepads[id];
    if (!gamepad.connected) return;
    if (gamepad.fd >= 0) close(gamepad.fd);
    gamepad = Gamepad();
    Engine::systemGamepadDisconnect(id);
}

void connectGamepad(int id, const char* path) {
    Gamepad& gamepad = backend->gamepads[id];
    const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    gamepad = Gamepad();
    gamepad.fd = fd;
    gamepad.connected = true;
    gamepad.path = path;
    gamepad.axes[4] = gamepad.axes[5] = -1.0f;
    ioctl(fd, JSIOCGAXMAP, gamepad.axisMap.data());
    ioctl(fd, JSIOCGBTNMAP, gamepad.buttonMap.data());
    char name[128] = "Gamepad";
    if (ioctl(fd, JSIOCGNAME(sizeof(name)), name) >= 0) name[sizeof(name) - 1] = '\0';
    gamepad.name = name;
    Engine::systemGamepadConnect(id, gamepad.name);
}

void scanGamepads(double now) {
    if (now < backend->nextGamepadScan) return;
    backend->nextGamepadScan = now + 1.0;
    glob_t paths{};
    if (glob("/dev/input/js*", 0, nullptr, &paths) == 0) {
        for (size_t i = 0; i < paths.gl_pathc; ++i) {
            const char* path = paths.gl_pathv[i];
            const char* number = path + std::strlen(path);
            while (number > path && number[-1] >= '0' && number[-1] <= '9') --number;
            const int id = std::atoi(number);
            if (id >= 0 && id < GAMEPAD_COUNT && !backend->gamepads[id].connected)
                connectGamepad(id, path);
        }
    }
    globfree(&paths);
}

void updateImGuiGamepad(const Gamepad* gamepad) {
    ImGuiIO& io = ImGui::GetIO();
    if (!(io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad)) return;
    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    if (!gamepad) return;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    auto button = [&](ImGuiKey key, int index) { io.AddKeyEvent(key, gamepad->buttons[index] != 0); };
    auto analog = [&](ImGuiKey key, int axis, float start, float end) {
        float value = (gamepad->axes[axis] - start) / (end - start);
        value = std::clamp(value, 0.0f, 1.0f);
        io.AddKeyAnalogEvent(key, value > 0.1f, value);
    };
    button(ImGuiKey_GamepadStart, 7);
    button(ImGuiKey_GamepadBack, 6);
    button(ImGuiKey_GamepadFaceLeft, 2);
    button(ImGuiKey_GamepadFaceRight, 1);
    button(ImGuiKey_GamepadFaceUp, 3);
    button(ImGuiKey_GamepadFaceDown, 0);
    button(ImGuiKey_GamepadDpadLeft, 14);
    button(ImGuiKey_GamepadDpadRight, 12);
    button(ImGuiKey_GamepadDpadUp, 11);
    button(ImGuiKey_GamepadDpadDown, 13);
    button(ImGuiKey_GamepadL1, 4);
    button(ImGuiKey_GamepadR1, 5);
    analog(ImGuiKey_GamepadL2, 4, -0.75f, 1.0f);
    analog(ImGuiKey_GamepadR2, 5, -0.75f, 1.0f);
    button(ImGuiKey_GamepadL3, 9);
    button(ImGuiKey_GamepadR3, 10);
    analog(ImGuiKey_GamepadLStickLeft, 0, -0.25f, -1.0f);
    analog(ImGuiKey_GamepadLStickRight, 0, 0.25f, 1.0f);
    analog(ImGuiKey_GamepadLStickUp, 1, -0.25f, -1.0f);
    analog(ImGuiKey_GamepadLStickDown, 1, 0.25f, 1.0f);
    analog(ImGuiKey_GamepadRStickLeft, 2, -0.25f, -1.0f);
    analog(ImGuiKey_GamepadRStickRight, 2, 0.25f, 1.0f);
    analog(ImGuiKey_GamepadRStickUp, 3, -0.25f, -1.0f);
    analog(ImGuiKey_GamepadRStickDown, 3, 0.25f, 1.0f);
}

void pollGamepads() {
    scanGamepads(monotonicSeconds());
    const Gamepad* first = nullptr;
    for (int id = 0; id < GAMEPAD_COUNT; ++id) {
        Gamepad& gamepad = backend->gamepads[id];
        if (!gamepad.connected) continue;
        js_event event{};
        for (;;) {
            const ssize_t count = read(gamepad.fd, &event, sizeof(event));
            if (count == static_cast<ssize_t>(sizeof(event))) {
                const unsigned char type = event.type & ~JS_EVENT_INIT;
                if (type == JS_EVENT_BUTTON && event.number < gamepad.buttonMap.size()) {
                    setGamepadButton(id, gamepad,
                                     gamepadButtonForCode(gamepad.buttonMap[event.number]),
                                     event.value != 0);
                } else if (type == JS_EVENT_AXIS && event.number < gamepad.axisMap.size()) {
                    const unsigned char code = gamepad.axisMap[event.number];
                    const int value = event.value < 0 ? -1 : (event.value > 0 ? 1 : 0);
                    if (code == ABS_HAT0X) {
                        updateHat(id, gamepad, true, value);
                    } else if (code == ABS_HAT0Y) {
                        updateHat(id, gamepad, false, value);
                    } else {
                        const int axis = gamepadAxisForCode(code);
                        if (axis >= 0) {
                            const float normalized = event.value < 0
                                ? static_cast<float>(event.value) / 32768.0f
                                : static_cast<float>(event.value) / 32767.0f;
                            if (std::fabs(normalized - gamepad.axes[axis]) > 0.001f) {
                                gamepad.axes[axis] = normalized;
                                Engine::systemGamepadAxisMove(id, axis, normalized);
                            }
                        }
                    }
                }
                continue;
            }
            if (count == 0 || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
                disconnectGamepad(id);
            break;
        }
        if (gamepad.connected && !first) first = &gamepad;
    }
    updateImGuiGamepad(first);
}

const char* getClipboardText(ImGuiContext*) {
    if (XGetSelectionOwner(backend->display, backend->clipboard) == backend->mainWindow->handle)
        return backend->clipboardText.c_str();

    auto readIncremental = [](Window window, Atom property, double deadline) {
        Atom actualType = None;
        int format = 0;
        unsigned long count = 0;
        unsigned long remaining = 0;
        unsigned char* value = nullptr;
        const int status = XGetWindowProperty(
            backend->display, window, property, 0, 1, False, AnyPropertyType,
            &actualType, &format, &count, &remaining, &value);
        if (value) XFree(value);
        if (status != Success || actualType == None) return false;
        if (actualType != backend->incr)
            return readProperty(window, property, backend->clipboardReadBuffer, true);

        backend->clipboardReadBuffer.clear();
        XDeleteProperty(backend->display, window, property);
        XFlush(backend->display);
        while (monotonicSeconds() < deadline) {
            while (XPending(backend->display) > 0) {
                XEvent event;
                XNextEvent(backend->display, &event);
                if (event.type == PropertyNotify &&
                    event.xproperty.window == window &&
                    event.xproperty.atom == property &&
                    event.xproperty.state == PropertyNewValue) {
                    std::string chunk;
                    if (!readProperty(window, property, chunk, true)) return false;
                    if (chunk.empty()) return true;
                    backend->clipboardReadBuffer += chunk;
                } else if (!XFilterEvent(&event, event.xany.window)) {
                    processEvent(event);
                }
            }
            pollfd fd{ConnectionNumber(backend->display), POLLIN, 0};
            poll(&fd, 1, 10);
        }
        return false;
    };

    const Atom targetTypes[] = {backend->utf8String, XA_STRING};
    for (Atom target : targetTypes) {
        XDeleteProperty(backend->display, backend->mainWindow->handle,
                        backend->clipboardProperty);
        XConvertSelection(backend->display, backend->clipboard, target,
                          backend->clipboardProperty,
                          backend->mainWindow->handle, CurrentTime);
        XFlush(backend->display);

        const double deadline = monotonicSeconds() + 0.5;
        bool selectionAnswered = false;
        while (monotonicSeconds() < deadline) {
            while (XPending(backend->display) > 0) {
                XEvent event;
                XNextEvent(backend->display, &event);
                if (event.type == SelectionNotify &&
                    event.xselection.selection == backend->clipboard) {
                    selectionAnswered = true;
                    if (event.xselection.property != None &&
                        readIncremental(event.xselection.requestor,
                                        event.xselection.property, deadline))
                        return backend->clipboardReadBuffer.c_str();
                    break;
                }
                if (!XFilterEvent(&event, event.xany.window)) processEvent(event);
            }
            if (selectionAnswered) break;
            pollfd fd{ConnectionNumber(backend->display), POLLIN, 0};
            poll(&fd, 1, 10);
        }
    }
    backend->clipboardReadBuffer.clear();
    return backend->clipboardReadBuffer.c_str();
}

void setClipboardText(ImGuiContext*, const char* textValue) {
    backend->clipboardText = textValue ? textValue : "";
    XSetSelectionOwner(backend->display, backend->clipboard,
                       backend->mainWindow->handle, CurrentTime);
    XFlush(backend->display);
}

void createViewport(ImGuiViewport* viewport) {
    const bool decorated = !(viewport->Flags & ImGuiViewportFlags_NoDecoration);
    const bool topmost = viewport->Flags & ImGuiViewportFlags_TopMost;
    const bool skipTaskbar = viewport->Flags & ImGuiViewportFlags_NoTaskBarIcon;
    NativeWindow* window = createNativeWindow(
        static_cast<int>(viewport->Pos.x), static_cast<int>(viewport->Pos.y),
        static_cast<int>(viewport->Size.x), static_cast<int>(viewport->Size.y),
        decorated, topmost, skipTaskbar, backend->mainWindow->context);
    IM_ASSERT(window != nullptr);
    viewport->PlatformUserData = window;
    viewport->PlatformHandle = window;
    viewport->PlatformHandleRaw = reinterpret_cast<void*>(static_cast<uintptr_t>(window->handle));
    setWindowTitle(window, "Doriax Engine");
    setSwapInterval(window, 0);
}

void destroyViewport(ImGuiViewport* viewport) {
    auto* window = static_cast<NativeWindow*>(viewport->PlatformUserData);
    if (window && window->owned) destroyNativeWindow(window);
    viewport->PlatformUserData = nullptr;
    viewport->PlatformHandle = nullptr;
    viewport->PlatformHandleRaw = nullptr;
}

void showViewport(ImGuiViewport* viewport) {
    auto* window = static_cast<NativeWindow*>(viewport->PlatformUserData);
    XMapRaised(backend->display, window->handle);
    XFlush(backend->display);
}

ImVec2 getViewportPosition(ImGuiViewport* viewport) {
    return getWindowPosition(static_cast<NativeWindow*>(viewport->PlatformUserData));
}

void setViewportPosition(ImGuiViewport* viewport, ImVec2 position) {
    auto* window = static_cast<NativeWindow*>(viewport->PlatformUserData);
    window->ignoreMoveFrame = ImGui::GetFrameCount();
    window->x = static_cast<int>(position.x);
    window->y = static_cast<int>(position.y);
    XMoveWindow(backend->display, window->handle,
                static_cast<int>(position.x), static_cast<int>(position.y));
}

ImVec2 getViewportSize(ImGuiViewport* viewport) {
    int width = 0;
    int height = 0;
    getWindowSize(static_cast<NativeWindow*>(viewport->PlatformUserData), width, height);
    return ImVec2(static_cast<float>(width), static_cast<float>(height));
}

void setViewportSize(ImGuiViewport* viewport, ImVec2 size) {
    auto* window = static_cast<NativeWindow*>(viewport->PlatformUserData);
    window->ignoreResizeFrame = ImGui::GetFrameCount();
    window->width = static_cast<int>(std::max(1.0f, size.x));
    window->height = static_cast<int>(std::max(1.0f, size.y));
    XResizeWindow(backend->display, window->handle,
                  static_cast<unsigned int>(std::max(1.0f, size.x)),
                  static_cast<unsigned int>(std::max(1.0f, size.y)));
}

void focusViewport(ImGuiViewport* viewport) {
    auto* window = static_cast<NativeWindow*>(viewport->PlatformUserData);
    XRaiseWindow(backend->display, window->handle);
    XSetInputFocus(backend->display, window->handle, RevertToParent, CurrentTime);
}

bool viewportFocused(ImGuiViewport* viewport) {
    return static_cast<NativeWindow*>(viewport->PlatformUserData)->focused;
}

bool viewportMinimized(ImGuiViewport* viewport) {
    return isWindowMinimized(static_cast<NativeWindow*>(viewport->PlatformUserData));
}

void titleViewport(ImGuiViewport* viewport, const char* title) {
    setWindowTitle(static_cast<NativeWindow*>(viewport->PlatformUserData), title);
}

void alphaViewport(ImGuiViewport* viewport, float alpha) {
    const unsigned long opacity = static_cast<unsigned long>(
        std::clamp(alpha, 0.0f, 1.0f) * 4294967295.0f);
    auto* window = static_cast<NativeWindow*>(viewport->PlatformUserData);
    XChangeProperty(backend->display, window->handle, backend->netWmWindowOpacity,
                    XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&opacity), 1);
}

void renderViewport(ImGuiViewport* viewport, void*) {
    auto* window = static_cast<NativeWindow*>(viewport->PlatformUserData);
    glXMakeCurrent(backend->display, window->handle, window->context);
}

void swapViewport(ImGuiViewport* viewport, void*) {
    auto* window = static_cast<NativeWindow*>(viewport->PlatformUserData);
    if (!isWindowMinimized(window)) glXSwapBuffers(backend->display, window->handle);
}

bool initImGuiPlatform() {
    ImGuiIO& io = ImGui::GetIO();
    IM_ASSERT(io.BackendPlatformUserData == nullptr);
    io.BackendPlatformUserData = backend;
    io.BackendPlatformName = "imgui_impl_doriax_x11";
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    io.BackendFlags |= ImGuiBackendFlags_HasSetMousePos;
    io.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports;

    ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
    platformIo.Platform_SetClipboardTextFn = setClipboardText;
    platformIo.Platform_GetClipboardTextFn = getClipboardText;
    platformIo.Platform_CreateWindow = createViewport;
    platformIo.Platform_DestroyWindow = destroyViewport;
    platformIo.Platform_ShowWindow = showViewport;
    platformIo.Platform_SetWindowPos = setViewportPosition;
    platformIo.Platform_GetWindowPos = getViewportPosition;
    platformIo.Platform_SetWindowSize = setViewportSize;
    platformIo.Platform_GetWindowSize = getViewportSize;
    platformIo.Platform_GetWindowFramebufferScale = [](ImGuiViewport*) { return ImVec2(1, 1); };
    platformIo.Platform_SetWindowFocus = focusViewport;
    platformIo.Platform_GetWindowFocus = viewportFocused;
    platformIo.Platform_GetWindowMinimized = viewportMinimized;
    platformIo.Platform_SetWindowTitle = titleViewport;
    platformIo.Platform_SetWindowAlpha = alphaViewport;
    platformIo.Platform_RenderWindow = renderViewport;
    platformIo.Platform_SwapBuffers = swapViewport;

    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    mainViewport->PlatformUserData = backend->mainWindow;
    mainViewport->PlatformHandle = backend->mainWindow;
    mainViewport->PlatformHandleRaw = reinterpret_cast<void*>(
        static_cast<uintptr_t>(backend->mainWindow->handle));
    updateMonitors();
    return true;
}

void shutdownImGuiPlatform() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::DestroyPlatformWindows();
    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    mainViewport->PlatformUserData = nullptr;
    mainViewport->PlatformHandle = nullptr;
    mainViewport->PlatformHandleRaw = nullptr;
    ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
    platformIo.Platform_SetClipboardTextFn = nullptr;
    platformIo.Platform_GetClipboardTextFn = nullptr;
    platformIo.Platform_CreateWindow = nullptr;
    platformIo.Platform_DestroyWindow = nullptr;
    platformIo.Platform_ShowWindow = nullptr;
    platformIo.Platform_SetWindowPos = nullptr;
    platformIo.Platform_GetWindowPos = nullptr;
    platformIo.Platform_SetWindowSize = nullptr;
    platformIo.Platform_GetWindowSize = nullptr;
    platformIo.Platform_GetWindowFramebufferScale = nullptr;
    platformIo.Platform_SetWindowFocus = nullptr;
    platformIo.Platform_GetWindowFocus = nullptr;
    platformIo.Platform_GetWindowMinimized = nullptr;
    platformIo.Platform_SetWindowTitle = nullptr;
    platformIo.Platform_SetWindowAlpha = nullptr;
    platformIo.Platform_RenderWindow = nullptr;
    platformIo.Platform_SwapBuffers = nullptr;
    io.BackendPlatformName = nullptr;
    io.BackendPlatformUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_HasMouseCursors |
                         ImGuiBackendFlags_HasSetMousePos |
                         ImGuiBackendFlags_PlatformHasViewports |
                         ImGuiBackendFlags_HasGamepad);
}

void newImGuiFrame() {
    ImGuiIO& io = ImGui::GetIO();
    int width = 0;
    int height = 0;
    getWindowSize(backend->mainWindow, width, height);
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DisplayFramebufferScale = ImVec2(1, 1);
    const double current = monotonicSeconds();
    io.DeltaTime = backend->time > 0.0
        ? static_cast<float>(current - backend->time) : 1.0f / 60.0f;
    backend->time = current;
    updateMouseData();
    updateMouseCursor();
}

bool initializeX11(int width, int height) {
    XInitThreads();
    backend->display = XOpenDisplay(nullptr);
    if (!backend->display) {
        std::fprintf(stderr, "Error: Could not open the X11 display. "
                     "Set DISPLAY or install/enable XWayland.\n");
        return false;
    }
    backend->screen = DefaultScreen(backend->display);
    backend->root = RootWindow(backend->display, backend->screen);
    int randrErrorBase = 0;
    if (XRRQueryExtension(backend->display, &backend->randrEventBase, &randrErrorBase)) {
        XRRSelectInput(backend->display, backend->root,
                       RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask |
                       RROutputChangeNotifyMask | RROutputPropertyNotifyMask);
    } else {
        backend->randrEventBase = -1;
    }

    const int attributes[] = {
        GLX_X_RENDERABLE, True,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 8,
        GLX_DEPTH_SIZE, 24,
        GLX_STENCIL_SIZE, 8,
        GLX_DOUBLEBUFFER, True,
        None
    };
    int count = 0;
    GLXFBConfig* configs = glXChooseFBConfig(
        backend->display, backend->screen, attributes, &count);
    if (!configs || count == 0) {
        std::fprintf(stderr, "Error: No suitable GLX framebuffer configuration.\n");
        if (configs) XFree(configs);
        return false;
    }
    backend->fbConfig = configs[0];
    backend->visual = glXGetVisualFromFBConfig(backend->display, backend->fbConfig);
    XFree(configs);
    if (!backend->visual) {
        std::fprintf(stderr, "Error: The selected GLX configuration has no X11 visual.\n");
        return false;
    }

    backend->wmDelete = atom("WM_DELETE_WINDOW");
    backend->wmProtocols = atom("WM_PROTOCOLS");
    backend->netWmName = atom("_NET_WM_NAME");
    backend->utf8String = atom("UTF8_STRING");
    backend->netWmState = atom("_NET_WM_STATE");
    backend->netWmStateMaxVert = atom("_NET_WM_STATE_MAXIMIZED_VERT");
    backend->netWmStateMaxHorz = atom("_NET_WM_STATE_MAXIMIZED_HORZ");
    backend->netWmStateAbove = atom("_NET_WM_STATE_ABOVE");
    backend->netWmStateSkipTaskbar = atom("_NET_WM_STATE_SKIP_TASKBAR");
    backend->netWmWindowType = atom("_NET_WM_WINDOW_TYPE");
    backend->netWmWindowTypeDialog = atom("_NET_WM_WINDOW_TYPE_DIALOG");
    backend->netWmWindowOpacity = atom("_NET_WM_WINDOW_OPACITY");
    backend->motifWmHints = atom("_MOTIF_WM_HINTS");
    backend->clipboard = atom("CLIPBOARD");
    backend->targets = atom("TARGETS");
    backend->text = atom("TEXT");
    backend->incr = atom("INCR");
    backend->clipboardProperty = atom("DORIAX_CLIPBOARD");
    backend->xdndAware = atom("XdndAware");
    backend->xdndEnter = atom("XdndEnter");
    backend->xdndPosition = atom("XdndPosition");
    backend->xdndStatus = atom("XdndStatus");
    backend->xdndDrop = atom("XdndDrop");
    backend->xdndFinished = atom("XdndFinished");
    backend->xdndSelection = atom("XdndSelection");
    backend->xdndTypeList = atom("XdndTypeList");
    backend->xdndActionCopy = atom("XdndActionCopy");
    backend->uriList = atom("text/uri-list");

    std::setlocale(LC_CTYPE, "");
    XSetLocaleModifiers("");
    backend->inputMethod = XOpenIM(backend->display, nullptr, nullptr, nullptr);
    Bool detectable = False;
    XkbSetDetectableAutoRepeat(backend->display, True, &detectable);

    const int x = std::max(0, (DisplayWidth(backend->display, backend->screen) - width) / 2);
    const int y = std::max(0, (DisplayHeight(backend->display, backend->screen) - height) / 2);
    backend->mainWindow = createNativeWindow(x, y, width, height, true, false, false, nullptr);
    if (!backend->mainWindow) {
        std::fprintf(stderr, "Error: Could not create the X11 window or OpenGL 4.1 context.\n");
        return false;
    }
    backend->mainWindow->owned = false;
    setWindowTitle(backend->mainWindow, "Doriax Engine");
    XMapWindow(backend->display, backend->mainWindow->handle);
    if (!glXMakeCurrent(backend->display, backend->mainWindow->handle,
                        backend->mainWindow->context)) {
        std::fprintf(stderr, "Error: Could not activate the GLX context.\n");
        return false;
    }
    const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    int glMajor = 0;
    int glMinor = 0;
    if (!glVersion || std::sscanf(glVersion, "%d.%d", &glMajor, &glMinor) != 2 ||
        glMajor < 4 || (glMajor == 4 && glMinor < 1)) {
        std::fprintf(stderr, "Error: OpenGL 4.1 or newer is required (found %s).\n",
                     glVersion ? glVersion : "no active OpenGL context");
        return false;
    }

    const char* glxExtensions = glXQueryExtensionsString(backend->display, backend->screen);
    auto hasGlxExtension = [glxExtensions](const char* name) {
        if (!glxExtensions || !name || std::strchr(name, ' ')) return false;
        const size_t length = std::strlen(name);
        const char* match = glxExtensions;
        while ((match = std::strstr(match, name)) != nullptr) {
            const bool startsToken = match == glxExtensions || match[-1] == ' ';
            const bool endsToken = match[length] == '\0' || match[length] == ' ';
            if (startsToken && endsToken) return true;
            match += length;
        }
        return false;
    };
    if (hasGlxExtension("GLX_EXT_swap_control"))
        backend->swapIntervalExt = reinterpret_cast<LinuxBackendData::SwapIntervalExt>(
            glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXSwapIntervalEXT")));
    if (hasGlxExtension("GLX_MESA_swap_control"))
        backend->swapIntervalMesa = reinterpret_cast<LinuxBackendData::SwapIntervalMesa>(
            glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXSwapIntervalMESA")));
    if (hasGlxExtension("GLX_SGI_swap_control"))
        backend->swapIntervalSgi = reinterpret_cast<LinuxBackendData::SwapIntervalSgi>(
            glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXSwapIntervalSGI")));
    setSwapInterval(backend->mainWindow, 1);

    backend->mouseCursors[ImGuiMouseCursor_Arrow] = XCreateFontCursor(backend->display, XC_left_ptr);
    backend->mouseCursors[ImGuiMouseCursor_TextInput] = XCreateFontCursor(backend->display, XC_xterm);
    backend->mouseCursors[ImGuiMouseCursor_ResizeAll] = XCreateFontCursor(backend->display, XC_fleur);
    backend->mouseCursors[ImGuiMouseCursor_ResizeNS] = XCreateFontCursor(backend->display, XC_sb_v_double_arrow);
    backend->mouseCursors[ImGuiMouseCursor_ResizeEW] = XCreateFontCursor(backend->display, XC_sb_h_double_arrow);
    backend->mouseCursors[ImGuiMouseCursor_ResizeNESW] = XCreateFontCursor(backend->display, XC_bottom_left_corner);
    backend->mouseCursors[ImGuiMouseCursor_ResizeNWSE] = XCreateFontCursor(backend->display, XC_bottom_right_corner);
    backend->mouseCursors[ImGuiMouseCursor_Hand] = XCreateFontCursor(backend->display, XC_hand2);
    backend->mouseCursors[ImGuiMouseCursor_NotAllowed] = XCreateFontCursor(backend->display, XC_pirate);
    const char emptyData[] = {0};
    Pixmap empty = XCreateBitmapFromData(backend->display, backend->root, emptyData, 1, 1);
    XColor black{};
    backend->invisibleCursor = XCreatePixmapCursor(
        backend->display, empty, empty, &black, &black, 0, 0);
    XFreePixmap(backend->display, empty);

    if (pipe2(backend->wakePipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        backend->wakePipe[0] = backend->wakePipe[1] = -1;
    }
    nativeWindowHandle.type = NFD_WINDOW_HANDLE_TYPE_X11;
    nativeWindowHandle.handle = reinterpret_cast<void*>(
        static_cast<uintptr_t>(backend->mainWindow->handle));
    XFlush(backend->display);
    return true;
}

void shutdownX11() {
    if (!backend) return;
    shutdownNativeMenu();
    for (int i = 0; i < GAMEPAD_COUNT; ++i)
        if (backend->gamepads[i].connected) disconnectGamepad(i);
    if (backend->pointerGrabbed) XUngrabPointer(backend->display, CurrentTime);
    for (Cursor cursor : backend->mouseCursors)
        if (cursor != None) XFreeCursor(backend->display, cursor);
    if (backend->invisibleCursor != None) XFreeCursor(backend->display, backend->invisibleCursor);
    if (backend->mainWindow) {
        backend->mainWindow->owned = true;
        destroyNativeWindow(backend->mainWindow);
        backend->mainWindow = nullptr;
    }
    if (backend->inputMethod) XCloseIM(backend->inputMethod);
    if (backend->visual) XFree(backend->visual);
    if (backend->wakePipe[0] >= 0) close(backend->wakePipe[0]);
    if (backend->wakePipe[1] >= 0) close(backend->wakePipe[1]);
    if (backend->display) XCloseDisplay(backend->display);
    delete backend;
    backend = nullptr;
    nativeWindowHandle = {};
}

} // namespace

editor::App editor::Backend::app;
std::string editor::Backend::title;

int editor::Backend::init(int argc, char* argv[]) {
    setEditorHost(&app);
    app.initializeSettings();

    backend = new LinuxBackendData();
    const int initialWidth = app.getInitialWindowWidth();
    const int initialHeight = app.getInitialWindowHeight();
    if (!initializeX11(initialWidth, initialHeight)) {
        shutdownX11();
        return -1;
    }
    if (app.getInitialWindowMaximized()) {
        sendWmState(backend->mainWindow, 1,
                    backend->netWmStateMaxVert, backend->netWmStateMaxHorz);
    }

    if (NFD_Init() != NFD_OKAY) {
        std::fprintf(stderr, "Error: NFD_Init failed: %s\n", NFD_GetError());
        shutdownX11();
        return -1;
    }

    CameraRender render;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    initImGuiPlatform();
    ImGui_ImplOpenGL3_Init("#version 410");

    app.setup();
    app.engineInit(argc, argv);
    // Match GLFW/SDL soft hide at startup: keep the OS cursor manageable by
    // ImGui. hideEditorCursor() sets NoMouseCursorChange and would leave the
    // pointer permanently invisible in NORMAL edit mode.
    XDefineCursor(backend->display, backend->mainWindow->handle,
                  backend->invisibleCursor);
    app.engineViewLoaded();

    Project* activeProject = app.getProject();
    const bool initialFrameSync = !activeProject->isPlaySessionActive() ||
                                  activeProject->isVSyncEnabled();
    int currentSwapInterval = initialFrameSync ? 1 : 0;
    setSwapInterval(backend->mainWindow, currentSwapInterval);

    app.setWakeCallback([]() {
        if (!backend || backend->wakePipe[1] < 0) return;
        const char byte = 1;
        const ssize_t ignored = write(backend->wakePipe[1], &byte, 1);
        (void)ignored;
    });

    double lastActivityTime = monotonicSeconds();
    constexpr double IDLE_ENTER_DELAY = 0.5;
    constexpr double IDLE_WAIT_TIMEOUT = 0.1;

    while (!backend->shouldClose) {
        const double frameStart = monotonicSeconds();
        const bool idleFrame = frameStart - lastActivityTime > IDLE_ENTER_DELAY;
        processEvents(idleFrame ? IDLE_WAIT_TIMEOUT : 0.0);
        pollGamepads();

        bool activityThisFrame = false;
        const bool minimized = isWindowMinimized(backend->mainWindow);
        const bool focused = backend->mainWindow->focused;
        const bool frameSync = !activeProject->isPlaySessionActive() ||
                               activeProject->isVSyncEnabled();
        setMouseControlSuspended(activeProject->isPlaySessionActive() &&
                                 !activeProject->isMainScenePlaying());

        const int desiredInterval = (focused && frameSync) ? 1 : 0;
        if (desiredInterval != currentSwapInterval) {
            setSwapInterval(backend->mainWindow, desiredInterval);
            currentSwapInterval = desiredInterval;
        }

        ImGui_ImplOpenGL3_NewFrame();
        newImGuiFrame();
        ImGui::NewFrame();

        if (!minimized) {
            app.engineRender();
            int width = 0;
            int height = 0;
            getWindowSize(backend->mainWindow, width, height);
            render.setClearColor(Vector4(0.45f, 0.55f, 0.60f, 1.00f));
            render.startRenderPass(width, height);
        } else {
            app.processMainThreadTasks();
        }

        app.show();
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
        if (!minimized) {
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            render.endRenderPass();
        }

        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            const GLXDrawable oldDrawable = glXGetCurrentDrawable();
            GLXContext oldContext = glXGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glXMakeCurrent(backend->display, oldDrawable, oldContext);
        }

        if (!minimized && !(idleFrame && !activityThisFrame))
            glXSwapBuffers(backend->display, backend->mainWindow->handle);

        if (!focused || minimized) {
            const int sleepMs = static_cast<int>(
                (backend->framePeriod - (monotonicSeconds() - frameStart)) * 1000.0);
            if (sleepMs > 0) usleep(static_cast<useconds_t>(sleepMs * 1000));
        }
    }

    app.shutdownBackgroundWork();
    int width = 0;
    int height = 0;
    getWindowSize(backend->mainWindow, width, height);
    Atom actualType = None;
    int format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* states = nullptr;
    bool maximized = false;
    if (XGetWindowProperty(backend->display, backend->mainWindow->handle,
                           backend->netWmState, 0, 64, False, XA_ATOM,
                           &actualType, &format, &count, &remaining, &states) == Success && states) {
        const Atom* values = reinterpret_cast<Atom*>(states);
        bool vertical = false;
        bool horizontal = false;
        for (unsigned long i = 0; i < count; ++i) {
            vertical |= values[i] == backend->netWmStateMaxVert;
            horizontal |= values[i] == backend->netWmStateMaxHorz;
        }
        maximized = vertical && horizontal;
        XFree(states);
    }
    app.saveWindowSettings(width, height, maximized);

    ImGui_ImplOpenGL3_Shutdown();
    shutdownImGuiPlatform();
    ImGui::DestroyContext();
    app.engineViewDestroyed();
    NFD_Quit();
    shutdownX11();
    app.engineShutdown();
    return 0;
}

editor::App& editor::Backend::getApp() {
    return app;
}

void editor::Backend::disableMouseCursor() {
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.MouseDrawCursor = false;
    backend->virtualMouseX = io.MousePos.x;
    backend->virtualMouseY = io.MousePos.y;
    int width = 0;
    int height = 0;
    getWindowSize(backend->mainWindow, width, height);
    XWarpPointer(backend->display, None, backend->mainWindow->handle,
                 0, 0, 0, 0, width / 2, height / 2);
    setPointerMode(true, false, backend->invisibleCursor);
}

void editor::Backend::enableMouseCursor() {
    if (backend->mouseControlSuspended) showEditorCursor();
    else setMouseMode(backend->gameMouseMode);
}

void editor::Backend::setMouseControlSuspended(bool suspended) {
    if (backend->mouseControlSuspended == suspended) return;
    backend->mouseControlSuspended = suspended;
    if (suspended) showEditorCursor();
    else setMouseMode(backend->gameMouseMode);
}

void editor::Backend::setMouseMode(MouseMode mode) {
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
    backend->gameCursorInSceneRect = inSceneRect;
    applyHoverVisibility();
}

void editor::Backend::closeWindow() {
    backend->shouldClose = true;
    if (backend->wakePipe[1] >= 0) {
        const char byte = 1;
        const ssize_t ignored = write(backend->wakePipe[1], &byte, 1);
        (void)ignored;
    }
}

bool editor::Backend::isRunningOnWayland() {
    // This backend deliberately uses X11. On a Wayland desktop it is a native
    // XWayland client, which retains the window positioning needed by ImGui's
    // detachable platform windows.
    return false;
}

float editor::Backend::setMainMenu(const PlatformMenuModel& model,
                                   PlatformMenuCallback callback) {
    if (!backend || !backend->display || !backend->mainWindow ||
        !initializeNativeMenu())
        return 0.0f;

    NativeMenu& menu = backend->menu;
    const bool changed = menu.model.menus != model.menus;
    if (changed) {
        menu.model = model;
        if (menu.activeTop >= static_cast<int>(menu.model.menus.size()))
            closeNativeMenu();
        else if (menu.activeTop >= 0)
            openTopLevelMenu(menu.activeTop);
        else
            drawMenuBar();
    }
    std::vector<PlatformMenuCommand> pendingCommands;
    pendingCommands.swap(menu.pendingCommands);
    for (const PlatformMenuCommand& command : pendingCommands)
        if (callback) callback(command);
    return static_cast<float>(MENU_BAR_HEIGHT);
}

void editor::Backend::updateWindowTitle(const std::string& projectName) {
    title = projectName.empty()
        ? "Empty project - Doriax Engine"
        : projectName + " - Doriax Engine";
    if (backend && backend->mainWindow) setWindowTitle(backend->mainWindow, title.c_str());
}

void* editor::Backend::getNFDWindowHandle() {
    return &nativeWindowHandle;
}
