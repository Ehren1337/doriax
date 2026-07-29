#include "App.h"

#include "imgui_internal.h"
#include "Platform.h"
#include "Backend.h"

#include "external/IconsFontAwesome6.h"
#include "command/CommandHandle.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/DuplicateEntityCmd.h"
#include "command/type/RemoveChildSceneCmd.h"

#include "util/ProjectUtils.h"
#include "util/Util.h"

#include "Out.h"
#include "AppSettings.h"
#include "Theme.h"
#include "resources/fonts/fa-solid-900_ttf.h"
#include "resources/fonts/jetbrains-mono-regular_ttf.h"
//#include "recources/fonts/roboto-v20-latin-regular_ttf.h"
#include "util/DefaultFont.h"

#include "shader/ShaderBuilder.h"
#include "subsystem/MeshSystem.h"

#include <filesystem>
#include <cstdlib>
#include <algorithm>
#include <limits>

#if defined(_WIN32)
  #include <windows.h>
  #include <shlobj.h>
#endif

using namespace doriax;

ImFont* editor::App::codeFont = nullptr;

editor::App::App(){
    mainThreadId = std::this_thread::get_id();
    propertiesWindow = new Properties(&project);
    outputWindow = new OutputWindow();
    sceneWindow = new SceneWindow(&project);
    propertiesWindow->setSceneWindow(sceneWindow);
    structureWindow = new Structure(&project, sceneWindow);
    codeEditor = new CodeEditor(&project);
    resourcesWindow = new ResourcesWindow(&project, codeEditor);
    loadingWindow = new LoadingWindow();
    animationWindow = new AnimationWindow(&project);
    terrainEditWindow = new TerrainEditWindow(&project);
    aiChatWindow = new AiChatWindow(&project, resourcesWindow);

    isInitialized = false;
    dockspaceNeedsRebuild = false;

    lastFocusedWindow = LastFocusedWindow::None;

    Out::setOutputWindow(outputWindow);

    isDroppedExternalPaths = false;

    resetLastActivatedScene();
}

void editor::App::saveFunc(){
    if (lastFocusedWindow == LastFocusedWindow::Code) {
        codeEditor->saveLastFocused();
    }else{
        project.saveLastSelectedScene();
    }
}

void editor::App::saveAllFunc(std::function<void(bool)> callback){
    codeEditor->saveAll();
    project.saveAllScenes(callback);
}

void editor::App::saveAllAndProject(std::function<void()> onSuccess){
    saveAllFunc([this, onSuccess](bool success) {
        if (!success) return;
        project.saveProject(true, onSuccess);
    });
}

void editor::App::openProjectFunc(){
    if (project.isAnyScenePlaying()) {
        registerAlert("Scene Running", "A scene is currently running or stopping. Stop it before opening another project.");
        return;
    }

    if (project.hasScenesUnsavedChanges() || codeEditor->hasUnsavedChanges() || project.isTempUnsavedProject()) {
        Backend::getApp().registerConfirmAlert(
            "Unsaved Changes",
            "There are unsaved changes. Do you want to save them before opening another project?",
            [this]() {
                saveAllAndProject([this]() {
                    this->project.openProject();
                });
            },
            [this]() {
                // No callback - just continue without saving
                project.openProject();
            }
        );
    } else {
        // No unsaved changes, proceed directly
        project.openProject();
    }
}

