// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MESHLODPOOL_H
#define MESHLODPOOL_H

#include "Export.h"
#include "Engine.h"
#include "render/Render.h"
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace doriax{

    struct MeshLodLevel{
        unsigned int indexBase = 0;
        unsigned int indexCount = 0;
        float error = 0; // deviation from the source, in mesh units
    };

    // simplified index ranges of one submesh over its own vertex buffer
    struct MeshLodData{
        std::vector<unsigned char> indices; // packed in the source index type
        AttributeDataType indexType = AttributeDataType::UNSIGNED_SHORT;
        MeshLodLevel levels[MAX_MESH_LODS]; // level 0 is the source and stays empty
        unsigned int numLevels = 1;
    };

    struct MeshLodSource{
        std::vector<float> positions;  // xyz
        std::vector<float> attributes; // normal xyz + uv, when present
        unsigned int attributeCount = 0;
        std::vector<uint32_t> indices;
        AttributeDataType indexType = AttributeDataType::UNSIGNED_SHORT;
    };

    typedef std::unordered_map< uint64_t, std::shared_future<std::shared_ptr<MeshLodData>> > lods_t;

    // Detail levels keyed by a hash of the geometry, built on the thread pool
    class DORIAX_API MeshLodPool{
    private:
        static lods_t& getMap();
        static std::mutex& getMutex();

        static std::shared_ptr<MeshLodData> simplify(const MeshLodSource& source);

    public:
        static uint64_t hash(uint64_t seed, const void* data, size_t size);

        static std::shared_ptr<MeshLodData> get(uint64_t key); // null while a build is pending
        static bool has(uint64_t key);
        static void build(uint64_t key, MeshLodSource&& source);

        // necessary for engine shutdown
        static void clear();
        // removes only entries no one else references (safe while other scenes are alive)
        static void clearUnused();
    };
}

#endif //MESHLODPOOL_H
