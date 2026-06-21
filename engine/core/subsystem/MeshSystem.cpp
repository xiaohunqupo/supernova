//
// (c) 2026 Eduardo Doria.
//

#include "MeshSystem.h"

#include "Scene.h"
#include "Engine.h"
#include "buffer/InterleavedBuffer.h"
#include "io/FileData.h"
#include "io/Data.h"
#include "pool/ModelPool.h"
#include "thread/ResourceProgress.h"
#include "thread/ThreadPoolManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sstream>
#include <thread>
#include <utility>
#include "tiny_obj_loader.h"
#include "tiny_gltf.h"
#include "stb_image_resize2.h"
#include "pool/ObjModelData.h"

using namespace doriax;

namespace {
    thread_local int g_imageDecodeMaxDimension = 0;
}

struct MeshSystem::AsyncModelLoadResult {
    bool obj = false;
    bool success = false;
    std::string filename;
    std::string warn;
    std::string err;
    std::shared_ptr<tinygltf::Model> gltfModel;
    std::shared_ptr<ObjModelData> objModel;
    // Texture pixels copied on the worker thread (id -> pooled face array), handed to
    // TextureDataPool on the main thread so the heavy memcpy stays off it.
    std::vector<std::pair<std::string, std::shared_ptr<std::array<TextureData,6>>>> prebuiltTextures;
};

std::mutex& MeshSystem::getAsyncModelMutex(){
    static std::mutex* mutex = new std::mutex();
    return *mutex;
}

MeshSystem::async_model_loads_t& MeshSystem::getPendingModelLoads(){
    static async_model_loads_t* loads = new async_model_loads_t();
    return *loads;
}

std::string MeshSystem::getModelFilenameKey(const std::string& filename){
    // FileData resolves project-relative and asset:// paths exactly as the file
    // loader does. This keeps the model/texture pools from treating the same
    // file as separate absolute and relative resources.
    return std::filesystem::path(FileData::getSystemPath(filename)).lexically_normal().generic_string();
}

MeshSystem::MeshSystem(Scene* scene): SubSystem(scene){
    signature.set(scene->getComponentId<MeshComponent>());
}

MeshSystem::~MeshSystem(){
    cancelAsyncModelLoads();
}

void MeshSystem::setImageDecodeMaxDimension(int dimension){
    g_imageDecodeMaxDimension = dimension;
}

void MeshSystem::applyDefaultGLTFMaterial(Material& material) {
    material = Material();
}

void MeshSystem::applyDefaultObjMaterial(Submesh& submesh) {
    submesh.attributes.clear();
    submesh.material.baseColorFactor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    submesh.material.emissiveFactor = Vector3(0.0f, 0.0f, 0.0f);
    submesh.material.metallicFactor = 0.0f;
    submesh.material.roughnessFactor = 1.0f;
    submesh.material.baseColorTexture = Texture();
    submesh.material.normalTexture = Texture();
    submesh.material.emissiveTexture = Texture();
    submesh.material.occlusionTexture = Texture();
}

bool MeshSystem::createSprite(SpriteComponent& sprite, MeshComponent& mesh, CameraComponent& camera){
    // Check texture loading state BEFORE clearing the buffer to avoid leaving
    // the mesh with an empty vertex buffer when the texture is still loading.
    Texture& mainTexture = mesh.submeshes[0].material.baseColorTexture;

    unsigned int texWidth = 0;
    unsigned int texHeight = 0;

    if (!mainTexture.empty()){
        TextureLoadResult texResult = mainTexture.load();
        if (texResult.state == ResourceLoadState::Finished){
            texWidth = mainTexture.getWidth();
            texHeight = mainTexture.getHeight();
        }else if (texResult.state == ResourceLoadState::Loading){
            return false;
        }
    }

    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.submeshes[0].hasTextureRect = true;
    mesh.submeshes[0].textureShadow = true;
    mesh.submeshes[0].faceCulling = false;
    mesh.numSubmeshes = 1;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);

    mesh.buffer.setUsage(BufferUsage::DYNAMIC);

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);

    if (texWidth == 0 || texHeight == 0){
        texWidth = sprite.width;
        texHeight = sprite.height;
    }

    if (sprite.width == 0 && sprite.height == 0){
        sprite.width = texWidth;
        sprite.height = texHeight;
    }

    Vector2 pivotPos = Vector2(0, 0);

    if (sprite.pivotPreset == PivotPreset::BOTTOM_RIGHT){
        pivotPos.x = sprite.width;
    }else if (sprite.pivotPreset == PivotPreset::TOP_RIGHT){
        pivotPos.x = sprite.width;
        pivotPos.y = sprite.height;
    }else if (sprite.pivotPreset == PivotPreset::TOP_LEFT){
        pivotPos.y = sprite.height;
    }else if (sprite.pivotPreset == PivotPreset::TOP_CENTER){
        pivotPos.x = sprite.width / 2.0;
        pivotPos.y = sprite.height;
    }else if (sprite.pivotPreset == PivotPreset::BOTTOM_CENTER){
        pivotPos.x = sprite.width / 2.0;
    }else if (sprite.pivotPreset == PivotPreset::RIGHT_CENTER){
        pivotPos.x = sprite.width;
        pivotPos.y = sprite.height / 2.0;
    }else if (sprite.pivotPreset == PivotPreset::LEFT_CENTER){
        pivotPos.y = sprite.height / 2.0;
    }else if (sprite.pivotPreset == PivotPreset::CENTER){
        pivotPos.x = sprite.width / 2.0;
        pivotPos.y = sprite.height / 2.0;
    }

    if (camera.type == CameraType::CAMERA_UI){
        pivotPos.y = sprite.height - pivotPos.y;
    }

    mesh.buffer.addVector3(attVertex, Vector3(-pivotPos.x, -pivotPos.y, 0));
    mesh.buffer.addVector3(attVertex, Vector3(sprite.width-pivotPos.x, -pivotPos.y, 0));
    mesh.buffer.addVector3(attVertex, Vector3(sprite.width-pivotPos.x,  sprite.height-pivotPos.y, 0));
    mesh.buffer.addVector3(attVertex, Vector3(-pivotPos.x,  sprite.height-pivotPos.y, 0));

    Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);

    float texCutRatioW = 0;
    float texCutRatioH = 0;
    if (texWidth != 0 && texHeight != 0){
        texCutRatioW = 1.0 / texWidth * sprite.textureScaleFactor;
        texCutRatioH = 1.0 / texHeight * sprite.textureScaleFactor;
    }

    if (!sprite.flipY){ 
        mesh.buffer.addVector2(attTexcoord, Vector2(texCutRatioW, texCutRatioH));
        mesh.buffer.addVector2(attTexcoord, Vector2(1.0-texCutRatioW, texCutRatioH));
        mesh.buffer.addVector2(attTexcoord, Vector2(1.0-texCutRatioW, 1.0-texCutRatioH));
        mesh.buffer.addVector2(attTexcoord, Vector2(texCutRatioW, 1.0-texCutRatioH));
    }else{
        mesh.buffer.addVector2(attTexcoord, Vector2(texCutRatioW, 1.0-texCutRatioH));
        mesh.buffer.addVector2(attTexcoord, Vector2(1.0-texCutRatioW, 1.0-texCutRatioH));
        mesh.buffer.addVector2(attTexcoord, Vector2(1.0-texCutRatioW, texCutRatioH));
        mesh.buffer.addVector2(attTexcoord, Vector2(texCutRatioW, texCutRatioH));
    }

    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);

    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));

    Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);

    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));


    static const uint16_t indices_array[] = {
        0,  1,  2,
        0,  2,  3,
    };

    mesh.indices.clear();

    mesh.indices.setValues(
        0, mesh.indices.getAttribute(AttributeType::INDEX),
        6, (char*)&indices_array[0], sizeof(uint16_t));

    calculateMeshAABB(mesh);

    if (mesh.loaded)
        mesh.needUpdateBuffer = true; // buffer is not immutable

    return true;
}

bool MeshSystem::createMeshPolygon(MeshPolygonComponent& polygon, MeshComponent& mesh){
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLE_STRIP;
    mesh.submeshes[0].faceCulling = false;
    mesh.numSubmeshes = 1;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);

    for (int i = 0; i < polygon.points.size(); i++){
        mesh.buffer.addVector3(AttributeType::POSITION, polygon.points[i].position);
        mesh.buffer.addVector3(AttributeType::NORMAL, Vector3(0.0f, 0.0f, 1.0f));
        mesh.buffer.addVector4(AttributeType::COLOR, polygon.points[i].color);
    }

    // Generation texcoords
    float min_X = std::numeric_limits<float>::max();
    float max_X = std::numeric_limits<float>::min();
    float min_Y = std::numeric_limits<float>::max();
    float max_Y = std::numeric_limits<float>::min();

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);

    for (unsigned int i = 0; i < mesh.buffer.getCount(); i++){
        min_X = fmin(min_X, mesh.buffer.getFloat(attVertex, i, 0));
        min_Y = fmin(min_Y, mesh.buffer.getFloat(attVertex, i, 1));
        max_X = fmax(max_X, mesh.buffer.getFloat(attVertex, i, 0));
        max_Y = fmax(max_Y, mesh.buffer.getFloat(attVertex, i, 1));
    }

    double k_X = 1/(max_X - min_X);
    double k_Y = 1/(max_Y - min_Y);

    float u = 0;
    float v = 0;

    for ( unsigned int i = 0; i < mesh.buffer.getCount(); i++){
        u = (mesh.buffer.getFloat(attVertex, i, 0) - min_X) * k_X;
        v = (mesh.buffer.getFloat(attVertex, i, 1) - min_Y) * k_Y;
        if (polygon.flipY) {
            mesh.buffer.addVector2(AttributeType::TEXCOORD1, Vector2(u, 1.0 - v));
        }else{
            mesh.buffer.addVector2(AttributeType::TEXCOORD1, Vector2(u, v));
        }
    }

    polygon.width = (int)(max_X - min_X);
    polygon.height = (int)(max_Y - min_Y);

    calculateMeshAABB(mesh);

    if (mesh.loaded)
        mesh.needReload = true;

    return true;
}

bool MeshSystem::createTilemap(TilemapComponent& tilemap, MeshComponent& mesh){
    // Pre-check all tile textures BEFORE clearing the buffer to avoid leaving
    // the mesh with an empty vertex buffer when textures are still loading.
    unsigned int preReserveTiles = tilemap.reserveTiles;
    for (int i = 0; i < (int)tilemap.numTiles; i++){
        if (tilemap.tiles[i].width == 0 && tilemap.tiles[i].height == 0 && preReserveTiles == 0){
            continue;
        }
        if (tilemap.tiles[i].rectId < 0 || (unsigned int)tilemap.tiles[i].rectId >= tilemap.numTilesRect){
            continue;
        }
        if (preReserveTiles > 0){
            preReserveTiles--;
        }
        TileRectData& rectData = tilemap.tilesRect[tilemap.tiles[i].rectId];
        if (rectData.submeshId < 0 || (unsigned int)rectData.submeshId >= mesh.numSubmeshes){
            continue;
        }
        Texture& texture = mesh.submeshes[rectData.submeshId].material.baseColorTexture;
        Texture& mainTexture = mesh.submeshes[0].material.baseColorTexture;
        if (!texture.empty()){
            TextureLoadResult texResult = texture.load();
            if (texResult.state == ResourceLoadState::Loading){
                return false;
            }
        }else if (!mainTexture.empty()){
            TextureLoadResult texResult = mainTexture.load();
            if (texResult.state == ResourceLoadState::Loading){
                return false;
            }
        }
    }

    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.submeshes[0].hasTextureRect = true;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);

    mesh.buffer.setUsage(BufferUsage::DYNAMIC);

    tilemap.width = 0;
    tilemap.height = 0;

    std::vector<std::vector<uint16_t>> indexMap;
    indexMap.resize(mesh.numSubmeshes);

    unsigned int numTiles = 0;
    unsigned int reserveTiles = tilemap.reserveTiles;

    for (int i = 0; i < (int)tilemap.numTiles; i++){

        if (tilemap.tiles[i].width == 0 && tilemap.tiles[i].height == 0 && reserveTiles == 0){
            continue;
        }

        if (tilemap.tiles[i].rectId < 0 || (unsigned int)tilemap.tiles[i].rectId >= tilemap.numTilesRect){
            continue;
        }

        if (reserveTiles > 0){
            reserveTiles--;
        }

        numTiles++;

        // Clamp tile position to non-negative — negative local coords are not supported
        if (tilemap.tiles[i].position.x < 0) tilemap.tiles[i].position.x = 0;
        if (tilemap.tiles[i].position.y < 0) tilemap.tiles[i].position.y = 0;

        Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
        mesh.buffer.addVector3(attVertex, Vector3(tilemap.tiles[i].position.x, tilemap.tiles[i].position.y, 0));
        mesh.buffer.addVector3(attVertex, Vector3(tilemap.tiles[i].position.x + tilemap.tiles[i].width, tilemap.tiles[i].position.y, 0));
        mesh.buffer.addVector3(attVertex, Vector3(tilemap.tiles[i].position.x + tilemap.tiles[i].width, tilemap.tiles[i].position.y + tilemap.tiles[i].height, 0));
        mesh.buffer.addVector3(attVertex, Vector3(tilemap.tiles[i].position.x, tilemap.tiles[i].position.y + tilemap.tiles[i].height, 0));

        if (tilemap.width < tilemap.tiles[i].position.x + tilemap.tiles[i].width)
            tilemap.width = static_cast<unsigned int>(tilemap.tiles[i].position.x + tilemap.tiles[i].width);
        if (tilemap.height < tilemap.tiles[i].position.y + tilemap.tiles[i].height)
            tilemap.height = static_cast<unsigned int>(tilemap.tiles[i].position.y + tilemap.tiles[i].height);

        TileRectData& rectData = tilemap.tilesRect[tilemap.tiles[i].rectId];
        int submeshId = (rectData.submeshId >= 0 && (unsigned int)rectData.submeshId < mesh.numSubmeshes) ? rectData.submeshId : 0;
        Rect tileRect = rectData.rect;

        Texture& texture = mesh.submeshes[submeshId].material.baseColorTexture;
        Texture& mainTexture = mesh.submeshes[0].material.baseColorTexture;

        unsigned int texWidth = 0;
        unsigned int texHeight = 0;
        if (!texture.empty()){
            TextureLoadResult texResult = texture.load();
            if (texResult.state == ResourceLoadState::Finished){
                tileRect = normalizeTileRect(tileRect, texture.getWidth(), texture.getHeight());
                texWidth = texture.getWidth();
                texHeight = texture.getHeight();
            }
        }else if (!mainTexture.empty()){
            TextureLoadResult texResult = mainTexture.load();
            if (texResult.state == ResourceLoadState::Finished){
                tileRect = normalizeTileRect(tileRect, mainTexture.getWidth(), mainTexture.getHeight());
                texWidth = mainTexture.getWidth();
                texHeight = mainTexture.getHeight();
            }
        }

        float texCutRatioW = 0;
        float texCutRatioH = 0;
        if (texWidth != 0 && texHeight != 0){
            texCutRatioW = 1.0 / texWidth * tilemap.textureScaleFactor;
            texCutRatioH = 1.0 / texHeight * tilemap.textureScaleFactor;
        }

        Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
        if (tilemap.flipY){
            mesh.buffer.addVector2(attTexcoord, Vector2(tileRect.getX()+texCutRatioW, tileRect.getY()+tileRect.getHeight()-texCutRatioH));
            mesh.buffer.addVector2(attTexcoord, Vector2(tileRect.getX()+tileRect.getWidth()-texCutRatioW, tileRect.getY()+tileRect.getHeight()-texCutRatioH));
            mesh.buffer.addVector2(attTexcoord, Vector2(tileRect.getX()+tileRect.getWidth()-texCutRatioW, tileRect.getY()+texCutRatioH));
            mesh.buffer.addVector2(attTexcoord, Vector2(tileRect.getX()+texCutRatioW, tileRect.getY()+texCutRatioH));
        }else{
            mesh.buffer.addVector2(attTexcoord, Vector2(tileRect.getX()+texCutRatioW, tileRect.getY()+texCutRatioH));
            mesh.buffer.addVector2(attTexcoord, Vector2(tileRect.getX()+tileRect.getWidth()-texCutRatioW, tileRect.getY()+texCutRatioH));
            mesh.buffer.addVector2(attTexcoord, Vector2(tileRect.getX()+tileRect.getWidth()-texCutRatioW, tileRect.getY()+tileRect.getHeight()-texCutRatioH));
            mesh.buffer.addVector2(attTexcoord, Vector2(tileRect.getX()+texCutRatioW, tileRect.getY()+tileRect.getHeight()-texCutRatioH));
        }

        Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);
        mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
        mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
        mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
        mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));

        Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);
        mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));

        indexMap[submeshId].push_back(0 + (i*4));
        indexMap[submeshId].push_back(1 + (i*4));
        indexMap[submeshId].push_back(2 + (i*4));
        indexMap[submeshId].push_back(0 + (i*4));
        indexMap[submeshId].push_back(2 + (i*4));
        indexMap[submeshId].push_back(3 + (i*4));

    }

    mesh.indices.clear();

    for (int i = 0; i < mesh.numSubmeshes; i++){
        addSubmeshAttribute(mesh.submeshes[i], "indices", AttributeType::INDEX, 1, AttributeDataType::UNSIGNED_SHORT, indexMap[i].size(), mesh.indices.getCount() * sizeof(uint16_t), false);

        if (indexMap[i].size() > 0){
            mesh.indices.setValues(mesh.indices.getCount(), mesh.indices.getAttribute(AttributeType::INDEX), indexMap[i].size(), (char*)&indexMap[i].front(), sizeof(uint16_t));
            mesh.indices.setRenderAttributes(false);
        }

        //TODO: necessary?
        //if (indexMap[i].size() == 0) {
        //     submesh not visible
        //} else {
        //     submesh visible
        //}
    }

    calculateMeshAABB(mesh);

    if (mesh.loaded){
        if (tilemap.renderedTiles != numTiles){
            mesh.needReload = true;
        }else{
            mesh.needUpdateBuffer = true; // buffer is not immutable
        }
    }
    tilemap.renderedTiles = numTiles;

    return true;
}

void MeshSystem::changeFlipY(bool& flipY, CameraComponent& camera, MeshComponent& mesh){
    // framebuffer textures need no special case: offscreen passes render with a
    // Y-flipped projection on GL, so they are top-left origin on every backend
    flipY = false;
    if (camera.type != CameraType::CAMERA_UI){
        flipY = !flipY;
    }
}

Rect MeshSystem::normalizeTileRect(Rect tileRect, unsigned int texWidth, unsigned int texHeight){
    Rect normalized = tileRect;

    if (!tileRect.isNormalized()){
        normalized.setRect((tileRect.getX()) / (float) texWidth,
                            (tileRect.getY()) / (float) texHeight,
                            (tileRect.getWidth()) / (float) texWidth,
                            (tileRect.getHeight()) / (float) texHeight);
    }

    return normalized;
}

std::vector<float> MeshSystem::getCylinderSideNormals(float baseRadius, float topRadius, float height, float slices){
    float sectorStep = 2 * M_PI / slices;
    float sectorAngle;  // radian

    // compute the normal vector at 0 degree first
    // tanA = (baseRadius-topRadius) / height
    float yAngle = atan2(baseRadius - topRadius, height);
    float x0 = cos(yAngle);     // nx
    float y0 = sin(yAngle);     // ny
    float z0 = 0;               // nz

    // rotate (x0,y0,z0) per sector angle
    std::vector<float> normals;
    for(int i = 0; i <= slices; ++i)
    {
        sectorAngle = i * sectorStep;
        normals.push_back(sin(sectorAngle)*x0);    // nx
        normals.push_back(y0);                     // ny
        normals.push_back(-cos(sectorAngle)*x0);   // nz
    }

    return normals;
}