void editor::App::showMenu(){
    SceneProject* selectedScene = project.getSelectedScene();
    uint32_t selectedSceneId = project.getSelectedSceneId();
    bool hasSelectedScene = selectedScene != nullptr;
    bool isProjectBusy = project.isAnyScenePlaying();
    bool isPlaying = hasSelectedScene && selectedScene->playState == ScenePlayState::PLAYING;
    bool isPaused = hasSelectedScene && selectedScene->playState == ScenePlayState::PAUSED;
    bool isLoading = hasSelectedScene && selectedScene->playState == ScenePlayState::LOADING;
    bool isSaving = hasSelectedScene && selectedScene->playState == ScenePlayState::SAVING;
    bool canRun = hasSelectedScene && !isProjectBusy;
    bool canPause = hasSelectedScene && isPlaying;
    bool canResume = hasSelectedScene && isPaused;
    bool canStop = hasSelectedScene && !isSaving && (isPlaying || isPaused || isLoading);
    bool canRemove = hasSelectedScene && !isProjectBusy && project.getScenes().size() > 1;

    // Remove menu bar border
    //ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    //ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    // Create the main menu bar
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            ImGui::BeginDisabled(isProjectBusy);
            if (ImGui::MenuItem("New Project")) {
                std::string projectName = "MyDoriaxProject";
                auto startFreshProject = [this, projectName]() {
                    if (project.createTempProject(projectName, true)) {
                        aiChatWindow->startNewChat();
                    }
                };
                if (project.hasScenesUnsavedChanges() || codeEditor->hasUnsavedChanges() || project.isTempUnsavedProject()) {
                    Backend::getApp().registerConfirmAlert(
                        "Unsaved Changes",
                        "There are unsaved changes. Do you want to save them before creating a new project?",
                        [this, startFreshProject]() {
                            saveAllAndProject(startFreshProject);
                        },
                        startFreshProject
                    );
                } else {
                    // No unsaved changes, just reset
                    startFreshProject();
                }
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(isProjectBusy);
            if (ImGui::BeginMenu("New Scene")) {
                if (ImGui::MenuItem(ICON_FA_CUBES "  3D Scene")) {
                    project.createNewScene("New Scene", SceneType::SCENE_3D);
                }
                if (ImGui::MenuItem(ICON_FA_CUBES_STACKED "  2D Scene")) {
                    project.createNewScene("New Scene", SceneType::SCENE_2D);
                }
                if (ImGui::MenuItem(ICON_FA_WINDOW_RESTORE "  UI Scene")) {
                    project.createNewScene("New Scene", SceneType::SCENE_UI);
                }
                ImGui::EndMenu();
            }
            ImGui::EndDisabled();

            ImGui::Separator();

            ImGui::BeginDisabled(isProjectBusy);
            if (ImGui::MenuItem("Open Project", "Ctrl+O")) {
                openProjectFunc();
            }
            if (ImGui::BeginMenu("Recent Projects")) {
                std::vector<std::filesystem::path> recentProjects = AppSettings::getRecentProjects();
                if (recentProjects.empty()) {
                    ImGui::MenuItem("No Recent Projects", nullptr, false, false);
                } else {
                    for (const auto& path : recentProjects) {
                        std::string label = path.filename().string() + " (" + path.string() + ")";
                        if (ImGui::MenuItem(label.c_str())) {
                            if (project.hasScenesUnsavedChanges() || codeEditor->hasUnsavedChanges() || project.isTempUnsavedProject()) {
                                registerConfirmAlert(
                                    "Unsaved Changes",
                                    "There are unsaved changes. Do you want to save them before opening another project?",
                                    [this, path]() {
                                        saveAllAndProject([this, path]() {
                                            this->project.loadProject(path);
                                        });
                                    },
                                    [this, path]() {
                                        // No callback - just continue without saving
                                        this->project.loadProject(path);
                                    }
                                );
                            } else {
                                project.loadProject(path);
                            }
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Clear Recent Projects")) {
                        AppSettings::clearRecentProjects();
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndDisabled();

            if (ImGui::MenuItem("Save Project")) {
                project.saveProject(true);
            }
            if (ImGui::MenuItem("Save Project As...")) {
                registerProjectSaveDialog([](){});
            }
            ImGui::Separator();
            bool canSave = false;
            if (lastFocusedWindow == LastFocusedWindow::Code) {
                canSave = codeEditor->hasLastFocusedUnsavedChanges();
            }else{
                canSave = !isProjectBusy && project.hasSelectedSceneUnsavedChanges();
            }
            bool canSaveAll = !isProjectBusy && (project.hasScenesUnsavedChanges() || codeEditor->hasUnsavedChanges());

            ImGui::BeginDisabled(!canSave);
            if (ImGui::MenuItem("Save")) {
                saveFunc();
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!canSaveAll);
            if (ImGui::MenuItem("Save All")) {
                saveAllFunc();
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            if (ImGui::MenuItem("Export Project...")) {
                exportWindow.open(&project);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                exit();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            bool canUndo = false;
            bool canRedo = false;
            if (lastFocusedWindow == LastFocusedWindow::Resources) {
                canUndo = project.getProjectCommandHistory()->canUndo();
                canRedo = project.getProjectCommandHistory()->canRedo();
            } else if (lastFocusedWindow == LastFocusedWindow::Code) {
                canUndo = codeEditor->canUndoLastFocused();
                canRedo = codeEditor->canRedoLastFocused();
            } else if (lastFocusedWindow == LastFocusedWindow::AI) {
                canUndo = false;
                canRedo = false;
            } else {
                canUndo = CommandHandle::get(project.getSelectedSceneId())->canUndo();
                canRedo = CommandHandle::get(project.getSelectedSceneId())->canRedo();
            }

            ImGui::BeginDisabled(!canUndo);
            if (ImGui::MenuItem("Undo")) {
                if (lastFocusedWindow == LastFocusedWindow::Resources) {
                    project.getProjectCommandHistory()->undo();
                    resourcesWindow->refreshCurrentDirectory();
                } else if (lastFocusedWindow == LastFocusedWindow::Code) {
                    codeEditor->undoLastFocused();
                } else if (lastFocusedWindow != LastFocusedWindow::AI) {
                    CommandHandle::get(project.getSelectedSceneId())->undo();
                }
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!canRedo);
            if (ImGui::MenuItem("Redo")) {
                if (lastFocusedWindow == LastFocusedWindow::Resources) {
                    project.getProjectCommandHistory()->redo();
                    resourcesWindow->refreshCurrentDirectory();
                } else if (lastFocusedWindow == LastFocusedWindow::Code) {
                    codeEditor->redoLastFocused();
                } else if (lastFocusedWindow != LastFocusedWindow::AI) {
                    CommandHandle::get(project.getSelectedSceneId())->redo();
                }
            }
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            bool structureOpen = structureWindow->isOpen();
            if (ImGui::MenuItem(Structure::WINDOW_NAME, nullptr, &structureOpen)) {
                structureWindow->setOpen(structureOpen);
            }

            bool propertiesOpen = propertiesWindow->isOpen();
            if (ImGui::MenuItem(Properties::WINDOW_NAME, nullptr, &propertiesOpen)) {
                propertiesWindow->setOpen(propertiesOpen);
            }

            bool resourcesOpen = resourcesWindow->isOpen();
            if (ImGui::MenuItem(ResourcesWindow::WINDOW_NAME, nullptr, &resourcesOpen)) {
                resourcesWindow->setOpen(resourcesOpen);
            }

            bool outputOpen = outputWindow->isOpen();
            if (ImGui::MenuItem(OutputWindow::WINDOW_NAME, nullptr, &outputOpen)) {
                outputWindow->setOpen(outputOpen);
            }

            bool animationOpen = animationWindow->isOpen();
            if (ImGui::MenuItem(AnimationWindow::WINDOW_NAME, nullptr, &animationOpen)) {
                animationWindow->setOpen(animationOpen);
            }

            bool terrainOpen = terrainEditWindow->isOpen();
            if (ImGui::MenuItem(TerrainEditWindow::WINDOW_NAME, nullptr, &terrainOpen)) {
                terrainEditWindow->setOpen(terrainOpen);
            }

            bool aiChatOpen = aiChatWindow->isOpen();
            if (ImGui::MenuItem(AiChatWindow::WINDOW_NAME, nullptr, &aiChatOpen)) {
                aiChatWindow->setOpen(aiChatOpen);
            }

            ImGui::Separator();
            // Reflect the saved preference, not the live imgui flag: on Wayland the
            // flag can't be activated until a restart forces X11, but the menu should
            // still show the user's choice as enabled.
            bool multiViewport = AppSettings::getMultiViewportEnabled();
            if (ImGui::MenuItem("Detachable Windows", nullptr, &multiViewport)) {
                AppSettings::setMultiViewportEnabled(multiViewport);
                AppSettings::saveSettings();

                // Activate the live flag only where extra OS windows actually work.
                // Wayland can't reposition windows, so leave it off until restart.
                if (multiViewport && !Backend::isRunningOnWayland())
                    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
                else
                    ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;

#ifdef __linux__
                if (multiViewport) {
                    if (Backend::isRunningOnWayland())
                        registerAlert("Restart Required",
                            "Detachable windows will take effect after you restart the editor.",
                            "Note: This feature may encounter issues on Linux.");
                    else
                        registerAlert("Experimental Feature",
                            "Detachable windows are now enabled.",
                            "Note: This feature may encounter issues on Linux.");
                }
#endif
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Reset Layout")) {
                structureWindow->setOpen(true);
                propertiesWindow->setOpen(true);
                resourcesWindow->setOpen(true);
                outputWindow->setOpen(true);
                animationWindow->setOpen(true);
                terrainEditWindow->setOpen(false);
                aiChatWindow->setOpen(true);
                buildDockspace(true);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Project")) {
            if (ImGui::MenuItem("Project Settings...")) {
                projectSettingsWindow.open(&project);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear Trash")) {
                project.clearTrash();
            }
            if (ImGui::MenuItem("Clear Shader Cache")) {
                std::filesystem::path cacheDir = getUserShaderCacheDir();
                if (std::filesystem::exists(cacheDir)) {
                    std::filesystem::remove_all(cacheDir);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Scene")) {
            ImGui::BeginDisabled(!canRun);
            if (ImGui::MenuItem("Run", "F5")) {
                project.start(selectedSceneId);
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(!canPause);
            if (ImGui::MenuItem("Pause", "F6")) {
                project.pause(selectedSceneId);
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(!canResume);
            if (ImGui::MenuItem("Resume", "F5")) {
                project.resume(selectedSceneId);
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(!canStop);
            if (ImGui::MenuItem("Stop", "F7")) {
                project.stop(selectedSceneId);
            }
            ImGui::EndDisabled();

            ImGui::Separator();

            ImGui::BeginDisabled(!canRemove);
            if (ImGui::MenuItem("Remove")) {
                project.checkUnsavedAndExecute(selectedSceneId, [this, selectedSceneId]() {
                    project.removeScene(selectedSceneId);
                });
            }
            ImGui::EndDisabled();

            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About Doriax")) {
                registerAlert("About Doriax", "Doriax Engine\n\nVersion: (development)\n\nDeveloped by Eduardo Doria");
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // Restore previous style
    //ImGui::PopStyleVar(2);
}

void editor::App::showFooter(){
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport) {
        return;
    }

    const float fontScale = 0.9f;
    const float footerHeight = (ImGui::GetTextLineHeight() * fontScale) + 10.0f;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                            ImGuiWindowFlags_NoMove |
                            ImGuiWindowFlags_NoDocking |
                            ImGuiWindowFlags_NoSavedSettings |
                            ImGuiWindowFlags_NoNav |
                            ImGuiWindowFlags_NoScrollbar |
                            ImGuiWindowFlags_NoFocusOnAppearing;

    // Position at the bottom of the main viewport
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - footerHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, footerHeight));
    ImGui::SetNextWindowBgAlpha(1.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_Border));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.60f, 0.60f, 1.00f));

    if (ImGui::Begin("##Footer", nullptr, flags)) {
        ImGui::SetWindowFontScale(fontScale);

        const float fps = footerFramerate;
        const float deltaMs = footerDeltaMs;
        const size_t queuedResources = Engine::getQueuedResourceCount();
        SceneProject* selectedScene = project.getSelectedScene();
        const bool hasSelectedScene = selectedScene != nullptr;
        const bool isPlaying = hasSelectedScene && selectedScene->playState == ScenePlayState::PLAYING;
        const bool isPaused = hasSelectedScene && selectedScene->playState == ScenePlayState::PAUSED;
        const bool isSaving = hasSelectedScene && selectedScene->playState == ScenePlayState::SAVING;
        const bool isLoading = hasSelectedScene && selectedScene->playState == ScenePlayState::LOADING;
        const bool isStopped = !hasSelectedScene || selectedScene->playState == ScenePlayState::STOPPED;
        const bool isCancelling = hasSelectedScene && selectedScene->playState == ScenePlayState::CANCELLING;
        const bool isAnySaving = project.isAnySceneSaving();
        const bool canPlayPause = hasSelectedScene && !isSaving && !isLoading && !isCancelling && (isPlaying || isPaused || (isStopped && !project.isAnyScenePlaying()));
        const bool canStop = hasSelectedScene && !isSaving && !isStopped && !isCancelling;
        const ImVec4 footerButtonHovered = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
        const ImVec4 footerButtonActive = ImVec4(1.0f, 1.0f, 1.0f, 0.14f);

        auto footerActionButton = [&](const char* label) -> bool {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, footerButtonHovered);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, footerButtonActive);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

            bool pressed = ImGui::Button(label);

            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(3);

            return pressed;
        };

        // Left side: Status
        bool statusShown = false;
        if (isSaving || isAnySaving) {
            ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), ICON_FA_FLOPPY_DISK " Saving scene");
            statusShown = true;
        } else if (isLoading) {
            ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), ICON_FA_SPINNER " Loading scene");
            statusShown = true;
        } else if (isCancelling) {
            ImGui::TextColored(ImVec4(0.8f, 0.7f, 0.4f, 1.0f), ICON_FA_STOP " Stopping");
            statusShown = true;
        } else if (queuedResources > 0) {
            ImGui::Text(ICON_FA_SPINNER " Loading resources: %zu", queuedResources);
            statusShown = true;
        } else {
             if (selectedScene) {
                if (selectedScene->playState == ScenePlayState::PLAYING){
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), ICON_FA_PLAY " Playing");
                    statusShown = true;
                } else if (selectedScene->playState == ScenePlayState::PAUSED){
                    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f), ICON_FA_PAUSE " Paused");
                    statusShown = true;
                }
             }
        }

        if (!statusShown) {
            ImGui::TextDisabled(ICON_FA_CHECK " Ready");
        }

        // Middle: Scene Info & Selection
        if (selectedScene) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::Text("Scene: %s", selectedScene->name.c_str());

            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();

            size_t selectionCount = selectedScene->selectedEntities.size();
            if (selectionCount == 0) {
                ImGui::TextDisabled("No Selection");
            } else if (selectionCount == 1) {
                ImGui::Text("Selected: 1 Entity");
            } else {
                ImGui::Text("Selected: %zu Entities", selectionCount);
            }

            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();

            ImGui::BeginDisabled(!canPlayPause);
            if (footerActionButton(isPlaying ? ICON_FA_PAUSE " Pause" : (isPaused ? ICON_FA_PLAY " Resume" : ICON_FA_PLAY " Play"))) {
                if (isPlaying) {
                    project.pause(selectedScene->id);
                } else if (isPaused) {
                    project.resume(selectedScene->id);
                } else {
                    project.start(selectedScene->id);
                }
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(!canStop);
            if (footerActionButton(ICON_FA_STOP " Stop")) {
                project.stop(selectedScene->id);
            }
            ImGui::EndDisabled();
        }

        // Right side: Performance stats
        char fpsText[32];
        char msText[32];
        sprintf(fpsText, ICON_FA_GAUGE_HIGH " %.1f FPS", fps);
        sprintf(msText, ICON_FA_CLOCK " %.2f ms", deltaMs);

        // Calculate width to position at right
        float statsWidth = ImGui::CalcTextSize(fpsText).x + ImGui::CalcTextSize(msText).x + 30.0f;
        float statsStartX = ImGui::GetWindowContentRegionMax().x - statsWidth;
        if (statsStartX > ImGui::GetCursorPosX()) {
            ImGui::SameLine(statsStartX);
        } else {
            ImGui::SameLine();
        }

        ImGui::Text("%s", fpsText);
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text("%s", msText);
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();

    // Reserve space from the viewport work area so dockspace doesn't overlap
    viewport->WorkSize.y -= footerHeight;
}

