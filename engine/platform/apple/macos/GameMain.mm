//
// (c) 2026 Eduardo Doria.
//
// Programmatic macOS entry point for exported games and for running a project
// outside the editor.
//
// The storyboard path (main.m + AppDelegate + ViewController + Main.storyboard)
// needs a real .app bundle: NSApplicationMain reads NSMainStoryboardFile from
// Info.plist and loads a compiled Main.storyboardc from the bundle resources.
// That only assembles under the Xcode generator. A plain Makefile/Ninja build
// produces a bare executable, where NSApplicationMain finds no storyboard and
// silently never opens a window. This file builds the same window, view and
// renderer in code instead, so macOS builds work under any CMake generator.

#import <Cocoa/Cocoa.h>
#import <MetalKit/MetalKit.h>

#import "EngineView.h"
#import "Renderer.h"

#include "Engine.h"

#ifndef DORIAX_WINDOW_MODE
// 0 = windowed, 1 = maximized (zoomed), 2 = fullscreen
#define DORIAX_WINDOW_MODE 0
#endif

#ifndef DORIAX_WINDOW_RESIZABLE
#define DORIAX_WINDOW_RESIZABLE 1
#endif

#ifndef DORIAX_WINDOW_TITLE
#define DORIAX_WINDOW_TITLE "Doriax"
#endif

@interface DoriaxGameDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic, strong) NSWindow* window;
@property (nonatomic, strong) EngineView* view;
@property (nonatomic, strong) Renderer* renderer;
@end

@implementation DoriaxGameDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    NSRect frame = NSMakeRect(0, 0, DEFAULT_WINDOW_WIDTH, DEFAULT_WINDOW_HEIGHT);

    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                              NSWindowStyleMaskMiniaturizable;
#if DORIAX_WINDOW_RESIZABLE
    style |= NSWindowStyleMaskResizable;
#endif

    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    [self.window setTitle:@DORIAX_WINDOW_TITLE];
    [self.window center];

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        NSLog(@"Metal is not supported on this device");
        [NSApp terminate:nil];
        return;
    }

    self.view = [[EngineView alloc] initWithFrame:frame device:device];
    [self.window setContentView:self.view];

    // Renderer::initWithMetalKitView runs Engine::systemInit and systemViewLoaded
    self.renderer = [[Renderer alloc]
        initWithMetalKitView:self.view
                    withArgs:[[NSProcessInfo processInfo] arguments]];
    [self.renderer mtkView:self.view drawableSizeWillChange:self.view.drawableSize];
    self.view.delegate = self.renderer;

    // Order matters. A process launched from a terminal is not the active app,
    // and an inactive app's window cannot become key -- it would render, but
    // every key press would go to whatever was in front. Activate first, then
    // order the window front, so makeKeyAndOrderFront actually makes it key.
    [NSApp activateIgnoringOtherApps:YES];
    [self.window makeKeyAndOrderFront:nil];

    // EngineView implements NSTextInputClient and accepts first responder; it
    // has to hold it or keyDown never reaches the engine.
    [self.window makeFirstResponder:self.view];

#if DORIAX_WINDOW_MODE == 1
    [self.window zoom:nil];
#elif DORIAX_WINDOW_MODE == 2
    [self.window toggleFullScreen:nil];
#endif
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    [self.renderer destroyView];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return YES;
}

@end

// Without a bundle the process launches as a background agent: it gets no dock
// icon, no menu bar and never becomes active, so the window would open behind
// everything and take no keyboard focus.
static void promoteToForegroundApp(void) {
    ProcessSerialNumber process = {0, kCurrentProcess};
    TransformProcessType(&process, kProcessTransformToForegroundApplication);
}

// A minimal menu bar, purely so Cmd+Q works as users expect
static void installMenuBar(void) {
    NSMenu* menuBar = [[NSMenu alloc] init];
    NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
    [menuBar addItem:appMenuItem];

    NSMenu* appMenu = [[NSMenu alloc] init];
    NSString* quitTitle = [NSString stringWithFormat:@"Quit %@", @DORIAX_WINDOW_TITLE];
    [appMenu addItemWithTitle:quitTitle
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    [appMenuItem setSubmenu:appMenu];

    [NSApp setMainMenu:menuBar];
}

int main(int argc, const char* argv[]) {
    (void)argc;
    (void)argv;
    @autoreleasepool {
        [NSApplication sharedApplication];
        promoteToForegroundApp();
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        installMenuBar();

        DoriaxGameDelegate* delegate = [[DoriaxGameDelegate alloc] init];
        [NSApp setDelegate:delegate];
        [NSApp run];
    }
    return 0;
}