// generate 3D vertices of a unit circle on XZ plane
std::vector<float> MeshSystem::buildUnitCircleVertices(float slices){
    float sectorStep = 2 * M_PI / slices;
    float sectorAngle;  // radian

    std::vector<float> unitCircleVertices;

    for(int i = 0; i <= slices; ++i){
        sectorAngle = i * sectorStep;
        unitCircleVertices.push_back(sin(sectorAngle));  // x
        unitCircleVertices.push_back(0);                 // y
        unitCircleVertices.push_back(-cos(sectorAngle)); // z
    }

    return unitCircleVertices;
}

std::string MeshSystem::readFileToString(const char* filename){
    Data filedata;

    if (filedata.open(filename) != FileErrors::FILEDATA_OK){
        Log::error("Model file not found: %s", filename);
        return "";
    }
    filedata.seek(0);
    return filedata.readString(filedata.length());
}

bool MeshSystem::fileExists(const std::string &abs_filename, void *) {
    File df;
    int res = df.open(abs_filename.c_str());

    if (!res) {
        return true;
    }

    return false;
}

bool MeshSystem::readWholeFile(std::vector<unsigned char> *out, std::string *err, const std::string &filepath, void *) {
    Data filedata;

    if (filedata.open(filepath.c_str()) != FileErrors::FILEDATA_OK){
        if (err) {
            (*err) += "File open error : " + filepath + "\n";
        }
        Log::error("Model file not found: %s", filepath.c_str());
        return false;
    }

    filedata.seek(0);
    std::istringstream f(filedata.readString(filedata.length()));

    if (!f) {
        if (err) {
            (*err) += "File open error : " + filepath + "\n";
        }
        return false;
    }

    f.seekg(0, f.end);
    size_t sz = static_cast<size_t>(f.tellg());
    f.seekg(0, f.beg);

    if (int(sz) < 0) {
        if (err) {
            (*err) += "Invalid file size : " + filepath +
                      " (does the path point to a directory?)";
        }
        return false;
    } else if (sz == 0) {
        if (err) {
            (*err) += "File is empty : " + filepath + "\n";
        }
        return false;
    }

    out->resize(sz);
    f.read(reinterpret_cast<char *>(&out->at(0)),
           static_cast<std::streamsize>(sz));
    //f.close();

    return true;
}

bool MeshSystem::getFileSizeInBytes(size_t *filesize_out, std::string *err, const std::string &filepath, void *userdata) {
  (void)userdata;

    Data filedata;

    if (filedata.open(filepath.c_str()) != FileErrors::FILEDATA_OK){
        if (err) {
            (*err) += "File open error : " + filepath + "\n";
        }
        Log::error("Model file not found: %s", filepath.c_str());
        return false;
    }

    (*filesize_out) = static_cast<size_t>(filedata.length());

    return true;
}

std::string MeshSystem::getAsyncModelLoadScenePrefix(const Scene* scene){
    std::ostringstream key;
    key << reinterpret_cast<uintptr_t>(scene) << '|';
    return key.str();
}

std::string MeshSystem::getAsyncModelLoadKey(const Scene* scene, const std::string& filename){
    std::ostringstream key;
    key << getAsyncModelLoadScenePrefix(scene) << getModelFilenameKey(filename);
    return key.str();
}

std::string MeshSystem::getAsyncModelLoadKey(const std::string& filename) const{
    return getAsyncModelLoadKey(scene, filename);
}

