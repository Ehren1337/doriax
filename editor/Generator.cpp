// Generator.cpp
#include "Generator.h"
#include "Factory.h"
#include "App.h"
#include "editor/Out.h"
#include "util/FileUtils.h"

#include <cstdlib>
#include <stdexcept>
#include <chrono>
#include <fstream>
#include <thread>
#include <cstring>
#include <cerrno>
#include <map>
#include <unordered_set>
#include <algorithm>

#ifdef _WIN32
    #include <windows.h>
    #include <io.h>
#else
    #include <signal.h>
    #include <unistd.h>
    #include <sys/wait.h>
    #include <fcntl.h>

    extern char **environ;
#endif

using namespace doriax;

fs::path editor::Generator::getGeneratedPath(const fs::path& projectInternalPath) {
    return projectInternalPath / "generated";
}

editor::Generator::Generator() :
    lastBuildSucceeded(false)
{
}

editor::Generator::~Generator() {
    // Request cancellation and wait for it to finish before destroying
    try {
        auto f = cancelBuild();
        if (f.valid()) f.wait();
    } catch (...) {}
    waitForBuildToComplete();
}

bool editor::Generator::cleanBuildDirectory(const fs::path& buildPath) {
    std::error_code ec;

    // Best-effort full wipe first.
    fs::remove_all(buildPath, ec);

    // CMakeCache.txt and CMakeFiles/ are what actually pin the previous
    // generator/compiler. If the full wipe above was blocked partway by a
    // locked file elsewhere in the tree (a loaded plugin DLL, an IDE's
    // CMake Tools extension, an antivirus scan), make sure at least these are
    // gone, retrying briefly in case the lock is transient.
    const fs::path cacheFile = buildPath / "CMakeCache.txt";
    const fs::path cacheDir  = buildPath / "CMakeFiles";
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (!fs::exists(cacheFile) && !fs::exists(cacheDir)) {
            break;
        }
        fs::remove(cacheFile, ec);
        fs::remove_all(cacheDir, ec);
        if (!fs::exists(cacheFile) && !fs::exists(cacheDir)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    fs::create_directories(buildPath, ec);

    if (fs::exists(cacheFile)) {
        Out::error("Could not remove the previous CMake cache at: %s", cacheFile.string().c_str());
        Out::error("The build directory is locked by another program. Close any editor or IDE using it (e.g. the VS Code \"CMake Tools\" extension), stop any running build, then delete the '.doriax/build' folder and try again.");
        return false;
    }
    return true;
}

bool editor::Generator::clearStaleCMakeCache(const fs::path& projectPath, const fs::path& buildPath) {
    fs::path cacheFile = buildPath / "CMakeCache.txt";
    if (!fs::exists(cacheFile)) {
        return true;
    }

    std::ifstream cache(cacheFile);
    if (!cache.is_open()) {
        return true;
    }

    std::string line;
    std::string cachedHomeDir;
    while (std::getline(cache, line)) {
        // Look for CMAKE_HOME_DIRECTORY:INTERNAL=<path>
        const std::string prefix = "CMAKE_HOME_DIRECTORY:INTERNAL=";
        if (line.compare(0, prefix.size(), prefix) == 0) {
            cachedHomeDir = line.substr(prefix.size());
            break;
        }
    }
    cache.close();

    if (cachedHomeDir.empty()) {
        return true;
    }

    // Compare canonical paths to handle trailing slashes, symlinks, etc.
    std::error_code ec;
    fs::path cachedPath = fs::canonical(cachedHomeDir, ec);
    if (ec) {
        // Old path no longer exists — definitely stale
        cachedPath = fs::path(cachedHomeDir);
    }
    fs::path currentPath = fs::canonical(projectPath, ec);
    if (ec) {
        currentPath = projectPath;
    }

    if (cachedPath != currentPath) {
        Out::warning("Project path changed. Cleaning build directory...");
        Out::warning("  Previous: %s", cachedHomeDir.c_str());
        Out::warning("  Current: %s", projectPath.string().c_str());

        return cleanBuildDirectory(buildPath);
    }
    return true;
}

void editor::Generator::resolveDefaultKit(std::string& cCompiler, std::string& cxxCompiler, std::string& generator) {
    // Only a "Default" selection (everything empty) is resolved; an explicit kit
    // is left untouched.
    if (!cCompiler.empty() || !cxxCompiler.empty() || !generator.empty()) {
        return;
    }

    // Windows-only: a bare cmake lets CMake auto-detect a toolchain that may not
    // match the editor's C++ ABI (e.g. a MinGW editor would get MSVC and fail to
    // link). Resolve Default to the best ABI-compatible detected kit so it obeys
    // the same ABI check as the Project Settings dropdown. Other platforms have a
    // single C++ ABI, so CMake's auto-detection is always safe and left alone.
    bool resolveDefault = false;
#ifdef _WIN32
    resolveDefault = true;
#endif
    if (!resolveDefault) {
        return;
    }

    std::vector<CMakeKit> kits = detectAvailableKits();
    const CMakeKit* chosen = nullptr;
    for (const auto& k : kits) {
        if (!k.available) continue;
        // Prefer the editor's native toolchain: the kit that needs no explicit
        // compiler or generator (CMake drives it directly, i.e. MSVC on an MSVC
        // editor). Otherwise fall back to the first available (already
        // ABI-filtered) kit.
        if (k.cCompiler.empty() && k.cxxCompiler.empty() && k.generator.empty()) {
            chosen = &k;
            break;
        }
        if (!chosen) chosen = &k;
    }
    if (chosen) {
        cCompiler = chosen->cCompiler;
        cxxCompiler = chosen->cxxCompiler;
        generator = chosen->generator;
        if (!chosen->displayName.empty()) {
            Out::info("Default compiler resolved to: %s", chosen->displayName.c_str());
        }
    }
}

bool editor::Generator::configureCMake(const fs::path& projectPath, const fs::path& buildPath, const std::string& configType, const std::string& cCompiler, const std::string& cxxCompiler, const std::string& generator, unsigned int parallelJobs) {
    if (!clearStaleCMakeCache(projectPath, buildPath)) {
        return false;
    }

    const fs::path exePath = FileUtils::getExecutableDir();

    // Detect kit change: if the compiler/generator selection changed since the
    // last configure, the build tree must be wiped (CMake does not support
    // switching generators or compilers in-place). The editor engine library
    // directory participates too: find_library() caches absolute paths, so a
    // moved/updated editor install must force a fresh library lookup instead
    // of retaining an old libdoriax path.
    {
        fs::path kitMarker = buildPath / ".doriax_kit";
        fs::path cacheFile = buildPath / "CMakeCache.txt";
        std::string currentKit = generator + "\n" + cCompiler + "\n" + cxxCompiler + "\n" + exePath.string();
        if (fs::exists(kitMarker)) {
            std::ifstream f(kitMarker);
            std::string prevKit((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            f.close();
            if (prevKit != currentKit) {
                Out::warning("Compiler kit or engine library directory changed. Cleaning build directory...");
                if (!cleanBuildDirectory(buildPath)) {
                    return false;
                }
            }
        } else if (fs::exists(cacheFile)) {
            std::ifstream cache(cacheFile);
            std::string line;
            std::string cachedGenerator;
            std::string cachedCCompiler;
            std::string cachedCxxCompiler;

            while (std::getline(cache, line)) {
                const std::string genPrefix = "CMAKE_GENERATOR:INTERNAL=";
                const std::string cPrefix = "CMAKE_C_COMPILER:FILEPATH=";
                const std::string cxxPrefix = "CMAKE_CXX_COMPILER:FILEPATH=";

                if (line.compare(0, genPrefix.size(), genPrefix) == 0) {
                    cachedGenerator = line.substr(genPrefix.size());
                } else if (line.compare(0, cPrefix.size(), cPrefix) == 0) {
                    cachedCCompiler = line.substr(cPrefix.size());
                } else if (line.compare(0, cxxPrefix.size(), cxxPrefix) == 0) {
                    cachedCxxCompiler = line.substr(cxxPrefix.size());
                }
            }

            bool kitChanged = false;
            if (!generator.empty()) {
                kitChanged = cachedGenerator != generator;
            } else if (!cachedGenerator.empty() && !cCompiler.empty()) {
                kitChanged = true;
            }
            if (!kitChanged && !cCompiler.empty()) {
                kitChanged = cachedCCompiler != cCompiler;
            }
            if (!kitChanged && !cxxCompiler.empty()) {
                kitChanged = cachedCxxCompiler != cxxCompiler;
            }
            if (!kitChanged && cCompiler.empty() && cxxCompiler.empty()) {
                kitChanged = !cachedCCompiler.empty() || !cachedCxxCompiler.empty();
            }

            if (kitChanged) {
                Out::warning("Compiler kit or engine library directory changed. Cleaning build directory...");
                if (!cleanBuildDirectory(buildPath)) {
                    return false;
                }
            }
        }
    }

    // Safety net: even when the kit bookkeeping above is out of sync (e.g. a
    // previous configure left a cache the marker does not describe), a
    // generator recorded in the cache that differs from the one we are about to
    // request is fatal to CMake ("Does not match the generator used
    // previously"). Force a clean so the configure can actually proceed.
    if (!generator.empty()) {
        fs::path cacheFile = buildPath / "CMakeCache.txt";
        std::ifstream cache(cacheFile);
        if (cache.is_open()) {
            const std::string genPrefix = "CMAKE_GENERATOR:INTERNAL=";
            std::string line;
            std::string cachedGenerator;
            while (std::getline(cache, line)) {
                if (line.compare(0, genPrefix.size(), genPrefix) == 0) {
                    cachedGenerator = line.substr(genPrefix.size());
                    break;
                }
            }
            cache.close();
            if (!cachedGenerator.empty() && cachedGenerator != generator) {
                Out::warning("CMake generator changed (%s -> %s). Cleaning build directory...",
                    cachedGenerator.c_str(), generator.c_str());
                if (!cleanBuildDirectory(buildPath)) {
                    return false;
                }
            }
        }
    }

#ifdef _WIN32
    // A compiler override without an explicit generator makes CMake fall back
    // to the Visual Studio generator, which ignores CMAKE_C/CXX_COMPILER
    // (MSBuild always drives cl.exe) and fails at link time. Fail early with
    // an actionable message instead. Honor CMAKE_GENERATOR if the user set it.
    if (generator.empty() && (!cCompiler.empty() || !cxxCompiler.empty())) {
        const char* envGenerator = std::getenv("CMAKE_GENERATOR");
        if (!envGenerator || !*envGenerator) {
            Out::error("The selected compiler '%s' cannot be used with the default Visual Studio generator. Install Ninja (https://ninja-build.org), add it to PATH and re-select the compiler in Project Settings, or switch to the MSVC compiler.",
                (!cxxCompiler.empty() ? cxxCompiler : cCompiler).c_str());
            return false;
        }
    }
#endif

    // CMake stores path values (e.g. CMAKE_C_COMPILER) into generated .cmake
    // files and re-parses them, treating backslashes as escape sequences. On
    // Windows a path like "C:\clang...\bin\clang.exe" then fails with
    // "Invalid character escape '\c'". CMake accepts forward slashes on every
    // platform, so normalize backslashes before building the command.
    auto toCMakePath = [](const std::string& p) {
        std::string out = p;
        std::replace(out.begin(), out.end(), '\\', '/');
        return out;
    };

    std::string cmakeCommand = "cmake ";
    if (!generator.empty()) {
        cmakeCommand += "-G \"" + generator + "\" ";
    }
    if (!cCompiler.empty()) {
        cmakeCommand += "-DCMAKE_C_COMPILER=\"" + toCMakePath(cCompiler) + "\" ";
    }
    if (!cxxCompiler.empty()) {
        cmakeCommand += "-DCMAKE_CXX_COMPILER=\"" + toCMakePath(cxxCompiler) + "\" ";
    }
    cmakeCommand += "-DCMAKE_BUILD_TYPE=" + configType + " ";
    // When configuring from inside the editor, ensure the generated project builds
    // in "plugin" mode (no Factory main.cpp/scene sources added).
    cmakeCommand += "-DDORIAX_EDITOR_PLUGIN=ON ";
    // The generated CMakeLists.txt stays machine-independent; the effective
    // local limit is supplied through the build tree's cache on every configure.
    cmakeCommand += "-DDORIAX_PARALLEL_BUILD_JOBS=" + std::to_string(parallelJobs) + " ";
    cmakeCommand += "\"" + toCMakePath(projectPath.string()) + "\" ";
    cmakeCommand += "-B \"" + toCMakePath(buildPath.string()) + "\" ";
    cmakeCommand += "-DDORIAX_LIB_DIR=\"" + toCMakePath(exePath.string()) + "\"";

    Out::info("Configuring CMake project with command: %s", cmakeCommand.c_str());
    bool result = runCommand(CommandRunner::msvcEnvPrefix(generator) + cmakeCommand, projectPath);

    // Record which kit was used so we can detect changes next time.
    if (result) {
        std::error_code ec;
        fs::create_directories(buildPath, ec);
        std::ofstream f(buildPath / ".doriax_kit");
        if (f.is_open()) {
            f << generator << "\n" << cCompiler << "\n" << cxxCompiler << "\n" << exePath.string();
        }
    }

    return result;
}

unsigned int editor::Generator::getAutomaticParallelBuildJobs() {
    return std::max(std::thread::hardware_concurrency(), 1u);
}

unsigned int editor::Generator::getMaxParallelBuildJobs() {
    constexpr unsigned int maxJobsPerLogicalCpu = 4;
    // Keeping the shared limit within MAX_SUPPORTED_PARALLEL_BUILD_JOBS lets
    // the same effective value drive every generator.
    const unsigned long long scaledJobs =
        static_cast<unsigned long long>(getAutomaticParallelBuildJobs()) * maxJobsPerLogicalCpu;
    return static_cast<unsigned int>(std::min(
        scaledJobs,
        static_cast<unsigned long long>(MAX_SUPPORTED_PARALLEL_BUILD_JOBS)
    ));
}

bool editor::Generator::buildProject(const fs::path& projectPath, const fs::path& buildPath, const std::string& configType, const std::string& generator, unsigned int parallelJobs) {
    std::string buildCommand = "cmake --build \"" + buildPath.string() + "\" --config " + configType;
    buildCommand += " --parallel " + std::to_string(parallelJobs);
    Out::info("Building project with %u parallel jobs...", parallelJobs);
    // The build step invokes the compiler/linker again, so it needs the same
    // MSVC environment as configure (see CommandRunner::msvcEnvPrefix).
    return runCommand(CommandRunner::msvcEnvPrefix(generator) + buildCommand, buildPath);
}

bool editor::Generator::runCommand(const std::string& command, const fs::path& workingDir) {
    return commandRunner.run(command, workingDir);
}

std::string editor::Generator::getPlatformCMakeConfig(const WindowSettings& windowSettings) {
    std::string content;
    content += "if (NOT DORIAX_EDITOR_PLUGIN)\n";
    content += "    add_definitions(\"-DDEFAULT_WINDOW_WIDTH=" + std::to_string(windowSettings.width) + "\")\n";
    content += "    add_definitions(\"-DDEFAULT_WINDOW_HEIGHT=" + std::to_string(windowSettings.height) + "\")\n";
    content += "\n";
    content += "    set(COMPILE_ZLIB OFF)\n";
    content += "    set(IS_ARM OFF)\n";
    content += "\n";
    content += "    add_definitions(\"-DSOKOL_GLCORE\")\n";
    content += "    add_definitions(\"-DWITH_MINIAUDIO\") # For SoLoud\n";
    content += "\n";
    content += "    list(APPEND PLATFORM_SOURCE\n";
    content += "        ${INTERNAL_DIR}/generated/PlatformEditor.cpp\n";
    content += "        ${INTERNAL_DIR}/generated/main.cpp\n";
    content += "    )\n";
    content += "\n";
    content += "    if(WIN32)\n";
    content += "        list(APPEND PLATFORM_LIBS opengl32 gdi32 user32 shell32 glfw)\n";
    content += "    elseif(APPLE)\n";
    content += "        list(APPEND PLATFORM_LIBS glfw \"-framework OpenGL\" \"-framework Cocoa\" \"-framework IOKit\" \"-framework CoreVideo\" \"-framework CoreFoundation\")\n";
    content += "    else()\n";
    content += "        list(APPEND PLATFORM_LIBS GL dl m glfw)\n";
    content += "    endif()\n";
    content += "endif() \n";
    return content;
}

std::string editor::Generator::buildInitSceneScriptsSource(const std::vector<SceneScriptSource>& scriptFiles) {
    std::string sourceContent;

    sourceContent += "\n";
    sourceContent += "using namespace doriax;\n";

    sourceContent += "\n";
    sourceContent += "#if defined(_MSC_VER)\n";
    sourceContent += "    #define PROJECT_API __declspec(dllexport)\n";
    sourceContent += "#else\n";
    sourceContent += "    #define PROJECT_API\n";
    sourceContent += "#endif\n\n";

    sourceContent += "extern \"C\" void PROJECT_API initScripts(doriax::Scene* scene) {\n";
    sourceContent += "    LuaBinding::initializeLuaScripts(scene);\n";

    if (!scriptFiles.empty()) {

        sourceContent += "\n";

        sourceContent += "    const auto& scriptsArray = scene->getComponentArray<ScriptComponent>();\n";
        sourceContent += "\n";

        sourceContent += "    for (size_t i = 0; i < scriptsArray->size(); i++) {\n";
        sourceContent += "        doriax::ScriptComponent& scriptComp = scriptsArray->getComponentFromIndex(i);\n";
        sourceContent += "        doriax::Entity entity = scriptsArray->getEntity(i);\n";
        sourceContent += "        for (auto& scriptEntry : scriptComp.scripts) {\n";
        sourceContent += "            if (scriptEntry.type == ScriptType::SCRIPT_LUA) \n";
        sourceContent += "                continue; \n";
        sourceContent += "\n";

        for (const auto& s : scriptFiles) {
            sourceContent += "            if (scriptEntry.className == \"" + s.className + "\") {\n";
            sourceContent += "                " + s.className + "* script = new " + s.className + "(scene, entity);\n";
            sourceContent += "                scriptEntry.instance = static_cast<void*>(script);\n";
            sourceContent += "            }\n";
        }
        sourceContent += "        }\n";
        sourceContent += "    }\n";

        sourceContent += "    for (size_t i = 0; i < scriptsArray->size(); i++) {\n";
        sourceContent += "        doriax::ScriptComponent& scriptComp = scriptsArray->getComponentFromIndex(i);\n";
        sourceContent += "        for (auto& scriptEntry : scriptComp.scripts) {\n";
        sourceContent += "            if (scriptEntry.type == ScriptType::SCRIPT_LUA) \n";
        sourceContent += "                continue; \n";
        sourceContent += "\n";

        for (const auto& s : scriptFiles) {
            if (s.properties.empty()) {
                continue;
            }
            sourceContent += "            if (scriptEntry.className == \"" + s.className + "\") {\n";

            sourceContent += "                " + s.className + "* typedScript = static_cast<" + s.className + "*>(scriptEntry.instance);\n";
            sourceContent += "\n";
            sourceContent += "                for (auto& prop : scriptEntry.properties) {\n";

            for (const auto& prop : s.properties) {
                sourceContent += "\n";
                sourceContent += "                    if (prop.name == \"" + prop.name + "\") {\n";

                if (prop.isPtr && !prop.ptrTypeName.empty()) {
                    sourceContent += "                        const auto& entRef = std::get<doriax::EntityReference>(prop.value);\n";
                    sourceContent += "                        doriax::Entity targetEntity = entRef.entity;\n";
                    sourceContent += "                        void* instancePtr = nullptr;\n";
                    sourceContent += "\n";
                    sourceContent += "                        if (targetEntity != NULL_ENTITY) {\n";
                    sourceContent += "                            doriax::Scene* targetScene = scene;\n";
                    sourceContent += "                            if (entRef.sceneId != 0) {\n";
                    sourceContent += "                                targetScene = SceneManager::getScenePtr(entRef.sceneId);\n";
                    sourceContent += "                            }\n";
                    sourceContent += "                            if (targetScene) {\n";
                    sourceContent += "                                doriax::ScriptComponent* targetScriptComp = targetScene->findComponent<doriax::ScriptComponent>(targetEntity);\n";
                    sourceContent += "                                if (targetScriptComp) {\n";
                    sourceContent += "                                    for (auto& targetScript : targetScriptComp->scripts) {\n";
                    sourceContent += "                                        if (targetScript.type != ScriptType::SCRIPT_LUA) {\n";
                    sourceContent += "                                            if (targetScript.className == \"" + prop.ptrTypeName + "\" && targetScript.instance) {\n";
                    sourceContent += "                                                instancePtr = targetScript.instance;\n";
                    sourceContent += "                                                #ifdef DORIAX_EDITOR_PLUGIN\n";
                    sourceContent += "                                                printf(\"[DEBUG]   Found matching C++ script instance: '%s'\\n\", targetScript.className.c_str());\n";
                    sourceContent += "                                                #endif\n";
                    sourceContent += "                                                break;\n";
                    sourceContent += "                                            }\n";
                    sourceContent += "                                        }\n";
                    sourceContent += "                                    }\n";
                    sourceContent += "                                }\n";
                    sourceContent += "\n";
                    if (!prop.ptrTypeName.empty()) {
                        sourceContent += "                                if (!instancePtr) {\n";
                        sourceContent += "                                    #ifdef DORIAX_EDITOR_PLUGIN\n";
                        sourceContent += "                                    printf(\"[DEBUG]   No C++ script instance found, creating '" + prop.ptrTypeName + "' type\\n\");\n";
                        sourceContent += "                                    #endif\n";
                        sourceContent += "                                    instancePtr = new " + prop.ptrTypeName + "(targetScene, targetEntity);\n";
                        sourceContent += "                                }\n";
                    }
                    sourceContent += "                            }\n";
                    sourceContent += "                        }\n";
                    sourceContent += "\n";
                    sourceContent += "                        typedScript->" + prop.name + " = nullptr;\n";
                    sourceContent += "                        if (instancePtr) {\n";
                    sourceContent += "                            typedScript->" + prop.name + " = static_cast<" + prop.ptrTypeName + "*>(instancePtr);\n";
                    sourceContent += "                        }\n";
                    sourceContent += "\n";
                }

                sourceContent += "                        prop.memberPtr = &typedScript->" + prop.name + ";\n";
                sourceContent += "                    }\n";
            }

            sourceContent += "\n";
            sourceContent += "                    prop.syncToMember();\n";
            sourceContent += "                }\n";

            sourceContent += "            }\n";
        }

        sourceContent += "\n";
        sourceContent += "        }\n";
        sourceContent += "    }\n";

    } else{
        sourceContent += "    (void)scene; // Suppress unused parameter warning\n";
    }

    sourceContent += "}\n\n";

    return sourceContent;
}

std::string editor::Generator::buildCleanupSceneScriptsSource(const std::vector<SceneScriptSource>& scriptFiles) {
    std::string sourceContent;

    sourceContent += "extern \"C\" void PROJECT_API cleanupScripts(doriax::Scene* scene) {\n";
    sourceContent += "    LuaBinding::cleanupLuaScripts(scene);\n";

    if (!scriptFiles.empty()) {
        sourceContent += "    const auto& scriptsArray = scene->getComponentArray<ScriptComponent>();\n";
        sourceContent += "    for (size_t i = 0; i < scriptsArray->size(); i++) {\n";
        sourceContent += "        doriax::ScriptComponent& scriptComp = scriptsArray->getComponentFromIndex(i);\n";
        sourceContent += "        for (auto& scriptEntry : scriptComp.scripts) {\n";
        sourceContent += "            if (scriptEntry.type == ScriptType::SCRIPT_LUA) continue;\n";
        sourceContent += "\n";
        sourceContent += "            if (scriptEntry.instance) {\n";
        for (const auto& s : scriptFiles) {
            sourceContent += "                if (scriptEntry.className == \"" + s.className + "\") {\n";
            sourceContent += "                    std::string addr = \"_\" + std::to_string(reinterpret_cast<std::uintptr_t>(scriptEntry.instance)) + \"_\";\n";
            sourceContent += "                    Engine::removeSubscriptionsByTag(addr);\n";
            sourceContent += "                    scene->removeSubscriptionsByTag(addr);\n";
            sourceContent += "                    delete static_cast<" + s.className + "*>(scriptEntry.instance);\n";
            sourceContent += "                }\n";
        }
        sourceContent += "                scriptEntry.instance = nullptr;\n";
        sourceContent += "            }\n";
        sourceContent += "        }\n";
        sourceContent += "    }\n";
    }

    sourceContent += "}\n";

    return sourceContent;
}

void editor::Generator::writeSourceFiles(const fs::path& projectPath, const fs::path& projectInternalPath, std::string libName, const std::vector<SceneScriptSource>& scriptFiles, const std::vector<editor::SceneBuildInfo>& scenes, const std::vector<editor::BundleSceneInfo>& bundles, const WindowSettings& windowSettings) {
    const fs::path exePath = FileUtils::getExecutableDir();

    fs::path relativeInternalPath = fs::relative(projectInternalPath, projectPath);
    fs::path engineApiRelativePath = relativeInternalPath / "engine-api";

    std::string internalPathStr = "${CMAKE_CURRENT_SOURCE_DIR}/" + relativeInternalPath.generic_string();
    std::string engineApiPathStr = "${CMAKE_CURRENT_SOURCE_DIR}/" + engineApiRelativePath.generic_string();

    // Build FACTORY_SOURCES list for CMake (generated by Factory in configure())
    std::string factorySources = "set(FACTORY_SOURCES\n";
    std::unordered_set<std::string> addedFactorySources;
    for (const auto& sceneData : scenes) {
        std::string sceneName = Factory::toIdentifier(sceneData.name);
        std::string filename = sceneName + ".cpp";
        if (addedFactorySources.insert(filename).second) {
            factorySources += "    " + internalPathStr + "/generated/" + filename + "\n";
        }
    }
    for (const auto& bundle : bundles) {
        std::string filename = Factory::bundleToFileName(bundle.bundlePath) + ".cpp";
        if (addedFactorySources.insert(filename).second) {
            factorySources += "    " + internalPathStr + "/generated/" + filename + "\n";
        }
    }
    factorySources += ")\n";

    // Build SCRIPT_SOURCES list for CMake
    std::unordered_set<std::string> scriptIncludeDirs;
    std::string scriptSources = "set(SCRIPT_SOURCES\n";
    for (const auto& s : scriptFiles) {
        if (s.path.is_relative()) {
            scriptSources += "    ${CMAKE_CURRENT_SOURCE_DIR}/" + s.path.generic_string() + "\n";
        } else {
            scriptSources += "    " + s.path.generic_string() + "\n";
        }

        if (!s.headerPath.empty()) {
            if (s.headerPath.is_relative()) {
                scriptIncludeDirs.insert("    ${CMAKE_CURRENT_SOURCE_DIR}\n");
            } else {
                scriptIncludeDirs.insert("    " + s.headerPath.parent_path().generic_string() + "\n");
            }
        }
    }
    scriptSources += ")\n";

    std::string scriptDirs;
    for (const auto& includeDir : scriptIncludeDirs) {
        scriptDirs += includeDir;
    }

    std::string cmakeContent;
    cmakeContent += "# This file is auto-generated by Doriax Editor. Do not edit manually.\n\n";
    cmakeContent += "cmake_minimum_required(VERSION 3.15)\n";
    cmakeContent += "project(" + libName + ")\n\n";
    cmakeContent += "# Editor-resolved parallel build job limit, supplied via -D on every editor\n";
    cmakeContent += "# configure. Declared here so it is documented and so CMake does not warn\n";
    cmakeContent += "# about an unused variable on generators that do not consume it (only the\n";
    cmakeContent += "# MSVC branch below reads it). Empty or 0 means no per-file compile limit.\n";
    cmakeContent += "set(DORIAX_PARALLEL_BUILD_JOBS \"\" CACHE STRING \"Editor-resolved parallel build job limit\")\n\n";
    cmakeContent += "set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR})\n";
    cmakeContent += "set(INTERNAL_DIR ${PROJECT_ROOT}/.doriax)\n\n";
    cmakeContent += "# Doriax runtime API headers for this project come from ${INTERNAL_DIR}/engine-api.\n";
    cmakeContent += "# Local engine API source used by this editor build: " + (exePath / "engine").generic_string() + "\n";
    cmakeContent += "# Full engine + editor source (including YAML serialization for *.scene/*.bundle/project.yaml): https://github.com/doriaxengine/doriax\n\n";

    cmakeContent += "# Specify C++ standard\n";
    cmakeContent += "set(CMAKE_CXX_STANDARD 17)\n";
    cmakeContent += "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";

    cmakeContent += "# Build mode: when ON, build as Doriax Editor plugin (shared library)\n";
    cmakeContent += "option(DORIAX_EDITOR_PLUGIN \"Build as Doriax Editor plugin\" OFF)\n";
    cmakeContent += "if(DORIAX_EDITOR_PLUGIN)\n";
    cmakeContent += "    add_compile_definitions(DORIAX_EDITOR_PLUGIN)\n";
    cmakeContent += "    # The editor links its own engine build, which is compiled as a shared\n";
    cmakeContent += "    # library with editor-specific data layouts. The plugin must match both:\n";
    cmakeContent += "    #  - DORIAX_SHARED makes DORIAX_API resolve to __declspec(dllimport) so\n";
    cmakeContent += "    #    static members such as Engine::onUpdate link against the import lib on MSVC.\n";
    cmakeContent += "    #  - DORIAX_EDITOR keeps HybridArray (and other editor-only layouts)\n";
    cmakeContent += "    #    ABI-compatible across the DLL boundary.\n";
    cmakeContent += "    add_compile_definitions(DORIAX_SHARED DORIAX_EDITOR)\n";
    cmakeContent += "    if(MSVC)\n";
    cmakeContent += "        # Match the editor engine's CRT (it builds DORIAX_SHARED with the\n";
    cmakeContent += "        # dynamic runtime) so std::string/std::vector cross the plugin<->engine\n";
    cmakeContent += "        # DLL boundary on a single shared CRT heap. A static (/MT) or\n";
    cmakeContent += "        # debug/release-mismatched runtime here corrupts memory and crashes on\n";
    cmakeContent += "        # play. The editor builds the plugin with the configuration matching\n";
    cmakeContent += "        # its own, so this expression resolves to the same runtime.\n";
    cmakeContent += "        set(CMAKE_MSVC_RUNTIME_LIBRARY \"MultiThreaded$<$<CONFIG:Debug>:Debug>DLL\")\n";
    cmakeContent += "    endif()\n";
    cmakeContent += "endif()\n\n";

    cmakeContent += getPlatformCMakeConfig(windowSettings) + "\n";

    cmakeContent += scriptSources + "\n";
    cmakeContent += factorySources + "\n";
    cmakeContent += "set(PROJECT_SOURCE " + internalPathStr + "/scene_scripts.cpp)\n\n";
    cmakeContent += "# Project target\n";
    cmakeContent += "if(NOT CMAKE_SYSTEM_NAME STREQUAL \"Android\" AND NOT DORIAX_EDITOR_PLUGIN)\n";
    cmakeContent += "    add_executable(" + libName + "\n";
    cmakeContent += "        ${PROJECT_SOURCE}\n";
    cmakeContent += "        ${SCRIPT_SOURCES}\n";
    cmakeContent += "        ${PLATFORM_SOURCE}\n";
    cmakeContent += "    )\n";
    cmakeContent += "else()\n";
    cmakeContent += "    add_library(" + libName + " SHARED\n";
    cmakeContent += "        ${PROJECT_SOURCE}\n";
    cmakeContent += "        ${SCRIPT_SOURCES}\n";
    cmakeContent += "        ${PLATFORM_SOURCE}\n";
    cmakeContent += "    )\n";
    cmakeContent += "endif()\n\n";

    cmakeContent += "# When building outside the editor, also compile Factory-generated sources\n";
    cmakeContent += "if(NOT DORIAX_EDITOR_PLUGIN)\n";
    cmakeContent += "    target_sources(" + libName + " PRIVATE ${FACTORY_SOURCES})\n";
    cmakeContent += "endif()\n\n";
    cmakeContent += "# To suppress warnings if not Debug\n";
    cmakeContent += "if(NOT CMAKE_BUILD_TYPE STREQUAL \"Debug\")\n";
    cmakeContent += "    set(DORIAX_LIB_SYSTEM SYSTEM)\n";
    cmakeContent += "endif()\n\n";
    cmakeContent += "target_include_directories(" + libName + " ${DORIAX_LIB_SYSTEM} PRIVATE\n";
    cmakeContent += scriptDirs + "\n";
    cmakeContent += "    " + engineApiPathStr + "\n";
    cmakeContent += "    " + engineApiPathStr + "/libs/sokol\n";
    cmakeContent += "    " + engineApiPathStr + "/libs/box2d/include\n";
    cmakeContent += "    " + engineApiPathStr + "/libs/joltphysics\n";
    cmakeContent += "    " + engineApiPathStr + "/renders\n";
    cmakeContent += "    " + engineApiPathStr + "/core\n";
    cmakeContent += "    " + engineApiPathStr + "/core/action\n";
    cmakeContent += "    " + engineApiPathStr + "/core/buffer\n";
    cmakeContent += "    " + engineApiPathStr + "/core/component\n";
    cmakeContent += "    " + engineApiPathStr + "/core/ecs\n";
    cmakeContent += "    " + engineApiPathStr + "/core/io\n";
    cmakeContent += "    " + engineApiPathStr + "/core/manager\n";
    cmakeContent += "    " + engineApiPathStr + "/core/math\n";
    cmakeContent += "    " + engineApiPathStr + "/core/object\n";
    cmakeContent += "    " + engineApiPathStr + "/core/object/sound\n";
    cmakeContent += "    " + engineApiPathStr + "/core/object/ui\n";
    cmakeContent += "    " + engineApiPathStr + "/core/object/environment\n";
    cmakeContent += "    " + engineApiPathStr + "/core/object/physics\n";
    cmakeContent += "    " + engineApiPathStr + "/core/pool\n";
    cmakeContent += "    " + engineApiPathStr + "/core/registry\n";
    cmakeContent += "    " + engineApiPathStr + "/core/render\n";
    cmakeContent += "    " + engineApiPathStr + "/core/script\n";
    cmakeContent += "    " + engineApiPathStr + "/core/shader\n";
    cmakeContent += "    " + engineApiPathStr + "/core/subsystem\n";
    cmakeContent += "    " + engineApiPathStr + "/core/texture\n";
    cmakeContent += "    " + engineApiPathStr + "/core/util\n";
    cmakeContent += "    " + engineApiPathStr + "/platform/glfw # GamepadMappings.h for PlatformEditor\n";
    cmakeContent += ")\n\n";

    cmakeContent += "# libdoriax is searched in DORIAX_LIB_DIR; by default it points to the Doriax editor executable directory.\n";
    cmakeContent += "if(NOT DEFINED DORIAX_LIB_DIR OR DORIAX_LIB_DIR STREQUAL \"\")\n";
    cmakeContent += "    set(DORIAX_LIB_DIR \"" + exePath.generic_string() + "\")\n";
    cmakeContent += "    # Default target is the editor's own engine build (shared, editor layouts).\n";
    cmakeContent += "    # Match its macros so symbols link (DORIAX_SHARED -> dllimport on MSVC) and\n";
    cmakeContent += "    # data layouts stay ABI-compatible (DORIAX_EDITOR), avoiding ODR/ABI mismatch.\n";
    cmakeContent += "    add_compile_definitions(DORIAX_SHARED DORIAX_EDITOR)\n";
    cmakeContent += "endif()\n\n";

    cmakeContent += "# Find doriax library in specified location. find_library() caches its\n";
    cmakeContent += "# result, so a stale DORIAX_LIB from a previous configure (e.g. before the\n";
    cmakeContent += "# editor/engine install moved) would otherwise stick around unchanged;\n";
    cmakeContent += "# force a fresh search on every configure instead.\n";
    cmakeContent += "unset(DORIAX_LIB CACHE)\n";
    cmakeContent += "find_library(DORIAX_LIB NAMES doriax PATHS \"${DORIAX_LIB_DIR}\" NO_DEFAULT_PATH)\n";
    cmakeContent += "if(NOT DORIAX_LIB)\n";
    cmakeContent += "    message(FATAL_ERROR \"Doriax library not found in ${DORIAX_LIB_DIR}\")\n";
    cmakeContent += "endif()\n\n";
    cmakeContent += "target_link_libraries(" + libName + " PRIVATE ${DORIAX_LIB} ${PLATFORM_LIBS})\n\n";
    cmakeContent += "# Set compile options based on compiler and platform\n";
    cmakeContent += "if(MSVC)\n";
    cmakeContent += "    # C4251/C4275: exported engine classes (DORIAX_API -> dllimport) expose STL\n";
    cmakeContent += "    # members (std::vector, std::string, FunctionSubscribe, ...). This is safe\n";
    cmakeContent += "    # here because the engine DLL and this plugin use the same dynamic CRT/STL,\n";
    cmakeContent += "    # so suppress the dll-interface warnings that would otherwise flood the build.\n";
    cmakeContent += "    # MSBuild's /m only parallelizes projects. /MP applies the editor-resolved\n";
    cmakeContent += "    # limit to translation units within this single-target plugin project.\n";
    cmakeContent += "    if(DEFINED DORIAX_PARALLEL_BUILD_JOBS AND DORIAX_PARALLEL_BUILD_JOBS GREATER 0)\n";
    cmakeContent += "        target_compile_options(" + libName + " PRIVATE /W4 /EHsc /MP${DORIAX_PARALLEL_BUILD_JOBS} /wd4251 /wd4275)\n";
    cmakeContent += "    else()\n";
    cmakeContent += "        target_compile_options(" + libName + " PRIVATE /W4 /EHsc /MP /wd4251 /wd4275)\n";
    cmakeContent += "    endif()\n";
    cmakeContent += "else()\n";
    cmakeContent += "    target_compile_options(" + libName + " PRIVATE -Wall -Wextra -fPIC)\n";
    cmakeContent += "    # Error on unresolved symbols at link time. Apple ld does this by default;\n";
    cmakeContent += "    # GNU ld needs '-z defs --no-undefined' and MinGW only understands '--no-undefined'.\n";
    cmakeContent += "    if(WIN32)\n";
    cmakeContent += "        target_link_options(" + libName + " PRIVATE -Wl,--no-undefined)\n";
    cmakeContent += "    elseif(NOT APPLE)\n";
    cmakeContent += "        target_link_options(" + libName + " PRIVATE -Wl,-z,defs,--no-undefined)\n";
    cmakeContent += "    endif()\n";
    cmakeContent += "endif()\n\n";
    cmakeContent += "# Set properties for the shared library\n";
    cmakeContent += "set_target_properties(" + libName + " PROPERTIES\n";
    cmakeContent += "    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}\n";
    cmakeContent += "    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}\n";
    cmakeContent += "    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}\n";
    cmakeContent += "    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}\n";
    cmakeContent += "    LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}\n";
    cmakeContent += "    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}\n";
    cmakeContent += "    LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}\n";
    cmakeContent += "    LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}\n";
    cmakeContent += "    OUTPUT_NAME \"" + libName + "\"\n";
    cmakeContent += "    PREFIX \"\"\n";
    cmakeContent += ")\n";

    // Build C++ source content
    std::string sourceContent;
    sourceContent += "// This file is auto-generated by Doriax Editor. Do not edit manually.\n\n";
    sourceContent += "// This file binds scene script metadata to compiled C++ and Lua scripts for the current build configuration.\n\n";
    sourceContent += "#include <vector>\n";
    sourceContent += "#include <string>\n";
    sourceContent += "#include <stdio.h>\n";
    sourceContent += "#include \"Doriax.h\"\n\n";

    for (const auto& s : scriptFiles) {
        fs::path headerPath = s.headerPath;
        if (headerPath.is_relative()) {
            headerPath = projectPath / headerPath;
        }
        if (!headerPath.empty() && fs::exists(headerPath) && fs::is_regular_file(headerPath)) {
            fs::path relativePath = fs::relative(headerPath, projectPath);
            std::string inc = relativePath.generic_string();
            sourceContent += "#include \"" + inc + "\"\n";
        }
    }

    sourceContent += buildInitSceneScriptsSource(scriptFiles);
    sourceContent += buildCleanupSceneScriptsSource(scriptFiles);

    const fs::path cmakeFile = projectPath / "CMakeLists.txt";
    const fs::path sourceFile = projectInternalPath / "scene_scripts.cpp";

    FileUtils::writeIfChanged(cmakeFile, cmakeContent);
    FileUtils::writeIfChanged(sourceFile, sourceContent);

    // Generate .vscode/settings.json for VS Code if it doesn't exist
    fs::path vscodeDir = projectPath / ".vscode";
    if (!fs::exists(vscodeDir)) {
        fs::create_directories(vscodeDir);
    }
    fs::path settingsFile = vscodeDir / "settings.json";
    if (!fs::exists(settingsFile)) {
        std::string settingsContent;
        settingsContent += "{\n";
        settingsContent += "    \"cmake.buildDirectory\": \"${workspaceFolder}/" + relativeInternalPath.generic_string() + "/externalbuild\"\n";
        settingsContent += "}\n";
        FileUtils::writeIfChanged(settingsFile, settingsContent);
    }

    fs::path agentsFile = projectPath / "AGENTS.md";
    std::string agentsContent;
    agentsContent += "<!-- This file is auto-generated by Doriax Editor. -->\n\n";
    agentsContent += "# Doriax Project Context\n\n";
    agentsContent += "This project was generated by Doriax Editor.\n\n";
    agentsContent += "## Source references\n\n";
    agentsContent += "- Runtime API headers used by this project (snapshot): `" + engineApiRelativePath.generic_string() + "`\n";
    agentsContent += "- Local engine API source used by this editor build: `" + (exePath / "engine").generic_string() + "`\n";
    agentsContent += "- Full engine **and editor** source (upstream): https://github.com/doriaxengine/doriax\n\n";
    agentsContent += "The two local paths above contain only the runtime engine API (what user code links against).\n";
    agentsContent += "They do **not** include the editor itself. To understand the YAML schema of `*.scene`, `*.bundle`, and `project.yaml`, or how the generator/factory produces C++, consult the upstream repository — specifically the `editor/` directory.\n\n";
    agentsContent += "## Generated files\n\n";
    agentsContent += "These files are produced by the editor when a scene is played/run and are intended for **in-editor testing only**. Project export/distribution uses a separate pipeline and does not reuse these files. Do not edit them manually — they will be overwritten on the next generation:\n\n";
    agentsContent += "- `CMakeLists.txt` (project root)\n";
    agentsContent += "- `" + relativeInternalPath.generic_string() + "/scene_scripts.cpp`\n";
    agentsContent += "- `" + relativeInternalPath.generic_string() + "/generated/` (scene factories, bundle factories, `main.cpp`, `PlatformEditor.h`, `PlatformEditor.cpp`)\n";
    agentsContent += "- `" + engineApiRelativePath.generic_string() + "/` (engine API snapshot copied from the editor)\n\n";
    agentsContent += "## Regenerating C++ code\n\n";
    agentsContent += "The generated C++ sources (scene factories, script bindings) are derived from `*.scene`, `*.bundle`, and `project.yaml` files.\n";
    agentsContent += "If you modify any `*.scene`, `*.bundle`, or `project.yaml` file, **you must return to Doriax Editor and play/run the scene** so the editor regenerates all C++ code under `" + relativeInternalPath.generic_string() + "/generated`.\n\n";
    agentsContent += "## C++ Scripts\n\n";
    agentsContent += "User C++ script files (`.h`/`.cpp`) can be placed anywhere in the project outside of `" + relativeInternalPath.generic_string() + "/`.\n";
    agentsContent += "Each script class must inherit from `doriax::Script`. "
                     "The editor discovers and registers scripts automatically; "
                     "they are added to `SCRIPT_SOURCES` in the generated `CMakeLists.txt` and compiled into the project.\n";
    agentsContent += "Lua scripts (`.lua`) are separate — they are loaded at runtime and are not compiled into the binary.\n\n";
    agentsContent += "## Build modes\n\n";
    agentsContent += "- **Editor mode** (`DORIAX_EDITOR_PLUGIN=ON`): builds as a shared library; `main.cpp` and Factory-generated scene sources are excluded. Used by the editor to hot-reload the project.\n";
    agentsContent += "- **Standalone mode** (`DORIAX_EDITOR_PLUGIN=OFF`, default): builds as an executable; includes `main.cpp` and all Factory sources. This standalone build is for local testing only — production distribution uses the editor's separate export pipeline.\n";

    FileUtils::writeIfChanged(agentsFile, agentsContent);
}

std::vector<editor::BundleInstanceInfo> editor::Generator::writeBundleSources(const std::map<fs::path, EntityBundle>& entityBundles, uint32_t sceneId, const fs::path& projectPath, const fs::path& projectInternalPath) {
    const fs::path generatedPath = getGeneratedPath(projectInternalPath);

    std::vector<BundleInstanceInfo> bundleInstances;

    for (const auto& [bundlePath, bundle] : entityBundles) {
        auto sceneIt = bundle.instances.find(sceneId);
        if (sceneIt == bundle.instances.end()) continue;

        for (const auto& instance : sceneIt->second) {
            BundleInstanceInfo info;
            info.bundlePath = bundlePath;
            info.rootEntity = instance.rootEntity;
            for (const auto& member : instance.members) {
                info.memberEntities.insert(member.localEntity);
            }

            // Build override info from instance overrides
            if (!instance.overrides.empty()) {
                for (const auto& [sceneEntity, bitmask] : instance.overrides) {
                    BundleOverrideInfo ovr;
                    ovr.sceneEntity = sceneEntity;

                    // Decode bitmask to ComponentType list
                    for (int bit = 0; bit < 64; bit++) {
                        if (bitmask & (1ULL << bit)) {
                            ovr.overriddenComponents.push_back(static_cast<ComponentType>(bit));
                        }
                    }

                    if (!ovr.overriddenComponents.empty()) {
                        info.overrides.push_back(std::move(ovr));
                    }
                }
            }

            bundleInstances.push_back(std::move(info));
        }

        // Write bundle .h and .cpp files
        std::string headerContent = Factory::createBundleHeader(bundlePath);
        std::string sourceContent = Factory::createBundle(bundlePath, bundle.registry.get(), bundle.registryEntities, projectPath, generatedPath);

        std::string bundleFileName = Factory::bundleToFileName(bundlePath);
        FileUtils::writeIfChanged(generatedPath / (bundleFileName + ".h"), headerContent);
        FileUtils::writeIfChanged(generatedPath / (bundleFileName + ".cpp"), sourceContent);
    }

    return bundleInstances;
}

void editor::Generator::writeSceneSource(Scene* scene, const std::string& sceneName, const std::vector<Entity>& entities, const Entity camera, const fs::path& projectPath, const fs::path& projectInternalPath, std::vector<BundleInstanceInfo>& bundleInstances){
    const fs::path generatedPath = getGeneratedPath(projectInternalPath);

    std::string sceneIdStr = Factory::toIdentifier(sceneName);

    std::string sceneContent = Factory::createScene(0, scene, sceneName, entities, camera, projectPath, generatedPath, bundleInstances);

    std::string filename = sceneIdStr + ".cpp";
    const fs::path sourceFile = generatedPath / filename;

    FileUtils::writeIfChanged(sourceFile, sceneContent);
}

void editor::Generator::clearSceneSource(const std::string& sceneName, const fs::path& projectInternalPath) {
    const fs::path generatedPath = getGeneratedPath(projectInternalPath);
    std::string filename = Factory::toIdentifier(sceneName) + ".cpp";
    const fs::path sourceFile = generatedPath / filename;

    if (fs::exists(sourceFile)) {
        std::error_code ec;
        fs::remove(sourceFile, ec);
        if (ec) {
            Out::error("Failed to remove scene source file '%s': %s", sourceFile.string().c_str(), ec.message().c_str());
        }
    }
}

void editor::Generator::configure(const std::vector<editor::SceneBuildInfo>& scenes, std::string libName, const std::vector<SceneScriptSource>& scriptFiles, const std::vector<editor::BundleSceneInfo>& bundles, const fs::path& projectPath, const fs::path& projectInternalPath, const fs::path& assetsPath, const fs::path& luaPath, Scaling scalingMode, TextureStrategy textureStrategy, unsigned int canvasWidth, unsigned int canvasHeight, bool vsyncEnabled, const WindowSettings& windowSettings){
    const fs::path generatedPath = getGeneratedPath(projectInternalPath);

    // Build main.cpp content
    std::string mainContent;
    mainContent += "// This file is auto-generated by Doriax Editor. Do not edit manually.\n\n";
    mainContent += "#include \"Doriax.h\"\n";
    mainContent += "#include \"PlatformEditor.h\"\n";

    // Include bundle headers (contain declarations for bundle creation functions)
    for (const auto& bundleData : bundles) {
        std::string headerName = Factory::bundleToFileName(bundleData.bundlePath) + ".h";
        mainContent += "#include \"" + headerName + "\"\n";
    }
    if (!bundles.empty()) {
        mainContent += "\n";
    }

    mainContent += "using namespace doriax;\n\n";

    // Forward declarations for per-scene initialization functions (defined in generated scene .cpp files)
    for (const auto& sceneData : scenes) {
        std::string sceneName = Factory::toIdentifier(sceneData.name);
        mainContent += "void create_" + sceneName + "(Scene* scene);\n";
    }
    mainContent += "extern \"C\" void initScripts(doriax::Scene* scene);\n";
    mainContent += "extern \"C\" void cleanupScripts(doriax::Scene* scene);\n";
    mainContent += "\n";

    std::map<uint32_t, std::string> sceneIdToName;
    for (const auto& sceneData : scenes) {
        std::string sceneName = "_" + Factory::toIdentifier(sceneData.name);
        mainContent += "static Scene* " + sceneName + " = nullptr;\n";
        sceneIdToName[sceneData.id] = sceneName;
    }
    mainContent += "\n";

    // Per-stack: static scene pointers + load function
    for (const auto& sceneData : scenes) {
        std::string stackId = Factory::toIdentifier(sceneData.name);
        mainContent += "// --- Scene stack: " + sceneData.name + " ---\n";
        mainContent += "void load_" + stackId + "() {\n";
        for (const auto sceneId : sceneData.involvedScenes) {
            std::string sceneName = "_" + Factory::toIdentifier(sceneIdToName[sceneId]);
            mainContent += "    bool " + sceneName + "_needsInit = false;\n";
        }
        mainContent += "\n";
        for (const auto sceneId : sceneData.involvedScenes) {
            std::string sceneName = "_" + Factory::toIdentifier(sceneIdToName[sceneId]);
            mainContent += "    if (!" + sceneName + "){\n";
            mainContent += "        " + sceneName + " = new Scene();\n";
            mainContent += "        SceneManager::setScenePtr(" + std::to_string(sceneId) + ", " + sceneName + ");\n";
            mainContent += "        " + sceneName + "_needsInit = true;\n";
            mainContent += "    }\n";
        }
        mainContent += "\n";
        for (const auto sceneId : sceneData.involvedScenes) {
            std::string sceneName = "_" + Factory::toIdentifier(sceneIdToName[sceneId]);
            mainContent += "    if (" + sceneName + "_needsInit) {\n";
            mainContent += "        create" + sceneName + "(" + sceneName + ");\n";
            mainContent += "    }\n";
        }
        mainContent += "\n";
        for (const auto sceneId : sceneData.involvedScenes) {
            std::string sceneName = "_" + Factory::toIdentifier(sceneIdToName[sceneId]);
            mainContent += "    if (" + sceneName + "_needsInit) {\n";
            mainContent += "        initScripts(" + sceneName + ");\n";
            mainContent += "    }\n";
        }
        mainContent += "\n";
        for (const auto sceneId : sceneData.activeScenes) {
            std::string sceneName = "_" + Factory::toIdentifier(sceneIdToName[sceneId]);
            if (sceneData.id == sceneId) {
                mainContent += "    Engine::setScene(" + sceneName + ");\n";
            } else {
                mainContent += "    Engine::addSceneLayer(" + sceneName + ");\n";
            }
        }
        mainContent += "\n";
        for (const auto& sceneDataAux: scenes) {
            if (sceneDataAux.id == sceneData.id) continue;
            if (std::find(sceneData.involvedScenes.begin(), sceneData.involvedScenes.end(), sceneDataAux.id) != sceneData.involvedScenes.end()) continue;
            std::string sceneName = "_" + Factory::toIdentifier(sceneDataAux.name);
            mainContent += "    if (" + sceneName + ") {\n";
            mainContent += "        cleanupScripts(" + sceneName + ");\n";
            mainContent += "        SceneManager::removeScenePtr(" + std::to_string(sceneDataAux.id) + ");\n";
            mainContent += "        delete " + sceneName + ";\n";
            mainContent += "        " + sceneName + " = nullptr;\n";
            mainContent += "    }\n";
        }
        mainContent += "}\n\n";
    }

    mainContent += "int main(int argc, char* argv[]) {\n";
    mainContent += "    return PlatformEditor::init(argc, argv);\n";
    mainContent += "}\n\n";

    mainContent += "DORIAX_INIT void init() {\n";
    mainContent += "    Engine::setCanvasSize(" + std::to_string(canvasWidth) + ", " + std::to_string(canvasHeight) + ");\n";

    // Scaling mode
    switch (scalingMode) {
        case Scaling::FITWIDTH:  mainContent += "    Engine::setScalingMode(Scaling::FITWIDTH);\n"; break;
        case Scaling::FITHEIGHT: mainContent += "    Engine::setScalingMode(Scaling::FITHEIGHT);\n"; break;
        case Scaling::LETTERBOX: mainContent += "    Engine::setScalingMode(Scaling::LETTERBOX);\n"; break;
        case Scaling::CROP:      mainContent += "    Engine::setScalingMode(Scaling::CROP);\n"; break;
        case Scaling::STRETCH:   mainContent += "    Engine::setScalingMode(Scaling::STRETCH);\n"; break;
        case Scaling::NATIVE:    mainContent += "    Engine::setScalingMode(Scaling::NATIVE);\n"; break;
    }

    // Texture strategy
    switch (textureStrategy) {
        case TextureStrategy::FIT:    mainContent += "    Engine::setTextureStrategy(TextureStrategy::FIT);\n"; break;
        case TextureStrategy::RESIZE: mainContent += "    Engine::setTextureStrategy(TextureStrategy::RESIZE);\n"; break;
        case TextureStrategy::NONE:   mainContent += "    Engine::setTextureStrategy(TextureStrategy::NONE);\n"; break;
    }

    mainContent += "\n";

    // Register all stacks with SceneManager
    for (const auto& sceneData : scenes) {
        std::string stackId = Factory::toIdentifier(sceneData.name);
        std::string sceneIds;
        for (size_t i = 0; i < sceneData.activeScenes.size(); i++) {
            if (i > 0) sceneIds += ", ";
            sceneIds += std::to_string(sceneData.activeScenes[i]);
        }
        mainContent += "    SceneManager::registerScene(" + std::to_string(sceneData.id) + ", \"" + sceneData.name + "\", load_" + stackId + ", {" + sceneIds + "});\n";
    }
    mainContent += "\n";

    // Register all bundles with BundleManager
    for (size_t i = 0; i < bundles.size(); i++) {
        fs::path noExt = bundles[i].bundlePath;
        noExt.replace_extension();
        std::string bundleName = noExt.generic_string();
        mainContent += "    BundleManager::registerBundle(" + std::to_string(i + 1) + ", \"" + bundleName + "\", " + bundles[i].functionName + ");\n";
    }
    if (!bundles.empty()) {
        mainContent += "\n";
    }

    for (const auto& scene : scenes) {
        if (scene.isMain) {
            mainContent += "    SceneManager::loadScene(\"" + scene.name + "\");\n";
            break; // Load the first main scene found, or the only scene if none are marked main
        }
    }
    mainContent += "}\n";

    const fs::path mainFile = generatedPath / "main.cpp";
    FileUtils::writeIfChanged(mainFile, mainContent);

    const fs::path platformHeaderFile = generatedPath / "PlatformEditor.h";
    FileUtils::writeIfChanged(platformHeaderFile, getPlatformEditorHeader());

    const fs::path platformSourceFile = generatedPath / "PlatformEditor.cpp";
    FileUtils::writeIfChanged(platformSourceFile, getPlatformEditorSource(assetsPath, luaPath, vsyncEnabled, windowSettings));

    writeSourceFiles(projectPath, projectInternalPath, libName, scriptFiles, scenes, bundles, windowSettings);
}

std::string editor::Generator::getPlatformEditorHeader() {
    std::string content;
    content += "// This file is auto-generated by Doriax Editor. Do not edit manually.\n\n";
    content += "#pragma once\n\n";
    content += "#define GLFW_INCLUDE_NONE\n";
    content += "#include \"GLFW/glfw3.h\"\n\n";
    content += "#include \"System.h\"\n\n";
    content += "class PlatformEditor: public doriax::System{\n\n";
    content += "private:\n\n";
    content += "    typedef struct GamepadState{\n";
    content += "        bool connected = false;\n";
    content += "        unsigned char buttons[GLFW_GAMEPAD_BUTTON_LAST + 1] = {0};\n";
    content += "        float axes[GLFW_GAMEPAD_AXIS_LAST + 1] = {0.0f};\n";
    content += "    } GamepadState;\n\n";
    content += "    static int windowPosX;\n";
    content += "    static int windowPosY;\n";
    content += "    static int windowWidth;\n";
    content += "    static int windowHeight;\n\n";
    content += "    static int screenWidth;\n";
    content += "    static int screenHeight;\n\n";
    content += "    static double mousePosX;\n";
    content += "    static double mousePosY;\n\n";
    content += "    static int sampleCount;\n\n";
    content += "    static GLFWwindow* window;\n";
    content += "    static GLFWmonitor* monitor;\n\n";
    content += "    static GamepadState gamepads[GLFW_JOYSTICK_LAST + 1];\n\n";
    content += "    static void pollGamepads();\n\n";
    content += "public:\n\n";
    content += "    PlatformEditor();\n\n";
    content += "    static int init(int argc, char **argv);\n\n";
    content += "    virtual int getScreenWidth();\n";
    content += "    virtual int getScreenHeight();\n\n";
    content += "    virtual int getSampleCount();\n\n";
    content += "    virtual bool isFullscreen();\n";
    content += "    virtual void requestFullscreen();\n";
    content += "    virtual void exitFullscreen();\n\n";
    content += "    virtual bool isWindowMaximized();\n";
    content += "    virtual void maximizeWindow();\n";
    content += "    virtual void restoreWindow();\n";
    content += "    virtual void setWindowSize(int width, int height);\n";
    content += "    virtual bool isWindowResizable();\n";
    content += "    virtual void setWindowResizable(bool resizable);\n";
    content += "    virtual void setWindowTitle(const std::string& title);\n\n";
    content += "    virtual void setMouseCursor(doriax::CursorType type);\n";
    content += "    virtual void setMouseMode(doriax::MouseMode mode);\n\n";
    content += "    virtual void setMousePosition(float x, float y);\n\n";
    content += "    virtual std::string getAssetPath();\n";
    content += "    virtual std::string getUserDataPath();\n";
    content += "    virtual std::string getLuaPath();\n";
    content += "    virtual std::string getShaderPath();\n\n";
    content += "};\n";
    return content;
}

std::string editor::Generator::getPlatformEditorSource(const fs::path& assetsPath, const fs::path& luaPath, bool vsyncEnabled, const WindowSettings& windowSettings) {
    // Escape the title for embedding in a generated C++ string literal.
    // Control characters are sanitized upstream in Project::getWindowSettings().
    std::string windowTitle;
    for (char c : windowSettings.title) {
        if (c == '\\' || c == '"') windowTitle += '\\';
        windowTitle += c;
    }

    std::string content;
    content += "// This file is auto-generated by Doriax Editor. Do not edit manually.\n\n";
    content += "#include \"PlatformEditor.h\"\n\n";
    content += "#include <math.h>\n\n";
    content += "#include \"Engine.h\"\n";
    content += "#include \"GamepadMappings.h\"\n\n";
    content += "int PlatformEditor::windowPosX;\n";
    content += "int PlatformEditor::windowPosY;\n";
    content += "int PlatformEditor::windowWidth;\n";
    content += "int PlatformEditor::windowHeight;\n\n";
    content += "int PlatformEditor::screenWidth;\n";
    content += "int PlatformEditor::screenHeight;\n\n";
    content += "double PlatformEditor::mousePosX;\n";
    content += "double PlatformEditor::mousePosY;\n\n";
    content += "int PlatformEditor::sampleCount;\n\n";
    content += "GLFWwindow* PlatformEditor::window;\n";
    content += "GLFWmonitor* PlatformEditor::monitor;\n\n";
    content += "PlatformEditor::GamepadState PlatformEditor::gamepads[GLFW_JOYSTICK_LAST + 1];\n\n\n";
    content += "PlatformEditor::PlatformEditor(){\n\n";
    content += "}\n\n";
    content += "void PlatformEditor::pollGamepads(){\n";
    content += "    for (int jid = 0; jid <= GLFW_JOYSTICK_LAST; jid++){\n";
    content += "        GamepadState& state = gamepads[jid];\n\n";
    content += "        GLFWgamepadstate glfwState;\n";
    content += "        bool connected = glfwJoystickPresent(jid) && glfwGetGamepadState(jid, &glfwState);\n\n";
    content += "        if (connected && !state.connected){\n";
    content += "            state = GamepadState();\n";
    content += "            state.connected = true;\n";
    content += "            state.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] = -1.0f;\n";
    content += "            state.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] = -1.0f;\n";
    content += "            const char* name = glfwGetGamepadName(jid);\n";
    content += "            doriax::Engine::systemGamepadConnect(jid, name ? name : \"Gamepad\");\n";
    content += "        }else if (!connected && state.connected){\n";
    content += "            state = GamepadState();\n";
    content += "            doriax::Engine::systemGamepadDisconnect(jid);\n";
    content += "        }\n\n";
    content += "        if (!connected)\n";
    content += "            continue;\n\n";
    content += "        for (int button = 0; button <= GLFW_GAMEPAD_BUTTON_LAST; button++){\n";
    content += "            if (glfwState.buttons[button] != state.buttons[button]){\n";
    content += "                state.buttons[button] = glfwState.buttons[button];\n";
    content += "                if (glfwState.buttons[button] == GLFW_PRESS){\n";
    content += "                    doriax::Engine::systemGamepadButtonDown(jid, button);\n";
    content += "                }else{\n";
    content += "                    doriax::Engine::systemGamepadButtonUp(jid, button);\n";
    content += "                }\n";
    content += "            }\n";
    content += "        }\n\n";
    content += "        for (int axis = 0; axis <= GLFW_GAMEPAD_AXIS_LAST; axis++){\n";
    content += "            if (fabsf(glfwState.axes[axis] - state.axes[axis]) > 0.001f){\n";
    content += "                state.axes[axis] = glfwState.axes[axis];\n";
    content += "                doriax::Engine::systemGamepadAxisMove(jid, axis, glfwState.axes[axis]);\n";
    content += "            }\n";
    content += "        }\n";
    content += "    }\n";
    content += "}\n\n";
    content += "int PlatformEditor::init(int argc, char **argv){\n";
    content += "    windowWidth = DEFAULT_WINDOW_WIDTH;\n";
    content += "    windowHeight = DEFAULT_WINDOW_HEIGHT;\n\n";
    content += "    sampleCount = 1;\n\n";
    content += "    doriax::Engine::systemInit(argc, argv, new PlatformEditor());\n\n";
    content += "    /* create window and GL context via GLFW */\n";
    content += "    glfwInit();\n\n";
    content += "    // GLFW's built-in gamepad database misses controllers newer than its\n";
    content += "    // release; without a mapping glfwGetGamepadState fails and the pad is\n";
    content += "    // treated as disconnected. Apply the community database over it.\n";
    content += "    for (size_t i = 0; i < DORIAX_GAMEPAD_MAPPINGS_COUNT; i++)\n";
    content += "        glfwUpdateGamepadMappings(DORIAX_GAMEPAD_MAPPINGS[i]);\n\n";
    content += "    glfwWindowHint(GLFW_SAMPLES, (sampleCount == 1) ? 0 : sampleCount);\n";
    content += "    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);\n";
    content += "    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);\n";
    content += "    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);\n";
    content += "    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);\n";
    content += std::string("    glfwWindowHint(GLFW_RESIZABLE, ") + (windowSettings.resizable ? "GLFW_TRUE" : "GLFW_FALSE") + ");\n\n";
    content += "    monitor = glfwGetPrimaryMonitor();\n\n";
    if (windowSettings.mode == WindowMode::FULLSCREEN) {
        content += "    // start fullscreen at the current desktop video mode; fall back to windowed\n";
        content += "    // when the monitor or its video mode is unavailable (e.g. headless session)\n";
        content += "    const GLFWvidmode* videoMode = monitor ? glfwGetVideoMode(monitor) : nullptr;\n";
        content += "    if (videoMode) {\n";
        content += "        glfwWindowHint(GLFW_RED_BITS, videoMode->redBits);\n";
        content += "        glfwWindowHint(GLFW_GREEN_BITS, videoMode->greenBits);\n";
        content += "        glfwWindowHint(GLFW_BLUE_BITS, videoMode->blueBits);\n";
        content += "        glfwWindowHint(GLFW_REFRESH_RATE, videoMode->refreshRate);\n";
        content += "        window = glfwCreateWindow(videoMode->width, videoMode->height, \"" + windowTitle + "\", monitor, 0);\n";
        content += "    } else {\n";
        content += "        window = glfwCreateWindow(windowWidth, windowHeight, \"" + windowTitle + "\", 0, 0);\n";
        content += "    }\n\n";
    } else {
        if (windowSettings.mode == WindowMode::MAXIMIZED) {
            content += "    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);\n";
        }
        content += "    window = glfwCreateWindow(windowWidth, windowHeight, \"" + windowTitle + "\", 0, 0);\n\n";
    }
    content += "    glfwMakeContextCurrent(window);\n";
    content += "    glfwSwapInterval(" + std::to_string(vsyncEnabled ? 1 : 0) + ");\n\n";
    content += "    glfwSetMouseButtonCallback(window, [](GLFWwindow*, int btn, int action, int mods) {\n";
    content += "        if (action==GLFW_PRESS){\n";
    content += "            doriax::Engine::systemMouseDown(btn, float(mousePosX), float(mousePosY), mods);\n";
    content += "        }else if (action==GLFW_RELEASE){\n";
    content += "            doriax::Engine::systemMouseUp(btn, float(mousePosX), float(mousePosY), mods);\n";
    content += "        }\n";
    content += "    });\n";
    content += "    glfwSetCursorPosCallback(window, [](GLFWwindow*, double pos_x, double pos_y) {\n";
    content += "        float xscale, yscale;\n";
    content += "        glfwGetWindowContentScale(window, &xscale, &yscale);\n\n";
    content += "        mousePosX = pos_x * xscale;\n";
    content += "        mousePosY = pos_y * yscale;\n";
    content += "        doriax::Engine::systemMouseMove(float(mousePosX), float(mousePosY), 0);\n";
    content += "    });\n";
    content += "    glfwSetScrollCallback(window, [](GLFWwindow*, double xoffset, double yoffset){\n";
    content += "        doriax::Engine::systemMouseScroll((float)xoffset, (float)yoffset, 0);\n";
    content += "    });\n";
    content += "    glfwSetKeyCallback(window, [](GLFWwindow*, int key, int /*scancode*/, int action, int mods){\n";
    content += "        if (action==GLFW_PRESS){\n";
    content += "            if (key == GLFW_KEY_TAB)\n";
    content += "                doriax::Engine::systemCharInput('\\t');\n";
    content += "            if (key == GLFW_KEY_BACKSPACE)\n";
    content += "                doriax::Engine::systemCharInput('\\b');\n";
    content += "            if (key == GLFW_KEY_ENTER)\n";
    content += "                doriax::Engine::systemCharInput('\\r');\n";
    content += "            if (key == GLFW_KEY_ESCAPE)\n";
    content += "                doriax::Engine::systemCharInput('\\e');\n";
    content += "            doriax::Engine::systemKeyDown(key, false, mods);\n";
    content += "        }else if (action==GLFW_REPEAT){\n";
    content += "            if (key == GLFW_KEY_TAB)\n";
    content += "                doriax::Engine::systemCharInput('\\t');\n";
    content += "            if (key == GLFW_KEY_BACKSPACE)\n";
    content += "                doriax::Engine::systemCharInput('\\b');\n";
    content += "            if (key == GLFW_KEY_ENTER)\n";
    content += "                doriax::Engine::systemCharInput('\\r');\n";
    content += "            if (key == GLFW_KEY_ESCAPE)\n";
    content += "                doriax::Engine::systemCharInput('\\e');\n";
    content += "            doriax::Engine::systemKeyDown(key, true, mods);\n";
    content += "        }else if (action==GLFW_RELEASE){\n";
    content += "            doriax::Engine::systemKeyUp(key, false, mods);\n";
    content += "        }\n";
    content += "    });\n";
    content += "    glfwSetCharCallback(window, [](GLFWwindow*, unsigned int codepoint){\n";
    content += "        doriax::Engine::systemCharInput(codepoint);\n";
    content += "    });\n\n";
    content += "    int cur_width, cur_height;\n";
    content += "    glfwGetFramebufferSize(window, &cur_width, &cur_height);\n\n";
    content += "    PlatformEditor::screenWidth = cur_width;\n";
    content += "    PlatformEditor::screenHeight = cur_height;\n\n";
    content += "    doriax::Engine::systemViewLoaded();\n";
    content += "    doriax::Engine::systemViewChanged();\n\n";
    content += "    /* draw loop */\n";
    content += "    while (!glfwWindowShouldClose(window)) {\n";
    content += "        int cur_width, cur_height;\n";
    content += "        glfwGetFramebufferSize(window, &cur_width, &cur_height);\n\n";
    content += "        if (cur_width != PlatformEditor::screenWidth || cur_height != PlatformEditor::screenHeight){\n";
    content += "            PlatformEditor::screenWidth = cur_width;\n";
    content += "            PlatformEditor::screenHeight = cur_height;\n";
    content += "            doriax::Engine::systemViewChanged();\n";
    content += "        }\n\n";
    content += "        doriax::Engine::systemDraw();\n\n";
    content += "        glfwSwapBuffers(window);\n";
    content += "        glfwPollEvents();\n";
    content += "        pollGamepads();\n";
    content += "    }\n\n";
    content += "    doriax::Engine::systemViewDestroyed();\n";
    content += "    doriax::Engine::systemShutdown();\n";
    content += "    glfwTerminate();\n";
    content += "    return 0;\n";
    content += "}\n\n";
    content += "int PlatformEditor::getScreenWidth(){\n";
    content += "    return PlatformEditor::screenWidth;\n";
    content += "}\n\n";
    content += "int PlatformEditor::getScreenHeight(){\n";
    content += "    return PlatformEditor::screenHeight;\n";
    content += "}\n\n";
    content += "int PlatformEditor::getSampleCount(){\n";
    content += "    return PlatformEditor::sampleCount;\n";
    content += "}\n\n";
    content += "bool PlatformEditor::isFullscreen(){\n";
    content += "    return glfwGetWindowMonitor(window) != nullptr;\n";
    content += "}\n\n";
    content += "void PlatformEditor::requestFullscreen(){\n";
    content += "    if (isFullscreen())\n";
    content += "        return;\n\n";
    content += "    // backup window position and window size\n";
    content += "    glfwGetWindowPos(window, &windowPosX, &windowPosY);\n";
    content += "    glfwGetWindowSize(window, &windowWidth, &windowHeight);\n\n";
    content += "    // get resolution of monitor\n";
    content += "    const GLFWvidmode * mode = glfwGetVideoMode(monitor);\n\n";
    content += "    // switch to full screen\n";
    content += "    glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, 0 );\n";
    content += "}\n\n";
    content += "void PlatformEditor::exitFullscreen(){\n";
    content += "    if (!isFullscreen())\n";
    content += "        return;\n\n";
    content += "    // restore last window size and position\n";
    content += "    glfwSetWindowMonitor(window, nullptr,  windowPosX, windowPosY, windowWidth, windowHeight, 0);\n";
    content += "}\n\n";
    content += "bool PlatformEditor::isWindowMaximized(){\n";
    content += "    return glfwGetWindowAttrib(window, GLFW_MAXIMIZED) == GLFW_TRUE;\n";
    content += "}\n\n";
    content += "void PlatformEditor::maximizeWindow(){\n";
    content += "    // GLFW ignores maximize on fullscreen windows\n";
    content += "    glfwMaximizeWindow(window);\n";
    content += "}\n\n";
    content += "void PlatformEditor::restoreWindow(){\n";
    content += "    glfwRestoreWindow(window);\n";
    content += "}\n\n";
    content += "void PlatformEditor::setWindowSize(int width, int height){\n";
    content += "    if (width < 1 || height < 1)\n";
    content += "        return;\n\n";
    content += "    if (isFullscreen()){\n";
    content += "        // keep the display mode; resize the window that exitFullscreen restores\n";
    content += "        windowWidth = width;\n";
    content += "        windowHeight = height;\n";
    content += "        return;\n";
    content += "    }\n\n";
    content += "    glfwSetWindowSize(window, width, height);\n";
    content += "}\n\n";
    content += "bool PlatformEditor::isWindowResizable(){\n";
    content += "    return glfwGetWindowAttrib(window, GLFW_RESIZABLE) == GLFW_TRUE;\n";
    content += "}\n\n";
    content += "void PlatformEditor::setWindowResizable(bool resizable){\n";
    content += "    glfwSetWindowAttrib(window, GLFW_RESIZABLE, resizable ? GLFW_TRUE : GLFW_FALSE);\n";
    content += "}\n\n";
    content += "void PlatformEditor::setWindowTitle(const std::string& title){\n";
    content += "    glfwSetWindowTitle(window, title.c_str());\n";
    content += "}\n\n";
    content += "void PlatformEditor::setMouseCursor(doriax::CursorType type){\n";
    content += "    GLFWcursor* cursor = NULL;\n\n";
    content += "    if (type == doriax::CursorType::ARROW){\n";
    content += "        cursor = glfwCreateStandardCursor(GLFW_ARROW_CURSOR);\n";
    content += "    }else if (type == doriax::CursorType::IBEAM){\n";
    content += "        cursor = glfwCreateStandardCursor(GLFW_IBEAM_CURSOR);\n";
    content += "    }else if (type == doriax::CursorType::CROSSHAIR){\n";
    content += "        cursor = glfwCreateStandardCursor(GLFW_CROSSHAIR_CURSOR);\n";
    content += "    }else if (type == doriax::CursorType::POINTING_HAND){\n";
    content += "        #ifdef GLFW_POINTING_HAND_CURSOR\n";
    content += "        cursor = glfwCreateStandardCursor(GLFW_POINTING_HAND_CURSOR);\n";
    content += "        #else\n";
    content += "        cursor = glfwCreateStandardCursor(GLFW_HAND_CURSOR);\n";
    content += "        #endif\n";
    content += "    }else if (type == doriax::CursorType::RESIZE_EW){\n";
    content += "        #ifdef GLFW_RESIZE_EW_CURSOR\n";
    content += "        cursor = glfwCreateStandardCursor(GLFW_RESIZE_EW_CURSOR);\n";
    content += "        #else\n";
    content += "        cursor = glfwCreateStandardCursor(GLFW_HRESIZE_CURSOR);\n";
    content += "        #endif\n";
    content += "    }else if (type == doriax::CursorType::RESIZE_NS){\n";
    content += "        #ifdef GLFW_RESIZE_NS_CURSOR\n";
    content += "        cursor = glfwCreateStandardCursor(GLFW_RESIZE_NS_CURSOR);\n";
    content += "        #else\n";
    content += "        cursor = glfwCreateStandardCursor(GLFW_VRESIZE_CURSOR);\n";
    content += "        #endif\n";
    content += "    }else if (type == doriax::CursorType::RESIZE_NWSE){\n";
    content += "        #ifdef GLFW_RESIZE_NWSE_CURSOR\n";
    content += "        cursor = glfwCreateStandardCursor(GLFW_RESIZE_NWSE_CURSOR);\n";
    content += "        #else\n";
    content += "        cursor = glfwCreateStandardCursor(GLFW_HRESIZE_CURSOR);\n";
    content += "        #endif\n";
    content += "    }else if (type == doriax::CursorType::RESIZE_NESW){\n";
    content += "        #ifdef GLFW_RESIZE_NESW_CURSOR\n";
    content += "        cursor = glfwCreateStandardCursor(GLFW_RESIZE_NESW_CURSOR);\n";
    content += "        #else\n";
    content += "        cursor = glfwCreateStandardCursor(GLFW_HRESIZE_CURSOR);\n";
    content += "        #endif\n";
    content += "    }else if (type == doriax::CursorType::RESIZE_ALL){\n";
    content += "        #ifdef GLFW_RESIZE_ALL_CURSOR\n";
    content += "        cursor = glfwCreateStandardCursor(GLFW_RESIZE_ALL_CURSOR);\n";
    content += "        #else\n";
    content += "        cursor = glfwCreateStandardCursor(GLFW_HRESIZE_CURSOR);\n";
    content += "        #endif\n";
    content += "    }else if (type == doriax::CursorType::NOT_ALLOWED){\n";
    content += "        #ifdef GLFW_NOT_ALLOWED_CURSOR\n";
    content += "        cursor = glfwCreateStandardCursor(GLFW_NOT_ALLOWED_CURSOR);\n";
    content += "        #else\n";
    content += "        cursor = glfwCreateStandardCursor(GLFW_ARROW_CURSOR);\n";
    content += "        #endif\n";
    content += "    }\n\n";
    content += "    if (cursor) {\n";
    content += "        glfwSetCursor(window, cursor);\n";
    content += "    } else {\n";
    content += "        // Handle error: cursor creation failed\n";
    content += "    }\n";
    content += "}\n\n";
    content += "void PlatformEditor::setMouseMode(doriax::MouseMode mode){\n";
    content += "    switch (mode){\n";
    content += "        case doriax::MouseMode::NORMAL:\n";
    content += "            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);\n";
    content += "            break;\n";
    content += "        case doriax::MouseMode::HIDDEN:\n";
    content += "            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);\n";
    content += "            break;\n";
    content += "        case doriax::MouseMode::CAPTURED:\n";
    content += "            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);\n";
    content += "            break;\n";
    content += "        case doriax::MouseMode::CONFINED:\n";
    content += "            // Visible cursor trapped inside the window (GLFW 3.4+).\n";
    content += "#ifdef GLFW_CURSOR_CAPTURED\n";
    content += "            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_CAPTURED);\n";
    content += "#else\n";
    content += "            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);\n";
    content += "#endif\n";
    content += "            break;\n";
    content += "    }\n";
    content += "}\n\n";
    content += "void PlatformEditor::setMousePosition(float x, float y){\n";
    content += "    float xscale, yscale;\n";
    content += "    glfwGetWindowContentScale(window, &xscale, &yscale);\n\n";
    content += "    mousePosX = x;\n";
    content += "    mousePosY = y;\n";
    content += "    glfwSetCursorPos(window, x / xscale, y / yscale);\n";
    content += "}\n\n";
    content += "std::string PlatformEditor::getAssetPath(){\n";
    content += "    return \"" + assetsPath.generic_string() + "\";\n";
    content += "}\n\n";
    content += "std::string PlatformEditor::getUserDataPath(){\n";
    content += "    return \".\";\n";
    content += "}\n\n";
    content += "std::string PlatformEditor::getLuaPath(){\n";
    content += "    return \"" + luaPath.generic_string() + "\";\n";
    content += "}\n\n";
    content += "std::string PlatformEditor::getShaderPath(){\n";
    content += "    return \"" + App::getUserShaderCacheDir().string() + "\";\n";
    content += "}\n";
    return content;
}

std::vector<editor::CMakeKit> editor::Generator::detectAvailableKits() {
    std::vector<CMakeKit> kits;

    auto runCmd = [](const std::string& cmd) -> std::string {
#ifdef _WIN32
        return CommandRunner::runCaptureNoWindow(cmd + " 2>nul");
#else
        FILE* pipe = popen((cmd + " 2>/dev/null").c_str(), "r");
        if (!pipe) return "";
        std::string result;
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe)) {
            result += buffer;
        }
        pclose(pipe);
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' '))
            result.pop_back();
        return result;
#endif
    };

    auto findCompiler = [&runCmd](const std::string& compiler) -> std::string {
#ifdef _WIN32
        std::string result = runCmd("where " + compiler);
#else
        std::string result = runCmd("which " + compiler);
#endif
        size_t nl = result.find('\n');
        if (nl != std::string::npos) result = result.substr(0, nl);
        while (!result.empty() && (result.back() == '\r' || result.back() == ' '))
            result.pop_back();
        return result;
    };

