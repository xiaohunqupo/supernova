#ifndef UI_COMPONENT_H
#define UI_COMPONENT_H

#include "buffer/Buffer.h"
#include "texture/Texture.h"
#include "render/ObjectRender.h"

namespace Supernova{

    struct UIComponent{
        // UI part
        int width = 0;
        int height = 0;

        bool focused = false;

        // Render part
        bool loaded = false;

        Buffer* buffer;
        Buffer* indices;

        unsigned int minBufferCount = 0;
        unsigned int minIndicesCount = 0;

        ObjectRender render;
        std::shared_ptr<ShaderRender> shader;
        std::string shaderProperties;
        int slotVSParams = -1;
        int slotFSParams = -1;

        PrimitiveType primitiveType = PrimitiveType::TRIANGLES;
        unsigned int vertexCount = 0;

        Texture texture;
        Vector4 color = Vector4(1.0, 1.0, 1.0, 1.0); //linear color

        bool needUpdateBuffer = false;
        bool needUpdateTexture = false;
        bool needReload = false;
    };
    
}

#endif //UI_COMPONENT_H