bool MeshSystem::hasPendingAsyncModelLoads() const{
    const std::string scenePrefix = getAsyncModelLoadScenePrefix(scene);
    std::lock_guard<std::mutex> lock(getAsyncModelMutex());
    auto& pendingModelLoads = getPendingModelLoads();
    for (const auto& [key, future] : pendingModelLoads) {
        if (key.rfind(scenePrefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

void MeshSystem::cancelAsyncModelLoads(){
    const std::string scenePrefix = getAsyncModelLoadScenePrefix(scene);
    std::vector<uint64_t> buildIds;
    {
        std::lock_guard<std::mutex> lock(getAsyncModelMutex());
        auto& pendingModelLoads = getPendingModelLoads();
        for (auto it = pendingModelLoads.begin(); it != pendingModelLoads.end();) {
            if (it->first.rfind(scenePrefix, 0) == 0) {
                buildIds.push_back(std::hash<std::string>{}(it->first));
                it = pendingModelLoads.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (uint64_t buildId : buildIds) {
        ResourceProgress::failBuild(buildId);
    }
}

void MeshSystem::cancelAllAsyncModelLoads(){
    std::vector<uint64_t> buildIds;
    {
        std::lock_guard<std::mutex> lock(getAsyncModelMutex());
        auto& pendingModelLoads = getPendingModelLoads();
        buildIds.reserve(pendingModelLoads.size());
        for (const auto& [key, future] : pendingModelLoads) {
            buildIds.push_back(std::hash<std::string>{}(key));
        }
        pendingModelLoads.clear();
    }
    for (uint64_t buildId : buildIds) {
        ResourceProgress::failBuild(buildId);
    }
}

bool MeshSystem::isAsyncModelLoadPending(Entity entity, const std::string& filename) const{
    (void)entity;
    const std::string key = getAsyncModelLoadKey(filename);
    std::lock_guard<std::mutex> lock(getAsyncModelMutex());
    auto& pendingModelLoads = getPendingModelLoads();
    return pendingModelLoads.find(key) != pendingModelLoads.end();
}

void MeshSystem::cancelAsyncModelLoad(Entity entity, const std::string& filename){
    (void)entity;
    const std::string key = getAsyncModelLoadKey(filename);
    const uint64_t buildId = std::hash<std::string>{}(key);
    bool erased = false;
    {
        std::lock_guard<std::mutex> lock(getAsyncModelMutex());
        auto& pendingModelLoads = getPendingModelLoads();
        erased = pendingModelLoads.erase(key) > 0;
    }
    if (erased){
        ResourceProgress::failBuild(buildId);
    }
}

// Decodes one image that SetImagesAsIs left still encoded in image.image.
void MeshSystem::decodeGLTFImage(tinygltf::Image& image, size_t index, int maxDimension) {
    if (image.image.empty()) {
        return; // no encoded bytes (e.g. external/unsupported image)
    }

    // Decode with tinygltf's own decoder (default RGBA8/16) so results match the serial path.
    std::vector<unsigned char> encoded = std::move(image.image);
    image.image.clear();
    std::string err;
    std::string warn;
    if (!tinygltf::LoadImageData(&image, static_cast<int>(index), &err, &warn, 0, 0,
                                 encoded.data(), static_cast<int>(encoded.size()), nullptr)) {
        Log::error("Failed to decode GLTF image %zu: %s", index, err.c_str());
        return;
    }

    // Downscale oversized maps for preview renders. Only 8-bit images are handled; 16-bit is rare
    // and left full-size. The transient full-size buffer is freed when this returns.
    if (maxDimension > 0 && image.bits == 8 && image.component >= 1 &&
            (image.width > maxDimension || image.height > maxDimension)) {
        const int comp = image.component;
        const double scale = static_cast<double>(maxDimension) / std::max(image.width, image.height);
        const int nw = std::max(1, static_cast<int>(std::lround(image.width * scale)));
        const int nh = std::max(1, static_cast<int>(std::lround(image.height * scale)));
        std::vector<unsigned char> resized(static_cast<size_t>(nw) * nh * comp);
        const stbir_pixel_layout layout = (comp == 4) ? STBIR_RGBA : (comp == 3) ? STBIR_RGB :
                                          (comp == 2) ? STBIR_RA : STBIR_1CHANNEL;
        if (stbir_resize_uint8_linear(image.image.data(), image.width, image.height, 0,
                                      resized.data(), nw, nh, 0, layout)) {
            image.image = std::move(resized);
            image.width = nw;
            image.height = nh;
        }
    }
}

// Runs fn(0..count-1) across a few threads, pulling indices off a shared counter so heavy items
// spread across workers instead of landing in one fixed range.
//
// NOTE: this deliberately uses std::async, not ThreadPoolManager. Background model parsing already
// runs on a ThreadPoolManager worker, so enqueuing onto that bounded pool and blocking could
// deadlock when several models load at once.
template<typename Fn>
void MeshSystem::parallelForIndexed(size_t count, Fn&& fn) {
    if (count == 0) {
        return;
    }

    const unsigned int hw = std::thread::hardware_concurrency();
    const size_t workers = std::max<size_t>(1, std::min<size_t>(count, hw ? hw : 4));

    std::atomic<size_t> next{0};
    auto worker = [&]() {
        for (size_t i = next.fetch_add(1); i < count; i = next.fetch_add(1)) {
            fn(i);
        }
    };

    if (workers == 1) {
        worker();
        return;
    }

    std::vector<std::future<void>> futures;
    for (size_t w = 1; w < workers; w++) {
        futures.push_back(std::async(std::launch::async, worker));
    }
    worker(); // the calling thread participates too
    for (auto& future : futures) {
        future.get();
    }
}

// glTF embedded images are decoded serially inside LoadBinaryFromFile, which dominates load time
// for texture-heavy models. Instead the loader is put in "as is" mode (it stores the still-encoded
// bytes), and this decodes them across a few threads afterwards.
void MeshSystem::decodeGLTFImagesParallel(tinygltf::Model& model, int maxDimension) {
    parallelForIndexed(model.images.size(), [&model, maxDimension](size_t i) {
        decodeGLTFImage(model.images[i], i, maxDimension);
    });
}

// Copies one decoded glTF texture's pixels into a pooled face array (the malloc + memcpy that
// otherwise stalls the main thread). Mirrors the ownership dance in loadGLTFTexture: build the
// TextureData with dataOwned=false, then mark the stored copy owned so there is a single owner.
std::shared_ptr<std::array<TextureData,6>> MeshSystem::buildGLTFTextureFaces(tinygltf::Model& model, int textureIndex) {
    if (textureIndex < 0 || static_cast<size_t>(textureIndex) >= model.textures.size()) {
        return nullptr;
    }
    const tinygltf::Texture& tex = model.textures[textureIndex];
    if (tex.source < 0 || static_cast<size_t>(tex.source) >= model.images.size()) {
        return nullptr;
    }
    const tinygltf::Image& image = model.images[tex.source];
    if (image.width <= 0 || image.height <= 0 || image.component <= 0 || image.image.empty()) {
        return nullptr;
    }
    const size_t imageSize = static_cast<size_t>(image.component) * image.width * image.height;
    if (image.image.size() < imageSize) {
        return nullptr;
    }
    ColorFormat colorFormat;
    if (image.component == 1) {
        colorFormat = ColorFormat::RED;
    } else if (image.component == 4) {
        colorFormat = ColorFormat::RGBA;
    } else {
        return nullptr; // renders only support 8bpp and 32bpp
    }

    unsigned char* imageCopy = static_cast<unsigned char*>(std::malloc(imageSize));
    if (!imageCopy) {
        return nullptr;
    }
    std::memcpy(imageCopy, image.image.data(), imageSize);

    auto faces = std::make_shared<std::array<TextureData,6>>();
    faces->at(0) = TextureData(image.width, image.height, static_cast<unsigned int>(imageSize), colorFormat, image.component, imageCopy);
    faces->at(0).setDataOwned(true); // single owner; copies above are shallow with dataOwned=false
    return faces;
}

std::shared_ptr<MeshSystem::AsyncModelLoadResult> MeshSystem::loadModelFileOnWorker(const std::string& filename, bool obj, uint64_t buildId){
    auto result = std::make_shared<AsyncModelLoadResult>();
    result->obj = obj;
    result->filename = filename;

    try {
        ResourceProgress::updateProgress(buildId, 0.1f);

        if (obj){
            tinyobj::FileReader::externalFunc = readFileToString;
            std::string baseDir = FileData::getBaseDir(filename);
            result->objModel = std::make_shared<ObjModelData>();
            result->success = tinyobj::LoadObj(&result->objModel->attrib, &result->objModel->shapes, &result->objModel->materials, &result->warn, &result->err, filename.c_str(), baseDir.c_str());
        }else{
            result->gltfModel = std::make_shared<tinygltf::Model>();

            tinygltf::TinyGLTF loader;
            loader.SetFsCallbacks({&fileExists, &tinygltf::ExpandFilePath, &readWholeFile, &tinygltf::WriteWholeFile, &getFileSizeInBytes});
            loader.SetImagesAsIs(true); // keep images encoded; decode in parallel below

            std::string ext = FileData::getFilePathExtension(filename);
            if (ext.compare("glb") == 0) {
                result->success = loader.LoadBinaryFromFile(result->gltfModel.get(), &result->err, &result->warn, filename);
            }else{
                result->success = loader.LoadASCIIFromFile(result->gltfModel.get(), &result->err, &result->warn, filename);
            }
            if (result->success) {
                decodeGLTFImagesParallel(*result->gltfModel, 0); // background loads are full resolution

                // Pre-copy texture pixels here (worker thread) so the main-thread mesh build only
                // does cheap pool lookups instead of the ~GiB memcpy. Keyed to match loadGLTFTexture.
                const std::string texKeyPrefix = getModelFilenameKey(filename) + "|gltf-texture|";
                result->prebuiltTextures.resize(result->gltfModel->textures.size());
                parallelForIndexed(result->gltfModel->textures.size(), [&](size_t i) {
                    if (auto faces = buildGLTFTextureFaces(*result->gltfModel, static_cast<int>(i))) {
                        result->prebuiltTextures[i] = { texKeyPrefix + std::to_string(i), std::move(faces) };
                    }
                });
            }
        }

        if (!result->err.empty() || !result->success){
            ResourceProgress::failBuild(buildId);
            return result;
        }

        ResourceProgress::updateProgress(buildId, 0.3f);
    } catch (const std::exception& e) {
        result->success = false;
        result->err = e.what();
        ResourceProgress::failBuild(buildId);
    }

    return result;
}

std::shared_ptr<MeshSystem::AsyncModelLoadResult> MeshSystem::pollOrStartAsyncModelLoad(const std::string& filename, bool obj){
    const std::string key = getAsyncModelLoadKey(filename);
    const uint64_t buildId = std::hash<std::string>{}(key);

    std::shared_future<std::shared_ptr<AsyncModelLoadResult>> future;
    {
        std::lock_guard<std::mutex> lock(getAsyncModelMutex());
        auto& pendingModelLoads = getPendingModelLoads();
        auto it = pendingModelLoads.find(key);
        if (it == pendingModelLoads.end()){
            std::filesystem::path filePath(filename);
            ResourceProgress::startBuild(buildId, ResourceType::Model, filePath.filename().string());

            pendingModelLoads[key] = ThreadPoolManager::getInstance().enqueue(
                [filename, obj, buildId]() {
                    return MeshSystem::loadModelFileOnWorker(filename, obj, buildId);
                }
            ).share();

            return nullptr;
        }

        future = it->second;
    }

    if (!future.valid()){
        std::lock_guard<std::mutex> lock(getAsyncModelMutex());
        auto& pendingModelLoads = getPendingModelLoads();
        pendingModelLoads.erase(key);
        ResourceProgress::failBuild(buildId);
        return nullptr;
    }

    if (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready){
        return nullptr;
    }

    std::shared_ptr<AsyncModelLoadResult> result = future.get();
    {
        std::lock_guard<std::mutex> lock(getAsyncModelMutex());
        auto& pendingModelLoads = getPendingModelLoads();
        pendingModelLoads.erase(key);
    }

    if (!result){
        ResourceProgress::failBuild(buildId);
        return nullptr;
    }

    if (!result->warn.empty()){
        if (obj){
            Log::warn("Loading OBJ model (%s): %s", filename.c_str(), result->warn.c_str());
        }else{
            Log::warn("Loading GLTF model (%s): %s", filename.c_str(), result->warn.c_str());
        }
    }

    if (!result->err.empty()){
        if (obj){
            Log::error("Can't load OBJ model (%s): %s", filename.c_str(), result->err.c_str());
        }else{
            Log::error("Can't load GLTF model (%s): %s", filename.c_str(), result->err.c_str());
        }
    }

    if (!result->success){
        if (!obj && result->err.empty()){
            Log::verbose("Failed to load glTF: %s", filename.c_str());
        }
        return result;
    }

    return result;
}

void MeshSystem::addSubmeshAttribute(Submesh& submesh, const std::string& bufferName, AttributeType attribute, unsigned int elements, AttributeDataType dataType, size_t size, size_t offset, bool normalized){
    Attribute attData;

    attData.setBufferName(bufferName);
    attData.setDataType(dataType);
    attData.setElements(elements);
    attData.setCount(size);
    attData.setOffset(offset);
    attData.setNormalized(normalized);

    submesh.attributes[attribute] = attData;
}

bool MeshSystem::loadGLTFBuffer(int bufferViewIndex, MeshComponent& mesh, ModelComponent& model, const int stride, std::vector<std::string>& loadedBuffers){
    if (!model.gltfModel || !isValidGLTFIndex(bufferViewIndex, model.gltfModel->bufferViews)) {
        Log::error("Invalid GLTF buffer view index %i", bufferViewIndex);
        return false;
    }

    const tinygltf::BufferView &bufferView = model.gltfModel->bufferViews[bufferViewIndex];
    if (!isValidGLTFIndex(bufferView.buffer, model.gltfModel->buffers)) {
        Log::error("Invalid GLTF buffer index %i for buffer view %i", bufferView.buffer, bufferViewIndex);
        return false;
    }

    std::vector<unsigned char>& bufferData = model.gltfModel->buffers[bufferView.buffer].data;
    if (bufferView.byteOffset > bufferData.size() || bufferView.byteLength > bufferData.size() - bufferView.byteOffset) {
        Log::error("Invalid GLTF buffer view range %i", bufferViewIndex);
        return false;
    }

    const std::string name = getBufferName(bufferViewIndex, model);

    if (std::find(loadedBuffers.begin(), loadedBuffers.end(), name) == loadedBuffers.end() && bufferView.target != 0) {
        if (!mesh.eBuffers.validIndex(static_cast<int>(mesh.numExternalBuffers))){
            Log::error("External buffer limit reached for GLTF model");
            return false;
        }

        loadedBuffers.push_back(name);

        if (bufferView.target == 34962) { //GL_ARRAY_BUFFER
            mesh.eBuffers[mesh.numExternalBuffers].setType(BufferType::VERTEX_BUFFER);
        } else if (bufferView.target == 34963) { //GL_ELEMENT_ARRAY_BUFFER
            mesh.eBuffers[mesh.numExternalBuffers].setType(BufferType::INDEX_BUFFER);
        }

        mesh.eBuffers[mesh.numExternalBuffers].setData(bufferData.data() + bufferView.byteOffset, bufferView.byteLength);
        mesh.eBuffers[mesh.numExternalBuffers].setStride(stride);
        mesh.eBuffers[mesh.numExternalBuffers].setName(name);
        mesh.eBuffers[mesh.numExternalBuffers].setRenderAttributes(false);

        mesh.numExternalBuffers++;

        return true;
    }

    return false;
}

int MeshSystem::convertGLTFByteIndicesToShort(const tinygltf::Accessor& indexAccessor, ModelComponent& model){
    // Sokol (and most modern backends) have no 8-bit index format, so 8-bit GLTF indices
    // are expanded to 16-bit into a synthetic buffer/bufferView appended to the model.
    // The model's buffers vector is reserved up-front by the caller so this append never
    // reallocates and invalidates pointers already handed to external buffers.
    if (!model.gltfModel || !isValidGLTFIndex(indexAccessor.bufferView, model.gltfModel->bufferViews)) {
        return -1;
    }

    const tinygltf::BufferView& srcView = model.gltfModel->bufferViews[indexAccessor.bufferView];
    if (!isValidGLTFIndex(srcView.buffer, model.gltfModel->buffers)) {
        return -1;
    }

    const std::vector<unsigned char>& srcData = model.gltfModel->buffers[srcView.buffer].data;
    const size_t srcOffset = srcView.byteOffset + indexAccessor.byteOffset;
    const size_t count = indexAccessor.count;

    // 8-bit indices are tightly packed (one byte each).
    if (srcOffset > srcData.size() || count > srcData.size() - srcOffset) {
        Log::error("GLTF byte index range out of bounds");
        return -1;
    }

    tinygltf::Buffer newBuffer;
    newBuffer.data.resize(count * sizeof(uint16_t));
    uint16_t* dst = reinterpret_cast<uint16_t*>(newBuffer.data.data());
    for (size_t k = 0; k < count; k++) {
        dst[k] = static_cast<uint16_t>(srcData[srcOffset + k]);
    }
    model.gltfModel->buffers.push_back(std::move(newBuffer));
    const int newBufferIndex = static_cast<int>(model.gltfModel->buffers.size()) - 1;

    tinygltf::BufferView newView;
    newView.buffer = newBufferIndex;
    newView.byteOffset = 0;
    newView.byteLength = count * sizeof(uint16_t);
    newView.byteStride = 0;
    newView.target = 34963; // GL_ELEMENT_ARRAY_BUFFER
    newView.name = "byteindex_synth";
    model.gltfModel->bufferViews.push_back(std::move(newView));

    return static_cast<int>(model.gltfModel->bufferViews.size()) - 1;
}

int MeshSystem::bakeGLTFTransformedAttribute(const tinygltf::Accessor& accessor, const Matrix4& matrix, const Matrix3& normalMatrix, bool isNormal, ModelComponent& model){
    // Bakes a node's world transform into a copy of a POSITION/NORMAL attribute, into a synthetic
    // tightly-packed FLOAT VEC3 buffer/view. Used only for flattened previews so a multi-node model
    // shows its layout without per-node entities. Caller must reserve buffers/bufferViews up-front.
    if (!model.gltfModel || !isValidGLTFIndex(accessor.bufferView, model.gltfModel->bufferViews)) {
        return -1;
    }
    if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT || accessor.type != TINYGLTF_TYPE_VEC3) {
        return -1; // only float vec3 positions/normals are baked
    }

    const tinygltf::BufferView& srcView = model.gltfModel->bufferViews[accessor.bufferView];
    if (!isValidGLTFIndex(srcView.buffer, model.gltfModel->buffers)) {
        return -1;
    }
    const std::vector<unsigned char>& srcData = model.gltfModel->buffers[srcView.buffer].data;

    const size_t count = accessor.count;
    const int stride = accessor.ByteStride(srcView);
    if (count == 0 || stride < static_cast<int>(3 * sizeof(float))) {
        return -1;
    }
    const size_t base = srcView.byteOffset + accessor.byteOffset;
    if (base + (count - 1) * static_cast<size_t>(stride) + 3 * sizeof(float) > srcData.size()) {
        Log::error("GLTF baked attribute range out of bounds");
        return -1;
    }

    tinygltf::Buffer newBuffer;
    newBuffer.data.resize(count * 3 * sizeof(float));
    float* dst = reinterpret_cast<float*>(newBuffer.data.data());
    for (size_t i = 0; i < count; i++) {
        const float* src = reinterpret_cast<const float*>(srcData.data() + base + i * static_cast<size_t>(stride));
        Vector3 v(src[0], src[1], src[2]);
        Vector3 r = isNormal ? (normalMatrix * v).normalized() : (matrix * v);
        dst[i * 3 + 0] = r.x;
        dst[i * 3 + 1] = r.y;
        dst[i * 3 + 2] = r.z;
    }
    model.gltfModel->buffers.push_back(std::move(newBuffer));
    const int newBufferIndex = static_cast<int>(model.gltfModel->buffers.size()) - 1;

    tinygltf::BufferView newView;
    newView.buffer = newBufferIndex;
    newView.byteOffset = 0;
    newView.byteLength = count * 3 * sizeof(float);
    newView.byteStride = 0; // tightly packed
    newView.target = 34962; // GL_ARRAY_BUFFER
    newView.name = "baked_attr_synth";
    model.gltfModel->bufferViews.push_back(std::move(newView));

    return static_cast<int>(model.gltfModel->bufferViews.size()) - 1;
}

bool MeshSystem::loadGLTFTexture(int textureIndex, ModelComponent& model, Texture& texture, const std::string& textureName){
    texture = Texture();

    if (textureIndex >= 0){
        if (!model.gltfModel || !isValidGLTFIndex(textureIndex, model.gltfModel->textures)) {
            Log::warn("Invalid GLTF texture index %i: %s", textureIndex, textureName.c_str());
            return true;
        }

        tinygltf::Texture &tex = model.gltfModel->textures[textureIndex];
        if (!isValidGLTFIndex(tex.source, model.gltfModel->images)) {
            Log::warn("Invalid GLTF image index %i for texture %i: %s", tex.source, textureIndex, textureName.c_str());
            return true;
        }

        tinygltf::Image &image = model.gltfModel->images[tex.source];
        if (image.width <= 0 || image.height <= 0 || image.component <= 0 || image.image.empty()) {
            Log::warn("Invalid GLTF image data for texture: %s", textureName.c_str());
            return true;
        }

        size_t imageSize = image.component * image.width * image.height; //in bytes
        if (image.image.size() < imageSize) {
            Log::warn("Incomplete GLTF image data for texture: %s", textureName.c_str());
            return true;
        }

        ColorFormat colorFormat;
        if (image.component == 1){
            colorFormat = ColorFormat::RED;
        }else if (image.component == 4){
            colorFormat = ColorFormat::RGBA;
        }else{
            Log::warn("Not compatible image %s: Renders only support 8bpp and 32bpp", textureName.c_str());
            return true;
        }

        std::string id = getModelFilenameKey(model.filename) + "|gltf-texture|" + std::to_string(textureIndex);

        auto existingData = TextureDataPool::get(id);
        const bool canReuse = existingData && existingData->at(0).getData() && existingData->at(0).getSize() >= imageSize;
        if (canReuse) {
            texture.setId(id);
        } else {
            unsigned char* imageCopy = static_cast<unsigned char*>(std::malloc(imageSize));
            if (!imageCopy) {
                Log::error("Out of memory while copying GLTF texture data for %s", textureName.c_str());
                return false;
            }

            std::memcpy(imageCopy, image.image.data(), imageSize);
            TextureData textureData(image.width, image.height, imageSize, colorFormat, image.component, imageCopy);

            texture.setData(id, textureData);

            TextureData& pooledData = texture.getData();
            if (pooledData.getData() == imageCopy) {
                pooledData.setDataOwned(true);
            } else {
                std::free(imageCopy);
            }
        }

        if (tex.sampler >= 0 && isValidGLTFIndex(tex.sampler, model.gltfModel->samplers)){
            tinygltf::Sampler &sampler = model.gltfModel->samplers[tex.sampler];
            texture.setMinFilter(convertFilter(sampler.minFilter));
            texture.setMagFilter(convertFilter(sampler.magFilter));
            texture.setWrapU(convertWrap(sampler.wrapS));
            texture.setWrapV(convertWrap(sampler.wrapT));
        }else if (tex.sampler >= 0){
            Log::warn("Invalid GLTF sampler index %i for texture %i", tex.sampler, textureIndex);
        }
        // Prevent GLTF release because GLTF can have multiple textures with the same data
        // Image data is stored in tinygltf::Image
        texture.setReleaseDataAfterLoad(false);
    }

    return true;
}

std::string MeshSystem::getBufferName(int bufferViewIndex, ModelComponent& model){
    if (!model.gltfModel || !isValidGLTFIndex(bufferViewIndex, model.gltfModel->bufferViews)) {
        return "buffer" + std::to_string(bufferViewIndex);
    }

    const tinygltf::BufferView &bufferView = model.gltfModel->bufferViews[bufferViewIndex];

    // glTF buffer view names are not required to be unique, so keep the view index
    // in the key with a delimiter to avoid accidental name+index collisions.
    if (!bufferView.name.empty())
        return bufferView.name + "#" + std::to_string(bufferViewIndex);
    else
        return "buffer" + std::to_string(bufferViewIndex);

}

Matrix4 MeshSystem::getGLTFNodeMatrix(int nodeIndex, ModelComponent& model){
    if (!model.gltfModel || !isValidGLTFIndex(nodeIndex, model.gltfModel->nodes)) {
        return Matrix4();
    }

    tinygltf::Node node = model.gltfModel->nodes[nodeIndex];

    Matrix4 matrix;

    if (node.matrix.size() == 16){
        matrix = Matrix4(
                node.matrix[0],node.matrix[4],node.matrix[8], node.matrix[12],
                node.matrix[1],node.matrix[5],node.matrix[9], node.matrix[13],
                node.matrix[2],node.matrix[6],node.matrix[10],node.matrix[14],
                node.matrix[3],node.matrix[7],node.matrix[11],node.matrix[15]);
    }else{

        Vector3 translation;
        Vector3 scale;
        Quaternion rotation;

        if (node.translation.size() == 3){
            translation = Vector3(node.translation[0], node.translation[1], node.translation[2]);
        }else{
            translation = Vector3(0.0, 0.0, 0.0);
        }
        if (node.rotation.size() == 4){
            rotation = Quaternion(node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]);
        }else{
            rotation = Quaternion(1.0, 0.0, 0.0, 0.0);
        }
        if (node.scale.size() == 3){
            scale = Vector3(node.scale[0], node.scale[1], node.scale[2]);
        }else{
            scale = Vector3(1.0, 1.0, 1.0);
        }

        Matrix4 scaleMatrix = Matrix4::scaleMatrix(scale);
        Matrix4 translateMatrix = Matrix4::translateMatrix(translation);
        Matrix4 rotationMatrix = rotation.getRotationMatrix();

        matrix = translateMatrix * rotationMatrix * scaleMatrix;
    }

    return matrix;
}

Matrix4 MeshSystem::getGLTFMeshGlobalMatrix(int nodeIndex, ModelComponent& model, std::map<int, int>& nodesParent){

    if (!model.gltfModel || !isValidGLTFIndex(nodeIndex, model.gltfModel->nodes)) {
        return Matrix4();
    }

    Matrix4 matrix = getGLTFNodeMatrix(nodeIndex, model);

    int parent = -1;
    auto parentIt = nodesParent.find(nodeIndex);
    if (parentIt != nodesParent.end()) {
        parent = parentIt->second;
    }

    if (parent >= 0){
        return getGLTFMeshGlobalMatrix(parent, model, nodesParent) * matrix;
    }

    return matrix;
}

Entity MeshSystem::generateSketetalStructure(Entity entity, ModelComponent& model, int nodeIndex, int skinIndex){
    if (!model.gltfModel || !isValidGLTFIndex(nodeIndex, model.gltfModel->nodes) || !isValidGLTFIndex(skinIndex, model.gltfModel->skins)) {
        return NULL_ENTITY;
    }

    tinygltf::Node node = model.gltfModel->nodes[nodeIndex];
    tinygltf::Skin skin = model.gltfModel->skins[skinIndex];

    int index = -1;

    for (int j = 0; j < skin.joints.size(); j++) {
        if (nodeIndex == skin.joints[j])
            index = j;
    }

    if (index < 0) {
        return NULL_ENTITY;
    }

    Matrix4 offsetMatrix;

    if (skin.inverseBindMatrices >= 0) {

        if (!isValidGLTFIndex(skin.inverseBindMatrices, model.gltfModel->accessors)) {
            Log::error("Skeleton error: Invalid inverse bind matrix accessor");
            return NULL_ENTITY;
        }

        tinygltf::Accessor accessor = model.gltfModel->accessors[skin.inverseBindMatrices];
        if (!isValidGLTFIndex(accessor.bufferView, model.gltfModel->bufferViews)) {
            Log::error("Skeleton error: Invalid inverse bind matrix buffer view");
            return NULL_ENTITY;
        }

        tinygltf::BufferView bufferView = model.gltfModel->bufferViews[accessor.bufferView];

        if (!isValidGLTFIndex(bufferView.buffer, model.gltfModel->buffers)) {
            Log::error("Skeleton error: Invalid inverse bind matrix buffer");
            return NULL_ENTITY;
        }

        std::vector<unsigned char>& bufferData = model.gltfModel->buffers[bufferView.buffer].data;
        const size_t matrixOffset = bufferView.byteOffset + accessor.byteOffset + (16 * sizeof(float) * index);
        if (matrixOffset > bufferData.size() || 16 * sizeof(float) > bufferData.size() - matrixOffset) {
            Log::error("Skeleton error: Invalid inverse bind matrix range");
            return NULL_ENTITY;
        }

        float *matrices = (float *) (bufferData.data() + matrixOffset);

        if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT || accessor.type != TINYGLTF_TYPE_MAT4) {
            Log::error("Skeleton error: Unknown inverse bind matrix data type");

            return NULL_ENTITY;
        }

        offsetMatrix = Matrix4(
                matrices[0], matrices[4], matrices[8], matrices[12],
                matrices[1], matrices[5], matrices[9], matrices[13],
                matrices[2], matrices[6], matrices[10], matrices[14],
                matrices[3], matrices[7], matrices[11], matrices[15]);

    }

    Entity bone;

    // User pool (not System): model-generated entities must not draw from the capped [1..1000]
    // System range. They stay out of serialization via the editor's entity list, like mesh nodes.
    bone = scene->createEntity();
    if (bone == NULL_ENTITY) {
        return NULL_ENTITY;
    }
    scene->addComponent<Transform>(bone);
    scene->addComponent<BoneComponent>(bone);

    Transform& bonetransform = scene->getComponent<Transform>(bone);
    BoneComponent& bonecomp = scene->getComponent<BoneComponent>(bone);

    bonecomp.index = index;
    scene->setEntityName(bone, node.name);

    Matrix4 matrix = getGLTFNodeMatrix(nodeIndex, model);
    matrix.decompose(bonecomp.bindPosition, bonecomp.bindScale, bonecomp.bindRotation);

    // move to bind
    bonetransform.position = bonecomp.bindPosition;
    bonetransform.rotation = bonecomp.bindRotation;
    bonetransform.scale = bonecomp.bindScale;

    bonecomp.offsetMatrix = offsetMatrix;
    bonecomp.model = entity;

    model.bonesNameMapping[scene->getEntityName(bone)] = bone;
    model.bonesIdMapping[nodeIndex] = bone;

    for (size_t i = 0; i < node.children.size(); i++){
        // here bonetransform and bonecomp losts references
        if (!isValidGLTFIndex(node.children[i], model.gltfModel->nodes)) {
            continue;
        }

        Entity child = generateSketetalStructure(entity, model, node.children[i], skinIndex);
        if (child != NULL_ENTITY) {
            scene->addEntityChild(bone, child, false);
        }
    }

    return bone;
}

TextureFilter MeshSystem::convertFilter(int filter){
    if (filter==TINYGLTF_TEXTURE_FILTER_NEAREST){
        return TextureFilter::NEAREST;
    }else if (filter==TINYGLTF_TEXTURE_FILTER_LINEAR){
        return TextureFilter::LINEAR;
    }else if (filter==TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_NEAREST){
        return TextureFilter::NEAREST_MIPMAP_NEAREST;
    }else if (filter==TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_NEAREST){
        return TextureFilter::LINEAR_MIPMAP_NEAREST;
    }else if (filter==TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR){
        return TextureFilter::NEAREST_MIPMAP_LINEAR;
    }else if (filter==TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR){
        return TextureFilter::LINEAR_MIPMAP_LINEAR;
    }

    return TextureFilter::NEAREST;
}

TextureWrap MeshSystem::convertWrap(int wrap){
    if (wrap==TINYGLTF_TEXTURE_WRAP_REPEAT){
        return TextureWrap::REPEAT;
    }else if (wrap==TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE){
        return TextureWrap::CLAMP_TO_EDGE;
    }else if (wrap==TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT){
        return TextureWrap::MIRRORED_REPEAT;
    }

    return TextureWrap::REPEAT;
}

void MeshSystem::calculateMeshAABB(MeshComponent& mesh){
    std::map<std::string, Buffer*> buffers;

    if (mesh.buffer.getSize() > 0){
        buffers["vertices"] = &mesh.buffer;
    }
    for (int i = 0; i < mesh.numExternalBuffers; i++){
        buffers[mesh.eBuffers[i].getName()] = &mesh.eBuffers[i];
    }

    Buffer* vertexBuffer = NULL;
    Attribute vertexAttr;

    for (auto const& buf : buffers){
        if (buf.second->getAttribute(AttributeType::POSITION)) {
            vertexBuffer = buf.second;
            vertexAttr = *buf.second->getAttribute(AttributeType::POSITION);
        }
    }

    mesh.verticesAABB = AABB::ZERO;

    for (size_t i = 0; i < mesh.numSubmeshes; i++) {
        for (auto const& attr : mesh.submeshes[i].attributes){
            if (attr.first == AttributeType::POSITION){
                vertexBuffer = buffers[attr.second.getBufferName()];
                vertexAttr = attr.second;
            }
        }

        if (!vertexBuffer || vertexAttr.getDataType() != AttributeDataType::FLOAT){
            // cannot create AABB of non float position vertex
            continue;
        }

        int verticesize = int(vertexAttr.getCount());
        for (int v = 0; v < verticesize; v++){
            mesh.verticesAABB.merge(vertexBuffer->getVector3(&vertexAttr, v));
        }
    }

    mesh.aabb = mesh.verticesAABB;

    mesh.needUpdateAABB = true;
}

void MeshSystem::createPlaneNodeSubmesh(unsigned int submeshIndex, TerrainComponent& terrain, MeshComponent& mesh, int width, int height, int widthSegments, int heightSegments){
    float width_half = (float)width / 2;
    float height_half = (float)height / 2;

    int gridX = widthSegments;
    int gridY = heightSegments;

    int gridX1 = gridX + 1;
    int gridY1 = gridY + 1;

    float segment_width = (float)width / gridX;
    float segment_height = (float)height / gridY;

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);

    Attribute* attIndice = mesh.indices.getAttribute(AttributeType::INDEX);

    int bufferCount = mesh.buffer.getCount();

    for (int iy = 0; iy < gridY1; iy++) {
        float y = iy * segment_height - height_half;
        for (int ix = 0; ix < gridX1; ix ++) {

            float x = ix * segment_width - width_half;

            mesh.buffer.addVector3(attVertex, Vector3(x, 0, -y));
            mesh.buffer.addVector3(attNormal, Vector3(0.0f, 1.0f, 0.0f));
        }
    }

    unsigned int bufferIndexCount = 0;
    unsigned int bufferIndexOffset = mesh.indices.getCount();

    for (int iy = 0; iy < gridY; iy++) {
        for (int ix = 0; ix < gridX; ix++) {

            int a = ix + gridX1 * iy;
            int b = ix + gridX1 * ( iy + 1 );
            int c = ( ix + 1 ) + gridX1 * ( iy + 1 );
            int d = ( ix + 1 ) + gridX1 * iy;

            mesh.indices.addUInt16(attIndice, a + bufferCount);
            mesh.indices.addUInt16(attIndice, d + bufferCount);
            mesh.indices.addUInt16(attIndice, b + bufferCount);

            mesh.indices.addUInt16(attIndice, b + bufferCount);
            mesh.indices.addUInt16(attIndice, d + bufferCount);
            mesh.indices.addUInt16(attIndice, c + bufferCount);

            bufferIndexCount += 6;

        }
    }

    addSubmeshAttribute(mesh.submeshes[submeshIndex], "indices", AttributeType::INDEX, 1, AttributeDataType::UNSIGNED_SHORT, bufferIndexCount, bufferIndexOffset * sizeof(uint16_t), false);
}

size_t MeshSystem::getTerrainGridArraySize(int rootGridSize, int levels){
    size_t size = 0;
    for (int i = 0; i<=(levels-1); i++){
        size += pow(4, i);
    }
    size = size * rootGridSize * rootGridSize;

    return size;
}

bool MeshSystem::raycastTerrainSurface(const Ray& ray, TerrainComponent& terrain, Transform& transform, Vector3& worldPoint){

    const unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    int channels = 0;

    if (!terrain.heightMap.empty() && !terrain.heightMap.isFramebuffer()){
        terrain.heightMap.setReleaseDataAfterLoad(false);
        TextureLoadResult result = terrain.heightMap.load();
        if (result.state == ResourceLoadState::Finished && result.data && terrain.heightMap.getData().getData()){
            TextureData& heightData = terrain.heightMap.getData();
            pixels = static_cast<const unsigned char*>(heightData.getData());
            width = heightData.getWidth();
            height = heightData.getHeight();
            channels = heightData.getChannels();
        }
    }

    auto sampleHeight = [&](float localX, float localZ, float& sampledHeight){
        if (terrain.terrainSize <= std::numeric_limits<float>::epsilon()){
            return false;
        }

        const float halfSize = terrain.terrainSize * 0.5f;
        if (localX < -halfSize || localX > halfSize || localZ < -halfSize || localZ > halfSize){
            return false;
        }

        sampledHeight = 0.0f;
        if (!pixels || width <= 0 || height <= 0 || channels <= 0){
            return true;
        }

        const float normalizedX = std::clamp((localX + halfSize) / terrain.terrainSize, 0.0f, 1.0f);
        const float normalizedZ = std::clamp((localZ + halfSize) / terrain.terrainSize, 0.0f, 1.0f);
        const float texelX = normalizedX * static_cast<float>(width - 1);
        const float texelZ = normalizedZ * static_cast<float>(height - 1);
        const int lowerX = std::clamp(static_cast<int>(std::floor(texelX)), 0, width - 1);
        const int lowerZ = std::clamp(static_cast<int>(std::floor(texelZ)), 0, height - 1);
        const int upperX = std::min(lowerX + 1, width - 1);
        const int upperZ = std::min(lowerZ + 1, height - 1);
        const float blendX = texelX - static_cast<float>(lowerX);
        const float blendZ = texelZ - static_cast<float>(lowerZ);

        auto samplePixel = [&](int sampleX, int sampleZ){
            const size_t index = (static_cast<size_t>(sampleZ) * static_cast<size_t>(width) + static_cast<size_t>(sampleX)) * static_cast<size_t>(channels);
            return pixels[index] / 255.0f;
        };

        const float height00 = samplePixel(lowerX, lowerZ);
        const float height10 = samplePixel(upperX, lowerZ);
        const float height01 = samplePixel(lowerX, upperZ);
        const float height11 = samplePixel(upperX, upperZ);
        const float lowerBlend = height00 + (height10 - height00) * blendX;
        const float upperBlend = height01 + (height11 - height01) * blendX;
        sampledHeight = (lowerBlend + (upperBlend - lowerBlend) * blendZ) * terrain.maxHeight;
        return true;
    };

    Matrix4 inverseModel = transform.modelMatrix.inverse();
    Vector3 localOrigin = inverseModel * ray.getOrigin();
    Vector3 localEnd = inverseModel * (ray.getOrigin() + ray.getDirection());
    Ray localRay(localOrigin, localEnd - localOrigin);

    const float halfSize = terrain.terrainSize * 0.5f;
    const float surfaceMinHeight = std::min(0.0f, terrain.maxHeight);
    const float surfaceMaxHeight = std::max(0.0f, terrain.maxHeight);
    const Vector3 rayOrigin = localRay.getOrigin();
    const Vector3 rayDirection = localRay.getDirection();

    float rayEntry = 0.0f;
    float rayExit = 1.0f;
    auto clipAxis = [&](float axisOrigin, float axisDirection, float minValue, float maxValue){
        if (std::abs(axisDirection) <= std::numeric_limits<float>::epsilon()){
            return axisOrigin >= minValue && axisOrigin <= maxValue;
        }

        float nearParameter = (minValue - axisOrigin) / axisDirection;
        float farParameter = (maxValue - axisOrigin) / axisDirection;
        if (nearParameter > farParameter){
            std::swap(nearParameter, farParameter);
        }
        rayEntry = std::max(rayEntry, nearParameter);
        rayExit = std::min(rayExit, farParameter);
        return rayEntry <= rayExit;
    };

    if (!clipAxis(rayOrigin.x, rayDirection.x, -halfSize, halfSize) ||
        !clipAxis(rayOrigin.y, rayDirection.y, surfaceMinHeight, surfaceMaxHeight) ||
        !clipAxis(rayOrigin.z, rayDirection.z, -halfSize, halfSize)){
        return false;
    }

    auto signedDistanceToSurface = [&](float rayParameter, float& signedDistance, Vector3& point){
        point = localRay.getPoint(rayParameter);
        float sampledHeight = 0.0f;
        if (!sampleHeight(point.x, point.z, sampledHeight)){
            return false;
        }
        signedDistance = point.y - sampledHeight;
        return true;
    };

    const int raycastSteps = 160;
    const float surfaceEpsilon = std::max(0.001f, std::abs(surfaceMaxHeight - surfaceMinHeight) * 0.0005f);
    float previousParameter = rayEntry;
    float previousDistance = 0.0f;
    Vector3 previousPoint;
    if (!signedDistanceToSurface(previousParameter, previousDistance, previousPoint)){
        return false;
    }

    float closestDistance = previousDistance;
    Vector3 closestPoint = previousPoint;
    if (std::abs(previousDistance) <= surfaceEpsilon){
        closestDistance = 0.0f;
    }

    for (int sampleIndex = 1; sampleIndex <= raycastSteps; sampleIndex++){
        const float rayParameter = rayEntry + (rayExit - rayEntry) * (static_cast<float>(sampleIndex) / static_cast<float>(raycastSteps));
        float currentDistance = 0.0f;
        Vector3 currentPoint;
        if (!signedDistanceToSurface(rayParameter, currentDistance, currentPoint)){
            continue;
        }

        if (std::abs(currentDistance) < std::abs(closestDistance)){
            closestDistance = currentDistance;
            closestPoint = currentPoint;
        }

        const bool crossedSurface = (previousDistance <= 0.0f && currentDistance >= 0.0f) ||
                                    (previousDistance >= 0.0f && currentDistance <= 0.0f);
        if (crossedSurface){
            float lowerParameter = previousParameter;
            float upperParameter = rayParameter;
            float lowerDistance = previousDistance;
            for (int refineIndex = 0; refineIndex < 12; refineIndex++){
                const float midParameter = (lowerParameter + upperParameter) * 0.5f;
                float midDistance = 0.0f;
                Vector3 midPoint;
                if (!signedDistanceToSurface(midParameter, midDistance, midPoint)){
                    break;
                }
                const bool sameSide = (lowerDistance <= 0.0f && midDistance <= 0.0f) ||
                                      (lowerDistance >= 0.0f && midDistance >= 0.0f);
                if (sameSide){
                    lowerParameter = midParameter;
                    lowerDistance = midDistance;
                }else{
                    upperParameter = midParameter;
                }
            }
            closestPoint = localRay.getPoint((lowerParameter + upperParameter) * 0.5f);
            closestDistance = 0.0f;
            break;
        }

        previousParameter = rayParameter;
        previousDistance = currentDistance;
    }

    float localHeight = 0.0f;
    if (std::abs(closestDistance) > surfaceEpsilon * 4.0f || !sampleHeight(closestPoint.x, closestPoint.z, localHeight)){
        return false;
    }

    Vector3 localPoint(closestPoint.x, localHeight, closestPoint.z);
    worldPoint = transform.modelMatrix * localPoint;
    return true;
}

float MeshSystem::getTerrainHeight(TerrainComponent& terrain, float x, float y){

    if (x < 0 || y < 0 || x >= terrain.terrainSize || y >= terrain.terrainSize)
        return 0;

    TextureData& textureData = terrain.heightMap.getData();

    int posX = floor(textureData.getWidth() * x / terrain.terrainSize);
    int posY = floor(textureData.getHeight() * y / terrain.terrainSize);

    float val = terrain.maxHeight*(textureData.getColorComponent(posX,posY,0)/255.0f);
    return val;
}

float MeshSystem::maxTerrainHeightArea(TerrainComponent& terrain, float x, float z, float w, float h) {
    float maxVal = std::numeric_limits<float>::min();
    for(float i = x; i < x+w; i++)
        for(float j = z; j < z+h; j++){
            float newVal = getTerrainHeight(terrain, i,j);
            if(newVal > maxVal) {
                maxVal = newVal;
            }
        }
    return maxVal;
}

float MeshSystem::minTerrainHeightArea(TerrainComponent& terrain, float x, float z, float w, float h) {
    float minVal = std::numeric_limits<float>::max();
    for(float i = x; i < x+w; i++)
        for(float j = z; j < z+h; j++){
            float newVal = getTerrainHeight(terrain, i,j);
            if(newVal < minVal) {
                minVal = newVal;
            }
        }
    return minVal;
}

void MeshSystem::createTerrainNode(TerrainComponent& terrain, float x, float y, float size, int lodDepth){
    TerrainNode& node = terrain.nodes[terrain.numNodes++];

    node.position = Vector2(x, y);
    node.size = size;

    node.currentRange = 0;
    node.resolution = terrain.resolution;

    float halfSize = size/2;

    if (lodDepth == 1){
        float terrainHalfSize = terrain.terrainSize / 2;
        float relativeX = x - halfSize + terrainHalfSize;
        float relativeY = y - halfSize + terrainHalfSize;

        node.hasChilds = false;

        node.maxHeight = maxTerrainHeightArea(terrain, relativeX, relativeY, size, size);
        node.minHeight = minTerrainHeightArea(terrain, relativeX, relativeY, size, size);
    }else{
        float quarterSize = halfSize/2;

        node.childs[0] = terrain.numNodes;
        createTerrainNode(terrain, x - quarterSize, y - quarterSize, halfSize, lodDepth - 1);
        terrain.nodes[node.childs[0]].visible = false;

        node.childs[1] = terrain.numNodes;
        createTerrainNode(terrain, x - quarterSize, y + quarterSize, halfSize, lodDepth - 1);
        terrain.nodes[node.childs[1]].visible = false;

        node.childs[2] = terrain.numNodes;
        createTerrainNode(terrain, x + quarterSize, y - quarterSize, halfSize, lodDepth - 1);
        terrain.nodes[node.childs[2]].visible = false;

        node.childs[3] = terrain.numNodes;
        createTerrainNode(terrain, x + quarterSize, y + quarterSize, halfSize, lodDepth - 1);
        terrain.nodes[node.childs[3]].visible = false;

        node.hasChilds = true;

        node.maxHeight = std::max(std::max(terrain.nodes[node.childs[0]].maxHeight, terrain.nodes[node.childs[1]].maxHeight), std::max(terrain.nodes[node.childs[2]].maxHeight, terrain.nodes[node.childs[3]].maxHeight));
        node.minHeight = std::min(std::min(terrain.nodes[node.childs[0]].minHeight, terrain.nodes[node.childs[1]].minHeight), std::min(terrain.nodes[node.childs[2]].minHeight, terrain.nodes[node.childs[3]].minHeight));
    }
}

bool MeshSystem::createTerrain(TerrainComponent& terrain, MeshComponent& mesh){
    // Check heightmap loading state BEFORE clearing buffers to avoid leaving
    // the mesh with empty data when the heightmap is still loading.
    terrain.heightMap.setReleaseDataAfterLoad(false);

    if (!terrain.heightMap.empty()){
        TextureLoadResult texResult = terrain.heightMap.load();
        if (texResult.state == ResourceLoadState::Loading){
            return false;
        }
    }

    for (int v = 0; v < MAX_TERRAIN_VIEWS; v++){
        for (int s = 0; s < 2; s++){
            terrain.views[v].nodesbuffer[s].clear();
            terrain.views[v].nodesbuffer[s].addAttribute(AttributeType::TERRAINNODEPOSITION, 2, true);
            terrain.views[v].nodesbuffer[s].addAttribute(AttributeType::TERRAINNODESIZE, 1, true);
            terrain.views[v].nodesbuffer[s].addAttribute(AttributeType::TERRAINNODERANGE, 1, true);
            terrain.views[v].nodesbuffer[s].addAttribute(AttributeType::TERRAINNODERESOLUTION, 1, true);
            terrain.views[v].nodesbuffer[s].setRenderAttributes(true);
            terrain.views[v].nodesbuffer[s].setInstanceBuffer(true);
            terrain.views[v].nodesbuffer[s].setUsage(BufferUsage::STREAM);
        }
    }

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);

    mesh.indices.clear();

    size_t idealSize = getTerrainGridArraySize(terrain.rootGridSize, terrain.levels);
    terrain.nodes.resize(idealSize);

    mesh.numSubmeshes = 2;
    // fullRes submesh
    createPlaneNodeSubmesh(0, terrain, mesh, 1, 1, terrain.resolution, terrain.resolution);
    // halfRes submesh (internal LOD)
    createPlaneNodeSubmesh(1, terrain, mesh, 1, 1, terrain.resolution/2, terrain.resolution/2);
    mesh.submeshes[1].generated = true;

    updateTerrainAutoRanges(terrain);

    float rootNodeSize = terrain.terrainSize / terrain.rootGridSize;

    if (terrain.autoSetRanges){
        CameraComponent& camera = scene->getComponent<CameraComponent>(scene->getCamera());
        if (camera.farClip < (rootNodeSize * 2)){
            Log::warn("Terrain quadtree root is not in camera field of view. Increase terrain root grid.");
        }
    }

    terrain.numNodes = 0;

    // center terrain
    float offset = (terrain.terrainSize / 2) - (rootNodeSize / 2);

    terrain.numNodes = 0;
    for (int i = 0; i < terrain.rootGridSize; i++) {
        for (int j = 0; j < terrain.rootGridSize; j++) {
            float xPos = i*rootNodeSize;
            float zPos = j*rootNodeSize;

            terrain.grid[j + i*terrain.rootGridSize] = terrain.numNodes;
            createTerrainNode(terrain, xPos-offset, zPos-offset, rootNodeSize, terrain.levels);
        }
    }

    Vector3 min = Vector3(-(terrain.terrainSize / 2), 0, -(terrain.terrainSize / 2));
    Vector3 max = Vector3((terrain.terrainSize / 2), terrain.maxHeight, (terrain.terrainSize / 2));

    mesh.aabb = AABB(min, max);

    terrain.heightMapLoaded = true;

    return true;
}

void MeshSystem::updateTerrainAutoRanges(TerrainComponent& terrain){
    if (!terrain.autoSetRanges || !terrain.heightMapLoaded){
        return;
    }

    if (scene->getCamera() == NULL_ENTITY){
        return;
    }

    CameraComponent& camera = scene->getComponent<CameraComponent>(scene->getCamera());

    bool rangesInitialized = terrain.ranges.size() == static_cast<size_t>(terrain.levels);
    if (!camera.needUpdate && rangesInitialized){
        return;
    }

    float rootNodeSize = terrain.terrainSize / terrain.rootGridSize;
    float lastLevel = std::max(camera.farClip, rootNodeSize * 2);

    if (rangesInitialized && terrain.ranges[terrain.levels - 1] == lastLevel){
        return;
    }

    terrain.ranges.clear();
    terrain.ranges.resize(terrain.levels);
    terrain.ranges[terrain.levels - 1] = lastLevel;
    for (int i = terrain.levels - 2; i >= 0; i--) {
        terrain.ranges[i] = terrain.ranges[i + 1] / 2;
    }
}

void MeshSystem::createPlane(MeshComponent& mesh, float width, float depth, unsigned int tiles){
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.numSubmeshes = 1;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);

    float halfWidth = width / 2.0;
    float halfDepth = depth / 2.0;

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, 0, -halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, 0, halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth, 0, halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth, 0, -halfDepth));

    Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 0.0f));

    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 1.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 1.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 1.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 1.0f, 0.0f));

    Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));

    static const uint16_t indices_array[] = {
        0,  1,  2,
        0,  2,  3,
    };

    mesh.indices.clear();

    mesh.indices.setValues(
        0, mesh.indices.getAttribute(AttributeType::INDEX),
        6, (char*)&indices_array[0], sizeof(uint16_t));

    calculateMeshAABB(mesh);

    if (mesh.loaded)
        mesh.needReload = true;
}

