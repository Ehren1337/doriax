#include "ProjectSettingsWindow.h"
#include "util/FileDialogs.h"
#include "AppSettings.h"
#include "Backend.h"
#include "window/Widgets.h"

#include <algorithm>

namespace doriax::editor {

static const char* scalingModeNames[] = { "Fit Width", "Fit Height", "Letterbox", "Crop", "Stretch", "Native" };
static const Scaling scalingModeValues[] = { Scaling::FITWIDTH, Scaling::FITHEIGHT, Scaling::LETTERBOX, Scaling::CROP, Scaling::STRETCH, Scaling::NATIVE };
static const int scalingModeCount = sizeof(scalingModeValues) / sizeof(scalingModeValues[0]);

static const char* textureStrategyNames[] = { "Fit", "Resize", "None" };
static const TextureStrategy textureStrategyValues[] = { TextureStrategy::FIT, TextureStrategy::RESIZE, TextureStrategy::NONE };
static const int textureStrategyCount = sizeof(textureStrategyValues) / sizeof(textureStrategyValues[0]);

static const char* windowModeNames[] = { "Windowed", "Maximized", "Fullscreen" };
static const WindowMode windowModeValues[] = { WindowMode::WINDOWED, WindowMode::MAXIMIZED, WindowMode::FULLSCREEN };
static const int windowModeCount = sizeof(windowModeValues) / sizeof(windowModeValues[0]);

static constexpr float dialogWidth = 600.0f;
static constexpr float dialogHeight = 480.0f;
static constexpr float settingsLabelWidth = 160.0f;
static constexpr float settingsPanelPadding = 12.0f;
static constexpr float settingsButtonWidth = 120.0f;
static constexpr ImGuiWindowFlags noScrollFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

static int findScalingIndex(Scaling mode) {
    for (int i = 0; i < scalingModeCount; i++) {
        if (scalingModeValues[i] == mode) return i;
    }
    return 0;
}

static int findTextureStrategyIndex(TextureStrategy strategy) {
    for (int i = 0; i < textureStrategyCount; i++) {
        if (textureStrategyValues[i] == strategy) return i;
    }
    return 0;
}

static int findWindowModeIndex(WindowMode mode) {
    for (int i = 0; i < windowModeCount; i++) {
        if (windowModeValues[i] == mode) return i;
    }
    return 0;
}

template <typename DrawContents>
static void drawSettingsPanel(const char* panelId, DrawContents drawContents) {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(settingsPanelPadding, settingsPanelPadding));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(style.CellPadding.x, 6.0f));

    // Allow vertical scrolling only when content exceeds the fixed dialog;
    // ImGui hides the scrollbar while everything fits.
    bool panelVisible = ImGui::BeginChild(panelId, ImVec2(0, 0), ImGuiChildFlags_Borders);
    float labelWidth = std::min(settingsLabelWidth, ImGui::GetContentRegionAvail().x * 0.4f);
    if (panelVisible && ImGui::BeginTable("##SettingsTable", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        drawContents();
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
}

static void beginSettingsRow(const char* label, const char* tooltip = nullptr) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip) ImGui::SetItemTooltip("%s", tooltip);
    ImGui::TableNextColumn();
}

