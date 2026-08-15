//
// (c) 2026 Eduardo Doria.
//

#ifndef BUNDLEMANAGER_H
#define BUNDLEMANAGER_H

#include "Export.h"
#include "Entity.h"
#include "EntityHandle.h"
#include <string>
#include <functional>
#include <vector>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace doriax {

    class Scene;

    // BundleManager registers named entity bundles and instantiates/destroys them at runtime.
    // A factory receives (Scene*, Entity root) and must return true on success. Failed
    // factories are not recorded; entities created during the call are rolled back.
    //
    // Entity IDs are scene-local. Resolve an existing root by name in the destination scene:
    //   BundleManager::createBundle("enemies/EnemyShip", mainScene, "spawn");
    //
    // Usage from C++ (standalone generated code):
    //   BundleManager::registerBundle(1, "enemies/EnemyShip", create_bundle_enemies_EnemyShip);
    //   Entity root = BundleManager::createBundle("enemies/EnemyShip", scene);
    //   BundleManager::destroyBundle(scene, root);
    //
    // Usage from Lua:
    //   local root = BundleManager.createBundle("enemies/EnemyShip", scene)
    //   BundleManager.destroyBundle(scene, root)

    class DORIAX_API BundleManager {
    private:
        struct BundleEntry {
            uint32_t id;
            std::string name;
            std::function<bool(Scene*, Entity)> factory;
            std::function<bool(Scene*, Entity)> destroyer;
        };

        struct BundleInstance {
            Entity rootEntity;
            Scene* scene;
            uint32_t bundleId;
            std::vector<Entity> entities; // all entities including root
        };

        static std::vector<BundleEntry> entries;
        static std::vector<BundleInstance> instances;

        static BundleEntry* findEntry(uint32_t id);
        static BundleEntry* findEntry(const std::string& name);
        static bool hasInstance(Scene* scene, Entity root);
        static Entity instantiate(uint32_t id, Scene* scene, Entity root, bool ownedRoot);

    public:
        // The factory must return true on success. Void-returning callables are accepted
        // and treated as always-successful.
        static void registerBundle(uint32_t id, const std::string& name, std::function<bool(Scene*, Entity)> factory, std::function<bool(Scene*, Entity)> destroyer = nullptr);

        template<typename Factory>
        static auto registerBundle(uint32_t id, const std::string& name, Factory&& factory, std::function<bool(Scene*, Entity)> destroyer = nullptr)
            -> std::enable_if_t<std::is_invocable_v<Factory&, Scene*, Entity>, void>
        {
            using Result = std::invoke_result_t<Factory&, Scene*, Entity>;
            if constexpr (std::is_void_v<Result>) {
                registerBundle(id, name, std::function<bool(Scene*, Entity)>(
                    [factory = std::forward<Factory>(factory)](Scene* scene, Entity root) mutable -> bool {
                        factory(scene, root);
                        return true;
                    }), std::move(destroyer));
            } else {
                registerBundle(id, name, std::function<bool(Scene*, Entity)>(
                    [factory = std::forward<Factory>(factory)](Scene* scene, Entity root) mutable -> bool {
                        return static_cast<bool>(factory(scene, root));
                    }), std::move(destroyer));
            }
        }

        static Entity createBundle(const std::string& name, Scene* scene);
        static Entity createBundle(uint32_t id, Scene* scene);

        // Looks up rootName in `scene` before spawning.
        static Entity createBundle(const std::string& name, Scene* scene, const std::string& rootName);
        static Entity createBundle(uint32_t id, Scene* scene, const std::string& rootName);

        // For objects that already carry their own scene (Object, Button, ...)
        static Entity createBundle(const std::string& name, const EntityHandle& root);
        static Entity createBundle(uint32_t id, const EntityHandle& root);

        static Entity createBundle(const std::string& name, Scene* scene, Entity root);
        static Entity createBundle(uint32_t id, Scene* scene, Entity root);

        static bool destroyBundle(Scene* scene, Entity rootEntity);

        static uint32_t getBundleId(const std::string& name);
        static std::string getBundleName(uint32_t id);
        static std::vector<std::string> getBundleNames();
        static int getBundleCount();

        static void destroyAllInstances(Scene* scene);
        static void clearAll();
    };

} // namespace doriax

#endif // BUNDLEMANAGER_H
