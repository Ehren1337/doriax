#include "Backend.h"
#include "EditorHost.h"
#include "AppSettings.h"

#include <GLFW/glfw3.h>

#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "nfd.hpp"
#include "nfd_glfw3.h"

#include <array>
#include <math.h>

#include "Engine.h"
#include "GamepadMappings.h"

using namespace doriax;

static GLFWwindow* window = nullptr;
static GLFWcursor* invisibleCursor = nullptr;
static nfdwindowhandle_t nativeWindow;
static MouseMode gameMouseMode = MouseMode::NORMAL;
static bool gameCursorInSceneRect = false;
static bool gameCursorHidden = false;
// While true the editor has reclaimed the cursor (play paused or loading).
// gameMouseMode still holds the game's intent and is reapplied when this clears.
// Driven every frame from the main loop.
static bool mouseControlSuspended = false;

editor::App editor::Backend::app;
std::string editor::Backend::title;

static void hideEditorCursor() {
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = false;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    if (invisibleCursor) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        glfwSetCursor(window, invisibleCursor);
    } else {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
    }
    gameCursorHidden = true;
}

static void showEditorCursor() {
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = false;

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    glfwSetCursor(window, nullptr);
    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    gameCursorHidden = false;
}

// CONFINED: keep the cursor visible but trapped inside the editor window
// (GLFW 3.4+). Falls back to a free cursor where capture mode isn't available.
static void confineEditorCursor() {
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = false;
    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    glfwSetCursor(window, nullptr);
#ifdef GLFW_CURSOR_CAPTURED
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_CAPTURED);
#else
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
#endif
    gameCursorHidden = false;
}

// HIDDEN mode hides the cursor only while the pointer is over the game viewport,
// so the surrounding editor UI stays usable. Called every frame with the hover
// state; the other modes don't depend on hover and are applied once by
// setMouseMode, so this is a no-op for them.
static void applyHoverVisibility(bool force = false) {
    if (mouseControlSuspended || gameMouseMode != MouseMode::HIDDEN) {
        return;
    }

    const bool shouldHide = gameCursorInSceneRect;
    if (!force && shouldHide == gameCursorHidden) {
        return;
    }

    if (shouldHide) {
        hideEditorCursor();
    } else {
        showEditorCursor();
    }
}

// Forward gamepad state to the engine so play-mode scripts get gamepad input.
// Always forwarded (unlike keyboard, gamepads don't conflict with ImGui);
// outside play mode nothing is subscribed and Engine::pauseGameEvents covers pause.
static void pollGamepads() {
    struct GamepadState {
        bool connected = false;
        unsigned char buttons[GLFW_GAMEPAD_BUTTON_LAST + 1] = {0};
        float axes[GLFW_GAMEPAD_AXIS_LAST + 1] = {0.0f};
    };
    static GamepadState gamepads[GLFW_JOYSTICK_LAST + 1];

    for (int jid = 0; jid <= GLFW_JOYSTICK_LAST; jid++) {
        GamepadState& state = gamepads[jid];

        GLFWgamepadstate glfwState;
        bool connected = glfwJoystickPresent(jid) && glfwGetGamepadState(jid, &glfwState);

        if (connected && !state.connected) {
            state = GamepadState();
            state.connected = true;
            // triggers rest at -1; seed so a resting trigger doesn't emit a
            // spurious axis move from 0 to -1 on connect
            state.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] = -1.0f;
            state.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] = -1.0f;
            const char* name = glfwGetGamepadName(jid);
            Engine::systemGamepadConnect(jid, name ? name : "Gamepad");
        } else if (!connected && state.connected) {
            state = GamepadState();
            Engine::systemGamepadDisconnect(jid);
        }

        if (!connected)
            continue;

        for (int button = 0; button <= GLFW_GAMEPAD_BUTTON_LAST; button++) {
            if (glfwState.buttons[button] != state.buttons[button]) {
                state.buttons[button] = glfwState.buttons[button];
                if (glfwState.buttons[button] == GLFW_PRESS) {
                    Engine::systemGamepadButtonDown(jid, button);
                } else {
                    Engine::systemGamepadButtonUp(jid, button);
                }
            }
        }

        for (int axis = 0; axis <= GLFW_GAMEPAD_AXIS_LAST; axis++) {
            if (fabsf(glfwState.axes[axis] - state.axes[axis]) > 0.001f) {
                state.axes[axis] = glfwState.axes[axis];
                Engine::systemGamepadAxisMove(jid, axis, glfwState.axes[axis]);
            }
        }
    }
}

