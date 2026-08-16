// (c) Eduardo Doria Lima
// SPDX-License-Identifier: MIT

#ifndef MIRROR_COMPONENT_H
#define MIRROR_COMPONENT_H

#include "math/Vector3.h"
#include "ecs/Entity.h"

namespace doriax{

    // Planar reflection: turns a flat mesh into a mirror. The engine creates and
    // drives an internal render-to-texture camera as the active camera reflected
    // across the mirror plane (entity world position + normal), binds its
    // framebuffer to the mesh base texture, and samples it projectively. Just add
    // the component and orient the mesh — no camera setup needed.
    // See RenderSystem::updateMirrors / createMirrorCamera.
    struct DORIAX_API MirrorComponent{
        // reflecting surface normal in local space (transformed by the entity
        // world rotation to build the mirror plane). Defaults to +Z to match a
        // Wall mesh (createWall normal). Flip its sign if the reflection clips
        // the wrong side.
        Vector3 normal = Vector3(0, 0, 1);

        // engine-managed internal reflection camera (created on first update,
        // destroyed with the component). Not serialized, not user-facing.
        Entity reflectionCamera = NULL_ENTITY;
    };

}

#endif //MIRROR_COMPONENT_H
