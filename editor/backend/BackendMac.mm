#include "Backend.h"
#include "EditorHost.h"

#include "DoriaxGameController.h"
#include "Engine.h"

#include "imgui_impl_metal.h"
#include "imgui_impl_osx.h"

#include "nfd.hpp"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace doriax;
using doriax::editor::PlatformMenuCallback;
using doriax::editor::PlatformMenuCommand;
using doriax::editor::PlatformMenuItem;
using doriax::editor::PlatformMenuItemType;
using doriax::editor::PlatformMenuModel;

@class DoriaxEditorApplicationDelegate;
@class DoriaxEditorMenuTarget;
@class DoriaxEditorMetalView;
@class DoriaxEditorWindowDelegate;

namespace {

constexpr double IDLE_ENTER_DELAY = 0.5;
constexpr double IDLE_WAIT_TIMEOUT = 0.1;

struct NativeMenu {
    PlatformMenuModel model;
    std::unordered_map<NSInteger, PlatformMenuCommand> commands;
    std::vector<PlatformMenuCommand> pendingCommands;
    NSInteger nextCommand = 1;
};

struct MacBackendData {
    __strong NSWindow* window = nil;
    __strong MTKView* view = nil;
    __strong DoriaxEditorApplicationDelegate* applicationDelegate = nil;
    __strong DoriaxEditorWindowDelegate* windowDelegate = nil;
    __strong DoriaxEditorMenuTarget* menuTarget = nil;
    __strong id eventMonitor = nil;

    __strong id<MTLDevice> device = nil;
    __strong id<MTLCommandQueue> commandQueue = nil;
    __strong MTLRenderPassDescriptor* frameDescriptor = nil;
    __strong id<MTLTexture> frameColorTexture = nil;
    __strong id<CAMetalDrawable> frameDrawable = nil;
    __strong NSTimer* liveResizeTimer = nil;

    NativeMenu menu;
    bool shouldClose = false;
    bool redrawRequested = true;
    bool applicationActive = true;
    bool cursorCaptured = false;
    bool cursorHidden = false;
    bool mouseControlSuspended = false;
    bool gameCursorInSceneRect = false;
    bool gameCursorHidden = false;
    bool hasSavedCursorPosition = false;
    bool renderingReady = false;
    bool frameInProgress = false;
    bool liveResizeActive = false;
    MouseMode gameMouseMode = MouseMode::NORMAL;
    CGPoint savedCursorPosition{};
    double virtualMouseX = 0.0;
    double virtualMouseY = 0.0;
    double rawMouseX = 0.0;
    double rawMouseY = 0.0;
    double framePeriod = 1.0 / 60.0;
};

MacBackendData* backend = nullptr;
nfdwindowhandle_t nativeWindowHandle{};

double monotonicSeconds() {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(
        Clock::now().time_since_epoch()).count();
}

NSString* stringFromUtf8(const std::string& text) {
    NSString* result = [[NSString alloc]
        initWithBytes:text.data()
               length:text.size()
             encoding:NSUTF8StringEncoding];
    return result ?: @"";
}

void requestRedraw() {
    if (backend) backend->redrawRequested = true;
}

void postWakeEvent() {
    if (!backend) return;
    @autoreleasepool {
        NSEvent* event = [NSEvent
            otherEventWithType:NSEventTypeApplicationDefined
                       location:NSZeroPoint
                  modifierFlags:0
                      timestamp:0
                   windowNumber:0
                        context:nil
                        subtype:0
                          data1:0
                          data2:0];
        [NSApp postEvent:event atStart:NO];
    }
}

void updateFramePeriod() {
    if (!backend || !backend->window) return;
    NSScreen* screen = backend->window.screen ?: NSScreen.mainScreen;
    NSInteger refreshRate = 60;
    if (@available(macOS 12.0, *))
        refreshRate = std::max<NSInteger>(screen.maximumFramesPerSecond, 1);
    backend->framePeriod = 1.0 / static_cast<double>(refreshRate);
}

bool syncDrawableSize() {
    if (!backend || !backend->view || !backend->window) return false;

    const NSSize backingSize = [backend->view
        convertSizeToBacking:backend->view.bounds.size];
    const CGSize desiredSize = CGSizeMake(
        std::max<CGFloat>(std::round(backingSize.width), 1.0),
        std::max<CGFloat>(std::round(backingSize.height), 1.0));
    CAMetalLayer* layer = static_cast<CAMetalLayer*>(backend->view.layer);
    const CGSize currentSize = layer.drawableSize;
    const bool changed = currentSize.width != desiredSize.width ||
        currentSize.height != desiredSize.height;

    layer.contentsScale = backend->window.backingScaleFactor;
    if (changed) layer.drawableSize = desiredSize;
    return changed;
}

CGPoint cocoaPointToQuartz(NSPoint point) {
    NSScreen* primary = NSScreen.screens.firstObject;
    const CGFloat primaryTop = NSMaxY(primary.frame);
    return CGPointMake(point.x, primaryTop - point.y);
}

NSCursor* invisibleCursor() {
    static NSCursor* cursor = nil;
    if (!cursor) {
        NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(1.0, 1.0)];
        [image lockFocus];
        [NSColor.clearColor set];
        NSRectFill(NSMakeRect(0.0, 0.0, 1.0, 1.0));
        [image unlockFocus];
        cursor = [[NSCursor alloc] initWithImage:image hotSpot:NSZeroPoint];
    }
    return cursor;
}