void editor::App::showAlert(){
    if (alert.needShow) {
        ImGui::OpenPopup((alert.title + "##AlertModal").c_str());

        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_Modal;

        if (ImGui::BeginPopupModal((alert.title + "##AlertModal").c_str(), nullptr, flags)) {
            ImGui::Text("%s", alert.message.c_str());
            if (!alert.note.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(Theme::Colors::WarningText, "%s", alert.note.c_str());
            }
            ImGui::Separator();

            if (alert.type == AlertType::Info) {
                // For info alerts, just show OK button
                ImGui::SetCursorPosX((ImGui::GetWindowSize().x - 120) * 0.5f);
                if (ImGui::Button("OK", ImVec2(120, 0))) {
                    alert.needShow = false;
                    ImGui::CloseCurrentPopup();
                }
            } else if (alert.type == AlertType::Confirm) {
                // For confirmation alerts, show Yes and No buttons
                float windowWidth = ImGui::GetWindowSize().x;
                float buttonsWidth = 250; // Total width for both buttons and spacing
                ImGui::SetCursorPosX((windowWidth - buttonsWidth) * 0.5f);

                if (ImGui::Button("Yes", ImVec2(120, 0))) {
                    alert.needShow = false;
                    ImGui::CloseCurrentPopup();
                    if (alert.onYes) {
                        alert.onYes();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("No", ImVec2(120, 0))) {
                    alert.needShow = false;
                    ImGui::CloseCurrentPopup();
                    if (alert.onNo) {
                        alert.onNo();
                    }
                }
            } else if (alert.type == AlertType::ThreeButton) {
                // For three-button alerts, show Yes, No and Cancel buttons
                float windowWidth = ImGui::GetWindowSize().x;
                float buttonsWidth = 370; // Width for three buttons
                ImGui::SetCursorPosX((windowWidth - buttonsWidth) * 0.5f);

                if (ImGui::Button("Yes", ImVec2(120, 0))) {
                    alert.needShow = false;
                    ImGui::CloseCurrentPopup();
                    if (alert.onYes) {
                        alert.onYes();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("No", ImVec2(120, 0))) {
                    alert.needShow = false;
                    ImGui::CloseCurrentPopup();
                    if (alert.onNo) {
                        alert.onNo();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                    alert.needShow = false;
                    ImGui::CloseCurrentPopup();
                    if (alert.onCancel) {
                        alert.onCancel();
                    }
                }
            }
            ImGui::EndPopup();
        }
    }
}

void editor::App::buildDockspace(bool resetLayout){
    // A reset forces every tab back to its default slot even if the ini has a
    // customized position saved for it (see dockTabWindow()).
    forceDockTabs = resetLayout;

    // Keep an existing layout (restored from the persisted ini) so the user's
    // customizations survive; only recover the scene node we dock tabs into.
    // A reset rebuilds the default skeleton from scratch instead.
    if (!resetLayout && ImGui::DockBuilderGetNode(dockspace_id) != nullptr) {
        dock_id_middle_top = getCentralDockId();
    } else {
        buildDefaultLayout();
    }

    dockProjectTabs();

    ImGui::DockBuilderFinish(dockspace_id);

    forceDockTabs = false;
}

void editor::App::buildDefaultLayout(){
    const ImVec2 viewport = ImGui::GetMainViewport()->Size;
    ImGuiID dock_id_left, dock_id_left_top, dock_id_left_bottom, dock_id_right, dock_id_middle, dock_id_middle_bottom;
    // dock_id_middle_top is a member: dockTabWindow() reads it after this runs.

    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, viewport);

    // Structure on the left, Resources split off its bottom.
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.0f, &dock_id_left, &dock_id_middle);
    ImGui::DockBuilderSetNodeSize(dock_id_left, ImVec2(14*ImGui::GetFontSize(), viewport.y));
    ImGui::DockBuilderDockWindow(Structure::WINDOW_NAME, dock_id_left);

    ImGui::DockBuilderSplitNode(dock_id_left, ImGuiDir_Down, 0.0f, &dock_id_left_bottom, &dock_id_left_top);
    ImGui::DockBuilderSetNodeSize(dock_id_left_bottom, ImVec2(viewport.x, 50*ImGui::GetFontSize()));
    ImGui::DockBuilderDockWindow(ResourcesWindow::WINDOW_NAME, dock_id_left_bottom);

    // Properties on the right.
    ImGui::DockBuilderSplitNode(dock_id_middle, ImGuiDir_Right, 0.0f, &dock_id_right, &dock_id_middle);
    ImGui::DockBuilderSetNodeSize(dock_id_right, ImVec2(19*ImGui::GetFontSize(), viewport.y));
    ImGui::DockBuilderDockWindow(Properties::WINDOW_NAME, dock_id_right);
    ImGui::DockBuilderDockWindow(AiChatWindow::WINDOW_NAME, dock_id_right);

    auto stampPanelDockOrder = [](const char* windowName, short order) {
        ImGuiID windowId = ImHashStr(windowName);
        ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(windowId);
        if (!settings) settings = ImGui::CreateNewWindowSettings(windowName);
        settings->DockOrder = order;
        if (ImGuiWindow* window = ImGui::FindWindowByName(windowName)) {
            window->DockOrder = order;
        }
    };
    stampPanelDockOrder(Properties::WINDOW_NAME, 0);
    stampPanelDockOrder(AiChatWindow::WINDOW_NAME, 1);

    // Output/Animation across the bottom; scenes fill the remaining centre.
    ImGui::DockBuilderSplitNode(dock_id_middle, ImGuiDir_Down, 0.0f, &dock_id_middle_bottom, &dock_id_middle_top);
    ImGui::DockBuilderSetNodeSize(dock_id_middle_bottom, ImVec2(viewport.x, 10*ImGui::GetFontSize()));
    ImGui::DockBuilderDockWindow(OutputWindow::WINDOW_NAME, dock_id_middle_bottom);
    ImGui::DockBuilderDockWindow(AnimationWindow::WINDOW_NAME, dock_id_middle_bottom);

    // Mark the scene area as the central node. CentralNode is a saved flag, so it
    // persists in the ini and lets getCentralDockId() recover this node on the
    // next launch without rebuilding the layout.
    if (ImGuiDockNode* centerNode = ImGui::DockBuilderGetNode(dock_id_middle_top)) {
        centerNode->SetLocalFlags(centerNode->LocalFlags | ImGuiDockNodeFlags_CentralNode);
    }
}

void editor::App::dockProjectTabs(){
    // Re-open code tabs and dock scenes in their saved order. dockTabWindow()
    // leaves already-known windows where the ini put them (unless resetting).
    const std::vector<TabEntry>& tabs = project.getTabs();

    auto dockSceneTab = [&](const TabEntry& tab) {
        for (auto& sceneProject : project.getScenes()) {
            if (sceneProject.opened && sceneProject.filepath.string() == tab.filepath) {
                addNewSceneToDock(sceneProject.id);
                break;
            }
        }
    };

    auto dockCodeTab = [&](const TabEntry& tab) {
        fs::path fullPath = project.getProjectPath() / tab.filepath;
        if (fs::exists(fullPath)) {
            if (!codeEditor->isFileOpen(tab.filepath)) {
                codeEditor->openFile(tab.filepath);
            } else {
                dockTabWindow("###" + tab.filepath);
            }
        }
    };

    auto dockOrphanScenes = [&]() {
        for (auto& sceneProject : project.getScenes()) {
            if (!sceneProject.opened) continue;
            if (!project.hasTab(TabType::SCENE, sceneProject.filepath.string())) {
                addNewSceneToDock(sceneProject.id);
            }
        }
    };

    if (forceDockTabs) {
        // Reset layout: scenes first, then code editors.
        for (const auto& tab : tabs) {
            if (tab.type == TabType::SCENE) dockSceneTab(tab);
        }
        dockOrphanScenes();
        for (const auto& tab : tabs) {
            if (tab.type == TabType::CODE_EDITOR) dockCodeTab(tab);
        }
    } else {
        for (const auto& tab : tabs) {
            if (tab.type == TabType::SCENE) dockSceneTab(tab);
            else if (tab.type == TabType::CODE_EDITOR) dockCodeTab(tab);
        }
        dockOrphanScenes();
    }

    // Make the saved tab list authoritative for tab ordering. Scene window ids
    // (###Scene<id>) are reassigned on every load, so the per-window DockOrder
    // ImGui keeps in its ini can't be matched back to a scene. Stamp a sequential
    // DockOrder onto each tab's window settings from project.tabs order so the
    // shared central node restores its tabs in the order the user left them
    // (captured live by captureTabOrder()). ImGui copies this onto the window's
    // DockOrder when it is created, and tabs appearing on the same frame are
    // sorted by it.
    short dockOrder = 0;
    auto stampDockOrder = [&](const std::string& windowName) {
        if (windowName.empty()) return;
        ImGuiID windowId = ImHashStr(windowName.c_str());
        ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(windowId);
        if (!settings) settings = ImGui::CreateNewWindowSettings(windowName.c_str());
        settings->DockOrder = dockOrder;
        if (ImGuiWindow* window = ImGui::FindWindowByName(windowName.c_str())) {
            window->DockOrder = dockOrder;
        }
        dockOrder++;
    };

    if (forceDockTabs) {
        for (const auto& tab : tabs) {
            if (tab.type == TabType::SCENE) stampDockOrder(tabWindowName(tab));
        }
        for (auto& sceneProject : project.getScenes()) {
            if (!sceneProject.opened) continue;
            if (!project.hasTab(TabType::SCENE, sceneProject.filepath.string())) {
                stampDockOrder("###Scene" + std::to_string(sceneProject.id));
            }
        }
        for (const auto& tab : tabs) {
            if (tab.type == TabType::CODE_EDITOR) stampDockOrder(tabWindowName(tab));
        }
    } else {
        for (const auto& tab : tabs) {
            stampDockOrder(tabWindowName(tab));
        }
    }
}

std::string editor::App::tabWindowName(const TabEntry& tab) const {
    if (tab.type == TabType::SCENE) {
        for (const auto& sceneProject : project.getScenes()) {
            if (sceneProject.opened && sceneProject.filepath.string() == tab.filepath) {
                return "###Scene" + std::to_string(sceneProject.id);
            }
        }
        return {};
    }
    // CodeEditor docks its windows as "###<relative-filepath>" (see getWindowTitle()).
    return "###" + tab.filepath;
}

void editor::App::captureTabOrder() {
    // Mirror the live ImGui tab order back into project.tabs so a user's
    // drag-reordering of scene/code tabs survives a save and the next launch.
    // ImGui keeps each window's visual position in its DockNode as DockOrder;
    // we reorder the (filepath-keyed, stable) tab list to match. Reordering a
    // tab triggers no save on its own, so we persist the change ourselves,
    // debounced until the user stops dragging.
    std::vector<TabEntry>& tabs = project.getTabs();
    if (tabs.size() >= 2) {
        struct OrderedTab { TabEntry entry; int order; };
        std::vector<OrderedTab> ordered;
        ordered.reserve(tabs.size());
        for (const TabEntry& tab : tabs) {
            ImGuiWindow* window = ImGui::FindWindowByName(tabWindowName(tab).c_str());
            // Windows that aren't currently docked (DockOrder < 0) or not yet
            // instantiated keep their relative position via the stable sort.
            int order = (window && window->DockOrder >= 0) ? window->DockOrder
                                                           : std::numeric_limits<int>::max();
            ordered.push_back({tab, order});
        }

        std::stable_sort(ordered.begin(), ordered.end(),
            [](const OrderedTab& a, const OrderedTab& b){ return a.order < b.order; });

        bool changed = false;
        for (size_t i = 0; i < tabs.size(); ++i) {
            if (tabs[i].type != ordered[i].entry.type || tabs[i].filepath != ordered[i].entry.filepath) {
                tabs[i] = ordered[i].entry;
                changed = true;
            }
        }

        if (changed) {
            tabsOrderDirty = true;
            tabsOrderChangeTime = ImGui::GetTime();
        }
    }

    // Persist a short moment after the last reorder so a drag results in one
    // write rather than one per frame while the tab slides past its neighbours.
    if (tabsOrderDirty && ImGui::GetTime() - tabsOrderChangeTime > 0.75) {
        project.saveProjectFile();
        tabsOrderDirty = false;
    }
}

void editor::App::dockTabWindow(const std::string& windowName, bool force){
    // Auto-dock only a window the layout ini has never seen, so we never override
    // a position the user chose (docked or intentionally floating). A layout reset
    // (forceDockTabs) and explicit user opens (force) snap the tab to the centre.
    if (force || forceDockTabs || !ImGui::FindWindowSettingsByID(ImHashStr(windowName.c_str()))) {
        dock_id_middle_top = getCentralDockId();
        ImGui::DockBuilderDockWindow(windowName.c_str(), dock_id_middle_top);
    }
}

ImGuiID editor::App::getCentralDockId(){
    // The scene area carries ImGuiDockNodeFlags_CentralNode (a saved flag, so it
    // survives in the ini). We scan for it rather than calling
    // DockBuilderGetCentralNode(), which reads root->CentralNode — only populated
    // once DockSpace() has processed the dockspace, not yet true on the first
    // frame after the layout is restored from the ini.
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    for (const ImGuiStoragePair& pair : ctx->DockContext.Nodes.Data) {
        ImGuiDockNode* node = static_cast<ImGuiDockNode*>(pair.val_p);
        if (node && node->IsCentralNode() && ImGui::DockNodeGetRootNode(node)->ID == dockspace_id) {
            return node->ID;
        }
    }

    // No central node yet (e.g. a pre-existing ini without one): fall back to the
    // dockspace root so windows still attach somewhere sensible.
    return dockspace_id;
}

void editor::App::showStyleEditor(){
    ImGui::Begin("Dear ImGui Style Editor", nullptr);
    {
        // Get the current IO object to access display size
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 windowSize = ImGui::GetWindowSize();

        // Calculate the centered position at the top
        float windowX = (io.DisplaySize.x - windowSize.x - 200) / 2;
        float windowY = 0.0f; // Top of the screen

        // Set the window position
        ImGui::SetWindowPos(ImVec2(windowX, windowY), ImGuiCond_Once);
        ImGui::SetWindowCollapsed(true, ImGuiCond_Once);

        ImGui::ShowStyleEditor();
    }
    ImGui::End();
}

ImFont* editor::App::getCodeFont() {
    return codeFont ? codeFont : ImGui::GetIO().Fonts->Fonts[0];
}

void editor::App::setup() {
    mainThreadId = std::this_thread::get_id();

    // Initialize application settings
    initializeSettings();

    ImGuiIO& io = ImGui::GetIO();

    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;      // Enable Docking
    // Activate only where it works: on Wayland the backend can't reposition windows,
    // so the feature stays off this session until a restart forces the X11 backend.
    if (AppSettings::getMultiViewportEnabled() && !Backend::isRunningOnWayland())
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;  // Multi-Viewport / Platform Windows (toggle in View menu)

    io.ConfigWindowsMoveFromTitleBarOnly = true;

    // Persist the docking layout next to settings.yaml at a stable absolute
    // path, so it survives restarts regardless of the working directory or
    // build type. (initializeSettings() above has already set the config dir.)
    layoutIniPath = (AppSettings::getConfigDirectory() / "editor_layout.ini").string();
    io.IniFilename = layoutIniPath.c_str();

    io.Fonts->AddFontDefault();

    ImFontConfig config;
    config.MergeMode = true;
    config.FontDataOwnedByAtlas = false;
    // Merge size 0.0f: inherit the implicit reference size of AddFontDefault().
    // Since 1.92, merging with an explicit size into an implicit-ref-size font asserts,
    // and specifying GlyphMinAdvanceX (monospacing) requires a non-zero reference size,
    // so it is omitted here. The visible icon font is font1 below, which keeps monospacing.
    static const ImWchar icon_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
    io.Fonts->AddFontFromMemoryTTF(fa_solid_900_ttf, fa_solid_900_ttf_len, 0.0f, &config, icon_ranges);

    ImFontConfig config1;
    strcpy(config1.Name, "roboto-v20-latin-regular (16 px)");
    config1.FontDataOwnedByAtlas = false;
    config1.OversampleH = 2;
    config1.OversampleV = 2;
    config1.RasterizerMultiply = 1.5f;
    ImFont* font1 = io.Fonts->AddFontFromMemoryTTF(roboto_v20_latin_regular_ttf, roboto_v20_latin_regular_ttf_len, 16.0f, &config1);

    ImFontConfig config2;
    config2.MergeMode = true;
    config2.GlyphMinAdvanceX = 16.0f; // Use if you want to make the icon monospaced
    config2.FontDataOwnedByAtlas = false;
    static const ImWchar icon_ranges2[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
    io.Fonts->AddFontFromMemoryTTF(fa_solid_900_ttf, fa_solid_900_ttf_len, 16.0f, &config2, icon_ranges2);

    ImFontConfig config3;
    strcpy(config3.Name, "jetbrains-mono-regular");
    config3.FontDataOwnedByAtlas = false;
    config3.OversampleH = 2;
    config3.OversampleV = 2;
    codeFont = io.Fonts->AddFontFromMemoryTTF(jetbrains_mono_regular_ttf, jetbrains_mono_regular_ttf_len, 16.0f, &config3);

    io.FontDefault = font1;

    io.ConfigDragClickToInputText = true;

    //io.BackendFlags |= ImGuiBackendFlags_HasSetMousePos;

    //ImGui::StyleColorsDark();
    Theme::apply();
}

void editor::App::show(){
    if (resourcesWindow->isFocused()) {
        lastFocusedWindow = LastFocusedWindow::Resources;
    } else if (codeEditor->isFocused()) {
        lastFocusedWindow = LastFocusedWindow::Code;
    } else if (aiChatWindow->isFocused()) {
        lastFocusedWindow = LastFocusedWindow::AI;
    }else{
        lastFocusedWindow = LastFocusedWindow::AnySceneWindow;
    }

    ImGuiIO& io = ImGui::GetIO();
    bool isUndo = (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z) && !io.KeyShift);
    bool isRedo = (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) || (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z) && io.KeyShift);

    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
        if (ImGui::GetIO().KeyShift) {
            // CTRL+SHIFT+S saves all files
            saveAllFunc();
        } else {
            saveFunc();
        }
    }

    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) {
        // CTRL+O opens a project
        openProjectFunc();
    }

    // Play/Pause/Stop shortcuts
    {
        SceneProject* selectedScene = project.getSelectedScene();
        uint32_t selectedSceneId = project.getSelectedSceneId();
        bool hasSelectedScene = selectedScene != nullptr;
        bool isPlaying = hasSelectedScene && selectedScene->playState == ScenePlayState::PLAYING;
        bool isPaused = hasSelectedScene && selectedScene->playState == ScenePlayState::PAUSED;
        bool isLoading = hasSelectedScene && selectedScene->playState == ScenePlayState::LOADING;

        if (ImGui::IsKeyPressed(ImGuiKey_F5)) {
            if (hasSelectedScene && !project.isAnyScenePlaying()) {
                project.start(selectedSceneId);
            } else if (isPaused) {
                project.resume(selectedSceneId);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F6)) {
            if (isPlaying) {
                project.pause(selectedSceneId);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F7)) {
            if (isPlaying || isPaused || isLoading) {
                project.stop(selectedSceneId);
            }
        }
    }

    if (isDroppedExternalPaths) {
        isDroppedExternalPaths = false;
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern)) {
            std::vector<char> payload = Util::encodeStringsPayload(droppedExternalPaths);
            ImGui::SetDragDropPayload("external_files", payload.data(), payload.size());
            ImGui::EndDragDropSource();
        }
    }

    if (!ImGui::IsAnyItemActive() && !ImGui::IsAnyItemFocused()){
        // space to keys events
    }

    if (!resourcesWindow->isFocused() && !codeEditor->isFocused() && !aiChatWindow->isFocused()){
        uint32_t sceneId = project.getSelectedSceneId();

        // Update the Undo and Redo button logic:
        if (isUndo) {
            CommandHandle::get(sceneId)->undo();
        }
        if (isRedo) {
            CommandHandle::get(sceneId)->redo();
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Delete)){
            uint32_t selectedSceneForProperties = project.getSelectedSceneForProperties();
            uint32_t targetSceneId = sceneId;
            if (selectedSceneForProperties != NULL_PROJECT_SCENE &&
                (selectedSceneForProperties == sceneId || project.hasChildScene(sceneId, selectedSceneForProperties))) {
                targetSceneId = selectedSceneForProperties;
            }

            // Check if a tile is selected — delete it instead of the entity
            SceneProject* sp = project.getScene(targetSceneId);
            bool tileDeleted = false;
            bool instanceDeleted = false;
            if (sp && sp->sceneRender) {
                int tileIdx = sp->sceneRender->getSelectedTileIndex();
                Entity tileEntity = sp->sceneRender->getSelectedTileEntity();
                if (tileIdx >= 0) {
                    Command* deleteCmd = ProjectUtils::buildDeleteTileCmd(&project, targetSceneId, tileEntity, (unsigned int)tileIdx);
                    if (deleteCmd) {
                        CommandHandle::get(sceneId)->addCommand(deleteCmd);
                        sp->sceneRender->clearTileSelection();
                        tileDeleted = true;
                    }
                }

                int instIdx = sp->sceneRender->getSelectedInstanceIndex();
                Entity instEntity = sp->sceneRender->getSelectedInstanceEntity();
                if (!tileDeleted && instIdx >= 0) {
                    Command* deleteCmd = ProjectUtils::buildDeleteInstanceCmd(&project, targetSceneId, instEntity, (unsigned int)instIdx);
                    if (deleteCmd) {
                        CommandHandle::get(sceneId)->addCommand(deleteCmd);
                        sp->sceneRender->clearInstanceSelection();
                        instanceDeleted = true;
                    }
                }
            }

            if (!tileDeleted && !instanceDeleted) {
            const std::vector<Entity>& selectedEntities = project.getSelectedEntities(targetSceneId);

            Command* lastCmd = nullptr;
            if (!selectedEntities.empty()) {
                for (const Entity& entity : selectedEntities){
                    lastCmd = new DeleteEntityCmd(&project, targetSceneId, entity);
                    CommandHandle::get(sceneId)->addCommand(lastCmd);
                }
            } else {
                if (selectedSceneForProperties != NULL_PROJECT_SCENE &&
                    selectedSceneForProperties != sceneId &&
                    project.hasChildScene(sceneId, selectedSceneForProperties)) {
                        lastCmd = new RemoveChildSceneCmd(&project, sceneId, selectedSceneForProperties);
                        CommandHandle::get(sceneId)->addCommand(lastCmd);
                }
            }
            if (lastCmd) {
                lastCmd->setNoMerge();
            }
            }
        }

        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)){
            uint32_t targetSceneId = sceneId;
            SceneProject* sp = project.getScene(targetSceneId);
            bool tileDuplicated = false;
            if (sp && sp->sceneRender) {
                int tileIdx = sp->sceneRender->getSelectedTileIndex();
                Entity tileEntity = sp->sceneRender->getSelectedTileEntity();
                if (tileIdx >= 0) {
                    Command* dupCmd = ProjectUtils::buildDuplicateTileCmd(&project, targetSceneId, tileEntity, (unsigned int)tileIdx);
                    if (dupCmd) {
                        CommandHandle::get(sceneId)->addCommand(dupCmd);
                        TilemapComponent* tilemap = sp->scene->findComponent<TilemapComponent>(tileEntity);
                        if (tilemap) {
                            sp->sceneRender->selectTile(tileEntity, (int)tilemap->numTiles - 1);
                        }
                        tileDuplicated = true;
                    }
                }
            }
            if (!tileDuplicated) {
                const std::vector<Entity>& selectedEntities = project.getSelectedEntities(targetSceneId);
                if (!selectedEntities.empty()){
                    CommandHandle::get(sceneId)->addCommandNoMerge(new DuplicateEntityCmd(&project, targetSceneId, selectedEntities));
                }
            }
        }
    }

    if (resourcesWindow->isFocused()) {
        if (isUndo) {
            project.getProjectCommandHistory()->undo();
            resourcesWindow->refreshCurrentDirectory();
        }
        if (isRedo) {
            project.getProjectCommandHistory()->redo();
            resourcesWindow->refreshCurrentDirectory();
        }
    }

    dockspace_id = ImGui::GetID("MyDockspace");

    showMenu();
    showFooter();

    isInitialized = true;

    if (dockspaceNeedsRebuild || ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
        buildDockspace();
        dockspaceNeedsRebuild = false;
    }

    ImGui::DockSpaceOverViewport(dockspace_id, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

    #ifdef SHOW_STYLE_WINDOW
    showStyleEditor();
    #endif

    showAlert();

    sceneSaveDialog.show();
    projectSaveDialog.show();
    exportWindow.show();
    projectSettingsWindow.show();

    if (!saveDialogInProgress && !sceneSaveDialog.isOpen() && !projectSaveDialog.isOpen() && !saveDialogQueue.empty()) {
        processNextSaveDialog();
    }

    structureWindow->show();
    resourcesWindow->show();
    outputWindow->show();
    animationWindow->show();
    terrainEditWindow->show();
    propertiesWindow->show();
    aiChatWindow->show();
    codeEditor->show();
    sceneWindow->show();

    loadingWindow->show();

    // Keep the persisted tab list in sync with the live tab order so a user's
    // drag-reordering of scene/code tabs is saved (and restored on next launch).
    captureTabOrder();

    // Closing a panel through its title-bar button changes the same state as the
    // View menu, so mirror the live state after all panels have rendered.
    persistPanelVisibilitySettings();
}

