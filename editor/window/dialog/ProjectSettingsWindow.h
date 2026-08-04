#pragma once

#include "Project.h"
#include "imgui.h"

#include <string>
#include <filesystem>
#include <unordered_map>

namespace doriax::editor {

    namespace fs = std::filesystem;

    class ProjectSettingsWindow {
    private:
        bool m_isOpen = false;
        Project* m_project = nullptr;

        // UI state
        char m_projectNameBuffer[256] = {0};
        std::string m_projectNameOriginal;
        int m_canvasWidth = 0;
        int m_canvasHeight = 0;
        int m_scalingModeIndex = 0;
        int m_textureStrategyIndex = 0;
        bool m_vsyncEnabled = true;
        int m_windowModeIndex = 0;
        int m_windowWidth = 0;
        int m_windowHeight = 0;
        bool m_windowResizable = true;
        char m_windowTitleBuffer[256] = {0};
        std::string m_windowTitleOriginal;
        fs::path m_windowIcon;

        // Per-frame preview textures over the pool-cached thumbnails, cleared
        // at the end of each draw (same lifecycle as Properties).
        std::unordered_map<std::string, Texture> m_thumbnailTextures;
        uint32_t m_startSceneId = NULL_PROJECT_SCENE;
        fs::path m_assetsDir;
        fs::path m_luaDir;
        std::vector<CMakeKit> m_availableKits;
        int m_cmakeKitIndex = 0;
        int m_cmakeBuildJobs = 0;
        std::string m_cmakeBuildJobsTooltip;

        void drawSettings();
        void drawGeneralSettings();
        void drawCanvasSettings();
        void drawWindowSettings();
        Texture* findThumbnail(const std::string& path);
        void drawDirectoriesSettings();
        void drawBuildSettings();
        void applySettings();

    public:
        ProjectSettingsWindow() = default;
        ~ProjectSettingsWindow() = default;

        void open(Project* project);
        void show();
        bool isOpen() const { return m_isOpen; }
    };

}