void setSystemCursorHidden(bool hidden) {
    if (!backend || hidden == backend->cursorHidden) return;
    if (hidden) [NSCursor hide];
    else [NSCursor unhide];
    backend->cursorHidden = hidden;
    // AppKit restores the view's cursor whenever it resets cursor rects, which
    // cancels the hide (and with it the mouse/cursor disassociation) while the
    // look drag is running. cursorUpdate: reapplies it; ask for one now.
    if (hidden) [invisibleCursor() set];
    else [[NSCursor arrowCursor] set];
    [backend->window invalidateCursorRectsForView:backend->view];
}

void releaseCapturedCursor(bool restorePosition) {
    if (!backend || !backend->cursorCaptured) return;
    CGAssociateMouseAndMouseCursorPosition(true);
    backend->cursorCaptured = false;
    backend->rawMouseX = backend->rawMouseY = 0.0;
    if (restorePosition && backend->hasSavedCursorPosition)
        CGWarpMouseCursorPosition(backend->savedCursorPosition);
    backend->hasSavedCursorPosition = false;
}

void showEditorCursor() {
    if (!backend) return;
    releaseCapturedCursor(true);
    if (ImGui::GetCurrentContext()) {
        ImGuiIO& io = ImGui::GetIO();
        io.MouseDrawCursor = false;
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    }
    setSystemCursorHidden(false);
    [[NSCursor arrowCursor] set];
    backend->gameCursorHidden = false;
}

void hideEditorCursor() {
    if (!backend) return;
    releaseCapturedCursor(true);
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = false;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    setSystemCursorHidden(true);
    backend->gameCursorHidden = true;
}