#ifdef _WIN32
    // The editor and the engine library it links (doriax.lib/.dll) are built with a
    // single C++ toolchain. A plugin compiled for a different C++ ABI cannot link
    // against that library, and would crash even if it did: MSVC and MinGW/GNU
    // disagree on name mangling, class layout, RTTI, exception handling and STL
    // types. Flag kits whose ABI does not match this editor's build so they cannot
    // be selected (otherwise the mismatch surfaces only as a cryptic wall of
    // "undefined reference" at link time). This is a Windows-only concern; other
    // platforms have a single system C++ ABI.
    enum class CxxAbi { MSVC, GNU };
#if defined(__MINGW32__)
    const CxxAbi editorAbi = CxxAbi::GNU;
    const char* editorAbiName = "MinGW/GNU";
#else
    const CxxAbi editorAbi = CxxAbi::MSVC;
    const char* editorAbiName = "MSVC";
#endif
    auto enforceAbi = [&](CMakeKit& kit, CxxAbi kitAbi, const char* kitAbiName) {
        if (kitAbi != editorAbi) {
            kit.available = false;
            kit.unavailableReason = std::string("incompatible C++ ABI (kit is ") + kitAbiName
                + ", this editor is " + editorAbiName + "): build plugins with the editor's "
                "toolchain, or rebuild the editor with this compiler";
        }
    };
