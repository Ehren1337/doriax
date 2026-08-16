// (c) Eduardo Doria Lima
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_COMPONENT_H
#define TERRAIN_COMPONENT_H

#define MAX_TERRAINGRID 16
// CDLOD node selection is per-view: index 0 is the main camera (also reused by the
// shadow depth pass), 1..N-1 are render-to-texture cameras (mirror reflections,
// scene captures), each selecting + morphing its own node cut. Extra RTT cameras
// beyond this cap fall back to the main camera's selection (view 0).
#define MAX_TERRAIN_VIEWS 4

// The quadtree materializes rootGridSize^2 * (4^levels - 1)/3 nodes, so "levels"
// grows the node count exponentially. These bounds keep the node vector from
// requesting an impossible allocation (a large "levels" would abort with bad_alloc).
// MAX_TERRAIN_LEVELS also keeps getTerrainGridArraySize's 4^i math within size_t.
#define MAX_TERRAIN_LEVELS 20
#define MAX_TERRAIN_NODES 2000000u

#include "buffer/InterleavedBuffer.h"
#include "buffer/IndexBuffer.h"
#include "texture/Material.h"
#include "Engine.h"

namespace doriax{

    struct TerrainNode{
        //-----u_vs_terrainNodeParams
        Vector2 position = Vector2(0, 0);
        float size = 0;
        float currentRange = 0;
        float resolution = 0; //int
        uint8_t _pad_20[12];
        //-----

        size_t childs[4];
        bool hasChilds = false;

        float maxHeight = 0;
        float minHeight = 0;
        
        float visible = false;
    };

    // Per-view CDLOD state. Index 0 is the main camera (also reused by the shadow
    // depth pass); 1..N-1 are render-to-texture cameras (mirror reflections, scene
    // captures), each selecting and morphing its own node cut. Grouping the per-view
    // buffers in a struct (instead of a 2-D array of InterleavedBuffer) keeps MSVC's
    // code generation from crashing when the ECS pool instantiates TerrainComponent.
    struct TerrainView{
        // 0 = fullRes, 1 = halfRes; selected and uploaded independently once per frame
        InterleavedBuffer nodesbuffer[2];
        // per-view morph origin, paired with this view's node selection
        Vector3 nodesEyePos;
        bool needUpdateNodesBuffer = false;
    };

    struct DORIAX_API TerrainComponent{
        // per-view CDLOD node selection (see TerrainView). Extra RTT cameras beyond
        // MAX_TERRAIN_VIEWS fall back to the main camera's selection (view 0).
        TerrainView views[MAX_TERRAIN_VIEWS];

        Texture heightMap;
        Texture blendMap;
        Texture textureDetailRed;
        Texture textureDetailGreen;
        Texture textureDetailBlue;

        bool autoSetRanges = true;
        bool heightMapLoaded = false;

        Vector2 offset;
        std::vector<float> ranges;

        //using std::vector to avoid chkstk.asm stack overflow error in Windows
        std::vector<TerrainNode> nodes;
        unsigned int numNodes = 0;

        size_t grid[MAX_TERRAINGRID]; //root nodes

        //-----u_vs_terrainParams
        // eyePos is the morph origin for the view currently being drawn; it is set
        // from views[view].nodesEyePos right before the uniform upload (keep this
        // block's memory layout intact — it is uploaded as a contiguous struct).
        Vector3 eyePos;
        float terrainSize = 200;
        float maxHeight = 5;
        float resolution = 32; //int
        float textureBaseTiles = 1; //int
        float textureDetailTiles = 20; //int
        //-----

        int rootGridSize = 2;
        int levels = 6;

        bool needUpdateTerrain = true;
        bool needUpdateTexture = false;
    };
    
}

#endif //TERRAIN_COMPONENT_H