// Vertical quad in the XY plane facing +Z (width along X, height along Y).
// Unlike createPlane (horizontal, +Y normal), a wall stands upright — its +Z
// normal matches the MirrorComponent default, so a mirror needs no rotation.
void MeshSystem::createWall(MeshComponent& mesh, float width, float height, unsigned int tiles){
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.numSubmeshes = 1;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);

    float halfWidth = width / 2.0;
    float halfHeight = height / 2.0;

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, -halfHeight, 0));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth, -halfHeight, 0));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth, halfHeight, 0));
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, halfHeight, 0));

    Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 0.0f));

    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));

    Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));

    static const uint16_t indices_array[] = {
        0,  1,  2,
        0,  2,  3,
    };

    mesh.indices.clear();

    mesh.indices.setValues(
        0, mesh.indices.getAttribute(AttributeType::INDEX),
        6, (char*)&indices_array[0], sizeof(uint16_t));

    calculateMeshAABB(mesh);

    if (mesh.loaded)
        mesh.needReload = true;
}

void MeshSystem::createBox(MeshComponent& mesh, float width, float height, float depth, unsigned int tiles){
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.numSubmeshes = 1;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);

    float halfWidth = width / 2.0;
    float halfHeight = height / 2.0;
    float halfDepth = depth / 2.0;

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);

    // Front face (Z+)
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, -halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth, -halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth,  halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, halfHeight,  halfDepth));

    // Back face (Z-)
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, -halfHeight, -halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth, -halfHeight, -halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth,  halfHeight, -halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth,  halfHeight, -halfDepth));

    // Left face (X-)
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, -halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth,  halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth,  halfHeight, -halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, -halfHeight, -halfDepth));

    // Right face (X+)
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth, -halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth,  halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth,  halfHeight, -halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth, -halfHeight, -halfDepth));

    // Top face (Y+)
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth,  halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth,  halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth,  halfHeight, -halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth,  halfHeight, -halfDepth));

    // Bottom face (Y-)
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, -halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth, -halfHeight,  halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(halfWidth, -halfHeight, -halfDepth));
    mesh.buffer.addVector3(attVertex, Vector3(-halfWidth, -halfHeight, -halfDepth));

    Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);

    // Front face (Z+)
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 0.0f));

    // Back face (Z-)
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 0.0f));

    // Left face (X-)
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 1.0f * tiles));

    // Right face (X+)
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 1.0f * tiles));

    // Top face (Y+)
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 0.0f));

    // Bottom face (Y-)
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 0.0f));
    mesh.buffer.addVector2(attTexcoord, Vector2(1.0f * tiles, 1.0f * tiles));
    mesh.buffer.addVector2(attTexcoord, Vector2(0.0f, 1.0f * tiles));

    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);

    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, 1.0f));
    
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, -1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, -1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, -1.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 0.0f, -1.0f));
    
    mesh.buffer.addVector3(attNormal, Vector3(-1.0f, 0.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(-1.0f, 0.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(-1.0f, 0.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(-1.0f, 0.0f, 0.0f));
    
    mesh.buffer.addVector3(attNormal, Vector3(1.0f, 0.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(1.0f, 0.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(1.0f, 0.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(1.0f, 0.0f, 0.0f));
    
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 1.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 1.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 1.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, 1.0f, 0.0f));
    
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, -1.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, -1.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, -1.0f, 0.0f));
    mesh.buffer.addVector3(attNormal, Vector3(0.0f, -1.0f, 0.0f));

    Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);

    for (int i = 0; i < 6; i++){
        mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    }

    static const uint16_t indices_array[] = {
            // front
            0,  1,  2,
            0,  2,  3,
            // back
            4,  6,  5,
            4,  7,  6,
            // left
            8,  9, 10,
            8, 10, 11,
            // right
            12, 14, 13,
            12, 15, 14,
            // top
            16, 17, 18,
            16, 18, 19,
            // bottom
            20, 22, 21,
            20, 23, 22,
    };

    mesh.indices.clear();

    mesh.indices.setValues(
        0, mesh.indices.getAttribute(AttributeType::INDEX),
        36, (char*)&indices_array[0], sizeof(uint16_t));

    calculateMeshAABB(mesh);

    if (mesh.loaded)
        mesh.needReload = true;
}

