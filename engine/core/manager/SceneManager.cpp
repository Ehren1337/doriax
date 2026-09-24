// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SceneManager.h"
#include "Engine.h"
#include "Scene.h"
#include "Log.h"
#include "subsystem/RenderSystem.h"
#include "thread/ResourceProgress.h"

#include <algorithm>

using namespace doriax;

// seconds before giving up on a loading scene or on a stack that stopped loading
static const double LOADING_SCENE_TIMEOUT = 2.0;
static const double LOADING_STALL_TIMEOUT = 5.0;

std::vector<SceneManager::SceneEntry> SceneManager::entries;
uint32_t SceneManager::currentId = 0;
std::optional<uint32_t> SceneManager::pendingId;
bool SceneManager::pendingBetweenFrames = false;
std::map<uint32_t, Scene*> SceneManager::scenePtrs;

uint32_t SceneManager::loadingSceneId = 0;
float SceneManager::loadingDelay = 0.0f;
SceneManager::LoadingState SceneManager::loadingState = SceneManager::LoadingState::None;
bool SceneManager::loadingSceneShown = false;
bool SceneManager::loadingSceneReady = false;
double SceneManager::loadingStateTime = 0.0;
size_t SceneManager::loadingCount = 0;
double SceneManager::loadingProgressTime = 0.0;

std::vector<uint32_t> SceneManager::buildSceneStackIds(uint32_t id, const std::vector<uint32_t>& sceneIds) {
    std::vector<uint32_t> result;
    result.reserve(sceneIds.size() + 1);

    auto addId = [&result](uint32_t sceneId) {
        if (std::find(result.begin(), result.end(), sceneId) == result.end()) {
            result.push_back(sceneId);
        }
    };

    addId(id);
    for (uint32_t sceneId : sceneIds) {
        addId(sceneId);
    }

    return result;
}

void SceneManager::registerScene(uint32_t id, const std::string& name, std::function<void()> loadFactory) {
    registerScene(id, name, std::move(loadFactory), nullptr, std::vector<uint32_t>{id});
}

void SceneManager::registerScene(uint32_t id, const std::string& name, std::function<void()> loadFactory, const std::vector<uint32_t>& sceneIds) {
    registerScene(id, name, std::move(loadFactory), nullptr, sceneIds);
}

void SceneManager::registerScene(uint32_t id, const std::string& name, std::function<void()> loadFactory, std::initializer_list<uint32_t> sceneIds) {
    registerScene(id, name, std::move(loadFactory), nullptr, std::vector<uint32_t>(sceneIds));
}

void SceneManager::registerScene(uint32_t id, const std::string& name, std::function<void()> loadFactory, std::function<void()> addFactory) {
    registerScene(id, name, std::move(loadFactory), std::move(addFactory), std::vector<uint32_t>{id});
}

void SceneManager::registerScene(uint32_t id, const std::string& name, std::function<void()> loadFactory, std::function<void()> addFactory, const std::vector<uint32_t>& sceneIds) {
    std::vector<uint32_t> stackSceneIds = buildSceneStackIds(id, sceneIds);

    // Overwrite if the id already exists
    for (auto& entry : entries) {
        if (entry.id == id) {
            entry.name = name;
            entry.loadFactory = std::move(loadFactory);
            entry.addFactory = std::move(addFactory);
            entry.sceneIds = std::move(stackSceneIds);
            return;
        }
    }
    entries.push_back({id, name, std::move(loadFactory), std::move(addFactory), std::move(stackSceneIds)});
}

bool SceneManager::loadScene(const std::string& name) {
    for (int i = 0; i < (int)entries.size(); ++i) {
        if (entries[i].name == name) {
            return loadScene(entries[i].id);
        }
    }
    Log::error("SceneManager: scene '%s' not found", name.c_str());
    return false;
}