void editor::App::engineInit(int argc, char** argv) {
    Engine::systemInit(argc, argv, new editor::Platform(&project));

    // Check if there's a last opened project
    std::filesystem::path lastProjectPath = AppSettings::getLastProjectPath();

    if (!lastProjectPath.empty() && std::filesystem::exists(lastProjectPath)) {
        // Try to load the last project
        if (project.loadProject(lastProjectPath)) {
            Out::info("Loaded last opened project: \"%s\"", lastProjectPath.string().c_str());
        } else {
            // If loading fails, create a new temp project
            project.createTempProject("MyDoriaxProject");
        }
    } else {
        // No last project, create a new temp project
        project.createTempProject("MyDoriaxProject");
    }

    Engine::pauseGameEvents(true);

    // Any edit/undo/redo flags its scene for redraw, so the loop can idle otherwise.
    CommandHistory::onSceneModified = [this](size_t sceneId, bool structural){
        if (SceneProject* sceneProject = project.getScene((uint32_t)sceneId)){
            sceneProject->needUpdateRender = true;
            // Only structural commands, so a gizmo drag doesn't rebuild the tree each frame.
            if (structural){
                sceneProject->structureVersion++;
            }
        }
    };

    ShaderPool::setShaderBuilder([this](doriax::ShaderKey shaderKey) -> doriax::ShaderBuildResult {
        static doriax::editor::ShaderBuilder builder;  // Make static to reuse
        return builder.buildShader(shaderKey, &project);
    });

    Engine::setAsyncLoading(true);
}