void confineEditorCursor() {
    if (!backend) return;
    releaseCapturedCursor(true);
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = false;
    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    setSystemCursorHidden(false);
    [[NSCursor arrowCursor] set];
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

void captureEditorCursor() {
    if (!backend || backend->cursorCaptured || !backend->applicationActive)
        return;
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = false;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    backend->virtualMouseX = std::isfinite(io.MousePos.x)
        ? io.MousePos.x : backend->view.bounds.size.width * 0.5;
    backend->virtualMouseY = std::isfinite(io.MousePos.y)
        ? io.MousePos.y : backend->view.bounds.size.height * 0.5;
    CGEventRef event = CGEventCreate(nullptr);
    if (event) {
        backend->savedCursorPosition = CGEventGetLocation(event);
        backend->hasSavedCursorPosition = true;
        CFRelease(event);
    }
    backend->rawMouseX = backend->rawMouseY = 0.0;
    CGAssociateMouseAndMouseCursorPosition(false);
    backend->cursorCaptured = true;
    setSystemCursorHidden(true);
    backend->gameCursorHidden = true;
}

void applyRelativeMouseData() {
    if (!backend || !backend->cursorCaptured || !backend->applicationActive) {
        if (backend) backend->rawMouseX = backend->rawMouseY = 0.0;
        return;
    }
    backend->virtualMouseX += backend->rawMouseX;
    // Unlike scrollingDeltaY, the deltaY of a move/drag event comes from the
    // CGEvent delta, which points down like ImGui's coordinates. Accumulate it
    // as-is; negating here inverted the look drag against Linux and Windows.
    backend->virtualMouseY += backend->rawMouseY;
    backend->rawMouseX = backend->rawMouseY = 0.0;
    ImGui::GetIO().AddMousePosEvent(
        static_cast<float>(backend->virtualMouseX),
        static_cast<float>(backend->virtualMouseY));
}

void confinePointerToWindow(NSWindow* window) {
    if (!backend || !window || !backend->applicationActive ||
        backend->gameMouseMode != MouseMode::CONFINED ||
        backend->mouseControlSuspended)
        return;
    NSView* view = window.contentView;
    if (!view) return;
    NSRect screenRect = [window convertRectToScreen:view.bounds];
    NSPoint point = NSEvent.mouseLocation;
    NSPoint clamped = NSMakePoint(
        std::clamp(point.x, NSMinX(screenRect), NSMaxX(screenRect) - 1.0),
        std::clamp(point.y, NSMinY(screenRect), NSMaxY(screenRect) - 1.0));
    if (!NSEqualPoints(point, clamped))
        CGWarpMouseCursorPosition(cocoaPointToQuartz(clamped));
}

void handleBackendEvent(NSEvent* event) {
    if (!backend) return;
    switch (event.type) {
        case NSEventTypeMouseMoved:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDragged:
            if (backend->cursorCaptured) {
                backend->rawMouseX += event.deltaX;
                backend->rawMouseY += event.deltaY;
            } else {
                confinePointerToWindow(event.window ?: backend->window);
            }
            requestRedraw();
            break;
        case NSEventTypeLeftMouseDown:
        case NSEventTypeLeftMouseUp:
        case NSEventTypeRightMouseDown:
        case NSEventTypeRightMouseUp:
        case NSEventTypeOtherMouseDown:
        case NSEventTypeOtherMouseUp:
        case NSEventTypeScrollWheel:
        case NSEventTypeKeyDown:
        case NSEventTypeKeyUp:
        case NSEventTypeFlagsChanged:
            requestRedraw();
            break;
        default:
            break;
    }
}

void applicationBecameActive() {
    if (!backend) return;
    backend->applicationActive = true;
    requestRedraw();
    if (!backend->mouseControlSuspended)
        editor::Backend::setMouseMode(backend->gameMouseMode);
}

void applicationResignedActive() {
    if (!backend) return;
    backend->applicationActive = false;
    releaseCapturedCursor(true);
    setSystemCursorHidden(false);
    if (ImGui::GetCurrentContext())
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
}

NSString* functionKeyEquivalent(int number) {
    unichar key = 0;
    switch (number) {
        case 1: key = NSF1FunctionKey; break;
        case 2: key = NSF2FunctionKey; break;
        case 3: key = NSF3FunctionKey; break;
        case 4: key = NSF4FunctionKey; break;
        case 5: key = NSF5FunctionKey; break;
        case 6: key = NSF6FunctionKey; break;
        case 7: key = NSF7FunctionKey; break;
        case 8: key = NSF8FunctionKey; break;
        case 9: key = NSF9FunctionKey; break;
        case 10: key = NSF10FunctionKey; break;
        case 11: key = NSF11FunctionKey; break;
        case 12: key = NSF12FunctionKey; break;
        default: return @"";
    }
    return [NSString stringWithCharacters:&key length:1];
}

#if defined(DORIAX_NATIVE_MENU)

void applyShortcut(NSMenuItem* menuItem, const std::string& shortcut) {
    if (shortcut.empty()) return;
    NSEventModifierFlags modifiers = 0;
    std::string key;
    size_t start = 0;
    while (start <= shortcut.size()) {
        const size_t plus = shortcut.find('+', start);
        const std::string token = shortcut.substr(
            start, plus == std::string::npos ? std::string::npos : plus - start);
        if (token == "Ctrl" || token == "Cmd")
            modifiers |= NSEventModifierFlagCommand;
        else if (token == "Shift")
            modifiers |= NSEventModifierFlagShift;
        else if (token == "Alt" || token == "Option")
            modifiers |= NSEventModifierFlagOption;
        else
            key = token;
        if (plus == std::string::npos) break;
        start = plus + 1;
    }

    NSString* equivalent = @"";
    if (key.size() > 1 && key[0] == 'F') {
        const int number = std::atoi(key.c_str() + 1);
        equivalent = functionKeyEquivalent(number);
    } else if (!key.empty()) {
        equivalent = [stringFromUtf8(key) lowercaseString];
    }
    menuItem.keyEquivalent = equivalent;
    menuItem.keyEquivalentModifierMask = modifiers;
}

bool appendMenuItems(NSMenu* menu,
                     const std::vector<PlatformMenuItem>& items) {
    for (const PlatformMenuItem& item : items) {
        if (item.type == PlatformMenuItemType::Separator) {
            [menu addItem:NSMenuItem.separatorItem];
            continue;
        }

        NSMenuItem* menuItem = [[NSMenuItem alloc]
            initWithTitle:stringFromUtf8(item.label)
                   action:nil
            keyEquivalent:@""];
        menuItem.enabled = item.enabled;
        menuItem.state = item.checked ? NSControlStateValueOn
                                      : NSControlStateValueOff;
        if (item.type == PlatformMenuItemType::Submenu) {
            NSMenu* submenu = [[NSMenu alloc] initWithTitle:menuItem.title];
            submenu.autoenablesItems = NO;
            if (!appendMenuItems(submenu, item.children)) return false;
            menuItem.submenu = submenu;
        } else {
            const NSInteger command = backend->menu.nextCommand++;
            backend->menu.commands.emplace(command, item.command);
            menuItem.tag = command;
            menuItem.target = backend->menuTarget;
            menuItem.action = @selector(performEditorCommand:);
            applyShortcut(menuItem, item.shortcut);
        }
        [menu addItem:menuItem];
    }
    return true;
}

#endif

void appendApplicationMenu(NSMenu* mainMenu) {
    NSString* applicationName = @"Doriax Engine";
    NSMenuItem* root = [[NSMenuItem alloc]
        initWithTitle:applicationName action:nil keyEquivalent:@""];
    NSMenu* menu = [[NSMenu alloc] initWithTitle:applicationName];
    menu.autoenablesItems = NO;

    NSMenuItem* hide = [[NSMenuItem alloc]
        initWithTitle:[@"Hide " stringByAppendingString:applicationName]
               action:@selector(hide:)
        keyEquivalent:@"h"];
    hide.target = NSApp;
    hide.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    [menu addItem:hide];

    NSMenuItem* hideOthers = [[NSMenuItem alloc]
        initWithTitle:@"Hide Others"
               action:@selector(hideOtherApplications:)
        keyEquivalent:@"h"];
    hideOthers.target = NSApp;
    hideOthers.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [menu addItem:hideOthers];

    NSMenuItem* showAll = [[NSMenuItem alloc]
        initWithTitle:@"Show All"
               action:@selector(unhideAllApplications:)
        keyEquivalent:@""];
    showAll.target = NSApp;
    [menu addItem:showAll];
    [menu addItem:NSMenuItem.separatorItem];

    NSMenuItem* quit = [[NSMenuItem alloc]
        initWithTitle:[@"Quit " stringByAppendingString:applicationName]
               action:@selector(requestEditorQuit:)
        keyEquivalent:@"q"];
    quit.target = backend->menuTarget;
    quit.keyEquivalentModifierMask = NSEventModifierFlagCommand;
    [menu addItem:quit];

    root.submenu = menu;
    [mainMenu addItem:root];
}

#if defined(DORIAX_NATIVE_MENU)

bool rebuildNativeMenu(const PlatformMenuModel& model) {
    if (!backend || !backend->menuTarget) return false;
    backend->menu.commands.clear();
    backend->menu.nextCommand = 1;

    NSMenu* mainMenu = [[NSMenu alloc] initWithTitle:@"Main Menu"];
    mainMenu.autoenablesItems = NO;
    appendApplicationMenu(mainMenu);
    for (const PlatformMenuItem& topLevel : model.menus) {
        NSMenuItem* root = [[NSMenuItem alloc]
            initWithTitle:stringFromUtf8(topLevel.label)
                   action:nil
            keyEquivalent:@""];
        root.enabled = topLevel.enabled;
        NSMenu* submenu = [[NSMenu alloc] initWithTitle:root.title];
        submenu.autoenablesItems = NO;
        if (!appendMenuItems(submenu, topLevel.children)) return false;
        root.submenu = submenu;
        [mainMenu addItem:root];
    }
    NSApp.mainMenu = mainMenu;
    backend->menu.model = model;
    requestRedraw();
    return true;
}

#endif

bool initializeFrameDescriptor() {
    MTLTextureDescriptor* colorDescriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                    width:1
                                   height:1
                                mipmapped:NO];
    colorDescriptor.usage = MTLTextureUsageRenderTarget;
    colorDescriptor.storageMode = MTLStorageModePrivate;
    backend->frameColorTexture =
        [backend->device newTextureWithDescriptor:colorDescriptor];

    if (!backend->frameColorTexture) return false;
    backend->frameDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    backend->frameDescriptor.colorAttachments[0].texture =
        backend->frameColorTexture;
    return true;
}

