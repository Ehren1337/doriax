// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "BundleManager.h"
#include "Scene.h"
#include "EntityHandle.h"
#include "Log.h"

#include <exception>
#include <unordered_set>

using namespace doriax;

std::vector<BundleManager::BundleEntry> BundleManager::entries;
std::vector<BundleManager::BundleInstance> BundleManager::instances;

namespace {

void rollbackSpawnedEntities(Scene* scene, const std::unordered_set<Entity>& beforeSet, Entity root, bool destroyRoot) {
    if (!scene)
        return;

    std::vector<Entity> afterEntities = scene->getEntityList();
    for (auto it = afterEntities.rbegin(); it != afterEntities.rend(); ++it) {
        Entity e = *it;
        if (e != root && beforeSet.find(e) == beforeSet.end() && scene->isEntityCreated(e))
            scene->destroyEntity(e);
    }

    if (destroyRoot && root != NULL_ENTITY && scene->isEntityCreated(root))
        scene->destroyEntity(root);
}

}

BundleManager::BundleEntry* BundleManager::findEntry(uint32_t id) {
    for (auto& entry : entries) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

BundleManager::BundleEntry* BundleManager::findEntry(const std::string& name) {
    for (auto& entry : entries) {
        if (entry.name == name)
            return &entry;
    }
    return nullptr;
}

void BundleManager::registerBundle(uint32_t id, const std::string& name, std::function<bool(Scene*, Entity)> factory, std::function<bool(Scene*, Entity)> destroyer) {
    if (BundleEntry* entry = findEntry(id)) {
        entry->name = name;
        entry->factory = std::move(factory);
        entry->destroyer = std::move(destroyer);
        return;
    }
    entries.push_back({id, name, std::move(factory), std::move(destroyer)});
}

bool BundleManager::hasInstance(Scene* scene, Entity root) {
    for (const auto& inst : instances) {
        if (inst.scene == scene && inst.rootEntity == root)
            return true;
    }
    return false;
}

Entity BundleManager::createBundle(const std::string& name, Scene* scene) {
    BundleEntry* entry = findEntry(name);
    if (!entry) {
        Log::error("BundleManager: bundle '%s' not found", name.c_str());
        return NULL_ENTITY;
    }
    return createBundle(entry->id, scene);
}

Entity BundleManager::createBundle(uint32_t id, Scene* scene) {
    if (!scene) {
        Log::error("BundleManager: scene is null");
        return NULL_ENTITY;
    }
    if (!findEntry(id)) {
        Log::error("BundleManager: bundle id %u not found", id);
        return NULL_ENTITY;
    }
    return instantiate(id, scene, scene->createEntity(), true);
}

Entity BundleManager::createBundle(const std::string& name, Scene* scene, const std::string& rootName) {
    BundleEntry* entry = findEntry(name);
    if (!entry) {
        Log::error("BundleManager: bundle '%s' not found", name.c_str());
        return NULL_ENTITY;
    }
    return createBundle(entry->id, scene, rootName);
}

Entity BundleManager::createBundle(uint32_t id, Scene* scene, const std::string& rootName) {
    if (!scene) {
        Log::error("BundleManager: scene is null");
        return NULL_ENTITY;
    }
    Entity root = scene->findEntity(rootName);
    if (root == NULL_ENTITY) {
        Log::error("BundleManager: root entity '%s' not found in the given scene", rootName.c_str());
        return NULL_ENTITY;
    }
    return instantiate(id, scene, root, false);
}

Entity BundleManager::createBundle(const std::string& name, Scene* scene, Entity root) {
    BundleEntry* entry = findEntry(name);
    if (!entry) {
        Log::error("BundleManager: bundle '%s' not found", name.c_str());
        return NULL_ENTITY;
    }
    return instantiate(entry->id, scene, root, false);
}

Entity BundleManager::createBundle(uint32_t id, Scene* scene, Entity root) {
    return instantiate(id, scene, root, false);
}

Entity BundleManager::createBundle(const std::string& name, const EntityHandle& root) {
    BundleEntry* entry = findEntry(name);
    if (!entry) {
        Log::error("BundleManager: bundle '%s' not found", name.c_str());
        return NULL_ENTITY;
    }
    return instantiate(entry->id, root.getScene(), root.getEntity(), false);
}

Entity BundleManager::createBundle(uint32_t id, const EntityHandle& root) {
    return instantiate(id, root.getScene(), root.getEntity(), false);
}

Entity BundleManager::instantiate(uint32_t id, Scene* scene, Entity root, bool ownedRoot) {
    if (!scene) {
        Log::error("BundleManager: scene is null");
        return NULL_ENTITY;
    }
    if (root == NULL_ENTITY || !scene->isEntityCreated(root)) {
        Log::error("BundleManager: root entity %u does not exist in the given scene", root);
        return NULL_ENTITY;
    }
    if (hasInstance(scene, root)) {
        Log::error("BundleManager: root entity %u already has a bundle instance in this scene", root);
        if (ownedRoot)
            scene->destroyEntity(root);
        return NULL_ENTITY;
    }

    BundleEntry* entry = findEntry(id);
    if (!entry) {
        Log::error("BundleManager: bundle id %u not found", id);
        if (ownedRoot)
            scene->destroyEntity(root);
        return NULL_ENTITY;
    }

    std::vector<Entity> beforeEntities = scene->getEntityList();
    std::unordered_set<Entity> beforeSet(beforeEntities.begin(), beforeEntities.end());

    bool ok = false;
    try {
        ok = entry->factory && entry->factory(scene, root);
    } catch (const std::exception& e) {
        Log::error("BundleManager: factory for bundle id %u threw an exception: %s", id, e.what());
    } catch (...) {
        Log::error("BundleManager: factory for bundle id %u threw an exception", id);
    }

    if (!ok) {
        Log::error("BundleManager: factory failed for bundle id %u", id);
        rollbackSpawnedEntities(scene, beforeSet, root, ownedRoot);
        return NULL_ENTITY;
    }

    BundleInstance instance;
    instance.rootEntity = root;
    instance.scene = scene;
    instance.bundleId = id;
    instance.entities.push_back(root);
    for (Entity e : scene->getEntityList()) {
        if (e != root && beforeSet.find(e) == beforeSet.end())
            instance.entities.push_back(e);
    }

    instances.push_back(std::move(instance));
    return root;
}

bool BundleManager::destroyBundle(Scene* scene, Entity rootEntity) {
    for (auto it = instances.begin(); it != instances.end(); ++it) {
        if (it->scene == scene && it->rootEntity == rootEntity) {
            uint32_t bundleId = it->bundleId;

            for (auto& entry : entries) {
                if (entry.id == bundleId && entry.destroyer) {
                    bool result = entry.destroyer(scene, rootEntity);
                    instances.erase(it);
                    return result;
                }
            }

            for (auto eit = it->entities.rbegin(); eit != it->entities.rend(); ++eit) {
                if (scene->isEntityCreated(*eit))
                    scene->destroyEntity(*eit);
            }
            instances.erase(it);
            return true;
        }
    }
    Log::error("BundleManager: bundle instance with root %u not found in scene", rootEntity);
    return false;
}

uint32_t BundleManager::getBundleId(const std::string& name) {
    BundleEntry* entry = findEntry(name);
    return entry ? entry->id : 0;
}

std::string BundleManager::getBundleName(uint32_t id) {
    BundleEntry* entry = findEntry(id);
    return entry ? entry->name : "";
}

std::vector<std::string> BundleManager::getBundleNames() {
    std::vector<std::string> names;
    names.reserve(entries.size());
    for (const auto& entry : entries)
        names.push_back(entry.name);
    return names;
}

int BundleManager::getBundleCount() {
    return (int)entries.size();
}

void BundleManager::destroyAllInstances(Scene* scene) {
    std::vector<Entity> roots;
    for (const auto& inst : instances) {
        if (inst.scene == scene)
            roots.push_back(inst.rootEntity);
    }
    for (Entity root : roots)
        destroyBundle(scene, root);
}

void BundleManager::clearAll() {
    entries.clear();
    instances.clear();
}