void editor::App::engineViewLoaded(){
    Engine::systemViewLoaded();
}

void editor::App::engineRender(){
    processMainThreadTasks();
    project.refreshLinkedMaterials();
    renderedSceneThisFrame = false;
    const uint32_t selectedSceneId = project.getSelectedSceneId();

    // Redraw the selected scene only when it changed (needUpdateRender) or while a
    // continuous state (play/load/preview/thumbnail) is active, so the loop can idle.
    const bool thumbnailsPending = resourcesWindow && resourcesWindow->hasPendingThumbnailWork();

    // Drop targets preview a dragged resource directly, with no command to flag the scene.
    const ImGuiPayload* dragPayload = ImGui::GetDragDropPayload();
    const bool draggingResourceFile = dragPayload && dragPayload->IsDataType("resource_files");

    // Keep drawing a few frames after the last change so late async uploads and
    // shader builds still get presented.
    const int RENDER_SETTLE_FRAMES = 30;

    auto sceneNeedsRender = [&](SceneProject& sp, bool isSelected) -> bool {
        bool active = sp.needUpdateRender;
        // A running play session must advance and redraw its simulation every frame.
        if (sp.playState == ScenePlayState::PLAYING) active = true;
        // A drag preview redraws until the settle frames cover its restore.
        if (draggingResourceFile) active = true;
        if (isSelected && sp.scene && sp.sceneRender) {
            // Still loading a model, previewing a camera, or capturing a thumbnail.
            auto ms = sp.scene->getSystem<MeshSystem>();
            if (ms && ms->hasPendingAsyncModelLoads()) active = true;
            // A parsed model still has per-mesh work that only runs while drawing.
            auto rs = sp.scene->getSystem<RenderSystem>();
            if (rs && !rs->isAllLoaded()) active = true;
            if (sp.sceneRender->isPreviewCameraActive()) active = true;
            if (thumbnailsPending) active = true;
            // Preview scenes (shape, material, direction) only draw inside systemDraw().
            if (Engine::hasScenesToExecuteOnce()) active = true;
        }

        if (active) {
            sp.renderSettleFrames = RENDER_SETTLE_FRAMES;
            return true;
        }
        if (sp.renderSettleFrames > 0) {
            sp.renderSettleFrames--;
            return true;
        }
        return false;
    };

    for (auto& sceneProject : project.getScenes()) {
        if (!sceneProject.opened) continue;
        if (!sceneProject.scene || !sceneProject.sceneRender) continue;
        if (sceneProject.playState == ScenePlayState::SAVING || sceneProject.playState == ScenePlayState::LOADING || sceneProject.playState == ScenePlayState::CANCELLING) continue;

        const bool isSelected = (sceneProject.id == selectedSceneId);

        auto meshSystem = sceneProject.scene->getSystem<MeshSystem>();
        bool hasPendingModelLoads = meshSystem && meshSystem->hasPendingAsyncModelLoads();
        if (hasPendingModelLoads && !sceneProject.needUpdateRender && !isSelected) {
            meshSystem->update(0);
            continue;
        }

        if (sceneNeedsRender(sceneProject, isSelected)){
            int width = sceneWindow->getWidth(sceneProject.id);
            int height = sceneWindow->getHeight(sceneProject.id);

            // Not laid out yet (background tab): nothing to draw, and activating it here
            // would thrash lastActivatedScene. Stays dirty until SceneWindow gives it a size.
            if (width == 0 || height == 0) continue;

            SceneRender* sceneRender = sceneProject.sceneRender;

            bool sceneChanged = false;

            // Collect loaded child scene layers (skip if playing — runtime manages its own layers)
            std::vector<Scene*> childLayers;
            if (sceneProject.playState == ScenePlayState::STOPPED) {
                for (const ChildSceneRef& childSceneRef : sceneProject.childScenes) {
                    uint32_t childId = childSceneRef.id;
                    const SceneProject* childScene = project.getScene(childId);
                    if (childScene && childScene->expandedInline && childScene->scene) {
                        childLayers.push_back(childScene->scene);
                    }
                }
            }
            sceneProject.sceneRender->setChildSceneLayers(childLayers);

            // activate() reloads this scene and every layer, so it runs only on a scene
            // change. Anything else needing it calls resetLastActivatedScene().
            if (lastActivatedScene != sceneProject.id){
                // Collected before activate(): it drops every engine layer, and a dropped
                // layer no longer counts as running, so this would come back empty.
                std::vector<Scene*> runtimeLayers = project.getRunningRuntimeLayers(sceneProject.id);
                sceneProject.sceneRender->activate();

                for (Scene* runtimeLayer : runtimeLayers) {
                    Engine::addSceneLayer(runtimeLayer);
                }

                lastActivatedScene = sceneProject.id;
                pendingResizeScene = sceneProject.id;
                sceneChanged = true;
                #ifdef _DEBUG
                printf("DEBUG: Activated scene %u\n", lastActivatedScene);
                #endif
            }

            if (pendingResizeScene == sceneProject.id && width != 0 && height != 0) {
                sceneChanged = true;
                pendingResizeScene = NULL_PROJECT_SCENE;
            }

            if (width != 0 && height != 0){
                if (Platform::setSizes(width, height) || sceneChanged){
                    Engine::systemViewChanged();
                    sceneRender->updateSize(width, height);
                    sceneChanged = false;
                }

                // Update child scene render systems so their entities display correctly
                for (const ChildSceneRef& childSceneRef : sceneProject.childScenes) {
                    uint32_t childId = childSceneRef.id;
                    SceneProject* childScene = project.getScene(childId);
                    if (childScene && childScene->expandedInline && childScene->scene) {
                        if (childScene->sceneRender) {
                            childScene->sceneRender->hideAllGizmos();
                        }
                        childScene->scene->getSystem<MeshSystem>()->update(0);
                        childScene->scene->getSystem<UISystem>()->update(0);
                        childScene->scene->getSystem<RenderSystem>()->update(0);
                        childScene->needUpdateRender = false;
                    }
                }

                // to avoid delay when move objects with gizmo
                sceneRender->updateRenderSystem();

                //TODO: avoid calling every frame
                sceneRender->update(project.getSelectedEntities(sceneProject.id), project.getEntities(sceneProject.id), sceneProject.mainCamera, sceneProject.displaySettings);

                Engine::systemDraw();

                resourcesWindow->processMaterialThumbnails();
                resourcesWindow->processModelThumbnails();

                // Only a real draw counts as activity, or the loop never idles.
                renderedSceneThisFrame = true;
                sceneProject.needUpdateRender = false;
            }
        }
    }

    // A frame resuming after an idle gap measures the gap, not a frame time.
    if (renderedSceneThisFrame && renderedScenePrevFrame){
        footerFramerate = Engine::getFramerate();
        footerDeltaMs = (float)(Engine::getDeltatime() * 1000.0);
    }else{
        footerFramerate = 0.0f;
        footerDeltaMs = 0.0f;
    }
    renderedScenePrevFrame = renderedSceneThisFrame;
}