void MeshSystem::createSphere(MeshComponent& mesh, float radius, unsigned int slices, unsigned int stacks){
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.numSubmeshes = 1;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
    Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);
    Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);

    float x, y, z, xz;                              // vertex position
    float nx, ny, nz, lengthInv = 1.0f / radius;    // vertex normal
    float s, t;                                     // vertex texCoord

    float sectorStep = 2 * M_PI / slices;
    float stackStep = M_PI / stacks;
    float sectorAngle, stackAngle;

    for(int i = 0; i <= stacks; ++i){
        stackAngle = M_PI / 2 - i * stackStep;      // starting from pi/2 to -pi/2
        xz = radius * cosf(stackAngle);             // r * cos(u)
        y = radius * sinf(stackAngle);              // r * sin(u)

        // add (sectorCount+1) vertices per stack
        // the first and last vertices have same position and normal, but different tex coords
        for(int j = 0; j <= slices; ++j){
            sectorAngle = j * sectorStep;           // starting from 0 to 2pi

            // vertex position (x, y, z)
            x = xz * sinf(sectorAngle);             // r * cos(u) * sin(v)
            z = -xz * cosf(sectorAngle);            // -r * cos(u) * cos(v)
            mesh.buffer.addVector3(attVertex, Vector3(x, y, z));

            // normalized vertex normal (nx, ny, nz)
            nx = x * lengthInv;
            ny = y * lengthInv;
            nz = z * lengthInv;
            mesh.buffer.addVector3(attNormal, Vector3(nx, ny, nz));

            // Z+ orientation means the texture faces the positive Z direction
            s = 0.5f + atan2f(x, z) / (2.0f * M_PI);

            // Remove Y flip since circle now starts at Z-
            t = (float)i / stacks;

            mesh.buffer.addVector2(attTexcoord, Vector2(s, t));

            // vertex color (white)
            mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        }
    }

    // generate CCW index list of sphere triangles
    std::vector<uint16_t> indices;
    int k1, k2;
    for(int i = 0; i < stacks; ++i){
        k1 = i * (slices + 1);     // beginning of current stack
        k2 = k1 + slices + 1;      // beginning of next stack

        for(int j = 0; j < slices; ++j, ++k1, ++k2){
            // 2 triangles per sector excluding first and last stacks
            // k1 => k2 => k1+1
            if(i != 0){
                indices.push_back(k1);
                indices.push_back(k1 + 1);
                indices.push_back(k2);
            }

            // k1+1 => k2 => k2+1
            if(i != (stacks-1)){
                indices.push_back(k1 + 1);
                indices.push_back(k2 + 1);
                indices.push_back(k2);
            }
        }
    }

    mesh.indices.clear();

    mesh.indices.setValues(
        0, mesh.indices.getAttribute(AttributeType::INDEX),
        indices.size(), (char*)&indices[0], sizeof(uint16_t));

    calculateMeshAABB(mesh);

    if (mesh.loaded)
        mesh.needReload = true;
}

void MeshSystem::createCylinder(MeshComponent& mesh, float baseRadius, float topRadius, float height, unsigned int slices, unsigned int stacks){
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.numSubmeshes = 1;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
    Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);
    Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);

    float x, y, z;                                  // vertex position
    float radius;                                   // radius for each stack

    std::vector<float> sideNormals = getCylinderSideNormals(baseRadius, topRadius, height, slices);
    std::vector<float> unitCircleVertices = buildUnitCircleVertices(slices);

    // put vertices of side cylinder to array by scaling unit circle
    for(int i = 0; i <= stacks; ++i){
        y = -(height * 0.5f) + (float)i / stacks * height;      // vertex position y
        radius = baseRadius + (float)i / stacks * (topRadius - baseRadius);     // lerp
        float t = 1.0f - (float)i / stacks;   // top-to-bottom

        for(int j = 0, k = 0; j <= slices; ++j, k += 3){
            x = unitCircleVertices[k];
            z = unitCircleVertices[k+2];
            mesh.buffer.addVector3(attVertex, Vector3(x * radius, y, z * radius));
            mesh.buffer.addVector3(attNormal, Vector3(sideNormals[k], sideNormals[k+1], sideNormals[k+2]));
            mesh.buffer.addVector2(attTexcoord, Vector2(1.0f - (float)j / slices, t));
            mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        }
    }

    // remember where the base.top vertices start
    unsigned int baseVertexIndex = mesh.buffer.getCount();

    // put vertices of base of cylinder
    y = -height * 0.5f;
    mesh.buffer.addVector3(attVertex, Vector3(0, y, 0));
    mesh.buffer.addVector3(attNormal, Vector3(0, -1, 0));
    mesh.buffer.addVector2(attTexcoord, Vector2(0.5f, 0.5f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    for(int i = 0, j = 0; i < slices; ++i, j += 3){
        x = unitCircleVertices[j];
        z = unitCircleVertices[j+2];
        mesh.buffer.addVector3(attVertex, Vector3(x * baseRadius, y, z * baseRadius));
        mesh.buffer.addVector3(attNormal, Vector3(0, -1, 0));
        mesh.buffer.addVector2(attTexcoord, Vector2(x * 0.5f + 0.5f, -z * 0.5f + 0.5f));
        mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    }

    // remember where the base vertices start
    unsigned int topVertexIndex = mesh.buffer.getCount();

    // put vertices of top of cylinder
    y = height * 0.5f;
    mesh.buffer.addVector3(attVertex, Vector3(0, y, 0));
    mesh.buffer.addVector3(attNormal, Vector3(0, 1, 0));
    mesh.buffer.addVector2(attTexcoord, Vector2(0.5f, 0.5f));
    mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    for(int i = 0, j = 0; i < slices; ++i, j += 3){
        x = unitCircleVertices[j];
        z = unitCircleVertices[j+2];
        mesh.buffer.addVector3(attVertex, Vector3(x * topRadius, y, z * topRadius));
        mesh.buffer.addVector3(attNormal, Vector3(0, 1, 0));
        mesh.buffer.addVector2(attTexcoord, Vector2(-x * 0.5f + 0.5f, -z * 0.5f + 0.5f));
        mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    }

    std::vector<uint16_t> indices;

    // put indices for sides
    unsigned int k1, k2;
    for(int i = 0; i < stacks; ++i){
        k1 = i * (slices + 1);     // bebinning of current stack
        k2 = k1 + slices + 1;      // beginning of next stack

        for(int j = 0; j < slices; ++j, ++k1, ++k2){
            // 2 trianles per sector
            indices.push_back(k1);
            indices.push_back(k2);
            indices.push_back(k1 + 1);

            indices.push_back(k2);
            indices.push_back(k2 + 1);
            indices.push_back(k1 + 1);
        }
    }

    // put indices for base
    for(int i = 0, k = baseVertexIndex + 1; i < slices; ++i, ++k){
        if(i < (slices - 1)){
            indices.push_back(baseVertexIndex);
            indices.push_back(k);
            indices.push_back(k + 1);
        }else{    // last triangle
            indices.push_back(baseVertexIndex);
            indices.push_back(k);
            indices.push_back(baseVertexIndex + 1);
        }
    }

    for(int i = 0, k = topVertexIndex + 1; i < slices; ++i, ++k)
    {
        if(i < (slices - 1)){
            indices.push_back(topVertexIndex);
            indices.push_back(k + 1);
            indices.push_back(k);
        }else{
            indices.push_back(topVertexIndex);
            indices.push_back(topVertexIndex + 1);
            indices.push_back(k);
        }
    }

    mesh.indices.clear();

    mesh.indices.setValues(
        0, mesh.indices.getAttribute(AttributeType::INDEX),
        indices.size(), (char*)&indices[0], sizeof(uint16_t));

    calculateMeshAABB(mesh);

    if (mesh.loaded)
        mesh.needReload = true;
}

void MeshSystem::createCapsule(MeshComponent& mesh, float baseRadius, float topRadius, float height, unsigned int slices, unsigned int stacks){
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.numSubmeshes = 1;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
    Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);
    Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);

    float stackHeight = height / stacks;
    float stackAngle = 2 * M_PI / slices;

    // Top Hemisphere
    for (int i = 0; i <= stacks / 2; ++i) {
        float theta = i * M_PI / stacks;
        float y = topRadius * cos(theta);
        float r = topRadius * sin(theta);
        for (int j = 0; j <= slices; ++j) {
            float phi = j * stackAngle;
            float x = r * sin(phi);
            float z = -r * cos(phi);
            float s = (float)j / slices;
            float t = (float)(stacks / 2 - i) / (stacks / 2);

            mesh.buffer.addVector3(attVertex, Vector3(x, y + height / 2, z));
            mesh.buffer.addVector3(attNormal, Vector3(x / topRadius, y / topRadius, z / topRadius));
            mesh.buffer.addVector2(attTexcoord, Vector2(s, t));
            mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        }
    }

    // Bottom Hemisphere
    for (int i = stacks / 2; i <= stacks; ++i) {
        float theta = i * M_PI / stacks;
        float y = baseRadius * cos(theta);
        float r = baseRadius * sin(theta);
        for (int j = 0; j <= slices; ++j) {
            float phi = j * stackAngle;
            float x = r * sin(phi);
            float z = -r * cos(phi);
            float s = (float)j / slices;
            float t = (float)(stacks - i) / (stacks / 2);

            mesh.buffer.addVector3(attVertex, Vector3(x, y - height / 2, z));
            mesh.buffer.addVector3(attNormal, Vector3(x / baseRadius, y / baseRadius, z / baseRadius));
            mesh.buffer.addVector2(attTexcoord, Vector2(s, t));
            mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        }
    }

    std::vector<uint16_t> indices;

    for (int i = 0; i < (stacks + 1); ++i) {
        for (int j = 0; j < (slices + 1); ++j) {
            int first = (i * (slices + 1)) + j;
            int second = first + slices + 1;

            indices.push_back(first);
            indices.push_back(first + 1);
            indices.push_back(second);

            indices.push_back(second);
            indices.push_back(first + 1);
            indices.push_back(second + 1);
        }
    }

    mesh.indices.clear();

    mesh.indices.setValues(
        0, mesh.indices.getAttribute(AttributeType::INDEX),
        indices.size(), (char*)&indices[0], sizeof(uint16_t));

    calculateMeshAABB(mesh);

    if (mesh.loaded)
        mesh.needReload = true;
}

void MeshSystem::createTorus(MeshComponent& mesh, float radius, float ringRadius, unsigned int sides, unsigned int rings){
    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.numSubmeshes = 1;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
    Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);
    Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);

    const float two_pi = 2.0f * M_PI;
    const float dv = 1.0f / sides;
    const float du = 1.0f / rings;

    // generate vertices
    for (uint32_t side = 0; side <= sides; side++) {
        const float phi = (side * two_pi) / sides;
        const float sin_phi = sinf(phi);
        const float cos_phi = cosf(phi);
        for (uint32_t ring = 0; ring <= rings; ring++) {
            const float theta = (ring * two_pi) / rings;
            const float sin_theta = sinf(theta);
            const float cos_theta = cosf(theta);

            // torus surface position
            const float spx = sin_theta * (radius - (ringRadius * cos_phi));
            const float spy = sin_phi * ringRadius;
            const float spz = -cos_theta * (radius - (ringRadius * cos_phi));

            // torus position with ring-radius zero (for normal computation)
            const float ipx = sin_theta * radius;
            const float ipy = 0.0f;
            const float ipz = -cos_theta * radius;

            mesh.buffer.addVector3(attVertex, Vector3(spx, spy, spz));
            mesh.buffer.addVector3(attNormal, Vector3(spx - ipx, spy - ipy, spz - ipz));
            mesh.buffer.addVector2(attTexcoord, Vector2(ring * du, side * dv));
            mesh.buffer.addVector4(attColor, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        }
    }

    // generate indices
    std::vector<uint16_t> indices;
    for (uint16_t side = 0; side < sides; side++) {
        const uint16_t row_a = side * (rings + 1);
        const uint16_t row_b = row_a + rings + 1;
        for (uint16_t ring = 0; ring < rings; ring++) {
            indices.push_back(row_a + ring);
            indices.push_back(row_a + ring + 1);
            indices.push_back(row_b + ring + 1);

            indices.push_back(row_a + ring);
            indices.push_back(row_b + ring + 1);
            indices.push_back(row_b + ring);
        }
    }

    mesh.indices.clear();

    mesh.indices.setValues(
        0, mesh.indices.getAttribute(AttributeType::INDEX),
        indices.size(), (char*)&indices[0], sizeof(uint16_t));

    calculateMeshAABB(mesh);

    if (mesh.loaded)
        mesh.needReload = true;
}