#endif

    // --- GCC ---
    {
        std::string cxxPath = findCompiler("g++");
        if (!cxxPath.empty()) {
            std::string ccPath = findCompiler("gcc");
            std::string version = runCmd("\"" + cxxPath + "\" -dumpfullversion -dumpversion");
            std::string machine = runCmd("\"" + cxxPath + "\" -dumpmachine");

            CMakeKit kit;
            kit.cxxCompiler = cxxPath;
            kit.cCompiler = ccPath;
            kit.displayName = "GCC " + version;
            if (!machine.empty()) kit.displayName += " " + machine;
#ifdef _WIN32
            kit.generator = "MinGW Makefiles";
            enforceAbi(kit, CxxAbi::GNU, "MinGW/GNU");
#endif
            kits.push_back(kit);
        }
    }

    // --- Clang ---
    {
        std::string cxxPath = findCompiler("clang++");
        if (!cxxPath.empty()) {
            std::string ccPath = findCompiler("clang");
            std::string versionOutput = runCmd("\"" + cxxPath + "\" --version");
            std::string version;
            size_t nl = versionOutput.find('\n');
            std::string firstLine = (nl != std::string::npos) ? versionOutput.substr(0, nl) : versionOutput;
            size_t vpos = firstLine.find("version ");
            if (vpos != std::string::npos) {
                vpos += 8;
                size_t vend = firstLine.find_first_of(" (", vpos);
                version = (vend != std::string::npos) ? firstLine.substr(vpos, vend - vpos) : firstLine.substr(vpos);
            }
            std::string machine = runCmd("\"" + cxxPath + "\" -dumpmachine");

            CMakeKit kit;
            kit.cxxCompiler = cxxPath;
            kit.cCompiler = ccPath;
            kit.displayName = "Clang " + version;
            if (!machine.empty()) kit.displayName += " " + machine;
#ifdef _WIN32
            // Determine generator from the target triple.
            // MinGW-targeting Clang uses MinGW Makefiles;
            // MSVC-targeting Clang requires Ninja: the Visual Studio
            // generator ignores CMAKE_C/CXX_COMPILER (MSBuild always drives
            // cl.exe), so without Ninja this kit has no usable generator.
            if (machine.find("mingw") != std::string::npos) {
                kit.generator = "MinGW Makefiles";
            } else {
                std::string ninjaPath = findCompiler("ninja");
                // Verify ninja actually executes: a broken binary (e.g. a pip
                // wrapper copied out of its Scripts folder, or a blocked
                // download) passes the PATH check but fails when CMake runs it.
                if (!ninjaPath.empty() && !runCmd("\"" + ninjaPath + "\" --version").empty()) {
                    kit.generator = "Ninja";
                } else if (ninjaPath.empty()) {
                    kit.available = false;
                    kit.unavailableReason = "requires Ninja on PATH (https://ninja-build.org)";
                } else {
                    kit.available = false;
                    kit.unavailableReason = "Ninja at '" + ninjaPath + "' failed to run; reinstall from https://ninja-build.org";
                }
            }
            // ABI follows the target triple: MinGW triples are GNU ABI; everything
            // else (e.g. x86_64-pc-windows-msvc) is MSVC ABI and can link an
            // MSVC-built engine. This is why Clang, unlike GCC, can target either.
            {
                CxxAbi clangAbi = (machine.find("mingw") != std::string::npos) ? CxxAbi::GNU : CxxAbi::MSVC;
                enforceAbi(kit, clangAbi, (clangAbi == CxxAbi::GNU) ? "MinGW/GNU" : "MSVC");
            }
#endif
            kits.push_back(kit);
        }
    }