static void drawCombo(const char* id, const char* const* names, int count, int& selectedIndex) {
    if (selectedIndex < 0 || selectedIndex >= count) selectedIndex = 0;

    ImGui::SetNextItemWidth(-1);
    if (!ImGui::BeginCombo(id, names[selectedIndex])) return;

    for (int i = 0; i < count; i++) {
        bool selected = selectedIndex == i;
        if (ImGui::Selectable(names[i], selected)) selectedIndex = i;
        if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
}

static void drawIntSetting(const char* label, const char* id, int& value, int minValue = 1, const char* tooltip = nullptr) {
    beginSettingsRow(label, tooltip);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputInt(id, &value);
    value = std::max(value, minValue);
}

static void drawDirectorySetting(
    Project* project,
    const char* label,
    const char* tooltip,
    const char* pathId,
    const char* buttonId,
    fs::path& directory,
    const fs::path& defaultDirectory,
    bool keepAbsolutePathOnError = false
) {
    beginSettingsRow(label, tooltip);

    const ImGuiStyle& style = ImGui::GetStyle();
    float browseWidth = ImGui::CalcTextSize("Browse").x + style.FramePadding.x * 2.0f;
    float pathWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - browseWidth - style.ItemSpacing.x);

    fs::path displayPath = directory.empty() ? defaultDirectory : directory;
    if (displayPath == ".") displayPath = "<Project root>";
    Widgets::pathDisplay(pathId, displayPath, Vector2(pathWidth, ImGui::GetFrameHeight()));

    ImGui::SameLine();
    if (!project) {
        ImGui::BeginDisabled();
        ImGui::Button(buttonId);
        ImGui::EndDisabled();
        return;
    }

    if (!ImGui::Button(buttonId)) return;

    std::string selectedPath = FileDialogs::openFileDialog(project->getProjectPath().string(), FILE_DIALOG_ALL, true);
    if (selectedPath.empty()) return;

    std::error_code ec;
    fs::path relativePath = fs::relative(fs::path(selectedPath), project->getProjectPath(), ec);
    if (!ec && !relativePath.empty()) {
        directory = relativePath;
    } else {
        directory = keepAbsolutePathOnError ? fs::path(selectedPath) : defaultDirectory;
    }
}

static void showDisabledItemTooltip(const std::string& text) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;

    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

