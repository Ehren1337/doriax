#ifndef EDITORAPP_H
#define EDITORAPP_H

#include "imgui.h"

#include "EditorHost.h"
#include "PlatformMenu.h"
#include "Project.h"

#include "window/Properties.h"
#include "window/Structure.h"
#include "window/OutputWindow.h"
#include "window/SceneWindow.h"
#include "window/ResourcesWindow.h"
#include "window/ImageViewerWindow.h"
#include "window/CodeEditor.h"
#include "window/AnimationWindow.h"
#include "window/TerrainEditWindow.h"
#include "window/AiChatWindow.h"

#include "window/LoadingWindow.h"

#include "window/dialog/ProjectSaveDialog.h"
#include "window/dialog/SceneSaveDialog.h"
#include "window/dialog/ExportWindow.h"
#include "window/dialog/ProjectSettingsWindow.h"

#include "render/SceneRender.h"

#include <mutex>
#include <queue>
#include <thread>

namespace doriax::editor{

    enum class AlertType {
        Info,
        Confirm,
        ThreeButton
    };

    struct AlertData{
        bool needShow = false;
        std::string title;
        std::string message;
        std::string note; // optional highlighted note rendered below the message
        AlertType type = AlertType::Info;
        std::function<void()> onYes = nullptr;
        std::function<void()> onNo = nullptr;
        std::function<void()> onCancel = nullptr;
    };

    enum class SaveDialogType {
        Scene,
        Project
    };

    struct SaveDialogQueueItem {
        SaveDialogType type;
        uint32_t sceneId;  // Only used for Scene dialogs
        std::function<void()> callback = nullptr;
    };

    class App : public EditorHost{
    private:
        Project project;

        std::mutex mainThreadTaskMutex;
        std::queue<std::function<void()>> mainThreadTasks;
        std::thread::id mainThreadId;

        // Whether engineRender() drew a scene this frame, and a wake callback the
        // backend loop uses to decide when it can idle.
        bool renderedSceneThisFrame = false;
        std::function<void()> wakeCallback;

        // Footer stats, sampled only from consecutive drawn frames (see engineRender).
        bool renderedScenePrevFrame = false;
        float footerFramerate = 0.0f;
        float footerDeltaMs = 0.0f;

        ImGuiID dockspace_id;
        ImGuiID dock_id_middle_top;

        static ImFont* codeFont;

        Structure* structureWindow;
        Properties* propertiesWindow;
        OutputWindow* outputWindow;
        SceneWindow* sceneWindow;
        CodeEditor* codeEditor;
        ResourcesWindow* resourcesWindow;
        ImageViewerWindow* imageViewerWindow;
        AnimationWindow* animationWindow;
        TerrainEditWindow* terrainEditWindow;
        AiChatWindow* aiChatWindow;

        LoadingWindow* loadingWindow;

        bool isInitialized;
        bool dockspaceNeedsRebuild;

        // True only during an explicit "Reset Layout": forces tabs back to their
        // default dock slot even when the ini has a saved position for them.
        bool forceDockTabs = false;

        // captureTabOrder() mirrors the live tab order into project.tabs every
        // frame; these debounce persisting that to project.yaml until the user
        // stops dragging (a reorder triggers no save on its own otherwise).
        bool tabsOrderDirty = false;
        double tabsOrderChangeTime = 0.0;

        // Backing buffer for ImGui's io.IniFilename (must outlive ImGui).
        std::string layoutIniPath;

        AlertData alert;
        ProjectSaveDialog projectSaveDialog;
        SceneSaveDialog sceneSaveDialog;
        ExportWindow exportWindow;
        ProjectSettingsWindow projectSettingsWindow;

        std::queue<SaveDialogQueueItem> saveDialogQueue;
        bool saveDialogInProgress = false;
        std::filesystem::path lastResourcesProjectPath;

        std::vector<std::string> droppedExternalPaths;
        bool isDroppedExternalPaths;

        uint32_t lastActivatedScene;
        uint32_t pendingResizeScene = NULL_PROJECT_SCENE;

        enum class LastFocusedWindow {
            None,
            AnySceneWindow, // Represents any of the scene-related windows (Scene, Properties, Structure)
            Resources,
            Code,
            AI
        } lastFocusedWindow;

        void saveFunc();
        void saveAllFunc(std::function<void(bool)> callback = nullptr);
        void saveAllAndProject(std::function<void()> onSuccess);
        void openProjectFunc();