#ifdef _WIN32
    // --- MSVC ---
    {
        std::string vswhere = CommandRunner::findVswherePath();
        std::string vsName;
        if (!vswhere.empty()) {
            vsName = runCmd("\"" + vswhere + "\" -products * -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property displayName");
        }
        // cl.exe on PATH (e.g. a Developer Command Prompt) also signals MSVC is
        // available even if vswhere cannot be located.
        bool clOnPath = !findCompiler("cl").empty();
        if (!vsName.empty() || clOnPath) {
            CMakeKit kit;
            kit.displayName = vsName.empty() ? "MSVC" : vsName;
            // Leave compiler and generator empty: CMake selects the MSVC toolchain
            // (cl.exe) on its own. Passing an explicit CMAKE_CXX_COMPILER with no
            // generator would be rejected by configureCMake's guard, since the
            // Visual Studio generator ignores it (MSBuild always drives cl.exe).
            enforceAbi(kit, CxxAbi::MSVC, "MSVC");
            kits.push_back(kit);
        }
    }
#endif

    return kits;
}

std::string editor::Generator::checkBuildTools() {
    std::string missing;

#ifdef _WIN32
    auto commandExists = [](const char* cmd) -> bool {
        // `where` prints the path when found, nothing when not. Use the no-window
        // runner so this probe doesn't flash a cmd.exe window.
        return !CommandRunner::runCaptureNoWindow(std::string("where ") + cmd + " 2>nul").empty();
    };
#else
    auto commandExists = [](const char* cmd) -> bool {
        std::string check = std::string("command -v ") + cmd + " >/dev/null 2>&1";
        return system(check.c_str()) == 0;
    };
#endif

    if (!commandExists("cmake")) {
#ifdef _WIN32
        missing += "- CMake: not found. Download from https://cmake.org/download/ and ensure it is added to PATH during installation.\n";
#elif defined(__APPLE__)
        missing += "- CMake: not found. Install with: brew install cmake\n";
#else
        missing += "- CMake: not found. Install with: sudo apt install cmake (Debian/Ubuntu) or sudo dnf install cmake (Fedora).\n";
#endif
    }

    bool hasCompiler = false;
#ifdef _WIN32
    hasCompiler = commandExists("cl") || commandExists("g++") || commandExists("clang++") || CommandRunner::hasVSWithCppTools();
#elif defined(__APPLE__)
    hasCompiler = commandExists("clang++") || commandExists("g++");
#else
    hasCompiler = commandExists("g++") || commandExists("clang++");
#endif

    if (!hasCompiler) {
#ifdef _WIN32
        missing += "- C++ compiler: not found. Install Visual Studio (https://visualstudio.microsoft.com/) and select the \"Desktop development with C++\" workload.\n";
#elif defined(__APPLE__)
        missing += "- C++ compiler: not found. Install Xcode Command Line Tools by running: xcode-select --install\n";
#else
        missing += "- C++ compiler (g++ or clang++): not found. Install with: sudo apt install build-essential (Debian/Ubuntu) or sudo dnf install gcc-c++ (Fedora).\n";
#endif
    }

    return missing;
}