bool MeshSystem::loadGLTF(Entity entity, const std::string filename, bool asyncLoad, bool skipEntities, bool changeRootTransform){
    const std::string poolKey = getModelFilenameKey(filename);

    // If parsed data is already cached, skip the async background-thread entirely.
    if (asyncLoad && ModelPool::getGLTF(poolKey)){
        asyncLoad = false;
    }

    std::shared_ptr<AsyncModelLoadResult> asyncResult;
    if (asyncLoad){
        asyncResult = pollOrStartAsyncModelLoad(filename, false);
        if (!asyncResult || !asyncResult->success || !asyncResult->gltfModel){
            return false;
        }
    }

    MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
    ModelComponent& model = scene->getComponent<ModelComponent>(entity);
    Transform& transform = scene->getComponent<Transform>(entity);

    destroyModel(model);

    model.filename = filename;

    uint64_t buildId = asyncLoad ? std::hash<std::string>{}(getAsyncModelLoadKey(filename)) : 0;

    std::string err;
    std::string warn;

    std::vector<std::string> loadedBuffers;

    mesh.numExternalBuffers = 0;
    mesh.normAdjustJoint = 1.0f;
    mesh.normAdjustWeight = 1.0f;
    for (int b = 0; b < MAX_BONES; b++) {
        mesh.bonesMatrix[b].identity();
    }

    bool res = false;

    if (asyncLoad){
        // Async loader already produced a parsed model; reuse it (and cache it).
        auto cached = ModelPool::getGLTF(poolKey);
        if (cached){
            model.gltfModel = cached; // textures already pooled by the original load
        }else{
            model.gltfModel = asyncResult->gltfModel;
            ModelPool::addGLTF(poolKey, model.gltfModel);
            // Publish the texture pixels the worker already copied, so loadGLTFTexture below hits
            // the pool's reuse path instead of doing the big memcpy on this (main) thread.
            for (auto& prebuilt : asyncResult->prebuiltTextures){
                if (prebuilt.second){
                    TextureDataPool::put(prebuilt.first, prebuilt.second);
                }
            }
        }
        res = true;
    }else{
        // Try cache before hitting disk.
        auto cached = ModelPool::getGLTF(poolKey);
        if (cached){
            model.gltfModel = cached;
            res = true;
        }else{
            model.gltfModel = std::make_shared<tinygltf::Model>();

            tinygltf::TinyGLTF loader;
            loader.SetFsCallbacks({&fileExists, &tinygltf::ExpandFilePath, &readWholeFile, &tinygltf::WriteWholeFile, &getFileSizeInBytes});
            loader.SetImagesAsIs(true); // keep images encoded; decode in parallel below

            std::string ext = FileData::getFilePathExtension(filename);

            if (ext.compare("glb") == 0) {
                res = loader.LoadBinaryFromFile(model.gltfModel.get(), &err, &warn, filename); // for binary glTF(.glb)
            }else{
                res = loader.LoadASCIIFromFile(model.gltfModel.get(), &err, &warn, filename);
            }

            if (!warn.empty()) {
                Log::warn("Loading GLTF model (%s): %s", filename.c_str(), warn.c_str());
            }

            if (!err.empty()) {
                Log::error("Can't load GLTF model (%s): %s", filename.c_str(), err.c_str());
                return false;
            }

            if (!res) {
                Log::verbose("Failed to load glTF: %s", filename.c_str());
                return false;
            }

            const int maxImageDim = g_imageDecodeMaxDimension;
            decodeGLTFImagesParallel(*model.gltfModel, maxImageDim);

            // Only cache full-resolution parses. A downscaled preview decode must not be reused
            // as the full-resolution model in the user's scene.
            if (maxImageDim == 0) {
                ModelPool::addGLTF(poolKey, model.gltfModel);
            }
        }
    }

    // Build parent map and collect all nodes that own a mesh primitive.
    int meshNode    = -1; // first skinned mesh node — used for skeleton lookup
    int transformNode = -1; // scene root — used for initial entity transform
    std::vector<int> meshNodes;
    std::map<int, int> nodesParent;

    for (size_t i = 0; i < model.gltfModel->nodes.size(); i++) {
        nodesParent[static_cast<int>(i)] = -1;
    }

    for (size_t i = 0; i < model.gltfModel->nodes.size(); i++) {
        const tinygltf::Node& node = model.gltfModel->nodes[i];

        if (node.mesh >= 0) {
            meshNodes.push_back(static_cast<int>(i));
            if (meshNode < 0 && node.skin >= 0) {
                meshNode = static_cast<int>(i);
            }
        }

        for (int child : node.children) {
            nodesParent[child] = static_cast<int>(i);
        }
    }

    if (meshNodes.empty()) {
        Log::error("GLTF model has no mesh nodes: %s", filename.c_str());
        if (asyncLoad) {
            ResourceProgress::failBuild(buildId);
        }
        return false;
    }

    // Multi-node models spread their mesh nodes across child entities (each with its own Transform)
    // so per-node placement survives save/load. Skinned models stay flattened on the root mesh,
    // because the skeleton — not the node transforms — drives their vertices.
    bool anyNodeSkinned = false;
    for (int nodeIdx : meshNodes) {
        if (model.gltfModel->nodes[nodeIdx].skin >= 0) { anyNodeSkinned = true; break; }
    }
    // Building the child entities mutates the ECS, which is only safe on the main thread. Off-thread
    // loads (e.g. the ResourcesWindow thumbnail worker) flatten into the root mesh instead — building
    // the hierarchy there corrupts the registry's component maps.
    bool useChildEntities = (meshNodes.size() > 1) && !anyNodeSkinned && !Engine::isAsyncThread();

    // Off-thread preview of a multi-node model can't build child entities, so instead bake each
    // node's world transform into its (flattened) vertices. This keeps the preview laid out correctly
    // without touching the ECS. Not used for the real scene load (it builds child entities).
    bool bakingFlatten = (meshNodes.size() > 1) && !anyNodeSkinned && !useChildEntities;

    if (meshNodes.size() > 1 && !useChildEntities && !bakingFlatten) {
        Log::warn("GLTF model (%s) has %zu mesh nodes but is skinned; loading them as submeshes on one entity. Per-node transforms are ignored",
                  filename.c_str(), meshNodes.size());
    }

    if (meshNode < 0) {
        meshNode = meshNodes[0];
    }

    // Prefer the GLTF scene root for the entity transform; fall back to the first mesh node.
    if (!model.gltfModel->scenes.empty()) {
        int sceneIdx = model.gltfModel->defaultScene >= 0 ? model.gltfModel->defaultScene : 0;
        if (isValidGLTFIndex(sceneIdx, model.gltfModel->scenes) && !model.gltfModel->scenes[sceneIdx].nodes.empty()) {
            transformNode = model.gltfModel->scenes[sceneIdx].nodes[0];
        }
    }
    if (transformNode < 0) {
        transformNode = meshNodes[0];
    }

    Matrix4 matrix = getGLTFMeshGlobalMatrix(transformNode, model, nodesParent);

    // In the child-entity path (and the baked-flatten preview) node transforms live on the children /
    // baked vertices, so the root entity keeps its drag/identity transform to avoid applying it twice.
    if (changeRootTransform && !useChildEntities && !bakingFlatten) {
        bool hasDefaultPosition = (transform.position == Vector3::ZERO);
        bool hasDefaultRotation = (transform.rotation == Quaternion::IDENTITY);
        bool hasDefaultScale    = (transform.scale    == Vector3::UNIT_SCALE);

        Vector3    newPosition;
        Vector3    newScale;
        Quaternion newRotation;
        matrix.decompose(newPosition, newScale, newRotation);

        if (hasDefaultPosition) transform.position = newPosition;
        if (hasDefaultRotation) transform.rotation = newRotation;
        if (hasDefaultScale)    transform.scale    = newScale;
        transform.needUpdate = true;
    }

    mesh.cullingMode = CullingMode::BACK;
    mesh.windingOrder = (matrix.determinant() < 0.0) ? WindingOrder::CW : WindingOrder::CCW;

    if (asyncLoad) {
        ResourceProgress::updateProgress(buildId, 0.4f); // Transform processing done
    }

    // Count total primitives to detect empty models early.
    unsigned int totalSubmeshes = 0;
    for (int nodeIdx : meshNodes) {
        int gltfMeshIndex = model.gltfModel->nodes[nodeIdx].mesh;
        if (isValidGLTFIndex(gltfMeshIndex, model.gltfModel->meshes)) {
            totalSubmeshes += static_cast<unsigned int>(model.gltfModel->meshes[gltfMeshIndex].primitives.size());
        }
    }

    if (totalSubmeshes == 0) {
        Log::error("GLTF model has no mesh primitives: %s", filename.c_str());
        if (asyncLoad) {
            ResourceProgress::failBuild(buildId);
        }
        return false;
    }

    model.morphNameMapping.clear();

    if (useChildEntities) {
        // Each node becomes its own child mesh; the root mesh stays an empty container (loadMesh
        // skips it). numExternalBuffers was already zeroed at the top of loadGLTF.
        mesh.numSubmeshes = 0;
    } else {
        // Flatten path: every node's primitives accumulate into the single root MeshComponent, so its
        // submesh array is sized for the total up-front and the buffer dedup/reserve happens once.
        mesh.numSubmeshes = totalSubmeshes;
        if (!mesh.submeshes.validIndex(static_cast<int>(totalSubmeshes) - 1)) {
            Log::error("Model %s has more submeshes than the maximum allowed (%i)", filename.c_str(), mesh.submeshes.size());
            mesh.numSubmeshes = static_cast<unsigned int>(mesh.submeshes.size());
        }
        loadedBuffers.clear();
        if (model.gltfModel) {
            // One synthetic view per primitive for byte-index expansion, plus two more (position +
            // normal) when baking node transforms for a preview. Reserve so appends never reallocate
            // and invalidate the non-owning external-buffer pointers.
            const size_t synthPerSubmesh = bakingFlatten ? 3 : 1;
            model.gltfModel->buffers.reserve(model.gltfModel->buffers.size() + mesh.numSubmeshes * synthPerSubmesh);
            model.gltfModel->bufferViews.reserve(model.gltfModel->bufferViews.size() + mesh.numSubmeshes * synthPerSubmesh);
        }
    }

    unsigned int submeshIndex = 0; // continuous across nodes for the flatten path
    for (int nodeIdx : meshNodes) {
        int gltfMeshIndex = model.gltfModel->nodes[nodeIdx].mesh;
        if (!isValidGLTFIndex(gltfMeshIndex, model.gltfModel->meshes)) {
            continue;
        }

        tinygltf::Mesh& gltfmesh = model.gltfModel->meshes[gltfMeshIndex];

        // For the baked-flatten preview, precompute this node's world matrix (for positions) and its
        // normal matrix (inverse-transpose of the linear part) to bake into the vertices below.
        Matrix4 nodeBakeMatrix;
        Matrix3 nodeBakeNormalMatrix;
        if (bakingFlatten) {
            nodeBakeMatrix = getGLTFMeshGlobalMatrix(nodeIdx, model, nodesParent);
            nodeBakeNormalMatrix = nodeBakeMatrix.linear().inverse(1e-6f).transpose();
        }

        // Select the MeshComponent this node's primitives load into: the root entity for the
        // single-node/skinned path, or a per-node child entity (created or reused) otherwise.
        Entity meshEntity = entity;
        if (useChildEntities) {
            meshEntity = NULL_ENTITY;
            auto existing = model.meshNodesMapping.find(nodeIdx);
            if (existing != model.meshNodesMapping.end() &&
                    scene->getSignature(existing->second).test(scene->getComponentId<MeshComponent>())) {
                meshEntity = existing->second; // reuse deserialized child (skipEntities reload)
            }
            if (meshEntity == NULL_ENTITY) {
                meshEntity = scene->createEntity();
                if (meshEntity == NULL_ENTITY) {
                    continue;
                }
                scene->addComponent<Transform>(meshEntity);
                scene->addComponent<MeshComponent>(meshEntity);

                const std::string& nodeName = model.gltfModel->nodes[nodeIdx].name;
                scene->setEntityName(meshEntity, nodeName.empty() ? ("MeshNode " + std::to_string(nodeIdx)) : nodeName);

                // Flat under the root: the child carries its full gltf-space global matrix as its
                // local transform, so root.world * childGlobal reproduces the node's placement.
                Transform& childTransform = scene->getComponent<Transform>(meshEntity);
                Matrix4 nodeGlobal = getGLTFMeshGlobalMatrix(nodeIdx, model, nodesParent);
                nodeGlobal.decompose(childTransform.position, childTransform.scale, childTransform.rotation);

                scene->addEntityChild(entity, meshEntity, false);
                model.meshNodesMapping[nodeIdx] = meshEntity;
            }

            // Refresh the world matrix (also on reuse from a reopened scene) so the child doesn't
            // render at a stale transform until it is moved.
            scene->getComponent<Transform>(meshEntity).needUpdate = true;
        }

        MeshComponent& mesh = scene->getComponent<MeshComponent>(meshEntity); // shadows root mesh

        // Per-node fresh setup for the child path only. The flatten path keeps the root mesh's state
        // set once before the loop so all nodes accumulate (a continuous submeshIndex, shared buffers).
        if (useChildEntities) {
            loadedBuffers.clear(); // dedup is per child MeshComponent
            submeshIndex = 0;      // each child mesh starts at submesh 0

            mesh.numExternalBuffers = 0;
            mesh.normAdjustJoint = 1.0f;
            mesh.normAdjustWeight = 1.0f;
            for (int b = 0; b < MAX_BONES; b++) {
                mesh.bonesMatrix[b].identity();
            }

            mesh.numSubmeshes = static_cast<unsigned int>(gltfmesh.primitives.size());
            if (mesh.numSubmeshes > 0 && !mesh.submeshes.validIndex(static_cast<int>(mesh.numSubmeshes) - 1)) {
                Log::error("Model %s has more submeshes than the maximum allowed (%i)", filename.c_str(), mesh.submeshes.size());
                mesh.numSubmeshes = static_cast<unsigned int>(mesh.submeshes.size());
            }

            mesh.cullingMode = CullingMode::BACK;
            mesh.windingOrder = (getGLTFMeshGlobalMatrix(nodeIdx, model, nodesParent).determinant() < 0.0) ? WindingOrder::CW : WindingOrder::CCW;

            // 8-bit indices expand to 16-bit synthetic buffers/views (one per primitive at most).
            // Reserve up-front so appends never reallocate and invalidate external-buffer pointers.
            if (model.gltfModel) {
                model.gltfModel->buffers.reserve(model.gltfModel->buffers.size() + mesh.numSubmeshes);
                model.gltfModel->bufferViews.reserve(model.gltfModel->bufferViews.size() + mesh.numSubmeshes);
            }
        }

        for (size_t p = 0; p < gltfmesh.primitives.size(); p++) {
            if (submeshIndex >= mesh.numSubmeshes) {
                break;
            }

            // Claim this slot now so any early-continue still advances the index correctly.
            const unsigned int i = submeshIndex++;

            if (asyncLoad) {
                float submeshProgress = 0.4f + (0.4f * (float(i) / float(mesh.numSubmeshes)));
                ResourceProgress::updateProgress(buildId, submeshProgress);
            }

            mesh.submeshes[i].attributes.clear();
            mesh.submeshes[i].primitiveType = PrimitiveType::TRIANGLES;
            applyDefaultGLTFMaterial(mesh.submeshes[i].material);

            const tinygltf::Primitive& primitive = gltfmesh.primitives[p];

            if (!isValidGLTFIndex(primitive.indices, model.gltfModel->accessors)) {
                Log::error("GLTF primitive %u has no valid index accessor: %s", i, filename.c_str());
                continue;
            }

            const tinygltf::Accessor& indexAccessor = model.gltfModel->accessors[primitive.indices];
            if (!isValidGLTFIndex(indexAccessor.bufferView, model.gltfModel->bufferViews)) {
                Log::error("GLTF primitive %u has no valid index buffer view: %s", i, filename.c_str());
                continue;
            }

            const tinygltf::Material* mat = nullptr;
            if (primitive.material >= 0) {
                if (isValidGLTFIndex(primitive.material, model.gltfModel->materials)) {
                    mat = &model.gltfModel->materials[primitive.material];
                } else {
                    Log::warn("GLTF primitive %u has invalid material index %i. Using default material: %s", i, primitive.material, filename.c_str());
                }
            }

            AttributeDataType indexType;
            int indexBufferView = indexAccessor.bufferView;
            size_t indexByteOffset = indexAccessor.byteOffset;
            if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                indexType = AttributeDataType::UNSIGNED_SHORT;
            } else if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                indexType = AttributeDataType::UNSIGNED_INT;
            } else if (indexAccessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                // No 8-bit index format in the renderer: expand to 16-bit via a synthetic buffer view.
                indexBufferView = convertGLTFByteIndicesToShort(indexAccessor, model);
                if (!isValidGLTFIndex(indexBufferView, model.gltfModel->bufferViews)) {
                    Log::error("Failed to convert 8-bit indices for primitive %u: %s", i, filename.c_str());
                    continue;
                }
                indexType = AttributeDataType::UNSIGNED_SHORT;
                indexByteOffset = 0;
            } else {
                Log::error("Unknown index type %i", indexAccessor.componentType);
                continue;
            }

            if (mat) {
                // glTF texCoord selects the UV set per texture. Only two sets are
                // supported (a_texcoord1 / a_texcoord2); any higher index falls back
                // to the second set (clamped to 1).
                auto clampTexCoord = [](int texCoord) -> int { return (texCoord >= 1) ? 1 : 0; };

                if (!loadGLTFTexture(mat->pbrMetallicRoughness.baseColorTexture.index, model,
                        mesh.submeshes[i].material.baseColorTexture, filename + "|baseColorTexture"))
                    continue;
                mesh.submeshes[i].material.baseColorTexCoord = clampTexCoord(mat->pbrMetallicRoughness.baseColorTexture.texCoord);

                if (!loadGLTFTexture(mat->pbrMetallicRoughness.metallicRoughnessTexture.index, model,
                        mesh.submeshes[i].material.metallicRoughnessTexture, filename + "|metallicRoughnessTexture"))
                    continue;
                mesh.submeshes[i].material.metallicRoughnessTexCoord = clampTexCoord(mat->pbrMetallicRoughness.metallicRoughnessTexture.texCoord);

                if (!loadGLTFTexture(mat->occlusionTexture.index, model,
                        mesh.submeshes[i].material.occlusionTexture, filename + "|occlusionTexture"))
                    continue;
                mesh.submeshes[i].material.occlusionTexCoord = clampTexCoord(mat->occlusionTexture.texCoord);

                if (!loadGLTFTexture(mat->emissiveTexture.index, model,
                        mesh.submeshes[i].material.emissiveTexture, filename + "|emissiveTexture"))
                    continue;
                mesh.submeshes[i].material.emissiveTexCoord = clampTexCoord(mat->emissiveTexture.texCoord);

                if (!loadGLTFTexture(mat->normalTexture.index, model,
                        mesh.submeshes[i].material.normalTexture, filename + "|normalTexture"))
                    continue;
                mesh.submeshes[i].material.normalTexCoord = clampTexCoord(mat->normalTexture.texCoord);

                mesh.submeshes[i].material.baseColorFactor = Vector4(
                    mat->pbrMetallicRoughness.baseColorFactor[0],
                    mat->pbrMetallicRoughness.baseColorFactor[1],
                    mat->pbrMetallicRoughness.baseColorFactor[2],
                    mat->pbrMetallicRoughness.baseColorFactor[3]);

                mesh.submeshes[i].material.metallicFactor  = mat->pbrMetallicRoughness.metallicFactor;
                mesh.submeshes[i].material.roughnessFactor = mat->pbrMetallicRoughness.roughnessFactor;

                mesh.submeshes[i].material.emissiveFactor = Vector3(
                    mat->emissiveFactor[0],
                    mat->emissiveFactor[1],
                    mat->emissiveFactor[2]);
            }

            int indexStride = (indexType == AttributeDataType::UNSIGNED_SHORT) ? sizeof(uint16_t) : sizeof(uint32_t);

            mesh.submeshes[i].faceCulling = mat ? !mat->doubleSided : true;

            loadGLTFBuffer(indexBufferView, mesh, model, indexStride, loadedBuffers);
            addSubmeshAttribute(mesh.submeshes[i], getBufferName(indexBufferView, model),
                AttributeType::INDEX, 1, indexType, indexAccessor.count, indexByteOffset, false);

            for (auto& attrib : primitive.attributes) {
                if (!isValidGLTFIndex(attrib.second, model.gltfModel->accessors)) {
                    Log::warn("Invalid GLTF accessor index %i for attribute %s", attrib.second, attrib.first.c_str());
                    continue;
                }

                const tinygltf::Accessor& accessor = model.gltfModel->accessors[attrib.second];
                if (!isValidGLTFIndex(accessor.bufferView, model.gltfModel->bufferViews)) {
                    Log::warn("Invalid GLTF buffer view index %i for attribute %s", accessor.bufferView, attrib.first.c_str());
                    continue;
                }

                // Baked-flatten preview: redirect POSITION/NORMAL to a synthetic buffer with the
                // node's world transform baked in (tightly packed), so the flat mesh is laid out.
                int attrBufferView = accessor.bufferView;
                size_t attrByteOffset = accessor.byteOffset;
                int byteStride = accessor.ByteStride(model.gltfModel->bufferViews[accessor.bufferView]);
                if (bakingFlatten && (attrib.first == "POSITION" || attrib.first == "NORMAL")) {
                    int baked = bakeGLTFTransformedAttribute(accessor, nodeBakeMatrix, nodeBakeNormalMatrix,
                                                             attrib.first == "NORMAL", model);
                    if (baked >= 0) {
                        attrBufferView = baked;
                        attrByteOffset = 0;
                        byteStride = static_cast<int>(3 * sizeof(float));
                    }
                }
                const std::string bufferName = getBufferName(attrBufferView, model);

                loadGLTFBuffer(attrBufferView, mesh, model, byteStride, loadedBuffers);

                int elements = (accessor.type != TINYGLTF_TYPE_SCALAR) ? accessor.type : 1;

                AttributeDataType dataType;
                if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE) {
                    dataType = AttributeDataType::BYTE;
                } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                    dataType = AttributeDataType::UNSIGNED_BYTE;
                } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT) {
                    dataType = AttributeDataType::SHORT;
                } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                    dataType = AttributeDataType::UNSIGNED_SHORT;
                } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                    //dataType = AttributeDataType::UNSIGNED_INT;
                } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
                    dataType = AttributeDataType::FLOAT;
                } else {
                    Log::error("Unknown data type %i of %s", accessor.componentType, attrib.first.c_str());
                    continue;
                }

                AttributeType attType;
                bool foundAttrs = false;
                if (attrib.first == "POSITION") {
                    mesh.vertexCount = accessor.count;
                    attType = AttributeType::POSITION;
                    foundAttrs = true;
                }
                if (attrib.first == "NORMAL") {
                    attType = AttributeType::NORMAL;
                    foundAttrs = true;
                }
                if (attrib.first == "TANGENT") {
                    attType = AttributeType::TANGENT;
                    foundAttrs = true;
                }
                if (attrib.first == "TEXCOORD_0") {
                    attType = AttributeType::TEXCOORD1;
                    foundAttrs = true;
                }
                if (attrib.first == "TEXCOORD_1") {
                    attType = AttributeType::TEXCOORD2;
                    foundAttrs = true;
                }
                if (attrib.first == "COLOR_0") {
                    if (elements == 4) {
                        attType = AttributeType::COLOR;
                        foundAttrs = true;
                        mesh.submeshes[i].hasVertexColor4 = true;
                    } else {
                        Log::warn("Not supported vector(3) of: %s", attrib.first.c_str());
                    }
                }
                bool attributeNormalized = accessor.normalized;

                if (attrib.first == "JOINTS_0") {
                    attType = AttributeType::BONEIDS;
                    foundAttrs = true;
                    // Sokol has no non-normalized USHORT4 format — USHORT4N is always used,
                    // so the shader must expand the [0,1] value back to an integer index.
                    if (dataType == AttributeDataType::UNSIGNED_SHORT) {
                        mesh.normAdjustJoint = 65535.0;
                    } else if (dataType == AttributeDataType::UNSIGNED_BYTE) {
                        // `a_boneIds` is declared as vec4 in the shader, so use the normalized
                        // float path for byte joints as well and expand back in the shader.
                        mesh.normAdjustJoint = 255.0;
                        attributeNormalized = true;
                    }
                }
                if (attrib.first == "WEIGHTS_0") {
                    attType = AttributeType::BONEWEIGHTS;
                    foundAttrs = true;
                    if (accessor.normalized) {
                        if (dataType == AttributeDataType::BYTE)               mesh.normAdjustWeight = 127.0;
                        else if (dataType == AttributeDataType::UNSIGNED_BYTE) mesh.normAdjustWeight = 255.0;
                        else if (dataType == AttributeDataType::SHORT)         mesh.normAdjustWeight = 32767.0;
                    }
                    // Sokol always normalizes unsigned short
                    if (dataType == AttributeDataType::UNSIGNED_SHORT) {
                        mesh.normAdjustWeight = 65535.0;
                    }
                }

                if (foundAttrs) {
                    for (int b = 0; b < mesh.numExternalBuffers; b++) {
                        if (mesh.eBuffers[b].getName() == bufferName)
                            mesh.eBuffers[b].setRenderAttributes(false);
                    }
                    addSubmeshAttribute(mesh.submeshes[i], bufferName, attType, elements, dataType,
                        accessor.count, attrByteOffset, attributeNormalized);
                } else if (attrib.first.rfind("TEXCOORD_", 0) != 0) {
                    // only two UV sets are supported (TEXCOORD_0/_1); silently ignore
                    // any higher TEXCOORD_n the model may carry
                    Log::warn("Model attribute unused: %s", attrib.first.c_str());
                }
            }

            bool morphTargets = false;
            bool morphNormals  = false;
            bool morphTangents = false;
            int  morphIndex    = 0;

            for (auto& morphs : primitive.targets) {
                for (auto& attribMorph : morphs) {
                    morphTargets = true;

                    if (!isValidGLTFIndex(attribMorph.second, model.gltfModel->accessors)) {
                        Log::warn("Invalid GLTF accessor index %i for morph target %s", attribMorph.second, attribMorph.first.c_str());
                        continue;
                    }

                    const tinygltf::Accessor& accessor = model.gltfModel->accessors[attribMorph.second];
                    if (!isValidGLTFIndex(accessor.bufferView, model.gltfModel->bufferViews)) {
                        Log::warn("Invalid GLTF buffer view index %i for morph target %s", accessor.bufferView, attribMorph.first.c_str());
                        continue;
                    }

                    int byteStride = accessor.ByteStride(model.gltfModel->bufferViews[accessor.bufferView]);
                    const std::string bufferName = getBufferName(accessor.bufferView, model);

                    loadGLTFBuffer(accessor.bufferView, mesh, model, byteStride, loadedBuffers);

                    int elements = (accessor.type != TINYGLTF_TYPE_SCALAR) ? accessor.type : 1;

                    AttributeDataType dataType;
                    if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE) {
                        dataType = AttributeDataType::BYTE;
                    } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                        dataType = AttributeDataType::UNSIGNED_BYTE;
                    } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT) {
                        dataType = AttributeDataType::SHORT;
                    } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                        dataType = AttributeDataType::UNSIGNED_SHORT;
                    } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                        dataType = AttributeDataType::UNSIGNED_INT;
                    } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
                        dataType = AttributeDataType::FLOAT;
                    } else {
                        Log::error("Unknown data type %i of morph target %s", accessor.componentType, attribMorph.first.c_str());
                        continue;
                    }

                    AttributeType attType;
                    bool foundMorph = false;

                    if (attribMorph.first == "POSITION") {
                        if      (morphIndex == 0) { attType = AttributeType::MORPHTARGET0; foundMorph = true; }
                        else if (morphIndex == 1) { attType = AttributeType::MORPHTARGET1; foundMorph = true; }
                        if ((!morphNormals && morphTangents) || (morphNormals && !morphTangents)) {
                            if      (morphIndex == 2) { attType = AttributeType::MORPHTARGET2; foundMorph = true; }
                            else if (morphIndex == 3) { attType = AttributeType::MORPHTARGET3; foundMorph = true; }
                            if (!morphNormals && !morphTangents) {
                                if      (morphIndex == 4) { attType = AttributeType::MORPHTARGET4; foundMorph = true; }
                                else if (morphIndex == 5) { attType = AttributeType::MORPHTARGET5; foundMorph = true; }
                                else if (morphIndex == 6) { attType = AttributeType::MORPHTARGET6; foundMorph = true; }
                                else if (morphIndex == 7) { attType = AttributeType::MORPHTARGET7; foundMorph = true; }
                            }
                        }
                    }
                    if (attribMorph.first == "NORMAL") {
                        morphNormals = true;
                        if      (morphIndex == 0) { attType = AttributeType::MORPHNORMAL0; foundMorph = true; }
                        else if (morphIndex == 1) { attType = AttributeType::MORPHNORMAL1; foundMorph = true; }
                        if (!morphTangents) {
                            if      (morphIndex == 2) { attType = AttributeType::MORPHNORMAL2; foundMorph = true; }
                            else if (morphIndex == 3) { attType = AttributeType::MORPHNORMAL3; foundMorph = true; }
                        }
                    }
                    if (attribMorph.first == "TANGENT") {
                        morphTangents = true;
                        if      (morphIndex == 0) { attType = AttributeType::MORPHTANGENT0; foundMorph = true; }
                        else if (morphIndex == 1) { attType = AttributeType::MORPHTANGENT1; foundMorph = true; }
                    }

                    if (foundMorph) {
                        for (int b = 0; b < mesh.numExternalBuffers; b++) {
                            if (mesh.eBuffers[b].getName() == bufferName)
                                mesh.eBuffers[b].setRenderAttributes(false);
                        }
                        addSubmeshAttribute(mesh.submeshes[i], bufferName, attType, elements, dataType,
                            accessor.count, accessor.byteOffset, accessor.normalized);
                    }
                }
                morphIndex++;
            }

            if (morphTargets) {
                for (int w = 0; w < static_cast<int>(gltfmesh.weights.size()) && w < MAX_MORPHTARGETS; w++) {
                    mesh.morphWeights[w] = gltfmesh.weights[w];
                }
                if (gltfmesh.extras.Has("targetNames") && gltfmesh.extras.Get("targetNames").IsArray()) {
                    for (int t = 0; t < gltfmesh.extras.Get("targetNames").Size(); t++) {
                        model.morphNameMapping[gltfmesh.extras.Get("targetNames").Get(t).Get<std::string>()] = t;
                    }
                }
            }
        }

        // Child meshes finalize here (per-node submesh count + reverse + AABB). The flatten path uses
        // a continuous submeshIndex across nodes and finalizes the root mesh once after the loop.
        if (useChildEntities) {
            mesh.numSubmeshes = submeshIndex; // reserved-but-skipped slots (invalid data) dropped
            std::reverse(mesh.submeshes.data(), mesh.submeshes.data() + mesh.numSubmeshes);
            calculateMeshAABB(mesh);
            mesh.needReload = true;
        }

    }

    if (!useChildEntities) {
        // Flatten path: submeshIndex accumulated every node's primitives into the root mesh.
        mesh.numSubmeshes = submeshIndex;
    }

    if (asyncLoad) {
        ResourceProgress::updateProgress(buildId, 0.8f); // Submeshes processed
    }

    int skinIndex = -1;
    if (isValidGLTFIndex(meshNode, model.gltfModel->nodes)) {
        skinIndex = model.gltfModel->nodes[meshNode].skin;
    }
    int skeletonRoot = -1;

    if (skinIndex >= 0){
        if (!isValidGLTFIndex(skinIndex, model.gltfModel->skins)) {
            Log::warn("GLTF mesh node has invalid skin index %i: %s", skinIndex, filename.c_str());
            skinIndex = -1;
        }
    }

    if (skinIndex >= 0){
        tinygltf::Skin skin = model.gltfModel->skins[skinIndex];

        if (skin.skeleton >= 0 && isValidGLTFIndex(skin.skeleton, model.gltfModel->nodes)) {
            skeletonRoot = skin.skeleton;
        }else {
            //Find skeleton root
            for (int j = 0; j < skin.joints.size(); j++) {
                int nodeIndex = skin.joints[j];

                if (!isValidGLTFIndex(nodeIndex, model.gltfModel->nodes)) {
                    continue;
                }

                if (std::find(skin.joints.begin(), skin.joints.end(), nodesParent[nodeIndex]) == skin.joints.end())
                    skeletonRoot = nodeIndex;
            }
        }

        if (skeletonRoot >= 0) {
            if (!skipEntities) {
                model.bonesNameMapping.clear();
                model.bonesIdMapping.clear();

                model.skeleton = generateSketetalStructure(entity, model, skeletonRoot, skinIndex);

                if (model.skeleton != NULL_ENTITY) {
                    if (skin.joints.size() > MAX_BONES){
                        Log::error("Cannot create skinning bigger than %i", MAX_BONES);
                        if (asyncLoad) {
                            ResourceProgress::failBuild(buildId);
                        }
                        return false;
                    }
                    scene->addEntityChild(entity, model.skeleton, false);
                }
            }

            // Repopulate bonesMatrix before the first render frame. loadGLTF reset
            // every bonesMatrix to identity above, so RenderSystem must recompute
            // the skinning matrices: flag the model transform (so its
            // inverseDerivedTransform is refreshed) and every bone transform for
            // update. This also covers reopening a saved project (skipEntities),
            // where the skeleton already exists from the deserialized scene; without
            // it the skinning is left stale and the mesh renders invisible until it
            // is moved or the game is played and stopped.
            if (model.skeleton != NULL_ENTITY) {
                transform.needUpdate = true;
                for (const auto& boneEntry : model.bonesIdMapping) {
                    Transform* boneTransform = scene->findComponent<Transform>(boneEntry.second);
                    if (boneTransform) {
                        boneTransform->needUpdate = true;
                    }
                }
            }
        }
    }

    if (!skipEntities) {
        model.animations.clear();

        for (size_t i = 0; i < model.gltfModel->animations.size(); i++) {
            const tinygltf::Animation &animation = model.gltfModel->animations[i];

            Entity anim;

            anim = scene->createEntity();
            if (anim == NULL_ENTITY) {
                continue;
            }
            scene->addComponent<ActionComponent>(anim);
            scene->addComponent<AnimationComponent>(anim);

            AnimationComponent& animcomp = scene->getComponent<AnimationComponent>(anim);

            animcomp.name = animation.name;

            std::string animName = animation.name.empty() ? "Animation " + std::to_string(i) : animation.name;
            scene->setEntityName(anim, animName);

            model.animations.push_back(anim);

            for (size_t j = 0; j < animation.channels.size(); j++) {

                const tinygltf::AnimationChannel &channel = animation.channels[j];
                if (!isValidGLTFIndex(channel.sampler, animation.samplers)) {
                    Log::warn("Cannot load animation: %s, channel %zu has invalid sampler", animation.name.c_str(), j);
                    continue;
                }

                const tinygltf::AnimationSampler &sampler = animation.samplers[channel.sampler];

                if (!isValidGLTFIndex(sampler.input, model.gltfModel->accessors) ||
                    !isValidGLTFIndex(sampler.output, model.gltfModel->accessors)) {
                    Log::warn("Cannot load animation: %s, channel %zu has invalid accessor", animation.name.c_str(), j);
                    continue;
                }

                tinygltf::Accessor accessorIn = model.gltfModel->accessors[sampler.input];
                if (!isValidGLTFIndex(accessorIn.bufferView, model.gltfModel->bufferViews)) {
                    Log::warn("Cannot load animation: %s, channel %zu has invalid input buffer view", animation.name.c_str(), j);
                    continue;
                }

                tinygltf::BufferView bufferViewIn = model.gltfModel->bufferViews[accessorIn.bufferView];

                tinygltf::Accessor accessorOut = model.gltfModel->accessors[sampler.output];
                if (!isValidGLTFIndex(accessorOut.bufferView, model.gltfModel->bufferViews)) {
                    Log::warn("Cannot load animation: %s, channel %zu has invalid output buffer view", animation.name.c_str(), j);
                    continue;
                }

                tinygltf::BufferView bufferViewOut = model.gltfModel->bufferViews[accessorOut.bufferView];

                if (!isValidGLTFIndex(bufferViewIn.buffer, model.gltfModel->buffers) ||
                    !isValidGLTFIndex(bufferViewOut.buffer, model.gltfModel->buffers) ||
                    accessorIn.count == 0 || accessorOut.count == 0) {
                    Log::warn("Cannot load animation: %s, channel %zu has invalid buffer data", animation.name.c_str(), j);
                    continue;
                }

                //TODO: Implement rotation and weights non float
                if (accessorOut.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {

                    if (accessorIn.count != accessorOut.count) {
                        Log::error("Incorrect frame size in animation: %s, sampler: %i",
                                animation.name.c_str(), channel.sampler);
                    }

                    std::vector<unsigned char>& inputData = model.gltfModel->buffers[bufferViewIn.buffer].data;
                    std::vector<unsigned char>& outputData = model.gltfModel->buffers[bufferViewOut.buffer].data;
                    if (inputData.empty() || outputData.empty()) {
                        Log::warn("Cannot load animation: %s, channel %zu has empty buffer data", animation.name.c_str(), j);
                        continue;
                    }

                    float *timeValues = (float *) (inputData.data() + bufferViewIn.byteOffset + accessorIn.byteOffset);
                    float *values = (float *) (outputData.data() + bufferViewOut.byteOffset + accessorOut.byteOffset);

                    float duration = timeValues[accessorIn.count - 1];

                    Entity track;

                    track = scene->createEntity();
                    if (track == NULL_ENTITY) {
                        continue;
                    }

                    std::string trackName = animName + " " + channel.target_path;
                    if (channel.target_node >= 0 && channel.target_node < model.gltfModel->nodes.size()) {
                        std::string nodeName = model.gltfModel->nodes[channel.target_node].name;
                        if (!nodeName.empty()) {
                            trackName = nodeName + " " + channel.target_path;
                        }
                    }
                    scene->setEntityName(track, trackName);

                    scene->addComponent<ActionComponent>(track);
                    scene->addComponent<KeyframeTracksComponent>(track);

                    ActionComponent& actiontrack = scene->getComponent<ActionComponent>(track);
                    KeyframeTracksComponent& keyframe = scene->getComponent<KeyframeTracksComponent>(track);

                    bool foundTrack = false;
                    if (channel.target_path.compare("translation") == 0) {
                        foundTrack = true;
                        scene->addComponent<TranslateTracksComponent>(track);
                        TranslateTracksComponent& translatetracks = scene->getComponent<TranslateTracksComponent>(track);
                        for (int c = 0; c < accessorIn.count; c++) {
                            Vector3 positionAc(values[3 * c], values[(3 * c) + 1], values[(3 * c) + 2]);

                            keyframe.times.push_back(timeValues[c]);
                            translatetracks.values.push_back(positionAc);
                        }
                    }
                    if (channel.target_path.compare("rotation") == 0) {
                        foundTrack = true;
                        scene->addComponent<RotateTracksComponent>(track);
                        RotateTracksComponent& rotatetracks = scene->getComponent<RotateTracksComponent>(track);
                        for (int c = 0; c < accessorIn.count; c++) {
                            Quaternion rotationAc(values[(4 * c) + 3], values[4 * c], values[(4 * c) + 1], values[(4 * c) + 2]);

                            keyframe.times.push_back(timeValues[c]);
                            rotatetracks.values.push_back(rotationAc);
                        }
                    }
                    if (channel.target_path.compare("scale") == 0) {
                        foundTrack = true;
                        scene->addComponent<ScaleTracksComponent>(track);
                        ScaleTracksComponent& scaletracks = scene->getComponent<ScaleTracksComponent>(track);
                        for (int c = 0; c < accessorIn.count; c++) {
                            Vector3 scaleAc(values[3 * c], values[(3 * c) + 1], values[(3 * c) + 2]);

                            keyframe.times.push_back(timeValues[c]);
                            scaletracks.values.push_back(scaleAc);
                        }
                    }
                    if (channel.target_path.compare("weights") == 0) {
                        foundTrack = true;
                        scene->addComponent<MorphTracksComponent>(track);
                        MorphTracksComponent& morphtracks = scene->getComponent<MorphTracksComponent>(track);
                        int morphNum = accessorOut.count / accessorIn.count;
                        for (int c = 0; c < accessorIn.count; c++) {
                            std::vector<float> weightsAc;
                            for (int m = 0; m < morphNum; m++) {
                                weightsAc.push_back(values[(morphNum * c) + m]);
                            }

                            keyframe.times.push_back(timeValues[c]);
                            morphtracks.values.push_back(weightsAc);
                        }
                    }

                    if (foundTrack) {
                        if (model.bonesIdMapping.count(channel.target_node)) {
                            actiontrack.target = model.bonesIdMapping[channel.target_node];
                        } else {
                            actiontrack.target = entity;
                        }
                        animcomp.actions.push_back({0, duration, track});
                    }else{
                        scene->destroyEntity(track);
                    }

                }else{
                    Log::error("Cannot load animation: %s, channel %i: no float elements", animation.name.c_str(), j);
                }
            }

            // need to get here because other actions were created
            ActionComponent& anim_actioncomp = scene->getComponent<ActionComponent>(anim);

            anim_actioncomp.target = entity;

        }
    } // !skipEntities

    if (asyncLoad) {
        ResourceProgress::updateProgress(buildId, 0.95f); // Animations processed
    }

/*
    //BEGIN DEBUG
    for (auto &gltfmesh : gltfModel->meshes) {
        Log::verbose("mesh : %s", gltfmesh.name.c_str());
        for (auto &primitive : gltfmesh.primitives) {
            const tinygltf::Accessor &indexAccessor = gltfModel->accessors[primitive.indices];

            Log::verbose("indexaccessor: count %i, type %i", indexAccessor.count,
                    indexAccessor.componentType);

            tinygltf::Material &mat = gltfModel->materials[primitive.material];
            for (auto &mats : mat.values) {
                Log::verbose("mat : %s", mats.first.c_str());
            }

            for (auto &image : gltfModel->images) {
                Log::verbose("image name : %s", image.uri.c_str());
                Log::verbose("  size : %i", image.image.size());
                Log::verbose("  w/h : %i/%i", image.width, image.height);
            }

            Log::verbose("indices : %i", primitive.indices);
            Log::verbose("mode     : %i", primitive.mode);

            for (auto &attrib : primitive.attributes) {
                Log::verbose("attribute : %s", attrib.first.c_str());
            }
        }
    }
    //END DEBUG
*/
    // Root-mesh finalization (single-node path only). In the child-entity path each child mesh was
    // already finalized in the loop, the root mesh is an empty container, and adding child
    // MeshComponents may have reallocated the component array — so the `mesh` reference captured
    // before the loop must not be dereferenced here.
    if (!useChildEntities) {
        std::reverse(mesh.submeshes.data(), mesh.submeshes.data() + mesh.numSubmeshes);

        calculateMeshAABB(mesh);

        if (mesh.loaded)
            mesh.needReload = true;
    }

    if (asyncLoad) {
        ResourceProgress::updateProgress(buildId, 1.0f); // Complete
        ResourceProgress::completeBuild(buildId);
    }

    model.loadedFilename = getModelFilenameKey(filename);

    return true;
}