bool acquireMainWindowDrawable() {
    if (!backend || !backend->view) return false;
    if (backend->frameDrawable) return true;

    syncDrawableSize();
    CAMetalLayer* layer = static_cast<CAMetalLayer*>(backend->view.layer);
    backend->frameDrawable = [layer nextDrawable];
    if (!backend->frameDrawable) return false;
    backend->frameDescriptor.colorAttachments[0].texture =
        backend->frameDrawable.texture;
    return true;
}

bool renderMainWindow(ImDrawData* drawData) {
    if (!backend || !backend->view || !backend->commandQueue) return false;
    if (!acquireMainWindowDrawable()) return false;
    MTLRenderPassDescriptor* pass = backend->frameDescriptor;
    id<CAMetalDrawable> drawable = backend->frameDrawable;

    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(
        0.45, 0.55, 0.60, 1.0);
    id<MTLCommandBuffer> commandBuffer =
        [backend->commandQueue commandBuffer];
    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    if (!commandBuffer || !encoder) return false;
    ImGui_ImplMetal_RenderDrawData(drawData, commandBuffer, encoder);
    [encoder endEncoding];
    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];
    // MTKView's currentDrawable is scoped to its delegate-driven draw callback.
    // This backend owns a manual event/render loop, so acquire from CAMetalLayer
    // and release explicitly after each presentation to avoid presenting the
    // same cached MTKView drawable again on the next loop iteration.
    backend->frameDrawable = nil;
    backend->frameDescriptor.colorAttachments[0].texture =
        backend->frameColorTexture;
    return true;
}