static void drawScalingPreview(Scaling mode, int canvasWidth, int canvasHeight) {
    if (canvasWidth <= 0 || canvasHeight <= 0) return;

    float canvasAspect = (float)canvasWidth / (float)canvasHeight;
    canvasAspect = std::clamp(canvasAspect, 15.0f / 70.0f, 80.0f / 15.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Base canvas size for preview. The bounded aspect ratio keeps the derived
    // height between 15 and 70 pixels.
    float baseW = std::clamp(50.0f * canvasAspect, 15.0f, 80.0f);
    float baseH = baseW / canvasAspect;

    // Three viewport configurations: reference, wider, taller
    float vpW[3] = { baseW * 1.15f, baseW * 1.8f,  baseW * 0.55f };
    float vpH[3] = { baseH * 1.15f, baseH * 0.7f,  baseH * 1.7f  };

    float spacing = 16.0f;
    float totalW = vpW[0] + vpW[1] + vpW[2] + spacing * 2;
    float maxH = 0;
    for (int i = 0; i < 3; i++) { if (vpH[i] > maxH) maxH = vpH[i]; }

    float availW = ImGui::GetContentRegionAvail().x;
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    float offsetX = (availW - totalW) * 0.5f;
    if (offsetX < 0) offsetX = 0;
    float startX = cursor.x + offsetX;
    float startY = cursor.y;

    ImU32 colVpBg        = IM_COL32(215, 228, 243, 255);
    ImU32 colVpBorder    = IM_COL32(155, 165, 180, 255);
    ImU32 colCanvasFill  = IM_COL32(190, 210, 235, 255);
    ImU32 colCanvasBorder= IM_COL32(35, 50, 75, 255);
    ImU32 colTriangle    = IM_COL32(30, 80, 150, 255);
    ImU32 colBlack       = IM_COL32(15, 15, 20, 255);

    float baseTW = baseW * 0.35f;
    float baseTH = baseH * 0.4f;

    float curX = startX;

    for (int i = 0; i < 3; i++) {
        float vw = vpW[i];
        float vh = vpH[i];
        float vx = curX;
        float vy = startY + (maxH - vh) * 0.5f;

        ImVec2 vpMin(vx, vy);
        ImVec2 vpMax(vx + vw, vy + vh);

        // Compute scale factors based on scaling mode
        float scaleX, scaleY;
        switch (mode) {
        case Scaling::FITWIDTH:
            scaleX = scaleY = vw / baseW;
            break;
        case Scaling::FITHEIGHT:
            scaleX = scaleY = vh / baseH;
            break;
        case Scaling::LETTERBOX: {
            float s = (vw / baseW < vh / baseH) ? vw / baseW : vh / baseH;
            scaleX = scaleY = s;
            break;
        }
        case Scaling::CROP: {
            float s = (vw / baseW > vh / baseH) ? vw / baseW : vh / baseH;
            scaleX = scaleY = s;
            break;
        }
        case Scaling::STRETCH:
            scaleX = vw / baseW;
            scaleY = vh / baseH;
            break;
        case Scaling::NATIVE:
        default:
            scaleX = scaleY = 1.0f;
            break;
        }

        float dispW = baseW * scaleX;
        float dispH = baseH * scaleY;

        // Canvas centered in viewport
        float cX = vx + (vw - dispW) * 0.5f;
        float cY = vy + (vh - dispH) * 0.5f;
        ImVec2 cMin(cX, cY);
        ImVec2 cMax(cX + dispW, cY + dispH);

        // Clip drawing to viewport bounds
        dl->PushClipRect(vpMin, vpMax, true);

        // Viewport background
        if (mode == Scaling::LETTERBOX) {
            dl->AddRectFilled(vpMin, vpMax, colBlack);
        } else {
            dl->AddRectFilled(vpMin, vpMax, colVpBg);
        }

        // Canvas fill
        dl->AddRectFilled(cMin, cMax, colCanvasFill);

        // Triangle (content indicator)
        float triW = baseTW * scaleX;
        float triH = baseTH * scaleY;
        float triCX = cX + dispW * 0.5f;
        float triCY = cY + dispH * 0.5f;

        ImVec2 triP1(triCX, triCY - triH * 0.5f);
        ImVec2 triP2(triCX - triW * 0.5f, triCY + triH * 0.5f);
        ImVec2 triP3(triCX + triW * 0.5f, triCY + triH * 0.5f);
        dl->AddTriangleFilled(triP1, triP2, triP3, colTriangle);
        dl->AddTriangle(triP1, triP2, triP3, colCanvasBorder, 1.0f);

        // Canvas border
        dl->AddRect(cMin, cMax, colCanvasBorder, 0, 0, 1.5f);

        // Viewport border
        dl->AddRect(vpMin, vpMax, colVpBorder);

        dl->PopClipRect();

        curX += vw + spacing;
    }

    ImGui::Dummy(ImVec2(totalW, maxH));
}

// Same thumbnail flow as Properties::findThumbnail: load the ResourcesWindow-
// generated thumbnail from the pool, requesting async generation when missing.
Texture* ProjectSettingsWindow::findThumbnail(const std::string& path) {
    if (path.empty()) return nullptr;

    std::filesystem::path texPath = path;
    const std::filesystem::path projectPath = m_project->getProjectPath();

    if (texPath.is_relative() && !projectPath.empty()) {
        texPath = projectPath / texPath;
    }
    texPath = texPath.lexically_normal();

    if (!texPath.is_absolute()) return nullptr;

    std::error_code ec;
    if (!std::filesystem::exists(texPath, ec) || ec) {
        return nullptr;
    }

    const std::filesystem::path thumbnailPath = m_project->getThumbnailPath(texPath);
    const std::string thumbPathStr = thumbnailPath.string();

    // Fast path: return from cache if already loaded
    auto thumbIt = m_thumbnailTextures.find(thumbPathStr);
    if (thumbIt != m_thumbnailTextures.end() && !thumbIt->second.empty()) {
        return &thumbIt->second;
    }

    std::error_code thumbEc;
    const bool thumbnailExists = std::filesystem::exists(thumbnailPath, thumbEc) && !thumbEc;

    if (thumbnailExists) {
        TextureData thumbData(thumbnailPath.string().c_str());
        if (thumbData.getData() && thumbData.getSize() > 0) {
            Texture thumbTexture(thumbPathStr, thumbData);
            m_thumbnailTextures[thumbPathStr] = thumbTexture;
            return &m_thumbnailTextures[thumbPathStr];
        }
    }

    // Thumbnail missing or failed to load — request generation
    if (ResourcesWindow* resourcesWindow = Backend::getApp().getResourcesWindow()) {
        resourcesWindow->requestThumbnailGeneration(texPath, thumbnailExists);
    }
    return nullptr;
}

void ProjectSettingsWindow::open(Project* project) {
    m_isOpen = true;
    m_project = project;
    m_projectNameOriginal = project->getName();
    snprintf(m_projectNameBuffer, sizeof(m_projectNameBuffer), "%s", m_projectNameOriginal.c_str());
    m_canvasWidth = project->getCanvasWidth();
    m_canvasHeight = project->getCanvasHeight();
    m_scalingModeIndex = findScalingIndex(project->getScalingMode());
    m_textureStrategyIndex = findTextureStrategyIndex(project->getTextureStrategy());
    m_vsyncEnabled = project->isVSyncEnabled();
    m_windowModeIndex = findWindowModeIndex(project->getWindowMode());
    m_windowWidth = (int)project->getWindowWidth();
    m_windowHeight = (int)project->getWindowHeight();
    m_windowResizable = project->isWindowResizable();
    m_windowTitleOriginal = project->getWindowTitle();
    snprintf(m_windowTitleBuffer, sizeof(m_windowTitleBuffer), "%s", m_windowTitleOriginal.c_str());
    m_windowIcon = project->getWindowIcon();
    m_assetsDir = project->getAssetsDir();
    m_luaDir = project->getLuaDir();

    m_startSceneId = project->getStartSceneId();
    const SceneProject* startScene = project->getScene(m_startSceneId);
    if (!startScene || startScene->filepath.empty()) {
        m_startSceneId = NULL_PROJECT_SCENE;
        for (const auto& scene : project->getScenes()) {
            if (!scene.filepath.empty()) {
                m_startSceneId = scene.id;
                break;
            }
        }
    }

    m_availableKits = Generator::detectAvailableKits();
    m_cmakeKitIndex = 0; // 0 = "Default"
    m_cmakeBuildJobs = static_cast<int>(project->getCMakeBuildJobs());
    m_cmakeBuildJobsTooltip =
        "Maximum number of concurrent build jobs used for C++ scripts. Set to 0 to automatically use " +
        std::to_string(Generator::getAutomaticParallelBuildJobs()) + " detected logical CPU threads. " +
        "Lower this on memory-constrained systems. Values above " +
        std::to_string(Generator::getMaxParallelBuildJobs()) + " are capped at build time on this machine.";
    std::string currentCxx = project->getCMakeCxxCompiler();
    std::string currentGen = project->getCMakeGenerator();
    if (!currentCxx.empty() || !currentGen.empty()) {
        for (size_t i = 0; i < m_availableKits.size(); i++) {
            if (m_availableKits[i].available && m_availableKits[i].cxxCompiler == currentCxx && m_availableKits[i].generator == currentGen) {
                m_cmakeKitIndex = static_cast<int>(i + 1);
                break;
            }
        }
    }
}

void ProjectSettingsWindow::show() {
    if (!m_isOpen) return;

    ImGui::OpenPopup("Project Settings##ProjectSettingsModal");

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 center = viewport->GetWorkCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImVec2 size(
        std::min(dialogWidth, viewport->WorkSize.x * 0.9f),
        std::min(dialogHeight, viewport->WorkSize.y * 0.9f)
    );
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_Modal |
                             ImGuiWindowFlags_NoResize |
                             noScrollFlags;

    bool popupOpen = ImGui::BeginPopupModal("Project Settings##ProjectSettingsModal", &m_isOpen, flags);

    if (popupOpen) {
        if (!m_isOpen) {
            ImGui::CloseCurrentPopup();
        } else {
            drawSettings();
        }
        ImGui::EndPopup();
    }
}

void ProjectSettingsWindow::drawSettings() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float footerY = ImGui::GetWindowHeight() - style.WindowPadding.y - ImGui::GetFrameHeight();
    float tabRegionHeight = std::max(1.0f, footerY - style.ItemSpacing.y - ImGui::GetCursorPosY());

    ImGui::BeginChild(
        "##ProjectSettingsTabRegion",
        ImVec2(0, tabRegionHeight),
        ImGuiChildFlags_None,
        noScrollFlags
    );

    if (ImGui::BeginTabBar("##ProjectSettingsTabs", ImGuiTabBarFlags_FittingPolicyShrink)) {
        if (ImGui::BeginTabItem("General")) {
            drawGeneralSettings();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Canvas")) {
            drawCanvasSettings();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Window")) {
            drawWindowSettings();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Directories")) {
            drawDirectoriesSettings();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Build")) {
            drawBuildSettings();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(style.WindowPadding.x, footerY - style.ItemSpacing.y));
    ImGui::Separator();

    float footerWidth = std::max(1.0f, ImGui::GetWindowWidth() - style.WindowPadding.x * 2.0f);
    float buttonWidth = std::clamp((footerWidth - style.ItemSpacing.x) * 0.5f, 1.0f, settingsButtonWidth);
    float buttonsWidth = buttonWidth * 2.0f + style.ItemSpacing.x;
    float buttonX = style.WindowPadding.x + std::max(0.0f, (footerWidth - buttonsWidth) * 0.5f);
    ImGui::SetCursorPos(ImVec2(buttonX, footerY));

    if (ImGui::Button("OK", ImVec2(buttonWidth, 0))) {
        applySettings();
        m_isOpen = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0))) {
        m_isOpen = false;
        ImGui::CloseCurrentPopup();
    }

    // Per-frame lifecycle, matching Properties: the pool keeps the underlying
    // thumbnail textures cached, so re-creating the wrappers next frame is cheap.
    m_thumbnailTextures.clear();
}