        PlatformMenuModel buildMenuModel();
        void executeMenuCommand(const PlatformMenuCommand& command);
        void showImGuiMenuItems(const std::vector<PlatformMenuItem>& items);
        void showImGuiMenu(const PlatformMenuModel& menu);
        void showMenu();
        void showAlert();
        void showFooter();
        void showStyleEditor();
        void buildDockspace(bool resetLayout = false);
        void buildDefaultLayout();
        void dockProjectTabs();
        void dockTabWindow(const std::string& windowName, bool force = false);
        void captureTabOrder();
        std::string tabWindowName(const TabEntry& tab) const;
        ImGuiID getCentralDockId();
        void applyPanelVisibilitySettings();
        void persistPanelVisibilitySettings();
        void processNextSaveDialog();
        bool popSaveDialogQueueItem();

        void closeWindow();

    public:
        void processMainThreadTasks() override;
        bool isMainThread() const override;

    public:

        App();

        void setup();

        void show();

        void engineInit(int argc, char** argv);
        void engineViewLoaded();
        void engineRender();
        void engineViewDestroyed();
        void engineShutdown();

        void addNewSceneToDock(uint32_t sceneId) override;
        void addNewCodeWindowToDock(fs::path path, bool force = false);
        void addImageViewerWindowToDock(fs::path path, bool force = false);
        void clearSceneWindowState(uint32_t sceneId) override;
        void prepareForProjectSwitch() override;

        void handleExternalDrop(const std::vector<std::string>& paths);
        void handleExternalDragEnter();
        void handleExternalDragLeave();

        void resetLastActivatedScene() override;
        bool shouldSyncEngineApi() const override;
        void updateResourcesPath() override;
        void requestDockspaceRebuild();
        void updateWindowTitle(const std::string& projectName) override;
        void stopTransientPreviews() override;
        void saveAllCodeEditors() override;
        void requestScenePlayFocus(uint32_t sceneId) override;

        void registerAlert(std::string title, std::string message) override;
        void registerAlert(std::string title, std::string message, std::string note); // note is rendered highlighted below the message
        // A file is only referenceable from inside the assets root
        void registerOutsideAssetsAlert(const std::string& path);
        void registerConfirmAlert(std::string title, std::string message, std::function<void()> onYes, std::function<void()> onNo = nullptr) override;
        void registerThreeButtonAlert(std::string title, std::string message, std::function<void()> onYes, std::function<void()> onNo = nullptr, std::function<void()> onCancel = nullptr) override;
        void registerSaveSceneDialog(uint32_t sceneId, std::function<void()> callback = nullptr) override;
        void registerProjectSaveDialog(std::function<void()> callback = nullptr) override;

        // Thread-safe: schedules a task to run on the main/GL thread during the next frame.
        void enqueueMainThreadTask(std::function<void()> task) override;

        bool didRenderScene() const { return renderedSceneThisFrame; }
        bool hasPendingMainThreadTasks();
        // The AI worker gets its own copy: capturing this static App would dangle.
        void setWakeCallback(std::function<void()> cb);

        // Stops background work outliving the main loop; run before backend teardown.
        void shutdownBackgroundWork();

        static std::filesystem::path getUserCacheBaseDir();
        static std::filesystem::path getUserShaderCacheDir();

        // Monospace font used by the code editor
        static ImFont* getCodeFont();

        // ImGui sizes fonts by their line box (ascent-descent) while CSS/VSCode use the em size.
        // JetBrains Mono's line box is (1020 - -300) / 1000upm = 1.32em; multiply a CSS-like
        // font size by this to get the equivalent ImGui font size.
        static constexpr float codeFontEmScale = 1.32f;

        // Tab notification helpers
        static void pushTabNotificationStyle();
        static void popTabNotificationStyle();

        Project* getProject();
        const Project* getProject() const;

        Properties* getPropertiesWindow() const override;
        CodeEditor* getCodeEditor() const override;
        ImageViewerWindow* getImageViewerWindow() const override;
        ResourcesWindow* getResourcesWindow() const;
        AnimationWindow* getAnimationWindow() const;
        TerrainEditWindow* getTerrainEditWindow() const;

        // Window settings methods
        int getInitialWindowWidth() const;
        int getInitialWindowHeight() const;
        bool getInitialWindowMaximized() const;
        void saveWindowSettings(int width, int height, bool maximized);
        void initializeSettings();
        void exit();
    };

}

#endif /* EDITORAPP_H */