bool renderLiveResizeFrame() {
    if (!backend || !backend->renderingReady || backend->frameInProgress ||
        !backend->liveResizeActive)
        return false;

    const bool visible = !backend->window.miniaturized &&
        (backend->window.occlusionState & NSWindowOcclusionStateVisible) != 0;
    if (!visible) return false;

    backend->frameInProgress = true;
    syncDrawableSize();
    if (!acquireMainWindowDrawable()) {
        backend->frameInProgress = false;
        return false;
    }

    ImGui_ImplMetal_NewFrame(backend->frameDescriptor);
    ImGui_ImplOSX_NewFrame(backend->view);
    applyRelativeMouseData();
    ImGui::NewFrame();

    editor::App& app = editor::Backend::getApp();
    app.engineRender();
    app.show();

    ImGui::Render();
    renderMainWindow(ImGui::GetDrawData());
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
    backend->frameInProgress = false;
    return true;
}

void stopLiveResizeTimer() {
    if (!backend) return;
    backend->liveResizeActive = false;
    if (!backend->liveResizeTimer) return;
    [backend->liveResizeTimer invalidate];
    backend->liveResizeTimer = nil;
}

void startLiveResizeTimer() {
    if (!backend) return;
    backend->liveResizeActive = true;
    if (backend->liveResizeTimer) return;
    const NSTimeInterval interval = std::max(backend->framePeriod, 1.0 / 120.0);
    backend->liveResizeTimer = [NSTimer
        timerWithTimeInterval:interval
                      repeats:YES
                        block:^(NSTimer*) {
                            renderLiveResizeFrame();
                        }];
    [[NSRunLoop mainRunLoop] addTimer:backend->liveResizeTimer
                              forMode:NSEventTrackingRunLoopMode];
}

void waitForGpu() {
    if (!backend || !backend->commandQueue) return;
    id<MTLCommandBuffer> commandBuffer =
        [backend->commandQueue commandBuffer];
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];
}

} // namespace

@interface DoriaxEditorApplicationDelegate : NSObject <NSApplicationDelegate>
@end

@implementation DoriaxEditorApplicationDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    if (backend) editor::Backend::getApp().exit();
    return NSTerminateCancel;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return NO;
}

- (void)applicationDidBecomeActive:(NSNotification*)notification {
    (void)notification;
    applicationBecameActive();
}

- (void)applicationDidResignActive:(NSNotification*)notification {
    (void)notification;
    applicationResignedActive();
}
@end

@interface DoriaxEditorWindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation DoriaxEditorWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    if (backend) editor::Backend::getApp().exit();
    return NO;
}

- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    syncDrawableSize();
    requestRedraw();
    renderLiveResizeFrame();
}

- (void)windowWillStartLiveResize:(NSNotification*)notification {
    (void)notification;
    startLiveResizeTimer();
}

- (void)windowDidEndLiveResize:(NSNotification*)notification {
    (void)notification;
    stopLiveResizeTimer();
    syncDrawableSize();
    requestRedraw();
}

- (void)windowDidChangeBackingProperties:(NSNotification*)notification {
    (void)notification;
    syncDrawableSize();
    requestRedraw();
}

- (void)windowDidChangeScreen:(NSNotification*)notification {
    (void)notification;
    updateFramePeriod();
    syncDrawableSize();
    requestRedraw();
}
@end

@interface DoriaxEditorMenuTarget : NSObject
- (void)performEditorCommand:(id)sender;
- (void)requestEditorQuit:(id)sender;
@end

@implementation DoriaxEditorMenuTarget
- (void)performEditorCommand:(id)sender {
    if (!backend || ![sender isKindOfClass:[NSMenuItem class]]) return;
    const NSInteger tag = [(NSMenuItem*)sender tag];
    const auto command = backend->menu.commands.find(tag);
    if (command != backend->menu.commands.end())
        backend->menu.pendingCommands.push_back(command->second);
    requestRedraw();
}

- (void)requestEditorQuit:(id)sender {
    (void)sender;
    if (backend) editor::Backend::getApp().exit();
}
@end

@interface DoriaxEditorMetalView : MTKView <NSDraggingDestination>
@end