void editor::Generator::build(const fs::path projectPath, const fs::path projectInternalPath, const fs::path buildPath, const std::string& cCompiler, const std::string& cxxCompiler, const std::string& generator, unsigned int parallelJobs) {
    cancelBuild();
    waitForBuildToComplete();

    lastBuildSucceeded.store(false, std::memory_order_relaxed);
    commandRunner.resetCancel();

    buildFuture = std::async(std::launch::async, [this, projectPath, buildPath, cCompiler, cxxCompiler, generator, parallelJobs]() {
        try {
            auto startTime = std::chrono::steady_clock::now();

            // The compiled plugin is loaded into the running editor and shares its
            // engine library, CRT and STL across the shared-library boundary, so it
            // must use the same build configuration. On Windows especially, a Debug
            // plugin (/MDd, _ITERATOR_DEBUG_LEVEL=2) loaded by a Release editor (/MD)
            // has incompatible std::vector/std::string layouts and crashes the moment
            // the editor calls into it. Match the editor's own configuration; _DEBUG
            // is defined iff this editor was built against the debug CRT.
            std::string configType = "Debug";
#if defined(_WIN32) && !defined(_DEBUG)
            configType = "Release";
#endif

            // Resolve a "Default" selection to a concrete ABI-compatible kit once,
            // so configure and the build step (which both need the generator, e.g.
            // for the vcvars prefix) agree on the same toolchain.
            std::string cc = cCompiler, cxx = cxxCompiler, gen = generator;
            resolveDefaultKit(cc, cxx, gen);

            // Resolve automatic mode and the per-machine safety cap once. The
            // exact same value must drive both MSVC /MP and cmake --parallel.
            const unsigned int requestedOrAutomaticJobs = parallelJobs == 0
                ? getAutomaticParallelBuildJobs()
                : parallelJobs;
            const unsigned int effectiveParallelJobs =
                std::min(requestedOrAutomaticJobs, getMaxParallelBuildJobs());

            if (!configureCMake(projectPath, buildPath, configType, cc, cxx, gen, effectiveParallelJobs)) {
                if (commandRunner.isCancelRequested()) {
                    Out::warning("Build configuration cancelled.");
                } else {
                    Out::error("CMake configuration failed");
                }
                lastBuildSucceeded.store(false, std::memory_order_relaxed);
                return;
            }

            if (!buildProject(projectPath, buildPath, configType, gen, effectiveParallelJobs)) {
                if (commandRunner.isCancelRequested()) {
                    Out::warning("Build cancelled.");
                } else {
                    Out::error("Build failed");
                }
                lastBuildSucceeded.store(false, std::memory_order_relaxed);
                return;
            }

            auto endTime = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
            double seconds = duration.count() / 1000.0;
            Out::build("Build completed successfully in %.2f seconds.", seconds);
            lastBuildSucceeded.store(true, std::memory_order_relaxed);
        } catch (const std::exception& ex) {
            Out::error("Build exception: %s", ex.what());
            lastBuildSucceeded.store(false, std::memory_order_relaxed);
        }
    });
}

bool editor::Generator::isBuildInProgress() const {
    return buildFuture.valid() &&
           buildFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

void editor::Generator::waitForBuildToComplete() {
    if (buildFuture.valid()) {
        buildFuture.wait();
    }
}

bool editor::Generator::didLastBuildSucceed() const {
    return lastBuildSucceeded.load(std::memory_order_relaxed);
}

std::future<void> editor::Generator::cancelBuild() {
    // Launch cancellation on a separate thread so the UI thread is not blocked.
    return std::async(std::launch::async, [this]() {
        // Check if build is in progress inside the async task
        if (!isBuildInProgress()) {
            commandRunner.resetCancel();
            return;
        }

        Out::warning("Cancelling build process...");
        // Attempt to terminate the running process, then wait for the build to complete.
        commandRunner.cancel();
        waitForBuildToComplete();
        commandRunner.resetCancel();
        lastBuildSucceeded.store(false, std::memory_order_relaxed);
        Out::warning("Build process cancelled.");
    });
}
