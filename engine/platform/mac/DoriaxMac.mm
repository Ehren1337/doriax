//
// (c) 2026 Eduardo Doria.
//

#include "DoriaxMac.h"

#import <Cocoa/Cocoa.h>

#import "MacViewGL.h"
#include "SystemMac.h"
#include "WindowMac.h"

#include "Engine.h"

#import "DoriaxGameController.h"

#ifndef DORIAX_VSYNC_ENABLED
#define DORIAX_VSYNC_ENABLED 1
#endif

// 0 = windowed, 1 = maximized (zoomed), 2 = fullscreen
#ifndef DORIAX_WINDOW_MODE
#define DORIAX_WINDOW_MODE 0
#endif

#ifndef DORIAX_WINDOW_RESIZABLE
#define DORIAX_WINDOW_RESIZABLE 1
#endif

#ifndef DORIAX_WINDOW_TITLE
#define DORIAX_WINDOW_TITLE "Doriax"
#endif

using namespace doriax;

namespace {
    MacViewGL* gameView = nil;
}

@interface DoriaxMacDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property (nonatomic, strong) NSTimer* frameTimer;
@end

@implementation DoriaxMacDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;

    WindowMacConfig config;
    config.title = DORIAX_WINDOW_TITLE;
    config.width = DEFAULT_WINDOW_WIDTH;
    config.height = DEFAULT_WINDOW_HEIGHT;
    config.resizable = DORIAX_WINDOW_RESIZABLE;

    WindowMac::create(config);
    WindowMac::setWindowDelegate((__bridge void*)self);
    WindowMac::setDrawableResizeCallback([](int, int) {
        Engine::systemViewChanged();
    });

    gameView = [[MacViewGL alloc] initWithFrame:NSMakeRect(0, 0, config.width, config.height)];
    if (!gameView || ![gameView pixelFormat]) {
        NSLog(@"Error: no OpenGL 4.1 core pixel format available");
        [NSApp terminate:nil];
        return;
    }

    WindowMac::setContentView((__bridge void*)gameView);
    [gameView makeContextCurrent];
    [gameView setSwapInterval:DORIAX_VSYNC_ENABLED ? 1 : 0];

    Engine::systemViewLoaded();
    Engine::systemViewChanged();

    [DoriaxGameController start];

    WindowMac::show(true);
    WindowMac::applyInitialWindowMode(DORIAX_WINDOW_MODE == 1, DORIAX_WINDOW_MODE == 2);

    // A timer-driven loop keeps the run loop live, so Cocoa still delivers
    // input and window events between frames. VSync paces the actual rate.
    self.frameTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 240.0
                                                      repeats:YES
                                                        block:^(NSTimer*){ [self renderFrame]; }];
    [[NSRunLoop currentRunLoop] addTimer:self.frameTimer forMode:NSRunLoopCommonModes];
}

- (void)renderFrame {
    if (!gameView) return;
    [gameView makeContextCurrent];
    Engine::systemDraw();
    [gameView presentFrame];
}

- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    WindowMac::refreshDrawableSize();
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    (void)notification;
    [self.frameTimer invalidate];
    self.frameTimer = nil;
    Engine::systemViewDestroyed();
    Engine::systemShutdown();
    gameView = nil;
    WindowMac::destroy();
}

@end

int DoriaxMac::init(int argc, char **argv) {
    @autoreleasepool {
        Engine::systemInit(argc, argv, new SystemMac());

        WindowMac::setupApplication();
        WindowMac::installMinimalMenuBar(DORIAX_WINDOW_TITLE);

        DoriaxMacDelegate* delegate = [[DoriaxMacDelegate alloc] init];
        [NSApp setDelegate:delegate];
        [NSApp run];
    }
    return 0;
}