@implementation DoriaxEditorMetalView
- (instancetype)initWithFrame:(NSRect)frameRect
                        device:(id<MTLDevice>)device {
    self = [super initWithFrame:frameRect device:device];
    if (self) {
        [self registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
        // NSTrackingEnabledDuringMouseDrag matters here: the fly camera hides the
        // cursor while the right button is held.
        [self addTrackingArea:[[NSTrackingArea alloc]
            initWithRect:NSZeroRect
                 options:NSTrackingCursorUpdate | NSTrackingActiveInKeyWindow |
                         NSTrackingInVisibleRect | NSTrackingEnabledDuringMouseDrag
                   owner:self
                userInfo:nil]];
    }
    return self;
}

- (void)cursorUpdate:(NSEvent*)event {
    if (backend && backend->cursorHidden) [invisibleCursor() set];
    else [super cursorUpdate:event];
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    (void)sender;
    if (backend) editor::Backend::getApp().handleExternalDragEnter();
    requestRedraw();
    return NSDragOperationCopy;
}

- (void)draggingExited:(id<NSDraggingInfo>)sender {
    (void)sender;
    if (backend) editor::Backend::getApp().handleExternalDragLeave();
    requestRedraw();
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSDictionary* options = @{
        NSPasteboardURLReadingFileURLsOnlyKey: @YES
    };
    NSArray<NSURL*>* urls = [sender.draggingPasteboard
        readObjectsForClasses:@[[NSURL class]]
                      options:options];
    std::vector<std::string> paths;
    paths.reserve(urls.count);
    for (NSURL* url in urls) {
        if (url.fileURL && url.path.UTF8String)
            paths.emplace_back(url.path.UTF8String);
    }
    if (backend && !paths.empty())
        editor::Backend::getApp().handleExternalDrop(paths);
    if (backend) editor::Backend::getApp().handleExternalDragLeave();
    requestRedraw();
    return !paths.empty();
}
@end

editor::App editor::Backend::app;
std::string editor::Backend::title;

int editor::Backend::init(int argc, char* argv[]) {
    @autoreleasepool {
        setEditorHost(&app);
        app.initializeSettings();
        backend = new MacBackendData();

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        backend->applicationDelegate =
            [[DoriaxEditorApplicationDelegate alloc] init];
        backend->windowDelegate = [[DoriaxEditorWindowDelegate alloc] init];
        backend->menuTarget = [[DoriaxEditorMenuTarget alloc] init];
        NSApp.delegate = backend->applicationDelegate;

        backend->device = MTLCreateSystemDefaultDevice();
        if (!backend->device) {
            std::fprintf(stderr,
                         "Error: Metal is not supported on this Mac.\n");
            delete backend;
            backend = nullptr;
            return -1;
        }

        const int initialWidth = app.getInitialWindowWidth();
        const int initialHeight = app.getInitialWindowHeight();
        NSRect contentRect = NSMakeRect(
            0, 0, std::max(initialWidth, 1), std::max(initialHeight, 1));
        const NSWindowStyleMask style = NSWindowStyleMaskTitled |
            NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
            NSWindowStyleMaskResizable;
        backend->window = [[NSWindow alloc]
            initWithContentRect:contentRect
                      styleMask:style
                        backing:NSBackingStoreBuffered
                          defer:NO];
        backend->window.title = @"Doriax Engine";
        backend->window.delegate = backend->windowDelegate;
        backend->window.releasedWhenClosed = NO;
        backend->window.tabbingMode = NSWindowTabbingModeDisallowed;
        backend->window.collectionBehavior |=
            NSWindowCollectionBehaviorFullScreenPrimary;
        [backend->window center];

        backend->view = [[DoriaxEditorMetalView alloc]
            initWithFrame:backend->window.contentView.bounds
                   device:backend->device];
        backend->view.autoresizingMask =
            NSViewWidthSizable | NSViewHeightSizable;
        backend->view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        // ImGui doesn't need a depth attachment, and its Metal viewport backend
        // creates color-only layers. Keeping the main pipeline color-only makes
        // that same pipeline valid for detachable windows.
        backend->view.depthStencilPixelFormat = MTLPixelFormatInvalid;
        backend->view.sampleCount = 1;
        backend->view.paused = YES;
        backend->view.enableSetNeedsDisplay = NO;
        backend->view.autoResizeDrawable = YES;
        backend->view.clearColor = MTLClearColorMake(0.45, 0.55, 0.60, 1.0);
        backend->window.contentView = backend->view;
        backend->window.acceptsMouseMovedEvents = YES;

        if (!initializeFrameDescriptor()) {
            std::fprintf(stderr,
                         "Error: Could not create Metal frame resources.\n");
            backend->window = nil;
            delete backend;
            backend = nullptr;
            return -1;
        }

        if (NFD_Init() != NFD_OKAY) {
            std::fprintf(stderr, "Error: NFD_Init failed: %s\n", NFD_GetError());
            backend->window = nil;
            delete backend;
            backend = nullptr;
            return -1;
        }
        nativeWindowHandle.type = NFD_WINDOW_HANDLE_TYPE_COCOA;
        nativeWindowHandle.handle = (__bridge void*)backend->window;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        const bool osxInitialized = ImGui_ImplOSX_Init(backend->view);
        bool metalInitialized = false;
        if (osxInitialized)
            metalInitialized = ImGui_ImplMetal_Init(backend->device);
        if (!metalInitialized) {
            std::fprintf(stderr,
                         "Error: Could not initialize Dear ImGui for macOS.\n");
            if (osxInitialized) ImGui_ImplOSX_Shutdown();
            ImGui::DestroyContext();
            NFD_Quit();
            backend->window = nil;
            delete backend;
            backend = nullptr;
            return -1;
        }

        NSEventMask eventMask = NSEventMaskMouseMoved |
            NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged |
            NSEventMaskOtherMouseDragged | NSEventMaskLeftMouseDown |
            NSEventMaskLeftMouseUp | NSEventMaskRightMouseDown |
            NSEventMaskRightMouseUp | NSEventMaskOtherMouseDown |
            NSEventMaskOtherMouseUp | NSEventMaskScrollWheel |
            NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged;
        backend->eventMonitor = [NSEvent
            addLocalMonitorForEventsMatchingMask:eventMask
                                         handler:^NSEvent*(NSEvent* event) {
                handleBackendEvent(event);
                return event;
            }];

        app.setup();
        app.engineInit(argc, argv);
        app.engineViewLoaded();
        backend->commandQueue = (__bridge id<MTLCommandQueue>)
            sg_mtl_command_queue();
        if (!backend->commandQueue) {
            std::fprintf(stderr,
                         "Error: Could not get Sokol's Metal command queue.\n");
            if (backend->eventMonitor) {
                [NSEvent removeMonitor:backend->eventMonitor];
                backend->eventMonitor = nil;
            }
            app.engineViewDestroyed();
            ImGui_ImplMetal_Shutdown();
            ImGui_ImplOSX_Shutdown();
            ImGui::DestroyContext();
            NFD_Quit();
            backend->window = nil;
            delete backend;
            backend = nullptr;
            app.engineShutdown();
            return -1;
        }
        backend->renderingReady = true;
        [DoriaxGameController start];

        [NSApp finishLaunching];
        [backend->window makeKeyAndOrderFront:nil];
        if (app.getInitialWindowMaximized()) [backend->window zoom:nil];
        [NSApp activateIgnoringOtherApps:YES];
        updateFramePeriod();

        app.setWakeCallback([]() { postWakeEvent(); });
        Project* activeProject = app.getProject();
        double lastActivityTime = monotonicSeconds();
        bool currentFrameSync = true;

        while (!backend->shouldClose) {
            @autoreleasepool {
                const double frameStart = monotonicSeconds();
                const bool idleFrame =
                    frameStart - lastActivityTime > IDLE_ENTER_DELAY;
                NSDate* deadline = idleFrame
                    ? [NSDate dateWithTimeIntervalSinceNow:IDLE_WAIT_TIMEOUT]
                    : NSDate.distantPast;
                NSEvent* event = [NSApp
                    nextEventMatchingMask:NSEventMaskAny
                                untilDate:deadline
                                   inMode:NSDefaultRunLoopMode
                                  dequeue:YES];
                while (event) {
                    [NSApp sendEvent:event];
                    event = [NSApp
                        nextEventMatchingMask:NSEventMaskAny
                                    untilDate:NSDate.distantPast
                                       inMode:NSDefaultRunLoopMode
                                      dequeue:YES];
                }
                [NSApp updateWindows];
                if (backend->shouldClose) break;

                const bool minimized = backend->window.miniaturized;
                const bool visible = !minimized &&
                    (backend->window.occlusionState &
                     NSWindowOcclusionStateVisible) != 0;
                const bool focused = NSApp.active;
                const bool playSessionActive =
                    activeProject->isPlaySessionActive();
                const bool frameSync = !playSessionActive ||
                    activeProject->isVSyncEnabled();
                setMouseControlSuspended(playSessionActive &&
                    !activeProject->isMainScenePlaying());

                const bool desiredFrameSync = focused && frameSync;
                if (desiredFrameSync != currentFrameSync) {
                    CAMetalLayer* layer =
                        static_cast<CAMetalLayer*>(backend->view.layer);
                    layer.displaySyncEnabled = desiredFrameSync;
                    currentFrameSync = desiredFrameSync;
                }

                const bool renderRequested = playSessionActive || !idleFrame ||
                    backend->redrawRequested ||
                    app.hasPendingMainThreadTasks();

                if (visible && renderRequested)
                    acquireMainWindowDrawable();

                backend->frameInProgress = true;
                ImGui_ImplMetal_NewFrame(backend->frameDescriptor);
                ImGui_ImplOSX_NewFrame(backend->view);
                applyRelativeMouseData();
                ImGui::NewFrame();

                if (visible && renderRequested) app.engineRender();
                else app.processMainThreadTasks();
                app.show();

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

                ImGui::Render();
                if (visible && (renderRequested || activity))
                    renderMainWindow(ImGui::GetDrawData());

                if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                    ImGui::UpdatePlatformWindows();
                    if (renderRequested || activity)
                        ImGui::RenderPlatformWindowsDefault();
                }
                backend->frameInProgress = false;

                if (!focused || minimized) {
                    const double remaining = backend->framePeriod -
                        (monotonicSeconds() - frameStart);
                    if (remaining > 0.0)
                        [NSThread sleepForTimeInterval:remaining];
                }
            }
        }

        backend->renderingReady = false;
        stopLiveResizeTimer();
        app.shutdownBackgroundWork();
        const NSSize finalSize = backend->window.contentView.bounds.size;
        app.saveWindowSettings(
            static_cast<int>(std::lround(finalSize.width)),
            static_cast<int>(std::lround(finalSize.height)),
            backend->window.zoomed);

        waitForGpu();
        if (backend->eventMonitor) {
            [NSEvent removeMonitor:backend->eventMonitor];
            backend->eventMonitor = nil;
        }
        showEditorCursor();
        ImGui_ImplMetal_Shutdown();
        ImGui_ImplOSX_Shutdown();
        ImGui::DestroyContext();
        app.engineViewDestroyed();
        backend->commandQueue = nil;
        NFD_Quit();
        [backend->window orderOut:nil];
        backend->window.delegate = nil;
        NSApp.delegate = nil;
        nativeWindowHandle = {};
        delete backend;
        backend = nullptr;
        app.engineShutdown();
        return 0;
    }
}