bool MeshSystem::loadOBJ(Entity entity, const std::string filename, bool asyncLoad){
    const std::string poolKey = getModelFilenameKey(filename);

    // If parsed data is already cached, skip the async background-thread entirely.
    if (asyncLoad && ModelPool::getObj(poolKey)){
        asyncLoad = false;
    }

    std::shared_ptr<AsyncModelLoadResult> asyncResult;
    if (asyncLoad){
        asyncResult = pollOrStartAsyncModelLoad(filename, true);
        if (!asyncResult || !asyncResult->success || !asyncResult->objModel){
            return false;
        }
    }

    MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
    ModelComponent& model = scene->getComponent<ModelComponent>(entity);
    Transform& transform = scene->getComponent<Transform>(entity);

    destroyModel(model);

    model.filename = filename;

    uint64_t buildId = asyncLoad ? std::hash<std::string>{}(getAsyncModelLoadKey(filename)) : 0;

    mesh.submeshes[0].primitiveType = PrimitiveType::TRIANGLES;
    mesh.numSubmeshes = 1;

    std::string warn;
    std::string err;

    tinyobj::FileReader::externalFunc = readFileToString;

    std::string baseDir = FileData::getBaseDir(filename);

    bool ret = false;

    if (asyncLoad){
        auto cached = ModelPool::getObj(poolKey);
        if (cached){
            model.objModel = cached;
        }else{
            model.objModel = asyncResult->objModel;
            ModelPool::addObj(poolKey, model.objModel);
        }
        ret = true;
    }else{
        auto cached = ModelPool::getObj(poolKey);
        if (cached){
            model.objModel = cached;
            ret = true;
        }else{
            model.objModel = std::make_shared<ObjModelData>();
            ret = tinyobj::LoadObj(&model.objModel->attrib, &model.objModel->shapes, &model.objModel->materials, &warn, &err, filename.c_str(), baseDir.c_str());

            if (!warn.empty()) {
                Log::warn("Loading OBJ model (%s): %s", filename.c_str(), warn.c_str());
            }

            if (!err.empty()) {
                Log::error("Can't load OBJ model (%s): %s", filename.c_str(), err.c_str());
                return false;
            }

            if (!ret) {
                return false;
            }

            ModelPool::addObj(poolKey, model.objModel);
        }
    }

    tinyobj::attrib_t& attrib = model.objModel->attrib;
    std::vector<tinyobj::shape_t>& shapes = model.objModel->shapes;
    std::vector<tinyobj::material_t>& materials = model.objModel->materials;

    mesh.buffer.clear();
    mesh.buffer.addAttribute(AttributeType::POSITION, 3);
    mesh.buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    mesh.buffer.addAttribute(AttributeType::NORMAL, 3);
    mesh.buffer.addAttribute(AttributeType::COLOR, 4);

    if (materials.size() > 0){
        mesh.numSubmeshes = materials.size();

    }

    if (mesh.numSubmeshes > 0 && !mesh.submeshes.validIndex(static_cast<int>(mesh.numSubmeshes) - 1)){
        Log::error("Model %s has more submeshes than the maximum allowed (%i)", filename.c_str(), mesh.submeshes.size());
        mesh.numSubmeshes = static_cast<unsigned int>(mesh.submeshes.size());
    }

    if (asyncLoad) {
        ResourceProgress::updateProgress(buildId, 0.4f); // Materials setup
    }

    const bool hasMaterials = !materials.empty();

    for (size_t i = 0; i < mesh.numSubmeshes; i++) {
        // Update progress for material processing
        if (asyncLoad) {
            float materialProgress = 0.4f + (0.2f * (float(i) / float(mesh.numSubmeshes)));
            ResourceProgress::updateProgress(buildId, materialProgress);
        }

        if (!hasMaterials) {
            applyDefaultObjMaterial(mesh.submeshes[i]);
            continue;
        }

        mesh.submeshes[i].attributes.clear();

        // Convert the blinn-phong model to the pbr metallic-roughness model
        // Based on https://github.com/CesiumGS/obj2gltf
        const tinyobj::material_t& objMaterial = materials[i];
        const float specularIntensity = objMaterial.specular[0] * 0.2125 + objMaterial.specular[1] * 0.7154 + objMaterial.specular[2] * 0.0721; //luminance

        float roughnessFactor = objMaterial.shininess;
        roughnessFactor = roughnessFactor / 1000.0;
        roughnessFactor = 1.0 - roughnessFactor;
        roughnessFactor = std::min(std::max(roughnessFactor, 0.0f), 1.0f); //clamp

        if (specularIntensity < 0.1) {
            roughnessFactor *= (1.0 - specularIntensity);
        }

        const float metallicFactor = 0.0;

        // ------ End convertion

        mesh.submeshes[i].material.baseColorFactor = Vector4(objMaterial.diffuse[0], objMaterial.diffuse[1], objMaterial.diffuse[2], 1.0);
        mesh.submeshes[i].material.emissiveFactor = Vector3(objMaterial.emission[0], objMaterial.emission[1], objMaterial.emission[2]);
        mesh.submeshes[i].material.metallicFactor = metallicFactor;
        mesh.submeshes[i].material.roughnessFactor = roughnessFactor;

        if (!objMaterial.diffuse_texname.empty())
            mesh.submeshes[i].material.baseColorTexture.setPath(baseDir+objMaterial.diffuse_texname);
        if (!objMaterial.normal_texname.empty())
            mesh.submeshes[i].material.normalTexture.setPath(baseDir+objMaterial.normal_texname);
        if (!objMaterial.emissive_texname.empty())
            mesh.submeshes[i].material.emissiveTexture.setPath(baseDir+objMaterial.emissive_texname);
        if (!objMaterial.ambient_texname.empty())
            mesh.submeshes[i].material.occlusionTexture.setPath(baseDir+objMaterial.ambient_texname);

        //TODO: occlusionFactor (Ka)
        //TODO: metallicroughnessTexture (map_Ks + map_Ns)

        if (objMaterial.dissolve < 1){
            mesh.transparent = true;
        }
    }

    if (asyncLoad) {
        ResourceProgress::updateProgress(buildId, 0.6f); // Materials processed, starting geometry
    }

    Attribute* attVertex = mesh.buffer.getAttribute(AttributeType::POSITION);
    Attribute* attTexcoord = mesh.buffer.getAttribute(AttributeType::TEXCOORD1);
    Attribute* attNormal = mesh.buffer.getAttribute(AttributeType::NORMAL);
    Attribute* attColor = mesh.buffer.getAttribute(AttributeType::COLOR);

    std::vector<std::vector<uint32_t>> indexMap;
    if (materials.size() > 0) {
        indexMap.resize(materials.size());
    }else{
        indexMap.resize(1);
    }

    for (size_t i = 0; i < shapes.size(); i++) {
        // Update progress for shape processing
        if (asyncLoad) {
            float shapeProgress = 0.6f + (0.3f * (float(i) / float(shapes.size())));
            ResourceProgress::updateProgress(buildId, shapeProgress);
        }

        size_t index_offset = 0;
        for (size_t f = 0; f < shapes[i].mesh.num_face_vertices.size(); f++) {
            size_t fnum = shapes[i].mesh.num_face_vertices[f];

            int material_id = shapes[i].mesh.material_ids[f];
            if (material_id < 0 || material_id >= static_cast<int>(indexMap.size()))
                material_id = 0;

            // For each vertex in the face
            for (size_t v = 0; v < fnum; v++) {
                tinyobj::index_t idx = shapes[i].mesh.indices[index_offset + v];

                indexMap[material_id].push_back(mesh.buffer.getCount());

                    mesh.buffer.addVector3(attVertex,
                                        Vector3(attrib.vertices[3*idx.vertex_index+0],
                                                attrib.vertices[3*idx.vertex_index+1],
                                                attrib.vertices[3*idx.vertex_index+2]));

                if (attrib.texcoords.size() > 0) {
                        mesh.buffer.addVector2(attTexcoord,
                                            Vector2(attrib.texcoords[2 * idx.texcoord_index + 0],
                                                    1.0f - attrib.texcoords[2 * idx.texcoord_index + 1]));
                }
                if (attrib.normals.size() > 0) {
                        mesh.buffer.addVector3(attNormal,
                                            Vector3(attrib.normals[3 * idx.normal_index + 0],
                                                    attrib.normals[3 * idx.normal_index + 1],
                                                    attrib.normals[3 * idx.normal_index + 2]));
                }

                if (attrib.colors.size() > 0){
                        mesh.buffer.addVector4(attColor,
                                            Vector4(attrib.colors[3 * idx.vertex_index + 0],
                                                    attrib.colors[3 * idx.vertex_index + 1],
                                                    attrib.colors[3 * idx.vertex_index + 2],
                                                    1.0));
                }else{
                        mesh.buffer.addVector4(attColor, Vector4(1.0, 1.0, 1.0, 1.0));
                }

            }

            index_offset += fnum;
        }
    }

    if (asyncLoad) {
        ResourceProgress::updateProgress(buildId, 0.9f); // Geometry processed
    }

    const bool useUint32Indices = mesh.buffer.getCount() > (static_cast<unsigned int>(std::numeric_limits<uint16_t>::max()) + 1u);

    mesh.indices.clear();
    mesh.indices.setStride(useUint32Indices ? sizeof(uint32_t) : sizeof(uint16_t));

    const AttributeDataType indexDataType = useUint32Indices ? AttributeDataType::UNSIGNED_INT : AttributeDataType::UNSIGNED_SHORT;
    const size_t indexTypeSize = useUint32Indices ? sizeof(uint32_t) : sizeof(uint16_t);

    for (size_t i = 0; i < mesh.numSubmeshes; i++) {
        addSubmeshAttribute(mesh.submeshes[i], "indices", AttributeType::INDEX, 1, indexDataType, indexMap[i].size(), mesh.indices.getCount() * indexTypeSize, false);

        if (indexMap[i].size() > 0){
            if (useUint32Indices) {
                mesh.indices.setValues(mesh.indices.getCount(), mesh.indices.getAttribute(AttributeType::INDEX), indexMap[i].size(), (char*)&indexMap[i].front(), sizeof(uint32_t));
            } else {
                std::vector<uint16_t> u16indices(indexMap[i].size());
                for (size_t j = 0; j < indexMap[i].size(); j++)
                    u16indices[j] = static_cast<uint16_t>(indexMap[i][j]);
                mesh.indices.setValues(mesh.indices.getCount(), mesh.indices.getAttribute(AttributeType::INDEX), u16indices.size(), (char*)&u16indices.front(), sizeof(uint16_t));
            }
            mesh.indices.setRenderAttributes(false);
        }
    }

    std::reverse(mesh.submeshes.data(), mesh.submeshes.data() + mesh.numSubmeshes);

    calculateMeshAABB(mesh);

    if (mesh.loaded)
        mesh.needReload = true;

    if (asyncLoad) {
        ResourceProgress::updateProgress(buildId, 1.0f); // Complete
        ResourceProgress::completeBuild(buildId);
    }

    model.loadedFilename = getModelFilenameKey(filename);

    return true;
}