void ProjectSettingsWindow::drawGeneralSettings() {
    drawSettingsPanel("##GeneralSettingsPanel", [this]() {
        beginSettingsRow("Project Name");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##ProjectName", m_projectNameBuffer, sizeof(m_projectNameBuffer));

        beginSettingsRow("Start Scene");

        const auto& scenes = m_project->getScenes();
        const SceneProject* selectedScene = m_project->getScene(m_startSceneId);
        if (!selectedScene || selectedScene->filepath.empty()) {
            selectedScene = nullptr;
            m_startSceneId = NULL_PROJECT_SCENE;
            for (const auto& scene : scenes) {
                if (!scene.filepath.empty()) {
                    selectedScene = &scene;
                    m_startSceneId = scene.id;
                    break;
                }
            }
        }

        if (!selectedScene) {
            ImGui::TextDisabled("No saved scenes");
            return;
        }

        ImGui::SetNextItemWidth(-1);
        if (!ImGui::BeginCombo("##StartScene", selectedScene->name.c_str())) return;

        for (const auto& scene : scenes) {
            if (scene.filepath.empty()) continue;

            bool selected = m_startSceneId == scene.id;
            if (ImGui::Selectable(scene.name.c_str(), selected)) m_startSceneId = scene.id;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    });
}

void ProjectSettingsWindow::drawCanvasSettings() {
    drawSettingsPanel("##CanvasSettingsPanel", [this]() {
        drawIntSetting("Canvas Width", "##CanvasWidth", m_canvasWidth);
        drawIntSetting("Canvas Height", "##CanvasHeight", m_canvasHeight);

        beginSettingsRow("Scaling Mode");
        drawCombo("##ScalingMode", scalingModeNames, scalingModeCount, m_scalingModeIndex);
        ImGui::Spacing();
        drawScalingPreview(scalingModeValues[m_scalingModeIndex], m_canvasWidth, m_canvasHeight);

        beginSettingsRow("Texture Strategy");
        drawCombo("##TextureStrategy", textureStrategyNames, textureStrategyCount, m_textureStrategyIndex);
    });
}

void ProjectSettingsWindow::drawWindowSettings() {
    drawSettingsPanel("##WindowSettingsPanel", [this]() {
        beginSettingsRow("Window Mode", "Initial window state of desktop builds. Web and mobile ignore window settings.");
        drawCombo("##WindowMode", windowModeNames, windowModeCount, m_windowModeIndex);

        drawIntSetting("Window Width", "##WindowWidth", m_windowWidth);
        drawIntSetting("Window Height", "##WindowHeight", m_windowHeight);

        beginSettingsRow("Window Resizable", "Applies to desktop builds. Exported Windows and macOS builds are always resizable.");
        ImGui::Checkbox("##WindowResizable", &m_windowResizable);

        beginSettingsRow("Window Title");
        std::string titleHint = m_project->getName().empty() ? "Doriax" : m_project->getName();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##WindowTitle", titleHint.c_str(), m_windowTitleBuffer, sizeof(m_windowTitleBuffer));

        beginSettingsRow("Icon", "Application icon for desktop builds: embedded into the Windows executable and used as the window/taskbar icon. Square PNG recommended (256x256 or larger).");
        {
            const ImGuiStyle& style = ImGui::GetStyle();
            float browseWidth = ImGui::CalcTextSize("Browse").x + style.FramePadding.x * 2.0f;
            float clearWidth = ImGui::CalcTextSize("Clear").x + style.FramePadding.x * 2.0f;
            float pathWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - browseWidth - clearWidth - style.ItemSpacing.x * 2.0f);

            fs::path iconDisplay = m_windowIcon.empty() ? fs::path("<None>") : m_windowIcon;
            Widgets::pathDisplay("##WindowIconPath", iconDisplay, Vector2(pathWidth, ImGui::GetFrameHeight()));

            ImGui::SameLine();
            if (ImGui::Button("Browse##windowicon")) {
                std::string defaultPath = m_project->getProjectPath().string();
                std::string selectedPath = FileDialogs::openFileDialog(defaultPath, FILE_DIALOG_IMAGE, false);
                if (!selectedPath.empty()) {
                    // Store project-relative when the file is inside the project
                    // so the setting survives moving the project folder.
                    std::error_code ec;
                    fs::path relPath = fs::relative(fs::path(selectedPath), m_project->getProjectPath(), ec);
                    if (ec || relPath.empty() || relPath.string().rfind("..", 0) == 0) {
                        m_windowIcon = fs::path(selectedPath);
                    } else {
                        m_windowIcon = relPath;
                    }
                }
            }

            ImGui::SameLine();
            ImGui::BeginDisabled(m_windowIcon.empty());
            if (ImGui::Button("Clear##windowicon")) {
                m_windowIcon.clear();
            }
            ImGui::EndDisabled();

            if (!m_windowIcon.empty()) {
                if (Texture* thumb = findThumbnail(m_windowIcon.string())) {
                    ImGui::Spacing();

                    const float previewSize = 64.0f;
                    float thumbWidth = (float)thumb->getWidth();
                    float thumbHeight = (float)thumb->getHeight();
                    float scale = previewSize / std::max(1.0f, std::max(thumbWidth, thumbHeight));
                    ImVec2 imageSize(std::max(1.0f, thumbWidth * scale), std::max(1.0f, thumbHeight * scale));

                    ImVec2 imagePos = ImGui::GetCursorScreenPos();
                    ImGui::Image(thumb->getRender()->getGLHandler(), imageSize);
                    ImGui::GetWindowDrawList()->AddRect(
                        imagePos, ImVec2(imagePos.x + imageSize.x, imagePos.y + imageSize.y),
                        ImGui::GetColorU32(ImGuiCol_Border));
                }
            }
        }

        beginSettingsRow("VSync", "Synchronize Play mode and supported desktop builds to the display refresh rate. macOS Metal exports remain synchronized.");
        ImGui::Checkbox("##VSync", &m_vsyncEnabled);
    });
}