int editor::Backend::init(int argc, char* argv[]) {
    setEditorHost(&app);

    // Load settings early so the platform choice below can honor the persisted
    // multi-viewport preference (file I/O only, no window system needed).
    app.initializeSettings();

#ifdef __linux__
    // Dear ImGui multi-viewport needs to reposition OS windows, which the Wayland
    // backend forbids (glfwSetWindowPos is a no-op there). Only when the feature is
    // enabled, prefer X11 (works under XWayland) so dockable panels can be torn off.
    // Falls back to the default backend if X11 isn't available.
    if (AppSettings::getMultiViewportEnabled() && glfwPlatformSupported(GLFW_PLATFORM_X11))
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif

    // Initialize GLFW
    if (!glfwInit())
        return -1;

    // GLFW's built-in gamepad database misses controllers newer than its
    // release; without a mapping glfwGetGamepadState fails and the pad is
    // treated as disconnected. Apply the community database over it.
    for (size_t i = 0; i < DORIAX_GAMEPAD_MAPPINGS_COUNT; i++)
        glfwUpdateGamepadMappings(DORIAX_GAMEPAD_MAPPINGS[i]);

    if (NFD_Init() != NFD_OKAY) {
        printf("Error: NFD_Init failed: %s\n", NFD_GetError());
        return -1;
    }

    CameraRender render;

    int sampleCount = 1;

    glfwWindowHint(GLFW_SAMPLES, (sampleCount == 1) ? 0 : sampleCount);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // Get saved window dimensions from app
    int windowWidth = app.getInitialWindowWidth();
    int windowHeight = app.getInitialWindowHeight();

    // Create a windowed mode window and its OpenGL context
    window = glfwCreateWindow(windowWidth, windowHeight, "Doriax Engine", NULL, NULL);
    if (!window) {
        NFD_Quit();
        glfwTerminate();
        return -1;
    }

    // Apply saved window state
    if (app.getInitialWindowMaximized()) {
        glfwMaximizeWindow(window);
    }

    std::array<unsigned char, 16 * 16 * 4> cursorPixels = {};
    GLFWimage cursorImage;
    cursorImage.width = 16;
    cursorImage.height = 16;
    cursorImage.pixels = cursorPixels.data();
    invisibleCursor = glfwCreateCursor(&cursorImage, 0, 0);

    NFD_GetNativeWindowFromGLFWWindow(window, &nativeWindow);

    // Make the window's context current
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Initial default; project/Wayland policy is applied below.

    // Setup Dear ImGui context - MUST BE DONE BEFORE app.setup()
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    // Setup Platform/Renderer bindings - MUST BE DONE AFTER ImGui::CreateContext()
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 410");

    // Now we can safely call app.setup() which uses ImGui
    app.setup();
    app.engineInit(argc, argv);

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);

    app.engineViewLoaded();

    glfwSetDropCallback(window, [](GLFWwindow* window, int count, const char** paths) {
        std::vector<std::string> droppedPaths;
        for (int i = 0; i < count; i++) {
            droppedPaths.push_back(paths[i]);
        }
        app.handleExternalDrop(droppedPaths);
    });

    // Set window close callback to handle confirmation dialogs
    glfwSetWindowCloseCallback(window, [](GLFWwindow* w) {
        // Don't close window immediately - we'll handle this ourselves
        glfwSetWindowShouldClose(w, GLFW_FALSE);

        // Call the app's exit function which will handle unsaved changes
        app.exit();
    });

    // Main loop
    //
    // On Wayland a vsync'd SwapBuffers waits on wl_surface.frame callbacks, and
    // the compositor stops sending those once the surface is hidden (minimized,
    // fully occluded, or the screen blanks). One blocked swap freezes this whole
    // loop — including the AI agent pump — until the window is visible again,
    // and Wayland gives no iconified signal to dodge it (GLFW_ICONIFIED never
    // becomes true there). Wayland sessions are always composited (no tearing),
    // so use swap interval zero there and manually pace only while the project's
    // VSync setting is on.
    const bool isWayland = glfwGetPlatform() == GLFW_PLATFORM_WAYLAND;
    if (isWayland) {
        glfwSwapInterval(0);
    }
    double framePeriod = 1.0 / 60.0;
    if (GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor()) {
        const GLFWvidmode* videoMode = glfwGetVideoMode(primaryMonitor);
        if (videoMode && videoMode->refreshRate > 0) {
            framePeriod = 1.0 / videoMode->refreshRate;
        }
    }

    Project* activeProject = app.getProject();
    const bool initialFrameSyncEnabled = !activeProject->isPlaySessionActive() || activeProject->isVSyncEnabled();
    const int initialSwapInterval = (!isWayland && initialFrameSyncEnabled) ? 1 : 0;
    if (!isWayland && initialSwapInterval != 1) {
        glfwSwapInterval(initialSwapInterval);
    }
    int currentSwapInterval = initialSwapInterval;

    // When idle, block on events (with a timeout) instead of repainting at the refresh
    // rate. OS input wakes the wait instantly; cross-thread work via glfwPostEmptyEvent.
    app.setWakeCallback([](){ glfwPostEmptyEvent(); });
    double lastActivityTime = glfwGetTime();
    const double IDLE_ENTER_DELAY = 0.5;   // seconds of no activity before idling
    const double IDLE_WAIT_TIMEOUT = 0.1;  // max seconds between idle repaints (~10 fps)

    while (!glfwWindowShouldClose(window)) {
        const double frameStart = glfwGetTime();

        // When idle, wait (up to a timeout) so the loop parks instead of spinning.
        const bool idleFrame = (frameStart - lastActivityTime) > IDLE_ENTER_DELAY;
        if (idleFrame) {
            glfwWaitEventsTimeout(IDLE_WAIT_TIMEOUT);
        } else {
            glfwPollEvents();
        }
        pollGamepads();

        // Set after app.show(). Skips presenting a truly-idle frame: on Wayland every
        // swap requests a frame callback that would wake the wait, pinning it to refresh.
        bool activityThisFrame = false;

        // Skip presenting while iconified (X11 only — Wayland never reports it):
        // SwapBuffers of a hidden window can block and stall clipboard + AI.
        // Keep polling/updating so both keep working.
        const bool iconified = glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0;

        // On X11, with vsync on, SwapBuffers can block waiting for a frame
        // callback the compositor won't send while the window is
        // unfocused/occluded, which stalls the event loop that serves clipboard
        // paste requests. Honor the project's VSync setting during Play mode and
        // keep regular editor use synchronized. Always drop the interval when
        // unfocused so swap never blocks; the sleep below then paces the idle loop.
        // On Wayland, project VSync is implemented by manual pacing (see above).
        const bool focused = glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;
        const bool frameSyncEnabled = !activeProject->isPlaySessionActive() || activeProject->isVSyncEnabled();

        // Hand the cursor back to the editor while a play session isn't actively
        // running (paused or loading) so a game-held cursor lock can't trap the
        // mouse; the game's mouse mode is restored on resume. Window focus is
        // deliberately NOT part of this gate: GLFW releases a disabled cursor on
        // focus loss natively (as an exported game does), and folding focus in here
        // broke capture under multi-viewport, where the main window reports
        // unfocused whenever input is on a viewport panel. No-op outside a session.
        setMouseControlSuspended(activeProject->isPlaySessionActive() &&
                                 !activeProject->isMainScenePlaying());

        if (!isWayland) {
            const int desiredInterval = (focused && frameSyncEnabled) ? 1 : 0;
            if (desiredInterval != currentSwapInterval) {
                glfwSwapInterval(desiredInterval);
                currentSwapInterval = desiredInterval;
            }
        }

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (!iconified) {
            app.engineRender();

            int display_w, display_h;
            glfwGetFramebufferSize(window, &display_w, &display_h);

            render.setClearColor(Vector4(0.45f, 0.55f, 0.60f, 1.00f));
            render.startRenderPass(display_w, display_h);
        } else {
            // engineRender() (skipped while minimized) is what normally drains
            // main-thread tasks. Keep draining them so AI-driven work, logging,
            // and async load callbacks still complete while minimized.
            app.processMainThreadTasks();
        }

        // Always run editor UI/update (AI agent pump, clipboard ownership via
        // continued event processing) even while the OS window is minimized.
        app.show();

        // Activity (input, an active widget, a scene redraw, or queued cross-thread
        // work) keeps the loop at full rate; its absence lets it idle next frame.
        {
            ImGuiIO& io = ImGui::GetIO();

            // The code editor reads keys itself, with no io.WantTextInput or active item
            // to check. Keyboard range only: a resting stick would never let it idle.
            bool typing = io.InputQueueCharacters.Size > 0;
            for (int key = ImGuiKey_Keyboard_BEGIN; !typing && key < ImGuiKey_Keyboard_END; key++) {
                typing = ImGui::IsKeyDown(static_cast<ImGuiKey>(key));
            }

            const bool activity =
                io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f ||
                io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f ||
                ImGui::IsAnyMouseDown() || typing ||
                io.WantTextInput || ImGui::IsAnyItemActive() ||
                app.didRenderScene() || app.hasPendingMainThreadTasks();
            if (activity) {
                lastActivityTime = glfwGetTime();
            }
            activityThisFrame = activity;
        }

        ImGui::Render();
        if (!iconified) {
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            render.endRenderPass();
        }

        // Multi-viewport: platform window maintenance must run every frame
        // (ImGui asserts otherwise), and torn-off panels can still be visible
        // while the main window is iconified. Render them in their own GL
        // contexts (they swap with vsync off and minimized ones are skipped),
        // then restore the main context so sokol's next-frame pass runs on the
        // right one.
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            GLFWwindow* backup_current_context = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup_current_context);
        }

        // Present unless this is an idle frame with nothing new to show (see above).
        if (!iconified && !(idleFrame && !activityThisFrame)) {
            glfwSwapBuffers(window);
        }

        // Pace the loop whenever project VSync must be emulated on Wayland, or
        // while unfocused/iconified to avoid wasting resources. A focused,
        // visible project with VSync off is intentionally left uncapped.
        if ((isWayland && frameSyncEnabled) || !focused || iconified) {
            const int sleepMs = static_cast<int>((framePeriod - (glfwGetTime() - frameStart)) * 1000.0);
            if (sleepMs > 0) {
                ImGui_ImplGlfw_Sleep(sleepMs);
            }
        }
    }

    // Stop the AI worker before tearing GLFW down; nothing else ever joins it.
    app.shutdownBackgroundWork();

    // Save window size and state before closing
    int width, height;
    glfwGetWindowSize(window, &width, &height);
    bool maximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED);

    // Save settings through app
    app.saveWindowSettings(width, height, maximized);

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    app.engineViewDestroyed();

    if (invisibleCursor) {
        glfwDestroyCursor(invisibleCursor);
        invisibleCursor = nullptr;
    }

    glfwDestroyWindow(window);
    NFD_Quit();
    glfwTerminate();

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

    if (invisibleCursor) {
        glfwSetCursor(window, invisibleCursor);
    }

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
}