void editor::App::enqueueMainThreadTask(std::function<void()> task) {
    if (!task) return;
    {
        std::lock_guard<std::mutex> lock(mainThreadTaskMutex);
        mainThreadTasks.push(std::move(task));
    }
    // Wake an idle backend loop so cross-thread work runs promptly.
    if (wakeCallback) wakeCallback();
}

bool editor::App::hasPendingMainThreadTasks() {
    std::lock_guard<std::mutex> lock(mainThreadTaskMutex);
    return !mainThreadTasks.empty();
}

void editor::App::engineViewDestroyed(){
    Engine::systemViewDestroyed();
}

void editor::App::engineShutdown(){
    Engine::systemShutdown();
}

void editor::App::addNewSceneToDock(uint32_t sceneId){
    if (isInitialized){
        const std::string windowName = "###Scene" + std::to_string(sceneId);
        const SceneProject* sceneProject = project.getScene(sceneId);
        const bool isUnsavedScene = sceneProject && sceneProject->filepath.empty();

        dockTabWindow(windowName, isUnsavedScene);
    }
}

void editor::App::clearSceneWindowState(uint32_t sceneId) {
    if (sceneWindow) {
        sceneWindow->clearSceneState(sceneId);
    }

    if (lastActivatedScene == sceneId) {
        resetLastActivatedScene();
    }
}