SceneManager::SceneEntry* SceneManager::findEntry(uint32_t id) {
    for (auto& entry : entries) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

void SceneManager::runFactory(uint32_t id) {
    SceneEntry* entry = findEntry(id);
    if (!entry) return;

    // Copied for the same reason as in addChildScene: the factory runs scripts
    std::function<void()> loadFactory = entry->loadFactory;

    std::vector<Scene*> loadingScenes;
    if (loadingSceneShown) {
        loadingScenes = getRunningScenes(loadingSceneId);
    }

    Engine::removeAllScenes();

    // still running, so the factory does not delete them
    for (Scene* scene : loadingScenes) {
        Engine::addSceneLayer(scene);
    }

    currentId = id;
    loadFactory();

    if (loadingSceneShown) {
        raiseLoadingScene();
    }
}

bool SceneManager::loadScene(uint32_t id) {
    if (!findEntry(id)) {
        Log::error("SceneManager: scene id %u not found", id);
        return false;
    }

    // input callbacks run between frames, but still inside the scenes being replaced
    if (Engine::isFrameRunning() || Engine::getMainScene()) {
        if (pendingId && *pendingId != id) {
            Log::warn("SceneManager: scene %u replaces pending scene %u", id, *pendingId);
        }
        pendingId = id;
        pendingBetweenFrames = !Engine::isFrameRunning();
        return true;
    }

    pendingId.reset();
    runFactory(id);
    beginLoading();
    return true;
}

bool SceneManager::isLoadPending() {
    return pendingId.has_value();
}

void SceneManager::applyPendingLoad() {
    if (!pendingId) return;

    uint32_t id = *pendingId;

    if (loadingState != LoadingState::Covering && canShowLoadingScene(id) && addChildScene(loadingSceneId)) {
        raiseLoadingScene();
        loadingSceneShown = true;
        loadingSceneReady = false;
        loadingState = LoadingState::Covering;
        loadingStateTime = Engine::getSystemTime();
        pendingBetweenFrames = false;
        return;
    }

    if (loadingState == LoadingState::Covering) {
        // the switch freezes on the last presented frame, which must show the loading scene
        if (!loadingSceneReady || Engine::getSystemTime() - loadingStateTime < loadingDelay) {
            return;
        }
    } else if (pendingBetweenFrames) {
        // one more update for the scenes the input callback ran in, e.g. to start its sound
        pendingBetweenFrames = false;
        return;
    }

    pendingId.reset();
    runFactory(id);
    beginLoading();
}

std::vector<Scene*> SceneManager::getRunningScenes(uint32_t id) {
    std::vector<Scene*> scenes;
    SceneEntry* entry = findEntry(id);
    if (!entry) return scenes;

    for (uint32_t sceneId : entry->sceneIds) {
        Scene* scene = getScenePtr(sceneId);
        if (scene && Engine::isSceneRunning(scene)) {
            scenes.push_back(scene);
        }
    }

    return scenes;
}

bool SceneManager::canShowLoadingScene(uint32_t id) {
    SceneEntry* loadingEntry = findEntry(loadingSceneId);
    SceneEntry* entry = findEntry(id);
    if (!loadingEntry || !entry) return false;

    // not for a stack that includes it
    for (uint32_t sceneId : loadingEntry->sceneIds) {
        if (std::find(entry->sceneIds.begin(), entry->sceneIds.end(), sceneId) != entry->sceneIds.end()) {
            return false;
        }
    }

    return true;
}

void SceneManager::raiseLoadingScene() {
    for (Scene* scene : getRunningScenes(loadingSceneId)) {
        if (scene == Engine::getMainScene()) continue;
        Engine::removeScene(scene);
        Engine::addSceneLayer(scene);
    }
}

void SceneManager::beginLoading() {
    loadingState = LoadingState::Loading;
    loadingCount = 0;
    loadingProgressTime = Engine::getSystemTime();
}

void SceneManager::getLoadCount(size_t& loaded, size_t& total) {
    loaded = 0;
    total = 0;

    for (Scene* scene : getRunningScenes(currentId)) {
        size_t sceneLoaded = 0;
        size_t sceneTotal = 0;
        scene->getSystem<RenderSystem>()->getLoadCount(sceneLoaded, sceneTotal);
        loaded += sceneLoaded;
        total += sceneTotal;
    }
}

bool SceneManager::setLoadingScene(const std::string& name) {
    if (name.empty()) {
        loadingSceneId = 0;
        return true;
    }

    uint32_t id = getSceneId(name);
    if (id == 0) {
        Log::error("SceneManager: scene '%s' not found", name.c_str());
        return false;
    }

    return setLoadingScene(id);
}

bool SceneManager::setLoadingScene(uint32_t id) {
    if (id != 0 && !findEntry(id)) {
        Log::error("SceneManager: scene id %u not found", id);
        return false;
    }

    loadingSceneId = id;
    return true;
}

uint32_t SceneManager::getLoadingSceneId() {
    return loadingSceneId;
}

void SceneManager::setLoadingDelay(float seconds) {
    loadingDelay = std::max(0.0f, seconds);
}

float SceneManager::getLoadingDelay() {
    return loadingDelay;
}

bool SceneManager::isLoading() {
    return pendingId.has_value() || loadingState != LoadingState::None;
}

float SceneManager::getLoadingProgress() {
    if (pendingId || loadingState == LoadingState::Covering) return 0.0f;
    if (loadingState == LoadingState::None) return 1.0f;

    size_t loaded;
    size_t total;
    getLoadCount(loaded, total);

    return (total > 0) ? (float)loaded / (float)total : 1.0f;
}

void SceneManager::updateLoading() {
    if (loadingState == LoadingState::None) return;

    double now = Engine::getSystemTime();

    if (loadingState == LoadingState::Covering) {
        if (!loadingSceneReady) {
            bool ready = true;
            for (Scene* scene : getRunningScenes(loadingSceneId)) {
                ready = ready && scene->getSystem<RenderSystem>()->isAllLoaded();
            }
            loadingSceneReady = ready || now - loadingStateTime > LOADING_SCENE_TIMEOUT;
        }
        return;
    }

    size_t loaded;
    size_t total;
    getLoadCount(loaded, total);

    if (loaded != loadingCount || ResourceProgress::hasActiveBuilds()) {
        loadingCount = loaded;
        loadingProgressTime = now;
    }

    if (loaded < total) {
        if (now - loadingProgressTime < LOADING_STALL_TIMEOUT) return;
        Log::warn("SceneManager: %zu of %zu drawables of '%s' did not load",
            total - loaded, total, getSceneName(currentId).c_str());
    }

    if (loadingSceneShown) {
        removeChildScene(loadingSceneId);
        loadingSceneShown = false;
    }
    loadingState = LoadingState::None;
}

bool SceneManager::addChildScene(uint32_t id) {
    SceneEntry* entry = findEntry(id);
    if (!entry) {
        Log::error("SceneManager: scene id %u not found", id);
        return false;
    }

    // The factory runs scripts, which can register scenes and reallocate entries
    std::vector<uint32_t> sceneIds = entry->sceneIds;
    std::function<void()> addFactory = entry->addFactory;

    // Even when the scenes exist: a registered pointer does not mean the stack is ready to
    // show. The editor keeps the pointers of a stack it switched away from after tearing its
    // scripts down, and only the factory puts them back. Both factories are idempotent.
    if (addFactory) {
        addFactory();
    }

    for (uint32_t sceneId : sceneIds) {
        if (!getScenePtr(sceneId)) {
            Log::error("SceneManager: scene id %u is not loaded", sceneId);
            return false;
        }
    }

    // In stack order, so the root stays below the layers it owns
    for (uint32_t sceneId : sceneIds) {
        Engine::addSceneLayer(getScenePtr(sceneId));
    }

    return true;
}

bool SceneManager::addChildScene(const std::string& name) {
    uint32_t id = getSceneId(name);
    if (id == 0) {
        Log::error("SceneManager: scene '%s' not found", name.c_str());
        return false;
    }

    return addChildScene(id);
}

bool SceneManager::removeChildScene(uint32_t id) {
    for (const auto& entry : entries) {
        if (entry.id != id) {
            continue;
        }

        bool removedScene = false;
        for (auto it = entry.sceneIds.rbegin(); it != entry.sceneIds.rend(); ++it) {
            Scene* scene = getScenePtr(*it);
            if (!scene || scene == Engine::getMainScene() || !Engine::isSceneRunning(scene)) {
                continue;
            }

            Engine::removeScene(scene);
            removedScene = true;
        }

        return removedScene;
    }

    Log::error("SceneManager: scene id %u not found", id);
    return false;
}

bool SceneManager::removeChildScene(const std::string& name) {
    uint32_t id = getSceneId(name);
    if (id == 0) {
        Log::error("SceneManager: scene '%s' not found", name.c_str());
        return false;
    }

    return removeChildScene(id);
}

uint32_t SceneManager::getSceneId(const std::string& name) {
    for (int i = 0; i < (int)entries.size(); ++i) {
        if (entries[i].name == name) return entries[i].id;
    }
    return 0;
}

std::string SceneManager::getSceneName(uint32_t id) {
    for (int i = 0; i < (int)entries.size(); ++i) {
        if (entries[i].id == id) return entries[i].name;
    }
    return "";
}

std::vector<std::string> SceneManager::getSceneNames() {
    std::vector<std::string> names;
    names.reserve(entries.size());
    for (const auto& entry : entries) {
        names.push_back(entry.name);
    }
    return names;
}

int SceneManager::getSceneCount() {
    return (int)entries.size();
}

uint32_t SceneManager::getCurrentSceneId() {
    return currentId;
}

std::string SceneManager::getCurrentSceneName() {
    return getSceneName(currentId);
}

void SceneManager::clearAll() {
    entries.clear();
    currentId = 0;
    pendingId.reset();
    pendingBetweenFrames = false;
    scenePtrs.clear();

    loadingSceneId = 0;
    loadingDelay = 0.0f;
    loadingState = LoadingState::None;
    loadingSceneShown = false;
}

void SceneManager::setScenePtr(uint32_t id, Scene* scene) {
    if (scene) {
        scenePtrs[id] = scene;
    } else {
        scenePtrs.erase(id);
    }
}

Scene* SceneManager::getScenePtr(uint32_t id) {
    auto it = scenePtrs.find(id);
    return (it != scenePtrs.end()) ? it->second : nullptr;
}

void SceneManager::removeScenePtr(uint32_t id) {
    scenePtrs.erase(id);
}