void MeshSystem::createInstancedMesh(Entity entity){
    Signature signature = scene->getSignature(entity);

    if (!signature.test(scene->getComponentId<InstancedMeshComponent>())){
        scene->addComponent<InstancedMeshComponent>(entity);
    }
}

void MeshSystem::removeInstancedMesh(Entity entity){
    Signature signature = scene->getSignature(entity);

    if (signature.test(scene->getComponentId<InstancedMeshComponent>())){
        //destroyInstancedMesh(scene->getComponent<Body3DComponent>(entity));
        scene->removeComponent<InstancedMeshComponent>(entity);

        if (signature.test(scene->getComponentId<MeshComponent>())){
            MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
            mesh.aabb = mesh.verticesAABB;
            if (mesh.loaded)
                mesh.needReload = true;
        }
    }
}

bool MeshSystem::hasInstancedMesh(Entity entity) const{
    Signature signature = scene->getSignature(entity);

    return signature.test(scene->getComponentId<InstancedMeshComponent>());
}

void MeshSystem::clearBoneMapping(ModelComponent& model){
    for (auto const& bone : model.bonesIdMapping){
        scene->destroyEntity(bone.second);
    }
    model.bonesIdMapping.clear();
    model.bonesNameMapping.clear();

    model.skeleton = NULL_ENTITY;
}

void MeshSystem::clearAnimationMapping(ModelComponent& model){
    for (int i = 0; i < model.animations.size(); i++){
        scene->destroyEntity(model.animations[i]);
    }
    model.animations.clear();
}

void MeshSystem::clearMeshNodeMapping(ModelComponent& model){
    for (auto const& node : model.meshNodesMapping){
        scene->destroyEntity(node.second);
    }
    model.meshNodesMapping.clear();
}

void MeshSystem::destroyModel(ModelComponent& model){
    if (model.gltfModel){
        model.gltfModel.reset();
    }

    if (model.objModel){
        model.objModel.reset();
    }

    if (!model.loadedFilename.empty()){
        ModelPool::removeGLTF(model.loadedFilename);
        ModelPool::removeObj(model.loadedFilename);
    }

    model.morphNameMapping.clear();

    model.filename = "";
    model.loadedFilename.clear();
}

void MeshSystem::resetModelToBindPose(ModelComponent& model){
    for (auto const& bone : model.bonesIdMapping){
        Entity boneEntity = bone.second;
        BoneComponent* boneComp = scene->findComponent<BoneComponent>(boneEntity);
        Transform* transform = scene->findComponent<Transform>(boneEntity);
        if (!boneComp || !transform){
            continue;
        }

        transform->position = boneComp->bindPosition;
        transform->rotation = boneComp->bindRotation;
        transform->scale = boneComp->bindScale;
        transform->needUpdate = true;
    }
}

bool MeshSystem::createOrUpdateSprite(SpriteComponent& sprite, MeshComponent& mesh){
    if (sprite.needUpdateSprite){
        CameraComponent& camera =  scene->getComponent<CameraComponent>(scene->getCamera());
        if (sprite.automaticFlipY){
            changeFlipY(sprite.flipY, camera, mesh);
        }

        if (createSprite(sprite, mesh, camera)){
            sprite.needUpdateSprite = false;
        }else{
            return false;
        }
    }

    return true;
}

bool MeshSystem::createOrUpdateTerrain(TerrainComponent& terrain, MeshComponent& mesh, Transform& transform){
    if (terrain.needUpdateTerrain){

        if (scene->getCamera() == NULL_ENTITY){
            Log::error("Cannot create terrain without defined camera in scene");
            terrain.needUpdateTerrain = false;
            return false;
        }

        if (MAX_TERRAINGRID < (terrain.rootGridSize*terrain.rootGridSize)){
            Log::error("Cannot create full terrain, increase MAX_TERRAINGRID to %u", (terrain.rootGridSize*terrain.rootGridSize));
            terrain.needUpdateTerrain = false;
            return false;
        }

        if (terrain.heightMap.empty()){
            terrain.heightMapLoaded = false;
            terrain.numNodes = 0;
            terrain.nodes.clear();
            for (int v = 0; v < MAX_TERRAIN_VIEWS; v++){
                for (int s = 0; s < 2; s++){
                    terrain.views[v].nodesbuffer[s].clear();
                }
                terrain.views[v].needUpdateNodesBuffer = false;
            }
            mesh.verticesAABB = AABB::ZERO;
            mesh.aabb = AABB::ZERO;
            mesh.worldAABB = AABB::ZERO;
            mesh.needUpdateAABB = false;
            if (mesh.loaded || mesh.loadCalled){
                mesh.needReload = true;
            }
            terrain.needUpdateTerrain = false;
            return false;
        }

        if (createTerrain(terrain, mesh)){
            terrain.needUpdateTerrain = false;

            transform.needUpdate = true;
        }else{
            return false;
        }
    }

    return true;
}

bool MeshSystem::createOrUpdateMeshPolygon(MeshPolygonComponent& polygon, MeshComponent& mesh){
    if (polygon.needUpdatePolygon){
        if (polygon.automaticFlipY){
            CameraComponent& camera =  scene->getComponent<CameraComponent>(scene->getCamera());
            changeFlipY(polygon.flipY, camera, mesh);
        }

        if (createMeshPolygon(polygon, mesh)){
            polygon.needUpdatePolygon = false;
        }else{
            return false;
        }
    }

    return true;
}

bool MeshSystem::createOrUpdateTilemap(TilemapComponent& tilemap, MeshComponent& mesh){
    if (tilemap.needUpdateTilemap){
        if (tilemap.automaticFlipY){
            CameraComponent& camera =  scene->getComponent<CameraComponent>(scene->getCamera());
            changeFlipY(tilemap.flipY, camera, mesh);
        }

        if (createTilemap(tilemap, mesh)){
            tilemap.needUpdateTilemap = false;
        }else{
            return false;
        }
    }

    return true;
}

bool MeshSystem::createOrUpdateModel(Entity entity, ModelComponent& model, MeshComponent& mesh){
    if (model.needUpdateModel){
        if (!model.filename.empty()){
            if (getModelFilenameKey(model.filename) == model.loadedFilename){
                model.needUpdateModel = false;
                return true;
            }

            std::string ext = FileData::getFilePathExtension(model.filename);
            bool skipEntities = !model.filename.empty() && (!model.bonesIdMapping.empty() || !model.animations.empty() || !model.meshNodesMapping.empty());
            bool asyncLoading = Engine::isAsyncLoading();
            bool ret = false;
            if (ext == "obj"){
                ret = loadOBJ(entity, model.filename, asyncLoading);
            }else{
                ret = loadGLTF(entity, model.filename, asyncLoading, skipEntities, false);
            }

            if (ret){
                model.needUpdateModel = false;
            }else{
                return false;
            }
        }else{
            model.loadedFilename.clear();
            model.needUpdateModel = false;
        }
    }

    return true;
}

void MeshSystem::load(){
    update(0);
}

void MeshSystem::destroy(){

}

void MeshSystem::update(double dt){
    if (paused) {
        return;
    }

    auto sprites = scene->getComponentArray<SpriteComponent>();
    for (int i = 0; i < sprites->size(); i++){
        SpriteComponent& sprite = sprites->getComponentFromIndex(i);

        Entity entity = sprites->getEntity(i);
        Signature signature = scene->getSignature(entity);

        if (signature.test(scene->getComponentId<MeshComponent>())){
            MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);

            createOrUpdateSprite(sprite, mesh);
        }
    }

    auto tilemaps = scene->getComponentArray<TilemapComponent>();
    for (int i = 0; i < tilemaps->size(); i++){
        TilemapComponent& tilemap = tilemaps->getComponentFromIndex(i);

        Entity entity = tilemaps->getEntity(i);
        Signature signature = scene->getSignature(entity);

        if (signature.test(scene->getComponentId<MeshComponent>())){
            MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);

            createOrUpdateTilemap(tilemap, mesh);
        }
    }

    auto polygons = scene->getComponentArray<MeshPolygonComponent>();
    for (int i = 0; i < polygons->size(); i++){
        MeshPolygonComponent& polygon = polygons->getComponentFromIndex(i);

        Entity entity = polygons->getEntity(i);
        Signature signature = scene->getSignature(entity);

        if (signature.test(scene->getComponentId<MeshComponent>())){
            MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);

            createOrUpdateMeshPolygon(polygon, mesh);
        }
    }

    auto terrains = scene->getComponentArray<TerrainComponent>();
    for (int i = 0; i < terrains->size(); i++){
        TerrainComponent& terrain = terrains->getComponentFromIndex(i);

        Entity entity = terrains->getEntity(i);
        Signature signature = scene->getSignature(entity);

        if (signature.test(scene->getComponentId<MeshComponent>()) && signature.test(scene->getComponentId<Transform>())){
            MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
            Transform& transform = scene->getComponent<Transform>(entity);

            createOrUpdateTerrain(terrain, mesh, transform);
            updateTerrainAutoRanges(terrain);
        }
    }

    auto models = scene->getComponentArray<ModelComponent>();
    for (int i = 0; i < models->size(); i++){
        ModelComponent& model = models->getComponentFromIndex(i);

        Entity entity = models->getEntity(i);
        Signature signature = scene->getSignature(entity);

        if (signature.test(scene->getComponentId<MeshComponent>())){
            MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);

            createOrUpdateModel(entity, model, mesh);
        }
    }

    auto meshes = scene->getComponentArray<MeshComponent>();
    for (int i = 0; i < meshes->size(); i++){
        MeshComponent& mesh = meshes->getComponentFromIndex(i);

        if (mesh.aabb == AABB::ZERO){
            calculateMeshAABB(mesh);
        }
    }

}

void MeshSystem::draw(){

}

void MeshSystem::onComponentAdded(Entity entity, ComponentId componentId) {
    if (componentId == scene->getComponentId<InstancedMeshComponent>()) {
        InstancedMeshComponent& instmesh = scene->getComponent<InstancedMeshComponent>(entity);
        if (instmesh.buffer.getAttributes().empty()) {
            instmesh.buffer.clear();
            instmesh.buffer.addAttribute(AttributeType::INSTANCEMATRIXCOL1, 4, 0, true);
            instmesh.buffer.addAttribute(AttributeType::INSTANCEMATRIXCOL2, 4, 4 * sizeof(float), true);
            instmesh.buffer.addAttribute(AttributeType::INSTANCEMATRIXCOL3, 4, 8 * sizeof(float), true);
            instmesh.buffer.addAttribute(AttributeType::INSTANCEMATRIXCOL4, 4, 12 * sizeof(float), true);
            instmesh.buffer.addAttribute(AttributeType::INSTANCECOLOR, 4, 16 * sizeof(float), true);
            instmesh.buffer.addAttribute(AttributeType::INSTANCETEXTURERECT, 4, 20 * sizeof(float), true);
            instmesh.buffer.setStride(24 * sizeof(float));
            instmesh.buffer.setRenderAttributes(true);
            instmesh.buffer.setInstanceBuffer(true);
            instmesh.buffer.setUsage(BufferUsage::STREAM);
        }
        if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)) {
            if (mesh->loaded)
                mesh->needReload = true;
        }
    }
}

void MeshSystem::onComponentRemoved(Entity entity, ComponentId componentId) {
    if (componentId == scene->getComponentId<ModelComponent>()) {
        ModelComponent& model = scene->getComponent<ModelComponent>(entity);
        destroyModel(model);
    }
    if (componentId == scene->getComponentId<InstancedMeshComponent>()) {
        if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)) {
            mesh->aabb = mesh->verticesAABB;
            mesh->needUpdateAABB = true;
            if (mesh->loaded) mesh->needReload = true;
        }
    }
}