void editor::App::prepareForProjectSwitch() {
    resourcesWindow->cancelThumbnailWork();
    // Thumbnail cancellation prevents new preview loads from being queued;
    // now quiesce any remaining scene/model jobs before pool cleanup.
    MeshSystem::cancelAllAsyncModelLoads();
    codeEditor->closeAll();
    outputWindow->clear();
    sceneWindow->resetProjectState();
    dockspaceNeedsRebuild = true;
}

void editor::App::requestScenePlayFocus(uint32_t sceneId) {
    if (!isMainThread()) {
        enqueueMainThreadTask([this, sceneId]() {
            requestScenePlayFocus(sceneId);
        });
        return;
    }

    if (sceneWindow) {
        sceneWindow->requestPlayFocus(sceneId);
    }
}

void editor::App::addNewCodeWindowToDock(fs::path path, bool force){
    if (isInitialized){
        dockTabWindow("###" + path.string(), force);
    }
}

void editor::App::handleExternalDrop(const std::vector<std::string>& paths) {
    isDroppedExternalPaths = true;
    droppedExternalPaths = paths;
}

void editor::App::handleExternalDragEnter() {
    resourcesWindow->handleExternalDragEnter();
}

void editor::App::handleExternalDragLeave() {
    resourcesWindow->handleExternalDragLeave();
}

void editor::App::resetLastActivatedScene(){
    lastActivatedScene = NULL_PROJECT_SCENE;
    pendingResizeScene = NULL_PROJECT_SCENE;
}

bool editor::App::shouldSyncEngineApi() const {
    return true;
}

void editor::App::updateResourcesPath(){
    std::filesystem::path currentProjectPath = project.getProjectPath().lexically_normal();
    bool projectPathChanged = lastResourcesProjectPath != currentProjectPath;

    if (isInitialized){
        if (projectPathChanged) {
            resourcesWindow->notifyProjectPathChange();
        } else {
            resourcesWindow->refreshCurrentDirectory();
        }
    }

    // Initial project loading happens before the Resources window is initialized,
    // but prepareForProjectSwitch() still suspends its worker.
    resourcesWindow->resumeThumbnailWork();

    lastResourcesProjectPath = currentProjectPath;
    resourcesWindow->cleanupThumbnails();
}

void editor::App::requestDockspaceRebuild() {
    dockspaceNeedsRebuild = true;
}

void editor::App::updateWindowTitle(const std::string& projectName) {
    Backend::updateWindowTitle(projectName);
}

void editor::App::stopTransientPreviews() {
    if (propertiesWindow) {
        propertiesWindow->stopTransientPreviews();
    }
}

void editor::App::saveAllCodeEditors() {
    if (codeEditor) {
        codeEditor->saveAll();
    }
}

void editor::App::registerAlert(std::string title, std::string message) {
    registerAlert(title, message, "");
}

void editor::App::registerAlert(std::string title, std::string message, std::string note) {
    alert.needShow = true;
    alert.title = title;
    alert.message = message;
    alert.note = note;
    alert.type = AlertType::Info;
    alert.onYes = nullptr;
    alert.onNo = nullptr;
}

void editor::App::registerConfirmAlert(std::string title, std::string message, std::function<void()> onYes, std::function<void()> onNo) {
    alert.needShow = true;
    alert.title = title;
    alert.message = message;
    alert.note.clear();
    alert.type = AlertType::Confirm;
    alert.onYes = onYes;
    alert.onNo = onNo;
}

void editor::App::registerThreeButtonAlert(std::string title, std::string message, std::function<void()> onYes, std::function<void()> onNo, std::function<void()> onCancel) {
    alert.needShow = true;
    alert.title = title;
    alert.message = message;
    alert.note.clear();
    alert.type = AlertType::ThreeButton;
    alert.onYes = onYes;
    alert.onNo = onNo;
    alert.onCancel = onCancel;
}

void editor::App::registerSaveSceneDialog(uint32_t sceneId, std::function<void()> callback) {
    // Add scene to the save dialog queue with callback
    SaveDialogQueueItem item = {SaveDialogType::Scene, sceneId, callback};
    saveDialogQueue.push(item);

    // If this is the only item in the queue, process it immediately
    if (saveDialogQueue.size() == 1 && !saveDialogInProgress && !sceneSaveDialog.isOpen() && !projectSaveDialog.isOpen()) {
        processNextSaveDialog();
    }
    // If queue has more items or another dialog is open, they'll be processed later
}

void editor::App::registerProjectSaveDialog(std::function<void()> callback) {
    // Add project save to the dialog queue with callback
    SaveDialogQueueItem item = {SaveDialogType::Project, 0, callback};  // sceneId is unused for Project saves
    saveDialogQueue.push(item);

    // If this is the only item in the queue, process it immediately
    if (saveDialogQueue.size() == 1 && !saveDialogInProgress && !sceneSaveDialog.isOpen() && !projectSaveDialog.isOpen()) {
        processNextSaveDialog();
    }
    // If queue has more items or another dialog is open, they'll be processed later
}

std::filesystem::path editor::App::getUserCacheBaseDir() {
    // Cache the result to avoid repeated syscalls/env lookups
    static std::filesystem::path cached = []() -> std::filesystem::path {
    #if defined(_WIN32)
        // Ensure COM is initialized for SHGetKnownFolderPath
        // Using COINIT_APARTMENTTHREADED is safe for most GUI apps
        HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        // hrInit == S_OK means we initialized, RPC_E_CHANGED_MODE means already initialized differently (OK)
        // We don't call CoUninitialize here since we want COM available for the app lifetime

        PWSTR widePath = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &widePath)) && widePath) {
            std::filesystem::path p(widePath);
            CoTaskMemFree(widePath);
            return p; // e.g. C:\Users\<you>\AppData\Local
        }
        // Fallback to environment variable
        if (const char* localAppData = std::getenv("LOCALAPPDATA"); localAppData && *localAppData) {
            return std::filesystem::path(localAppData);
        }
        return std::filesystem::temp_directory_path();

    #elif defined(__APPLE__)
        // Conventional macOS cache location: ~/Library/Caches
        const char* home = std::getenv("HOME");
        if (home && *home) {
            return std::filesystem::path(home) / "Library" / "Caches";
        }
        return std::filesystem::temp_directory_path();

    #else
        // Linux / other Unix: XDG Base Dir spec
        // Prefer $XDG_CACHE_HOME, fallback to ~/.cache
        const char* xdg = std::getenv("XDG_CACHE_HOME");
        if (xdg && *xdg) {
            return std::filesystem::path(xdg);
        }
        const char* home = std::getenv("HOME");
        if (home && *home) {
            return std::filesystem::path(home) / ".cache";
        }
        return std::filesystem::temp_directory_path();
    #endif
    }();

    return cached;
}

std::filesystem::path editor::App::getUserShaderCacheDir(){
    // Bump this version on any shader source change to invalidate stale cached
    // .sdat (the cache is keyed by shaderType+properties, not source content).
    // v4: mirror (USE_MIRROR) variants changed the u_fs_mirror block guard.
    // v5: SSAO (mesh.frag USE_SSAO + viewportInfo, new ssao/ssao_blur shaders).
    // v6: SSR (G-buffer geometry pass + ssr/ssr_blur/composite fullscreen shaders;
    //     energy-conserving SSR-over-IBL, glossy roughness blur, debug modes).
    // v7: second UV set (HAS_UV_SET2) — per-texture UV selection via u_fs_texCoordSets.
    // v8: directional/spot shadows share u_shadowAtlas (replaces u_shadowMap1..6),
    //     3x3 atlas, SSAO on terrain (USE_SSAO + depth HAS_TERRAIN).
    // v9: dual shadow atlases — u_shadowAtlas (directional/spot, projective) and
    //     u_shadowPointAtlas (point cube faces); removes the u_shadowCubeMap* samplers
    //     and keeps each atlas within the GPU max texture size.
    // v10: 2D lighting — mesh USE_LIGHT2D/USE_SHADOWS_2D variants (u_fs_lighting2d,
    //      u_normalTexture on unlit, v_position guard) and the shadow2d 1D polar pass.
    // v11: uniform-driven shadow PCF (ShadowQuality) — USE_SHADOWS_PCF variant removed,
    //      shadows.glsl/lighting2d.glsl loop by radius from cameraDir.w / atlasInfo.w.
    // v12: fixed game resolution — new blit fullscreen shader (blit.frag).
    // v13: box-projected local reflection probes in the mesh IBL path.
    // v14: G-buffer (SSR) terrain fix — gbuffer.vert no longer declares a_texcoord1
    //      for terrain (generates the base-tile UV in-shader), so the terrain G-buffer
    //      pipeline's vertex layout is continuous and passes sokol validation.
    // v15: skinning capacity increased to 128 bones; every skinned shader uniform block changed.
    // v16: projected spotlight masks add spotUp_maskAspect[MAX_LIGHTS] to u_fs_lighting
    //      and add the shared u_spotMaskAtlas sampler to punctual mesh variants,
    //      using the engine's native texture Y orientation.
    // v17: projective shadows use a depth-only atlas with a comparison sampler.
    return App::getUserCacheBaseDir() / "doriax" / "shaders" / "v17";
}

