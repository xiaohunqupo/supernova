//
// (c) 2026 Eduardo Doria.
//

#ifndef TERRAIN_COMPONENT_H
#define TERRAIN_COMPONENT_H

#define MAX_TERRAINGRID 16
// CDLOD node selection is per-view: index 0 is the main camera (also reused by the
// shadow depth pass), 1..N-1 are render-to-texture cameras (mirror reflections,
// scene captures), each selecting + morphing its own node cut. Extra RTT cameras
// beyond this cap fall back to the main camera's selection (view 0).
#define MAX_TERRAIN_VIEWS 4

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

    struct DORIAX_API TerrainComponent{
        // per-view CDLOD node instance buffers: [view][0 = fullRes, 1 = halfRes].
        // Each view is selected and uploaded independently (once per frame), so a
        // mirror/RTT camera renders complete, correctly-tessellated terrain instead
        // of reusing the main camera's cut.
        InterleavedBuffer nodesbuffer[MAX_TERRAIN_VIEWS][2];

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
        // from nodesEyePos[view] right before the uniform upload (keep this block's
        // memory layout intact — it is uploaded as a contiguous struct).
        Vector3 eyePos;
        float terrainSize = 200;
        float maxHeight = 5;
        float resolution = 32; //int
        float textureBaseTiles = 1; //int
        float textureDetailTiles = 20; //int
        //-----

        // per-view morph origin, paired with each view's node selection
        Vector3 nodesEyePos[MAX_TERRAIN_VIEWS];

        int rootGridSize = 2;
        int levels = 6;

        bool needUpdateTerrain = true;
        bool needUpdateTexture = false;
        bool needUpdateNodesBuffer[MAX_TERRAIN_VIEWS] = {false};
    };
    
}

#endif //TERRAIN_COMPONENT_H