void editor::Backend::enableMouseCursor() {
    // Editor viewport navigation (fly-camera) released its temporary cursor lock.
    // Restore the cursor the editor should currently show: while a play session has
    // the cursor suspended (paused/loading) the editor owns it, so hand it back
    // rather than re-asserting the game's mode (which setMouseMode would no-op
    // anyway, leaving the cursor stuck locked). Otherwise reapply the game's
    // requested mode (NORMAL while editing).
    if (mouseControlSuspended) {
        showEditorCursor();
    } else {
        setMouseMode(gameMouseMode);
    }
}

void editor::Backend::setMouseControlSuspended(bool suspended) {
    if (mouseControlSuspended == suspended) {
        return;
    }
    mouseControlSuspended = suspended;
    if (suspended) {
        // Hand the cursor back to the editor without forgetting gameMouseMode.
        showEditorCursor();
    } else {
        // Restore whatever the game asked for.
        setMouseMode(gameMouseMode);
    }
}

void editor::Backend::setMouseMode(MouseMode mode) {
    gameMouseMode = mode;
    if (mouseControlSuspended) {
        // Editor owns the cursor right now; just record the intent.
        return;
    }
    switch (mode) {
        case MouseMode::CAPTURED:
            // FPS lock (GLFW_CURSOR_DISABLED). Held for the whole session: the
            // hover policy is disabled for this mode so a drifting virtual cursor
            // can't release it (see applyHoverVisibility).
            disableMouseCursor();
            break;
        case MouseMode::CONFINED:
            confineEditorCursor();
            break;
        case MouseMode::HIDDEN:
            applyHoverVisibility(true);
            break;
        case MouseMode::NORMAL:
            showEditorCursor();
            break;
    }
}

void editor::Backend::setGameCursorInSceneRect(bool inSceneRect) {
    gameCursorInSceneRect = inSceneRect;
    applyHoverVisibility();
}

void editor::Backend::closeWindow() {
    glfwSetWindowShouldClose(window, GLFW_TRUE);
}

bool editor::Backend::isRunningOnWayland() {
    return glfwGetPlatform() == GLFW_PLATFORM_WAYLAND;
}

float editor::Backend::setMainMenu(const PlatformMenuModel&, PlatformMenuCallback) {
    return 0.0f;
}

ImTextureID editor::Backend::getImGuiTexture(TextureRender* texture) {
    return texture ? static_cast<ImTextureID>(texture->getGLHandler()) : ImTextureID{};
}

void editor::Backend::updateWindowTitle(const std::string& projectName) {
    if (projectName.empty()) {
        title = "Empty project - Doriax Engine";
    } else {
        title = projectName + " - Doriax Engine";
    }
    if (window){
        glfwSetWindowTitle(window, title.c_str());
    }
}

void* editor::Backend::getNFDWindowHandle() {
    return &nativeWindow;
}