editor::App& editor::Backend::getApp() {
    return app;
}

void editor::Backend::disableMouseCursor() {
    captureEditorCursor();
}

void editor::Backend::enableMouseCursor() {
    if (!backend) return;
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
    if (backend->mouseControlSuspended || !backend->applicationActive) return;
    switch (mode) {
        case MouseMode::CAPTURED: captureEditorCursor(); break;
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
    postWakeEvent();
}

bool editor::Backend::isRunningOnWayland() {
    return false;
}

float editor::Backend::setMainMenu(const PlatformMenuModel& model,
                                   PlatformMenuCallback callback) {
    if (!backend || !NSApp) return 0.0f;
#if !defined(DORIAX_NATIVE_MENU)
    // Keep the application menu, macOS needs it for Quit and Hide, and let App
    // draw the editor menus with ImGui.
    (void)model;
    (void)callback;
    if (!NSApp.mainMenu) {
        NSMenu* mainMenu = [[NSMenu alloc] initWithTitle:@"Main Menu"];
        mainMenu.autoenablesItems = NO;
        appendApplicationMenu(mainMenu);
        NSApp.mainMenu = mainMenu;
    }
    return 0.0f;
#else
    if (backend->menu.model.menus != model.menus &&
        !rebuildNativeMenu(model))
        return 0.0f;

    std::vector<PlatformMenuCommand> pendingCommands;
    pendingCommands.swap(backend->menu.pendingCommands);
    for (const PlatformMenuCommand& command : pendingCommands)
        if (callback) callback(command);

    // The macOS menu bar is global and consumes no window client area.
    return -1.0f;
#endif
}

ImTextureID editor::Backend::getImGuiTexture(TextureRender* texture) {
    if (!texture || !texture->isCreated()) return ImTextureID{};
    return (ImTextureID)(intptr_t)texture->getMetalHandler();
}

sg_environment editor::Backend::getSokolEnvironment() {
    sg_environment environment{};
    environment.defaults.sample_count = 1;
    environment.defaults.color_format = SG_PIXELFORMAT_BGRA8;
    environment.defaults.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;
    environment.metal.device = (__bridge const void*)backend->device;
    return environment;
}

sg_swapchain editor::Backend::getSokolSwapchain() {
    sg_swapchain swapchain{};
    if (!backend || !backend->view) return swapchain;
    CAMetalLayer* layer = static_cast<CAMetalLayer*>(backend->view.layer);
    const CGSize size = layer.drawableSize;
    swapchain.width = static_cast<int>(size.width);
    swapchain.height = static_cast<int>(size.height);
    swapchain.sample_count = 1;
    swapchain.color_format = SG_PIXELFORMAT_BGRA8;
    swapchain.depth_format = SG_PIXELFORMAT_NONE;
    swapchain.metal.current_drawable =
        (__bridge const void*)backend->frameDrawable;
    swapchain.metal.depth_stencil_texture = nullptr;
    swapchain.metal.msaa_color_texture = nullptr;
    return swapchain;
}

void editor::Backend::updateWindowTitle(const std::string& projectName) {
    title = projectName.empty()
        ? "Empty project - Doriax Engine"
        : projectName + " - Doriax Engine";
    if (backend && backend->window)
        backend->window.title = stringFromUtf8(title);
}

void* editor::Backend::getNFDWindowHandle() {
    return &nativeWindowHandle;
}