void ProjectSettingsWindow::drawDirectoriesSettings() {
    drawSettingsPanel("##DirectoriesSettingsPanel", [this]() {
        drawDirectorySetting(
            m_project, "Assets Directory", nullptr, "##AssetsPath", "Browse##assets",
            m_assetsDir, fs::path(".")
        );
        drawDirectorySetting(
            m_project, "Lua Directory", nullptr, "##LuaPath", "Browse##lua",
            m_luaDir, fs::path("."), true
        );
    });
}

void ProjectSettingsWindow::drawBuildSettings() {
    drawSettingsPanel("##BuildSettingsPanel", [this]() {
        beginSettingsRow("Compiler");

        if (m_cmakeKitIndex < 0 || m_cmakeKitIndex > static_cast<int>(m_availableKits.size())) {
            m_cmakeKitIndex = 0;
        }

        const char* currentLabel = m_cmakeKitIndex == 0
            ? "Default"
            : m_availableKits[m_cmakeKitIndex - 1].displayName.c_str();

        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##CMakeKit", currentLabel)) {
            bool selected = m_cmakeKitIndex == 0;
            if (ImGui::Selectable("Default", selected)) m_cmakeKitIndex = 0;
            if (selected) ImGui::SetItemDefaultFocus();

            for (size_t i = 0; i < m_availableKits.size(); i++) {
                const auto& kit = m_availableKits[i];
                if (!kit.available) {
                    ImGui::BeginDisabled();
                    ImGui::Selectable((kit.displayName + "  (unavailable)").c_str(), false);
                    ImGui::EndDisabled();
                    showDisabledItemTooltip(kit.unavailableReason);
                    continue;
                }

                selected = m_cmakeKitIndex == static_cast<int>(i + 1);
                if (ImGui::Selectable(kit.displayName.c_str(), selected)) {
                    m_cmakeKitIndex = static_cast<int>(i + 1);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (m_cmakeKitIndex > 0) {
            const auto& kit = m_availableKits[m_cmakeKitIndex - 1];
            if (!kit.cCompiler.empty() || !kit.cxxCompiler.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("C: %s\nCXX: %s", kit.cCompiler.c_str(), kit.cxxCompiler.c_str());
                ImGui::PopStyleColor();
            }
        }

        drawIntSetting("Parallel Jobs", "##CMakeBuildJobs", m_cmakeBuildJobs, 0, m_cmakeBuildJobsTooltip.c_str());
    });
}

void ProjectSettingsWindow::applySettings() {
    // The edit buffer truncates long names. Only write back when the value
    // changed, since setName also rebuilds libName and the window title.
    std::string projectName = m_projectNameBuffer;
    if (!projectName.empty() && projectName != m_projectNameOriginal.substr(0, sizeof(m_projectNameBuffer) - 1)) {
        m_project->setName(projectName);
    }

    m_project->setCanvasSize(m_canvasWidth, m_canvasHeight);
    m_project->setScalingMode(scalingModeValues[m_scalingModeIndex]);
    m_project->setTextureStrategy(textureStrategyValues[m_textureStrategyIndex]);
    m_project->setVSyncEnabled(m_vsyncEnabled);
    m_project->setWindowMode(windowModeValues[m_windowModeIndex]);
    m_project->setWindowSize(m_windowWidth, m_windowHeight);
    m_project->setWindowResizable(m_windowResizable);

    // The edit buffer truncates long titles. Preserve an untouched title that
    // is longer than the buffer instead of writing back the truncated value.
    if (m_windowTitleBuffer != m_windowTitleOriginal.substr(0, sizeof(m_windowTitleBuffer) - 1)) {
        m_project->setWindowTitle(m_windowTitleBuffer);
    }
    m_project->setWindowIcon(m_windowIcon);

    // Moves the referenced files in and rewrites every reference to the new roots
    m_project->changeAssetRoots(m_assetsDir, m_luaDir);

    const SceneProject* startScene = m_project->getScene(m_startSceneId);
    if (startScene && !startScene->filepath.empty()) {
        m_project->setStartSceneId(startScene->id);
    } else {
        m_project->setStartSceneId(NULL_PROJECT_SCENE);
    }

    if (m_cmakeKitIndex > 0) {
        const auto& kit = m_availableKits[m_cmakeKitIndex - 1];
        m_project->setCMakeKit(kit.cCompiler, kit.cxxCompiler, kit.generator);
        AppSettings::setLastCMakeKit(kit.cCompiler, kit.cxxCompiler, kit.generator);
    } else {
        m_project->setCMakeKit("", "", "");
        AppSettings::setLastCMakeKit("", "", "");
    }
    m_project->setCMakeBuildJobs(static_cast<unsigned int>(m_cmakeBuildJobs));

    m_project->saveProjectFile();
}

} // namespace doriax::editor
