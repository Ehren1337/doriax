#pragma once

#include "Project.h"
#include "shader/ShaderBuilder.h"
#include "ShaderDataSerializer.h"
#include "util/CommandRunner.h"

#include <filesystem>
#include <set>
#include <vector>
#include <string>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>

namespace doriax::editor {

    namespace fs = std::filesystem;

    enum class ShaderOutputFormat {
        Header,
        Binary,
        Json
    };

    enum class ExportMode {
        SourceCode,
        Desktop,
        Web
    };

    struct ExportConfig {
        ExportMode mode = ExportMode::SourceCode;
        fs::path targetDir;        // SourceCode: user dir; Desktop/Web: overwritten with the build cache dir
        fs::path destinationDir;   // Desktop/Web only: where the final artifacts are copied
        bool overwriteTarget = false;  // SourceCode: allow exporting into a non-empty targetDir
        fs::path assetsDir;
        fs::path luaDir;
        uint32_t startSceneId = 0;
        std::set<ShaderKey> selectedShaderKeys;
        std::set<::doriax::Platform> selectedPlatforms;
        fs::path shaderOutputDir;
        ShaderOutputFormat shaderOutputFormat = ShaderOutputFormat::Header;
        // Desktop: compiler kit from project settings ("" = CMake default toolchain)
        std::string cmakeCCompiler;
        std::string cmakeCxxCompiler;
        std::string cmakeGenerator;
        unsigned int buildJobs = 0;          // 0 = automatic
        std::string buildType = "Release";
        // Desktop/Web: value passed to the exported CMake GRAPHIC_BACKEND
        // setting (glcore, gles3, d3d11, metal, or vulkan).
        std::string graphicBackend;
        // Web: emsdk root override ("" = auto-detect via EMSDK env / PATH)
        std::string emsdkPath;
    };

    struct ExportProgress {
        std::string currentStep;
        std::string detailLine;              // last streamed build-output line
        float overallProgress = 0.0f;
        bool started = false;
        bool finished = false;
        bool failed = false;
        std::string errorMessage;
    };

    struct EmsdkInfo {
        bool found = false;
        std::string emcmake;                 // emcmake command or full path
        std::string description;             // where it was found, for the UI
    };

    class Exporter {
    private:
        Project* project = nullptr;
        ExportConfig config;
        ExportProgress progress;
        mutable std::mutex progressMutex;
        std::atomic<bool> cancelRequested{false};

        std::thread exportThread;

        ShaderBuilder shaderBuilder;
        CommandRunner commandRunner;

        // Scales values passed to setProgress so the generation steps (written
        // for the [0,1] SourceCode range) map into [0, 0.5] for Desktop/Web,
        // leaving the upper half for configure/build/collect.
        float progressScale = 1.0f;

        void setProgress(const std::string& step, float value);
        void setProgressRaw(const std::string& step, float value);
        void setDetail(const std::string& line);
        void setError(const std::string& message);
        void runExport();

        bool isCancelled() const;

        void collectSelectedShaderKeys(bool mergeWithExisting = false);

        fs::path getExportProjectRoot() const;
        fs::path getShaderOutputDir() const;
        static bool shouldSkipExportSupportFile(const fs::path& relativePath);
        static bool isCppHeaderFile(const fs::path& path);
        static bool isCppSourceFile(const fs::path& path);

        std::string getAppName() const;
        fs::path getBuildCacheDir() const;
        // Generator cmake will actually use: the project kit's, or with no
        // explicit -G, the CMAKE_GENERATOR environment variable cmake honors.
        std::string getEffectiveGenerator() const;

        bool checkTargetDir();
        bool prepareCacheTarget();
        bool configureBuild();
        bool runBuild();
        bool collectDesktopArtifacts();
        bool collectWebArtifacts();
        bool clearGenerated();
        bool loadAndSaveAllScenes();
        bool copyGenerated();
        bool copyAssets();
        bool copyLua();
        bool copyCppScripts();
        bool copyEngine();
        bool writeAppIcon();
        bool buildAndSaveShaders();

        // Aggregates SceneMaxValues across all project scenes and builds the
        // add_definitions() block that sizes the engine's fixed-capacity
        // HybridArrays (MAX_SUBMESHES, MAX_SPRITE_FRAMES, ...) for the export.
        std::string buildSceneMaxValuesDefinitions() const;

        static void copyTree(const fs::path& src, const fs::path& dst, std::error_code& ec);
        static void copyTreeIfChanged(const fs::path& src, const fs::path& dst, std::error_code& ec, bool pruneStale = false);

    public:
        Exporter();
        ~Exporter();

        void startExport(Project* project, const ExportConfig& config);
        bool exportProject(Project* project, const ExportConfig& config);
        bool generateShaders(const ExportConfig& config);
        void cancelExport();
        ExportProgress getProgress() const;
        bool isRunning() const;

        static std::string getShaderDisplayName(ShaderType type, uint32_t properties, uint16_t customId = 0);
        static std::string getPlatformName(::doriax::Platform platform);
        static EmsdkInfo detectEmsdk(const std::string& overridePath);
    };

}