void editor::App::pushTabNotificationStyle(){
    ImGui::PushStyleColor(ImGuiCol_Tab,        ImVec4(0.22f, 0.30f, 0.40f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabDimmed,   ImVec4(0.22f, 0.30f, 0.40f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered,  ImVec4(0.26f, 0.35f, 0.46f, 1.00f));
}

void editor::App::popTabNotificationStyle(){
    ImGui::PopStyleColor(3);
}

editor::Project* editor::App::getProject(){
    return &project;
}

const editor::Project* editor::App::getProject() const{
    return &project;
}

editor::Properties* editor::App::getPropertiesWindow() const{
    return propertiesWindow;
}

editor::CodeEditor* editor::App::getCodeEditor() const{
    return codeEditor;
}

editor::ResourcesWindow* editor::App::getResourcesWindow() const{
    return resourcesWindow;
}

bool editor::App::popSaveDialogQueueItem() {
    if (saveDialogQueue.empty()) {
        Out::warning("Attempted to pop an empty save dialog queue.");
        return false;
    }

    saveDialogQueue.pop();
    return true;
}

editor::AnimationWindow* editor::App::getAnimationWindow() const{
    return animationWindow;
}

editor::TerrainEditWindow* editor::App::getTerrainEditWindow() const{
    return terrainEditWindow;
}

void editor::App::processNextSaveDialog() {
    // Check if there's anything to process and no dialogs are currently open
    if (saveDialogInProgress || saveDialogQueue.empty() || sceneSaveDialog.isOpen() || projectSaveDialog.isOpen()) {
        return;
    }

    // Get the next item from the queue
    SaveDialogQueueItem item = saveDialogQueue.front();
    // Store the callback to use it later
    std::function<void()> completionCallback = item.callback;

    if (item.type == SaveDialogType::Scene) {
        // Process scene save dialog
        uint32_t sceneId = item.sceneId;
        SceneProject* sceneProject = project.getScene(sceneId);
        if (!sceneProject) {
            // Invalid scene, remove from queue and try next item
            popSaveDialogQueueItem();
            processNextSaveDialog();
            return;
        }

        // Set default filename
        std::string defaultName = sceneProject->name + ".scene";
        fs::path initialDirectory = project.getProjectPath();
        if (resourcesWindow) {
            fs::path resourcesPath = resourcesWindow->getCurrentPath();
            if (!resourcesPath.empty()) {
                initialDirectory = resourcesPath;
            }
        }

        saveDialogInProgress = true;

        // Open dialog for the current scene
        sceneSaveDialog.open(
            project.getProjectPath(),
            initialDirectory,
            defaultName,
            // Save callback
            [this, sceneId, completionCallback](const fs::path& fullPath) {
                bool saveStarted = false;
                SceneProject* sceneProject = project.getScene(sceneId);
                if (sceneProject) {
                    // Create directory if it doesn't exist
                    std::filesystem::create_directories(fullPath.parent_path());

                    // Save the scene
                    std::error_code ec;
                    fs::path relPath = fs::relative(fullPath, project.getProjectPath(), ec);
                    if (ec || relPath.empty()) {
                        Out::error("Scene filepath must be relative to project path: %s", fullPath.string().c_str());
                        Backend::getApp().registerAlert("Error", "Scene file must be inside the project folder.");
                    } else {
                        sceneProject->filepath = relPath;
                        saveStarted = true;
                        project.saveSceneToPathAsync(sceneId, fullPath, [this, completionCallback](bool success) {
                            saveDialogInProgress = false;
                            popSaveDialogQueueItem();

                            if (success && completionCallback) {
                                completionCallback();
                            }

                            processNextSaveDialog();
                        });
                    }
                }

                if (!saveStarted) {
                    saveDialogInProgress = false;
                    popSaveDialogQueueItem();
                    processNextSaveDialog();
                }
            },
            // Cancel callback
            [this, completionCallback]() {
                // Remove the current item from the queue without saving
                saveDialogInProgress = false;
                popSaveDialogQueueItem();

                // Process the next item if available
                processNextSaveDialog();
            }
        );
    }
    else if (item.type == SaveDialogType::Project) {
        // Process project save dialog
        std::string defaultName = project.getName();
        if (defaultName.empty()) {
            defaultName = "MyProject";
        }

        saveDialogInProgress = true;

        projectSaveDialog.open(
            defaultName,
            // Save callback
            [this, completionCallback](const std::string& projectName, const fs::path& projectPath) {
                // Set the project name if provided
                if (!projectName.empty()) {
                    project.setName(projectName);
                }

                // Save the project to the selected path
                project.saveProjectToPath(projectPath);

                // Remove this item from the queue
                saveDialogInProgress = false;
                popSaveDialogQueueItem();

                // Execute the completion callback if provided
                if (completionCallback) {
                    completionCallback();
                }

                // Process the next item if available
                processNextSaveDialog();
            },
            // Cancel callback
            [this, completionCallback]() {
                // Remove the current item from the queue without saving
                saveDialogInProgress = false;
                popSaveDialogQueueItem();

                // Process the next item if available
                processNextSaveDialog();
            }
        );
    }
}

void editor::App::processMainThreadTasks() {
    std::queue<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(mainThreadTaskMutex);
        std::swap(tasks, mainThreadTasks);
    }

    while (!tasks.empty()) {
        auto task = std::move(tasks.front());
        tasks.pop();
        if (task) task();
    }
}

bool editor::App::isMainThread() const {
    return std::this_thread::get_id() == mainThreadId;
}

void editor::App::initializeSettings() {
    AppSettings::initialize();
    applyPanelVisibilitySettings();
}

void editor::App::applyPanelVisibilitySettings() {
    const PanelVisibilitySettings visibility = AppSettings::getPanelVisibility();
    structureWindow->setOpen(visibility.structure);
    propertiesWindow->setOpen(visibility.properties);
    resourcesWindow->setOpen(visibility.resources);
    outputWindow->setOpen(visibility.output);
    animationWindow->setOpen(visibility.animation);
    terrainEditWindow->setOpen(visibility.terrain);
    aiChatWindow->setOpen(visibility.aiChat);
}

void editor::App::persistPanelVisibilitySettings() {
    PanelVisibilitySettings visibility;
    visibility.structure = structureWindow->isOpen();
    visibility.properties = propertiesWindow->isOpen();
    visibility.resources = resourcesWindow->isOpen();
    visibility.output = outputWindow->isOpen();
    visibility.animation = animationWindow->isOpen();
    visibility.terrain = terrainEditWindow->isOpen();
    visibility.aiChat = aiChatWindow->isOpen();

    if (visibility != AppSettings::getPanelVisibility()) {
        AppSettings::setPanelVisibility(visibility);
    }
}

int editor::App::getInitialWindowWidth() const {
    return AppSettings::getWindowWidth();
}

int editor::App::getInitialWindowHeight() const {
    return AppSettings::getWindowHeight();
}

bool editor::App::getInitialWindowMaximized() const {
    return AppSettings::getIsMaximized();
}

void editor::App::saveWindowSettings(int width, int height, bool maximized) {
    AppSettings::setWindowWidth(width);
    AppSettings::setWindowHeight(height);
    AppSettings::setIsMaximized(maximized);
    AppSettings::saveSettings();
}

void editor::App::exit() {
    // Check if any modal popup is currently open (including ComponentAddDialog, ScriptCreateDialog, etc.)
    ImGuiWindow* modal = ImGui::GetTopMostAndVisiblePopupModal();
    if (modal != nullptr) {
        // A modal is open - close it first
        ImGui::CloseCurrentPopup();
        return;  // Don't proceed with exit yet, user needs to click close again
    }

    // Also check if the scene save dialog is specifically open
    if (sceneSaveDialog.isOpen()) {
        sceneSaveDialog.close();
        return;
    }

    if (project.isAnySceneSaving()) {
        registerAlert("Saving Scene", "Wait for the current scene save to finish before exiting.");
        return;
    }

    if (project.hasScenesUnsavedChanges() || codeEditor->hasUnsavedChanges() || project.isTempUnsavedProject()) {
        registerThreeButtonAlert(
            "Unsaved Changes",
            "There are unsaved changes. Do you want to save them before exiting?",
            [this]() {
                saveAllAndProject([this]() {
                    closeWindow();
                });
            },
            [this]() {
                // No callback - just exit without saving
                closeWindow();
            },
            []() {
                // Cancel callback - do nothing, just close the dialog
            }
        );
    } else {
        // No unsaved changes, proceed with exit
        closeWindow();
    }
}

void editor::App::closeWindow(){
    // Flush a still-pending tab reorder (debounced in captureTabOrder()) so it
    // isn't lost when quitting right after dragging a tab.
    if (tabsOrderDirty){
        project.saveProjectFile();
        tabsOrderDirty = false;
    }

    // Stop all playing scenes before shutdown to properly cleanup script instances
    for (auto& sceneProject : project.getScenes()) {
        if (sceneProject.playState == ScenePlayState::PLAYING || 
            sceneProject.playState == ScenePlayState::PAUSED ||
            sceneProject.playState == ScenePlayState::LOADING) {
            project.stop(sceneProject.id);
        }
    }

    project.waitForPlaySessionToFinish();

    project.clearTrash();

    editor::ShaderBuilder::requestShutdown();
    Backend::closeWindow();
}
