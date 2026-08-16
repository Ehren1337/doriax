// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "AppSettings.h"
#include "Out.h"
#include <fstream>
#include <algorithm>

namespace doriax::editor {

// Initialize static members
std::filesystem::path AppSettings::configFilePath;
YAML::Node AppSettings::settingsData;
std::vector<std::filesystem::path> AppSettings::recentProjects;
std::filesystem::path AppSettings::lastProjectPath;
std::string AppSettings::lastCMakeCCompiler;
std::string AppSettings::lastCMakeCxxCompiler;
std::string AppSettings::lastCMakeGenerator;
std::string AppSettings::emsdkPath;
int AppSettings::windowWidth = 1280;
int AppSettings::windowHeight = 720;
bool AppSettings::isMaximized = false;
int AppSettings::resourcesIconSize = 32;
int AppSettings::resourcesLayout = 0;
int AppSettings::resourcesItemViewStyle = 1;
float AppSettings::resourcesLeftPanelWidth = 200.0f;
float AppSettings::codeEditorFontSize = AppSettings::defaultCodeEditorFontSize;
bool AppSettings::multiViewportEnabled = false;
PanelVisibilitySettings AppSettings::panelVisibility;
ai::Settings AppSettings::aiSettings;

bool AppSettings::initialize() {
    // Get config file path in the application directory
    configFilePath = std::filesystem::current_path() / "settings.yaml";
    
    ensureConfigDirectory();
    return loadSettings();
}

std::filesystem::path AppSettings::getConfigDirectory() {
    if (configFilePath.empty()) {
        return std::filesystem::current_path();
    }
    return configFilePath.parent_path();
}

void AppSettings::ensureConfigDirectory() {
    std::filesystem::path configDir = configFilePath.parent_path();
    if (!std::filesystem::exists(configDir)) {
        try {
            std::filesystem::create_directories(configDir);
        } catch (const std::exception& e) {
            Out::error("Failed to create config directory: %s", e.what());
        }
    }
}

bool AppSettings::loadSettings() {
    try {
        if (!std::filesystem::exists(configFilePath)) {
            settingsData = YAML::Node();
            return false;
        }
        
        settingsData = YAML::LoadFile(configFilePath.string());
        
        // Load last project path
        if (settingsData["last_project_path"]) {
            std::string path = settingsData["last_project_path"].as<std::string>();
            if (!path.empty() && std::filesystem::exists(path)) {
                lastProjectPath = std::filesystem::path(path);
            }
        }
        
        // Load last compiler kit
        if (settingsData["cmake_kit"]) {
            auto kitNode = settingsData["cmake_kit"];
            if (kitNode["c_compiler"]) lastCMakeCCompiler = kitNode["c_compiler"].as<std::string>();
            if (kitNode["cxx_compiler"]) lastCMakeCxxCompiler = kitNode["cxx_compiler"].as<std::string>();
            if (kitNode["generator"]) lastCMakeGenerator = kitNode["generator"].as<std::string>();
        }

        // Load Emscripten SDK path override
        if (settingsData["emsdk"] && settingsData["emsdk"]["path"]) {
            emsdkPath = settingsData["emsdk"]["path"].as<std::string>();
        }

        // Load recent projects
        recentProjects.clear();
        if (settingsData["recent_projects"]) {
            for (const auto& path : settingsData["recent_projects"]) {
                std::string pathStr = path.as<std::string>();
                if (!pathStr.empty() && std::filesystem::exists(pathStr)) {
                    recentProjects.push_back(std::filesystem::path(pathStr));
                }
            }
        }
        
        // Load window settings
        if (settingsData["window"]) {
            auto windowNode = settingsData["window"];
            if (windowNode["width"]) {
                windowWidth = windowNode["width"].as<int>();
            }
            if (windowNode["height"]) {
                windowHeight = windowNode["height"].as<int>();
            }
            if (windowNode["maximized"]) {
                isMaximized = windowNode["maximized"].as<bool>();
            }
        }
        
        // Load resources window settings
        if (settingsData["resources_window"]) {
            auto resNode = settingsData["resources_window"];
            if (resNode["icon_size"]) {
                resourcesIconSize = resNode["icon_size"].as<int>();
            }
            if (resNode["layout"]) {
                resourcesLayout = resNode["layout"].as<int>();
            }
            if (resNode["item_view_style"]) {
                resourcesItemViewStyle = resNode["item_view_style"].as<int>();
            }
            if (resNode["left_panel_width"]) {
                resourcesLeftPanelWidth = resNode["left_panel_width"].as<float>();
            }
        }

        // Load code editor settings
        if (settingsData["code_editor"]) {
            auto codeNode = settingsData["code_editor"];
            if (codeNode["font_size"]) {
                setCodeEditorFontSize(codeNode["font_size"].as<float>());
            }
        }

        // Load editor viewport settings
        if (settingsData["editor"]) {
            auto editorNode = settingsData["editor"];
            if (editorNode["multi_viewport"]) {
                multiViewportEnabled = editorNode["multi_viewport"].as<bool>();
            }
            if (editorNode["panels"]) {
                auto panelsNode = editorNode["panels"];
                if (panelsNode["structure"]) panelVisibility.structure = panelsNode["structure"].as<bool>();
                if (panelsNode["properties"]) panelVisibility.properties = panelsNode["properties"].as<bool>();
                if (panelsNode["resources"]) panelVisibility.resources = panelsNode["resources"].as<bool>();
                if (panelsNode["output"]) panelVisibility.output = panelsNode["output"].as<bool>();
                if (panelsNode["animation"]) panelVisibility.animation = panelsNode["animation"].as<bool>();
                if (panelsNode["terrain"]) panelVisibility.terrain = panelsNode["terrain"].as<bool>();
                if (panelsNode["ai_chat"]) panelVisibility.aiChat = panelsNode["ai_chat"].as<bool>();
            }
        }

        // Load AI assistant settings (no API keys)
        if (settingsData["ai_assistant"]) {
            auto aiNode = settingsData["ai_assistant"];
            if (aiNode["provider"]) aiSettings.provider = ai::providerFromString(aiNode["provider"].as<std::string>());
            if (aiNode["model"]) aiSettings.model = aiNode["model"].as<std::string>();
            if (aiNode["custom_endpoint"]) aiSettings.customEndpoint = aiNode["custom_endpoint"].as<std::string>();
            if (aiNode["approval_mode"]) aiSettings.approvalMode = ai::approvalModeFromString(aiNode["approval_mode"].as<std::string>());
            if (aiNode["request_timeout_seconds"]) aiSettings.requestTimeoutSeconds = aiNode["request_timeout_seconds"].as<int>();
            if (aiNode["max_output_tokens"]) aiSettings.maxOutputTokens = aiNode["max_output_tokens"].as<int>();
            if (aiNode["max_tool_rounds"]) aiSettings.maxToolRounds = aiNode["max_tool_rounds"].as<int>();
            if (aiSettings.model.empty()) {
                aiSettings.model = ai::defaultModelForProvider(aiSettings.provider);
            }
        }

        return true;
    } catch (const std::exception& e) {
        Out::error("Failed to load settings: %s", e.what());
        return false;
    }
}

bool AppSettings::saveSettings() {
    try {
        // Update settings data
        
        // Project settings
        if (!lastProjectPath.empty() && std::filesystem::exists(lastProjectPath)) {
            settingsData["last_project_path"] = lastProjectPath.string();
        } else {
            settingsData.remove("last_project_path");
        }
        
        YAML::Node recentProjectsNode;
        for (const auto& path : recentProjects) {
            recentProjectsNode.push_back(path.string());
        }
        settingsData["recent_projects"] = recentProjectsNode;

        // Last compiler kit
        if (!lastCMakeCCompiler.empty() || !lastCMakeCxxCompiler.empty() || !lastCMakeGenerator.empty()) {
            YAML::Node kitNode;
            kitNode["c_compiler"] = lastCMakeCCompiler;
            kitNode["cxx_compiler"] = lastCMakeCxxCompiler;
            kitNode["generator"] = lastCMakeGenerator;
            settingsData["cmake_kit"] = kitNode;
        } else {
            settingsData.remove("cmake_kit");
        }

        // Emscripten SDK path override
        if (!emsdkPath.empty()) {
            YAML::Node emsdkNode;
            emsdkNode["path"] = emsdkPath;
            settingsData["emsdk"] = emsdkNode;
        } else {
            settingsData.remove("emsdk");
        }
        
        // Window settings
        YAML::Node windowNode;
        windowNode["width"] = windowWidth;
        windowNode["height"] = windowHeight;
        windowNode["maximized"] = isMaximized;
        settingsData["window"] = windowNode;
        
        // Resources window settings
        YAML::Node resNode;
        resNode["icon_size"] = resourcesIconSize;
        resNode["layout"] = resourcesLayout;
        resNode["item_view_style"] = resourcesItemViewStyle;
        resNode["left_panel_width"] = resourcesLeftPanelWidth;
        settingsData["resources_window"] = resNode;

        // Code editor settings
        YAML::Node codeNode;
        codeNode["font_size"] = codeEditorFontSize;
        settingsData["code_editor"] = codeNode;

        // Editor viewport settings
        YAML::Node editorNode;
        editorNode["multi_viewport"] = multiViewportEnabled;
        YAML::Node panelsNode;
        panelsNode["structure"] = panelVisibility.structure;
        panelsNode["properties"] = panelVisibility.properties;
        panelsNode["resources"] = panelVisibility.resources;
        panelsNode["output"] = panelVisibility.output;
        panelsNode["animation"] = panelVisibility.animation;
        panelsNode["terrain"] = panelVisibility.terrain;
        panelsNode["ai_chat"] = panelVisibility.aiChat;
        editorNode["panels"] = panelsNode;
        settingsData["editor"] = editorNode;

        // AI assistant settings. Secrets must never be serialized here.
        YAML::Node aiNode;
        aiNode["provider"] = ai::toString(aiSettings.provider);
        aiNode["model"] = aiSettings.model;
        aiNode["custom_endpoint"] = aiSettings.customEndpoint;
        aiNode["approval_mode"] = ai::toString(aiSettings.approvalMode);
        aiNode["request_timeout_seconds"] = aiSettings.requestTimeoutSeconds;
        aiNode["max_output_tokens"] = aiSettings.maxOutputTokens;
        aiNode["max_tool_rounds"] = aiSettings.maxToolRounds;
        settingsData["ai_assistant"] = aiNode;

        // Save to file
        std::ofstream fout(configFilePath.string());
        fout << YAML::Dump(settingsData);
        fout.close();
        
        return true;
    } catch (const std::exception& e) {
        Out::error("Failed to save settings: %s", e.what());
        return false;
    }
}

std::filesystem::path AppSettings::getLastProjectPath() {
    return lastProjectPath;
}

void AppSettings::setLastProjectPath(const std::filesystem::path& path) {
    lastProjectPath = path;
    // Also add to recent projects
    addToRecentProjects(path, false);

    saveSettings();
}

std::string AppSettings::getLastCMakeCCompiler() {
    return lastCMakeCCompiler;
}

std::string AppSettings::getLastCMakeCxxCompiler() {
    return lastCMakeCxxCompiler;
}

std::string AppSettings::getLastCMakeGenerator() {
    return lastCMakeGenerator;
}

void AppSettings::setLastCMakeKit(const std::string& cCompiler, const std::string& cxxCompiler, const std::string& generator) {
    lastCMakeCCompiler = cCompiler;
    lastCMakeCxxCompiler = cxxCompiler;
    lastCMakeGenerator = generator;
    saveSettings();
}

std::string AppSettings::getEmsdkPath() {
    return emsdkPath;
}

void AppSettings::setEmsdkPath(const std::string& path) {
    emsdkPath = path;
    saveSettings();
}

std::vector<std::filesystem::path> AppSettings::getRecentProjects() {
    return recentProjects;
}

void AppSettings::addToRecentProjects(const std::filesystem::path& path, bool needSave) {
    if (path.empty() || !std::filesystem::exists(path)) {
        return;
    }
    
    // Check if path already exists
    auto it = std::find_if(recentProjects.begin(), recentProjects.end(),
        [&path](const std::filesystem::path& p) {
            return p == path;
        });
    
    // If found, remove it (to add it to the front)
    if (it != recentProjects.end()) {
        recentProjects.erase(it);
    }
    
    // Add to the front
    recentProjects.insert(recentProjects.begin(), path);
    
    // Keep only the most recent 10 projects
    if (recentProjects.size() > 10) {
        recentProjects.resize(10);
    }
    
    // Save changes
    if (needSave)
        saveSettings();
}

void AppSettings::clearRecentProjects() {
    recentProjects.clear();
    saveSettings();
}

int AppSettings::getWindowWidth() {
    return windowWidth;
}

int AppSettings::getWindowHeight() {
    return windowHeight;
}

bool AppSettings::getIsMaximized() {
    return isMaximized;
}

void AppSettings::setWindowWidth(int width) {
    windowWidth = width;
}

void AppSettings::setWindowHeight(int height) {
    windowHeight = height;
}

void AppSettings::setIsMaximized(bool maximized) {
    isMaximized = maximized;
}

int AppSettings::getResourcesIconSize() {
    return resourcesIconSize;
}

int AppSettings::getResourcesLayout() {
    return resourcesLayout;
}

int AppSettings::getResourcesItemViewStyle() {
    return resourcesItemViewStyle;
}

float AppSettings::getResourcesLeftPanelWidth() {
    return resourcesLeftPanelWidth;
}

void AppSettings::setResourcesIconSize(int size) {
    resourcesIconSize = size;
}

void AppSettings::setResourcesLayout(int layout) {
    resourcesLayout = layout;
}

void AppSettings::setResourcesItemViewStyle(int style) {
    resourcesItemViewStyle = style;
}

void AppSettings::setResourcesLeftPanelWidth(float width) {
    resourcesLeftPanelWidth = width;
}

float AppSettings::getCodeEditorFontSize() {
    return codeEditorFontSize;
}

void AppSettings::setCodeEditorFontSize(float size) {
    codeEditorFontSize = std::clamp(size, minCodeEditorFontSize, maxCodeEditorFontSize);
}

bool AppSettings::getMultiViewportEnabled() {
    return multiViewportEnabled;
}

void AppSettings::setMultiViewportEnabled(bool enabled) {
    multiViewportEnabled = enabled;
}

PanelVisibilitySettings AppSettings::getPanelVisibility() {
    return panelVisibility;
}

void AppSettings::setPanelVisibility(const PanelVisibilitySettings& visibility) {
    panelVisibility = visibility;
    saveSettings();
}

ai::Settings AppSettings::getAiSettings() {
    return aiSettings;
}

void AppSettings::setAiSettings(const ai::Settings& settings) {
    aiSettings = settings;
    if (aiSettings.model.empty()) {
        aiSettings.model = ai::defaultModelForProvider(aiSettings.provider);
    }
    saveSettings();
}

} // namespace doriax::editor
