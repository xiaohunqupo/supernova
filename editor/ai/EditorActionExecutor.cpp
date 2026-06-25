#include "EditorActionExecutor.h"

#include "AiPathUtils.h"
#include "AiTerrainCapability.h"
#include "EditorActionRegistry.h"
#include "HttpClient.h"

#include "Out.h"
#include "Project.h"
#include "Catalog.h"
#include "Exporter.h"
#include "Stream.h"
#include "util/ProjectUtils.h"
#include "component/Body3DComponent.h"
#include "component/MeshComponent.h"
#include "component/ScriptComponent.h"
#include "component/TerrainComponent.h"
#include "command/CommandHandle.h"
#include "command/type/AddChildSceneCmd.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/AddEntityToBundleCmd.h"
#include "command/type/ComponentToBundleLocalCmd.h"
#include "command/type/ComponentToBundleSharedCmd.h"
#include "command/type/CopyFileCmd.h"
#include "command/type/CreateDirCmd.h"
#include "command/type/CreateEntityBundleCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/CreateMaterialFileCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/DeleteFileCmd.h"
#include "command/type/DuplicateEntityCmd.h"
#include "command/type/EntityNameCmd.h"
#include "command/type/ImportEntityBundleCmd.h"
#include "command/type/LinkMaterialCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "command/type/MoveEntityOrderCmd.h"
#include "command/type/ObjectTransformCmd.h"
#include "command/type/PropertyCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/RemoveChildSceneCmd.h"
#include "command/type/RemoveComponentCmd.h"
#include "command/type/RemoveEntityFromBundleCmd.h"
#include "command/type/RenameFileCmd.h"
#include "command/type/SceneNameCmd.h"
#include "command/type/ScenePropertyCmd.h"
#include "command/type/SetChildSceneStartActiveCmd.h"
#include "command/type/UnlinkMaterialCmd.h"
#include "texture/Texture.h"
#include "util/Util.h"
#include "window/ResourcesWindow.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace doriax::editor::ai {

namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string sceneTypeName(SceneType type) {
    switch (type) {
        case SceneType::SCENE_3D: return "3D";
        case SceneType::SCENE_2D: return "2D";
        case SceneType::SCENE_UI: return "UI";
    }
    return "Unknown";
}

bool parseVector3(const Json& value, Vector3& out) {
    if (!value.is_object()) return false;
    if (value.contains("x") && value["x"].is_number()) out.x = value["x"].get<float>();
    if (value.contains("y") && value["y"].is_number()) out.y = value["y"].get<float>();
    if (value.contains("z") && value["z"].is_number()) out.z = value["z"].get<float>();
    return true;
}

bool parseVector2(const Json& value, Vector2& out) {
    if (!value.is_object()) return false;
    if (value.contains("x") && value["x"].is_number()) out.x = value["x"].get<float>();
    if (value.contains("y") && value["y"].is_number()) out.y = value["y"].get<float>();
    return true;
}

bool parseVector4(const Json& value, Vector4& out) {
    if (!value.is_object()) return false;
    if (value.contains("x") && value["x"].is_number()) out.x = value["x"].get<float>();
    if (value.contains("y") && value["y"].is_number()) out.y = value["y"].get<float>();
    if (value.contains("z") && value["z"].is_number()) out.z = value["z"].get<float>();
    if (value.contains("w") && value["w"].is_number()) out.w = value["w"].get<float>();
    return true;
}

bool parseQuaternion(const Json& value, Quaternion& out) {
    if (!value.is_object()) return false;
    if (value.contains("w") && value["w"].is_number()) out.w = value["w"].get<float>();
    if (value.contains("x") && value["x"].is_number()) out.x = value["x"].get<float>();
    if (value.contains("y") && value["y"].is_number()) out.y = value["y"].get<float>();
    if (value.contains("z") && value["z"].is_number()) out.z = value["z"].get<float>();
    return true;
}

Json vector2Json(const Vector2& value) {
    return Json{{"x", value.x}, {"y", value.y}};
}

Json vector3Json(const Vector3& value) {
    return Json{{"x", value.x}, {"y", value.y}, {"z", value.z}};
}

Json vector4Json(const Vector4& value) {
    return Json{{"x", value.x}, {"y", value.y}, {"z", value.z}, {"w", value.w}};
}

Json quaternionJson(const Quaternion& value) {
    return Json{{"w", value.w}, {"x", value.x}, {"y", value.y}, {"z", value.z}};
}

uint32_t resolveSceneId(Project* project, const Json& args) {
    if (args.contains("scene_id") && args["scene_id"].is_number_unsigned()) {
        return args["scene_id"].get<uint32_t>();
    }
    if (args.contains("scene_id") && args["scene_id"].is_number_integer()) {
        int value = args["scene_id"].get<int>();
        if (value >= 0) return static_cast<uint32_t>(value);
    }
    return project ? project->getSelectedSceneId() : NULL_PROJECT_SCENE;
}

Entity resolveEntity(SceneProject* sceneProject, const Json& args) {
    if (!sceneProject || !sceneProject->scene) return NULL_ENTITY;
    if (args.contains("entity_id") && args["entity_id"].is_number_unsigned()) {
        Entity entity = args["entity_id"].get<Entity>();
        for (Entity existing : sceneProject->entities) {
            if (existing == entity) return entity;
        }
    }
    if (args.contains("entity_id") && args["entity_id"].is_number_integer()) {
        int64_t raw = args["entity_id"].get<int64_t>();
        if (raw >= 0) {
            Entity entity = static_cast<Entity>(raw);
            for (Entity existing : sceneProject->entities) {
                if (existing == entity) return entity;
            }
        }
    }
    if (args.contains("entity_name") && args["entity_name"].is_string()) {
        const std::string name = args["entity_name"].get<std::string>();
        for (Entity entity : sceneProject->entities) {
            if (sceneProject->scene->getEntityName(entity) == name) {
                return entity;
            }
        }
    }
    return NULL_ENTITY;
}

Entity resolveEntityByKeys(SceneProject* sceneProject, const Json& args,
                           const char* idKey, const char* nameKey) {
    if (!sceneProject || !sceneProject->scene) return NULL_ENTITY;
    if (args.contains(idKey)) {
        Json tmp = Json::object();
        tmp["entity_id"] = args[idKey];
        return resolveEntity(sceneProject, tmp);
    }
    if (args.contains(nameKey) && args[nameKey].is_string()) {
        Json tmp = Json::object();
        tmp["entity_name"] = args[nameKey];
        return resolveEntity(sceneProject, tmp);
    }
    return NULL_ENTITY;
}

bool parseComponentType(const std::string& name, ComponentType& out) {
    try {
        out = Catalog::getComponentType(name);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parseEntityType(const std::string& typeName, EntityCreationType& type) {
    static const std::unordered_map<std::string, EntityCreationType> map = {
        {"empty", EntityCreationType::EMPTY},
        {"object", EntityCreationType::OBJECT},
        {"box", EntityCreationType::BOX},
        {"plane", EntityCreationType::PLANE},
        {"wall", EntityCreationType::WALL},
        {"mirror", EntityCreationType::MIRROR},
        {"sphere", EntityCreationType::SPHERE},
        {"cylinder", EntityCreationType::CYLINDER},
        {"capsule", EntityCreationType::CAPSULE},
        {"torus", EntityCreationType::TORUS},
        {"image", EntityCreationType::IMAGE},
        {"sprite", EntityCreationType::SPRITE},
        {"tilemap", EntityCreationType::TILEMAP},
        {"text", EntityCreationType::TEXT},
        {"button", EntityCreationType::BUTTON},
        {"scrollbar", EntityCreationType::SCROLLBAR},
        {"progressbar", EntityCreationType::PROGRESSBAR},
        {"textedit", EntityCreationType::TEXTEDIT},
        {"panel", EntityCreationType::PANEL},
        {"polygon", EntityCreationType::POLYGON},
        {"container", EntityCreationType::CONTAINER},
        {"camera", EntityCreationType::CAMERA},
        {"point_light", EntityCreationType::POINT_LIGHT},
        {"directional_light", EntityCreationType::DIRECTIONAL_LIGHT},
        {"spot_light", EntityCreationType::SPOT_LIGHT},
        {"joint2d", EntityCreationType::JOINT2D},
        {"joint3d", EntityCreationType::JOINT3D},
        {"body2d", EntityCreationType::BODY2D},
        {"body3d", EntityCreationType::BODY3D},
        {"sky", EntityCreationType::SKY},
        {"fog", EntityCreationType::FOG},
        {"sound", EntityCreationType::SOUND},
        {"sound_3d", EntityCreationType::SOUND_3D},
        {"animation", EntityCreationType::ANIMATION},
        {"sprite_animation", EntityCreationType::SPRITE_ANIMATION},
        {"position_action", EntityCreationType::POSITION_ACTION},
        {"rotation_action", EntityCreationType::ROTATION_ACTION},
        {"scale_action", EntityCreationType::SCALE_ACTION},
        {"color_action", EntityCreationType::COLOR_ACTION},
        {"alpha_action", EntityCreationType::ALPHA_ACTION},
        {"model", EntityCreationType::MODEL},
        {"particles", EntityCreationType::PARTICLES},
        {"points", EntityCreationType::POINTS},
        {"lines", EntityCreationType::LINES},
        {"mesh_polygon", EntityCreationType::MESH_POLYGON},
        {"terrain", EntityCreationType::TERRAIN}
    };
    auto it = map.find(lower(typeName));
    if (it == map.end()) return false;
    type = it->second;
    return true;
}

bool hasComponent(EntityRegistry* registry, Entity entity, ComponentType component) {
    if (!registry || entity == NULL_ENTITY) return false;
    std::vector<ComponentType> components = Catalog::findComponents(registry, entity);
    return std::find(components.begin(), components.end(), component) != components.end();
}

bool parseSceneType(const std::string& typeName, SceneType& type) {
    std::string token = lower(typeName);
    if (token == "3d" || token == "scene_3d") {
        type = SceneType::SCENE_3D;
        return true;
    }
    if (token == "2d" || token == "scene_2d") {
        type = SceneType::SCENE_2D;
        return true;
    }
    if (token == "ui" || token == "scene_ui") {
        type = SceneType::SCENE_UI;
        return true;
    }
    return false;
}

bool parsePlatformName(const std::string& value, Platform& out) {
    std::string token = lower(value);
    token.erase(std::remove_if(token.begin(), token.end(), [](char c) {
        return c == ' ' || c == '_' || c == '-';
    }), token.end());
    if (token == "linux") { out = Platform::Linux; return true; }
    if (token == "windows" || token == "win") { out = Platform::Windows; return true; }
    if (token == "macos" || token == "mac") { out = Platform::MacOS; return true; }
    if (token == "ios") { out = Platform::iOS; return true; }
    if (token == "android") { out = Platform::Android; return true; }
    if (token == "web" || token == "wasm" || token == "emscripten") { out = Platform::Web; return true; }
    return false;
}

std::set<Platform> parsePlatformList(const std::string& text, std::string& error) {
    std::set<Platform> platforms;
    std::stringstream ss(text.empty() ? "all" : text);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token.erase(token.begin(), std::find_if(token.begin(), token.end(), [](unsigned char c) {
            return !std::isspace(c);
        }));
        token.erase(std::find_if(token.rbegin(), token.rend(), [](unsigned char c) {
            return !std::isspace(c);
        }).base(), token.end());
        if (token.empty()) continue;
        if (lower(token) == "all") {
            platforms.insert({Platform::Linux, Platform::Windows, Platform::MacOS,
                              Platform::iOS, Platform::Android, Platform::Web});
            continue;
        }
        Platform platform;
        if (!parsePlatformName(token, platform)) {
            error = "Unknown platform: " + token;
            return {};
        }
        platforms.insert(platform);
    }
    if (platforms.empty()) {
        platforms.insert({Platform::Linux, Platform::Windows, Platform::MacOS,
                          Platform::iOS, Platform::Android, Platform::Web});
    }
    return platforms;
}

bool parseShaderOutputFormat(const std::string& value, ShaderOutputFormat& out) {
    const std::string token = lower(value.empty() ? "header" : value);
    if (token == "binary" || token == "sdat") {
        out = ShaderOutputFormat::Binary;
        return true;
    }
    if (token == "header") {
        out = ShaderOutputFormat::Header;
        return true;
    }
    if (token == "json") {
        out = ShaderOutputFormat::Json;
        return true;
    }
    return false;
}

bool parseScriptType(const std::string& value, ScriptType& out) {
    const std::string token = lower(value);
    if (token == "lua" || token == "script_lua") {
        out = ScriptType::SCRIPT_LUA;
        return true;
    }
    if (token == "cpp_subclass" || token == "subclass") {
        out = ScriptType::SUBCLASS;
        return true;
    }
    if (token == "cpp_script_class" || token == "script_class" || token == "cpp") {
        out = ScriptType::SCRIPT_CLASS;
        return true;
    }
    return false;
}

std::string scriptTypeExtension(ScriptType type, bool header) {
    if (type == ScriptType::SCRIPT_LUA) return ".lua";
    return header ? ".h" : ".cpp";
}

std::string sanitizeIdentifier(const std::string& value, const std::string& fallback) {
    std::string result;
    result.reserve(value.size());
    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            result.push_back(c);
        }
    }
    if (result.empty() || std::isdigit(static_cast<unsigned char>(result.front()))) {
        result = fallback;
    }
    return result;
}

ActionResult okResult(const std::string& message, Json data = Json::object()) {
    return {true, message, std::move(data)};
}

ActionResult failResult(const std::string& message) {
    return {false, message, Json::object()};
}

std::string fileTypeFromPath(const fs::path& path) {
    const std::string value = path.string();
    if (Util::isModelFile(value)) return "model";
    if (Util::isImageFile(value)) return "image";
    if (Util::isMaterialFile(value)) return "material";
    if (Util::isSceneFile(value)) return "scene";
    if (Util::isScriptFile(value)) return "script";
    if (Util::isAudioFile(value)) return "audio";
    if (Util::isFontFile(value)) return "font";
    return "file";
}

std::string propertyTypeName(PropertyType type) {
    switch (type) {
        case PropertyType::Bool: return "bool";
        case PropertyType::String: return "string";
        case PropertyType::Float: return "float";
        case PropertyType::Double: return "double";
        case PropertyType::Vector2: return "vector2";
        case PropertyType::Vector3: return "vector3";
        case PropertyType::Vector4: return "vector4";
        case PropertyType::Quat: return "quaternion";
        case PropertyType::Int: return "int";
        case PropertyType::UInt: return "uint";
        case PropertyType::Material: return "material";
        case PropertyType::Texture: return "texture";
        case PropertyType::Enum: return "enum";
        case PropertyType::Ease: return "ease";
        case PropertyType::Custom: return "custom";
        case PropertyType::Entity: return "entity";
        case PropertyType::EntityReference: return "entity_reference";
    }
    return "unknown";
}

Json propertyValueToJson(const PropertyData& property) {
    if (!property.ref) return Json();
    switch (property.type) {
        case PropertyType::Bool:
            return *static_cast<bool*>(property.ref);
        case PropertyType::String:
            return *static_cast<std::string*>(property.ref);
        case PropertyType::Float:
            return *static_cast<float*>(property.ref);
        case PropertyType::Double:
            return *static_cast<double*>(property.ref);
        case PropertyType::Vector2:
            return vector2Json(*static_cast<Vector2*>(property.ref));
        case PropertyType::Vector3:
            return vector3Json(*static_cast<Vector3*>(property.ref));
        case PropertyType::Vector4:
            return vector4Json(*static_cast<Vector4*>(property.ref));
        case PropertyType::Quat:
            return quaternionJson(*static_cast<Quaternion*>(property.ref));
        case PropertyType::Int:
        case PropertyType::Enum:
        case PropertyType::Ease:
            return *static_cast<int*>(property.ref);
        case PropertyType::UInt:
            return *static_cast<unsigned int*>(property.ref);
        case PropertyType::Texture:
            return static_cast<Texture*>(property.ref)->getPath();
        case PropertyType::Entity:
            return *static_cast<Entity*>(property.ref);
        case PropertyType::EntityReference: {
            const EntityReference& ref = *static_cast<EntityReference*>(property.ref);
            return Json{{"entity", ref.entity}, {"scene_id", ref.sceneId}};
        }
        case PropertyType::Material: {
            const Material& material = *static_cast<Material*>(property.ref);
            return Json{{"name", material.name},
                        {"baseColorFactor", vector4Json(material.baseColorFactor)},
                        {"metallicFactor", material.metallicFactor},
                        {"roughnessFactor", material.roughnessFactor},
                        {"emissiveFactor", vector3Json(material.emissiveFactor)},
                        {"baseColorTexture", material.baseColorTexture.getPath()}};
        }
        case PropertyType::Custom:
            return Json{{"unsupported", "custom"}};
    }
    return Json();
}

bool safeRelativePath(Project* project, const Json& args, const char* key, fs::path& rel, std::string& error,
                      bool requireExists = false) {
    if (!args.contains(key) || !args[key].is_string()) {
        error = std::string(key) + " is required.";
        return false;
    }
    rel = fs::path(args[key].get<std::string>()).lexically_normal();
    if (rel.empty()) rel = ".";
    if (!PathUtils::isSafeRelativePath(rel)) {
        error = std::string(key) + " must be a safe project-relative path.";
        return false;
    }
    if (requireExists && project && !fs::exists(project->getProjectPath() / rel)) {
        error = std::string(key) + " does not exist in the project.";
        return false;
    }
    return true;
}

bool valueFieldPresent(const Json& args, const char* key) {
    return args.contains(key) && !args[key].is_null();
}

Command* buildPropertyCommand(Project* project, uint32_t sceneId, Entity entity, ComponentType component,
                              const std::string& propertyName, const PropertyData& property,
                              const Json& args, std::string& error, std::function<void()> onChanged = nullptr) {
    switch (property.type) {
        case PropertyType::Bool:
            if (!valueFieldPresent(args, "bool_value") || !args["bool_value"].is_boolean()) {
                error = "Property requires bool_value.";
                return nullptr;
            }
            return new PropertyCmd<bool>(project, sceneId, entity, component, propertyName, args["bool_value"].get<bool>(), onChanged);
        case PropertyType::String:
            if (!valueFieldPresent(args, "string_value") || !args["string_value"].is_string()) {
                error = "Property requires string_value.";
                return nullptr;
            }
            return new PropertyCmd<std::string>(project, sceneId, entity, component, propertyName, args["string_value"].get<std::string>(), onChanged);
        case PropertyType::Float: {
            if (!valueFieldPresent(args, "number_value") || !args["number_value"].is_number()) {
                error = "Property requires number_value.";
                return nullptr;
            }
            return new PropertyCmd<float>(project, sceneId, entity, component, propertyName, args["number_value"].get<float>(), onChanged);
        }
        case PropertyType::Double: {
            if (!valueFieldPresent(args, "number_value") || !args["number_value"].is_number()) {
                error = "Property requires number_value.";
                return nullptr;
            }
            return new PropertyCmd<double>(project, sceneId, entity, component, propertyName, args["number_value"].get<double>(), onChanged);
        }
        case PropertyType::Vector2: {
            Vector2 value = Vector2::ZERO;
            if (!valueFieldPresent(args, "vector2_value") || !parseVector2(args["vector2_value"], value)) {
                error = "Property requires vector2_value.";
                return nullptr;
            }
            return new PropertyCmd<Vector2>(project, sceneId, entity, component, propertyName, value, onChanged);
        }
        case PropertyType::Vector3: {
            Vector3 value = Vector3::ZERO;
            if (!valueFieldPresent(args, "vector3_value") || !parseVector3(args["vector3_value"], value)) {
                error = "Property requires vector3_value.";
                return nullptr;
            }
            return new PropertyCmd<Vector3>(project, sceneId, entity, component, propertyName, value, onChanged);
        }
        case PropertyType::Vector4: {
            Vector4 value = Vector4::ZERO;
            if (!valueFieldPresent(args, "vector4_value") || !parseVector4(args["vector4_value"], value)) {
                error = "Property requires vector4_value.";
                return nullptr;
            }
            return new PropertyCmd<Vector4>(project, sceneId, entity, component, propertyName, value, onChanged);
        }
        case PropertyType::Quat: {
            Quaternion value = Quaternion::IDENTITY;
            if (!valueFieldPresent(args, "quat_value") || !parseQuaternion(args["quat_value"], value)) {
                error = "Property requires quat_value.";
                return nullptr;
            }
            return new PropertyCmd<Quaternion>(project, sceneId, entity, component, propertyName, value, onChanged);
        }
        case PropertyType::Int:
        case PropertyType::Enum:
        case PropertyType::Ease:
            if (!valueFieldPresent(args, "int_value") || !args["int_value"].is_number_integer()) {
                error = "Property requires int_value.";
                return nullptr;
            }
            return new PropertyCmd<int>(project, sceneId, entity, component, propertyName, args["int_value"].get<int>(), onChanged);
        case PropertyType::UInt:
            if (!valueFieldPresent(args, "int_value") || !args["int_value"].is_number_integer()) {
                error = "Property requires int_value.";
                return nullptr;
            }
            return new PropertyCmd<unsigned int>(project, sceneId, entity, component, propertyName,
                                                static_cast<unsigned int>(std::max(0, args["int_value"].get<int>())), onChanged);
        case PropertyType::Texture: {
            if (!valueFieldPresent(args, "texture_path") || !args["texture_path"].is_string()) {
                error = "Property requires texture_path.";
                return nullptr;
            }
            fs::path rel(args["texture_path"].get<std::string>());
            if (!PathUtils::isSafeRelativePath(rel)) {
                error = "texture_path must be a safe project-relative path.";
                return nullptr;
            }
            return new PropertyCmd<Texture>(project, sceneId, entity, component, propertyName, Texture(rel.generic_string()), onChanged);
        }
        case PropertyType::Entity:
            if (!valueFieldPresent(args, "entity_value") || !args["entity_value"].is_number_integer()) {
                error = "Property requires entity_value.";
                return nullptr;
            }
            return new PropertyCmd<Entity>(project, sceneId, entity, component, propertyName,
                                           static_cast<Entity>(std::max(0, args["entity_value"].get<int>())), onChanged);
        case PropertyType::EntityReference: {
            if (!valueFieldPresent(args, "entity_value") || !args["entity_value"].is_number_integer()) {
                error = "Property requires entity_value.";
                return nullptr;
            }
            EntityReference ref;
            ref.entity = static_cast<Entity>(std::max(0, args["entity_value"].get<int>()));
            ref.sceneId = args.value("entity_scene_id", static_cast<int>(sceneId));
            return new PropertyCmd<EntityReference>(project, sceneId, entity, component, propertyName, ref, onChanged);
        }
        case PropertyType::Material:
        case PropertyType::Custom:
            error = "Property type " + propertyTypeName(property.type) + " is not supported by generic set_component_property.";
            return nullptr;
    }
    error = "Unsupported property type.";
    return nullptr;
}

std::string filenameFromUrl(const std::string& url, const std::string& fallback) {
    std::string trimmed = url;
    size_t query = trimmed.find('?');
    if (query != std::string::npos) trimmed = trimmed.substr(0, query);
    size_t slash = trimmed.find_last_of('/');
    std::string name = slash == std::string::npos ? trimmed : trimmed.substr(slash + 1);
    if (name.empty() || name.find('\\') != std::string::npos || name.find('/') != std::string::npos) {
        name = fallback;
    }
    return name;
}

Entity resolveTerrainEntity(Project* project, SceneProject* sceneProject, const Json& args) {
    Entity entity = resolveEntity(sceneProject, args);
    if (entity != NULL_ENTITY &&
        sceneProject->scene->findComponent<TerrainComponent>(entity)) {
        return entity;
    }

    const std::vector<Entity>& selected = project->getSelectedEntities(sceneProject->id);
    if (selected.size() == 1 &&
        sceneProject->scene->findComponent<TerrainComponent>(selected[0])) {
        return selected[0];
    }

    return NULL_ENTITY;
}

} // namespace

EditorActionExecutor::EditorActionExecutor(Project* project, ResourcesWindow* resourcesWindow, const HttpClient* httpClient)
    : project(project)
    , resourcesWindow(resourcesWindow)
    , httpClient(httpClient) {
}

ActionResult EditorActionExecutor::execute(const std::string& name,
                                           const Json& arguments,
                                           const std::atomic<bool>* cancel) {
    ValidationResult validation = EditorActionRegistry::validate(name, arguments);
    if (!validation.ok) {
        return failResult(validation.error);
    }

    if (!project) {
        return failResult("No project is available.");
    }

    if (name == "get_project_summary") return getProjectSummary();
    if (name == "search_resources") return searchResources(arguments);
    if (name == "list_scene_entities") return listSceneEntities(arguments);
    if (name == "inspect_entity") return inspectEntity(arguments);
    if (name == "inspect_component") return inspectComponent(arguments);
    if (name == "list_component_types") return listComponentTypes();
    if (name == "create_entity") return createEntity(arguments);
    if (name == "set_entity_transform") return setEntityTransform(arguments);
    if (name == "rename_entity") return renameEntity(arguments);
    if (name == "delete_entity") return deleteEntity(arguments);
    if (name == "duplicate_entity") return duplicateEntity(arguments);
    if (name == "reparent_entity") return reparentEntity(arguments);
    if (name == "move_entity_order") return moveEntityOrder(arguments);
    if (name == "select_entities") return selectEntities(arguments);
    if (name == "add_component") return addComponent(arguments);
    if (name == "remove_component") return removeComponent(arguments);
    if (name == "set_component_property") return setComponentProperty(arguments);
    if (name == "create_scene") return createScene(arguments);
    if (name == "rename_scene") return renameScene(arguments);
    if (name == "save_scene") return saveScene(arguments);
    if (name == "save_all_scenes") return saveAllScenes();
    if (name == "set_start_scene") return setStartScene(arguments);
    if (name == "set_scene_property") return setSceneProperty(arguments);
    if (name == "control_play_mode") return controlPlayMode(arguments);
    if (name == "add_child_scene") return addChildScene(arguments);
    if (name == "remove_child_scene") return removeChildScene(arguments);
    if (name == "set_child_scene_start_active") return setChildSceneStartActive(arguments);
    if (name == "load_child_scene_inline") return loadChildSceneInline(arguments);
    if (name == "create_folder") return createFolder(arguments);
    if (name == "rename_resource") return renameResource(arguments);
    if (name == "delete_resource") return deleteResource(arguments);
    if (name == "import_resource_file") return importResourceFile(arguments);
    if (name == "link_material") return linkMaterial(arguments);
    if (name == "unlink_material") return unlinkMaterial(arguments);
    if (name == "create_material_file") return createMaterialFile(arguments);
    if (name == "set_terrain_textures") return setTerrainTextures(arguments);
    if (name == "create_terrain_blendmap") return createTerrainBlendmap(arguments);
    if (name == "add_terrain_collision") return addTerrainCollision(arguments);
    if (name == "create_script") return createScript(arguments);
    if (name == "attach_script") return attachScript(arguments);
    if (name == "create_bundle_from_entity") return createBundleFromEntity(arguments);
    if (name == "import_bundle_instance") return importBundleInstance(arguments);
    if (name == "add_entity_to_bundle") return addEntityToBundle(arguments);
    if (name == "remove_entity_from_bundle") return removeEntityFromBundle(arguments);
    if (name == "make_bundle_component_unique") return makeBundleComponentUnique(arguments);
    if (name == "revert_bundle_component") return revertBundleComponent(arguments);
    if (name == "export_project") return exportProject(arguments, cancel);
    if (name == "generate_shaders") return generateShaders(arguments, cancel);
    if (name == "create_terrain_heightmap") return createTerrainHeightmap(arguments);
    if (name == "import_project_model") return importProjectModel(arguments);
    if (name == "search_curated_assets") return searchCuratedAssets(arguments, cancel);
    if (name == "download_curated_asset") return downloadCuratedAsset(arguments, cancel);

    return failResult("Unknown action.");
}

ActionResult EditorActionExecutor::getProjectSummary() {
    Json data;
    data["name"] = project->getName();
    data["path"] = project->getProjectPath().string();
    data["selected_scene_id"] = project->getSelectedSceneId();
    data["assets_dir"] = project->getAssetsDir().generic_string();
    data["scenes"] = Json::array();
    for (const auto& scene : project->getScenes()) {
        data["scenes"].push_back({
            {"id", scene.id},
            {"name", scene.name},
            {"type", sceneTypeName(scene.sceneType)},
            {"opened", scene.opened},
            {"entity_count", scene.entities.size()},
            {"selected_entities", scene.selectedEntities.size()}
        });
    }
    return okResult("Read project summary.", data);
}

ActionResult EditorActionExecutor::searchResources(const Json& arguments) {
    const std::string query = lower(arguments.value("query", ""));
    const std::string wantedType = lower(arguments.value("type", "any"));
    int maxResults = arguments.value("max_results", 20);
    maxResults = std::max(1, std::min(50, maxResults));

    const fs::path projectPath = project->getProjectPath();
    if (projectPath.empty() || !fs::exists(projectPath)) {
        return failResult("Project path does not exist.");
    }

    Json matches = Json::array();
    std::error_code ec;
    for (fs::recursive_directory_iterator it(projectPath, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec && static_cast<int>(matches.size()) < maxResults;
         it.increment(ec)) {
        const fs::path path = it->path();
        if (it->is_directory(ec)) {
            if (path.filename() == ".doriax" || path.filename() == "build") {
                it.disable_recursion_pending();
            }
            continue;
        }

        fs::path rel = fs::relative(path, projectPath, ec);
        if (ec) continue;
        const std::string relText = rel.generic_string();
        if (lower(relText).find(query) == std::string::npos) continue;
        const std::string type = fileTypeFromPath(rel);
        if (wantedType != "any" && !wantedType.empty() && type != wantedType) continue;

        matches.push_back({{"path", relText}, {"type", type}});
    }

    return okResult("Found " + std::to_string(matches.size()) + " resource(s).",
                    Json{{"matches", matches}});
}

ActionResult EditorActionExecutor::listSceneEntities(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return failResult("Scene not found.");
    }

    Json entities = Json::array();
    for (Entity entity : sceneProject->entities) {
        Json components = Json::array();
        for (ComponentType type : Catalog::findComponents(sceneProject->scene, entity)) {
            components.push_back(Catalog::getComponentName(type, true));
        }
        entities.push_back({
            {"id", entity},
            {"name", sceneProject->scene->getEntityName(entity)},
            {"components", components}
        });
    }

    return okResult("Listed entities in scene " + sceneProject->name + ".",
                    Json{{"scene_id", sceneId}, {"scene_name", sceneProject->name}, {"entities", entities}});
}

ActionResult EditorActionExecutor::inspectEntity(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");

    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");

    Json data = {
        {"scene_id", sceneId},
        {"entity_id", entity},
        {"name", sceneProject->scene->getEntityName(entity)},
        {"selected", project->isSelectedEntity(sceneId, entity)},
        {"in_bundle", project->isEntityInBundle(sceneId, entity)}
    };

    Transform* transform = sceneProject->scene->findComponent<Transform>(entity);
    if (transform) {
        data["transform"] = {
            {"position", vector3Json(transform->position)},
            {"rotation", quaternionJson(transform->rotation)},
            {"scale", vector3Json(transform->scale)},
            {"parent", transform->parent},
            {"visible", transform->visible}
        };
    }

    data["components"] = Json::array();
    const bool includeProperties = arguments.value("include_properties", false);
    for (ComponentType type : Catalog::findComponents(sceneProject->scene, entity)) {
        Json comp = {
            {"name", Catalog::getComponentName(type)},
            {"short_name", Catalog::getComponentName(type, true)}
        };
        if (includeProperties) {
            Json props = Json::object();
            for (const auto& [propName, prop] : Catalog::findEntityProperties(sceneProject->scene, entity, type)) {
                props[propName] = {{"type", propertyTypeName(prop.type)}, {"value", propertyValueToJson(prop)}};
            }
            comp["properties"] = props;
        }
        data["components"].push_back(comp);
    }

    return okResult("Inspected entity.", data);
}

ActionResult EditorActionExecutor::inspectComponent(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");

    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");

    ComponentType component;
    if (!parseComponentType(arguments.value("component", ""), component)) {
        return failResult("Unknown component type.");
    }

    auto properties = Catalog::findEntityProperties(sceneProject->scene, entity, component);
    if (properties.empty()) {
        return failResult("Component not found on entity or has no inspectable properties.");
    }

    Json props = Json::object();
    for (const auto& [propName, prop] : properties) {
        props[propName] = {{"type", propertyTypeName(prop.type)}, {"value", propertyValueToJson(prop)}};
    }

    return okResult("Inspected component.",
                    Json{{"scene_id", sceneId},
                         {"entity_id", entity},
                         {"component", Catalog::getComponentName(component)},
                         {"properties", props}});
}

ActionResult EditorActionExecutor::listComponentTypes() {
    Json components = Json::array();
    for (int i = static_cast<int>(ComponentType::Transform); i <= static_cast<int>(ComponentType::MirrorComponent); ++i) {
        ComponentType type = static_cast<ComponentType>(i);
        std::string name = Catalog::getComponentName(type);
        if (!name.empty()) {
            components.push_back({{"name", name}, {"short_name", Catalog::getComponentName(type, true)}});
        }
    }
    return okResult("Listed component types.", Json{{"components", components}});
}

ActionResult EditorActionExecutor::createEntity(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return failResult("Scene not found.");
    }

    EntityCreationType type = EntityCreationType::EMPTY;
    if (!parseEntityType(arguments.value("type", "empty"), type)) {
        return failResult("Unsupported entity type: " + arguments.value("type", ""));
    }

    Entity parent = NULL_ENTITY;
    if (arguments.contains("parent_id")) {
        parent = resolveEntityByKeys(sceneProject, arguments, "parent_id", "parent_name");
        if (parent == NULL_ENTITY) return failResult("Parent entity not found.");
    }

    auto* cmd = parent != NULL_ENTITY
        ? new CreateEntityCmd(project, sceneId, arguments.value("name", "Entity"), type, parent)
        : new CreateEntityCmd(project, sceneId, arguments.value("name", "Entity"), type);
    if (arguments.contains("position")) {
        Vector3 position = Vector3::ZERO;
        if (parseVector3(arguments["position"], position)) {
            cmd->addProperty<Vector3>(ComponentType::Transform, "position", position);
        }
    }
    CommandHandle::get(sceneId)->addCommandNoMerge(cmd);

    return okResult("Created entity through the command history.",
                    Json{{"scene_id", sceneId}, {"entity_id", cmd->getEntity()}});
}

ActionResult EditorActionExecutor::setEntityTransform(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return failResult("Scene not found.");
    }

    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) {
        return failResult("Entity not found.");
    }

    Transform* transform = sceneProject->scene->findComponent<Transform>(entity);
    if (!transform) {
        return failResult("Entity has no Transform component.");
    }

    Vector3 position = transform->position;
    Quaternion rotation = transform->rotation;
    Vector3 scale = transform->scale;
    if (arguments.contains("position")) parseVector3(arguments["position"], position);
    if (arguments.contains("rotation")) parseQuaternion(arguments["rotation"], rotation);
    if (arguments.contains("scale")) parseVector3(arguments["scale"], scale);

    CommandHandle::get(sceneId)->addCommandNoMerge(
        new ObjectTransformCmd(project, sceneId, entity, position, rotation, scale));

    return okResult("Updated entity transform through the command history.");
}

ActionResult EditorActionExecutor::renameEntity(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");
    CommandHandle::get(sceneId)->addCommandNoMerge(new EntityNameCmd(project, sceneId, entity, arguments.value("new_name", "")));
    return okResult("Renamed entity through the command history.");
}

ActionResult EditorActionExecutor::deleteEntity(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");
    CommandHandle::get(sceneId)->addCommandNoMerge(new DeleteEntityCmd(project, sceneId, entity));
    return okResult("Deleted entity through the command history.");
}

ActionResult EditorActionExecutor::duplicateEntity(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");
    auto* cmd = new DuplicateEntityCmd(project, sceneId, std::vector<Entity>{entity});
    CommandHandle::get(sceneId)->addCommandNoMerge(cmd);
    Json created = Json::array();
    for (Entity createdEntity : cmd->getCreatedEntities()) created.push_back(createdEntity);
    return okResult("Duplicated entity through the command history.", Json{{"created_entities", created}});
}

ActionResult EditorActionExecutor::reparentEntity(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");

    Entity parent = NULL_ENTITY;
    if (arguments.contains("parent_id") || arguments.contains("parent_name")) {
        parent = resolveEntityByKeys(sceneProject, arguments, "parent_id", "parent_name");
        if (parent == NULL_ENTITY && arguments.value("parent_id", 0) != 0) return failResult("Parent entity not found.");
    }

    if (parent != NULL_ENTITY) {
        CommandHandle::get(sceneId)->addCommandNoMerge(new MoveEntityOrderCmd(project, sceneId, entity, parent, InsertionType::INTO));
        return okResult("Reparented entity through the command history.");
    }

    Entity rootTarget = NULL_ENTITY;
    for (Entity candidate : sceneProject->entities) {
        if (candidate != entity && sceneProject->scene->findComponent<Transform>(candidate)) {
            Transform* transform = sceneProject->scene->findComponent<Transform>(candidate);
            if (transform && transform->parent == NULL_ENTITY) {
                rootTarget = candidate;
                break;
            }
        }
    }
    if (rootTarget == NULL_ENTITY) {
        return failResult("Cannot move to scene root because there is no root-level target to order against.");
    }
    CommandHandle::get(sceneId)->addCommandNoMerge(new MoveEntityOrderCmd(project, sceneId, entity, rootTarget, InsertionType::BEFORE));
    return okResult("Moved entity to scene root through the command history.");
}

ActionResult EditorActionExecutor::moveEntityOrder(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveEntity(sceneProject, arguments);
    Entity target = resolveEntityByKeys(sceneProject, arguments, "target_id", "target_name");
    if (entity == NULL_ENTITY || target == NULL_ENTITY) return failResult("Entity or target not found.");

    std::string insertion = lower(arguments.value("insertion", ""));
    InsertionType type = InsertionType::AFTER;
    if (insertion == "before") type = InsertionType::BEFORE;
    else if (insertion == "after") type = InsertionType::AFTER;
    else if (insertion == "child_first" || insertion == "child_last" || insertion == "into") type = InsertionType::INTO;
    else return failResult("Unsupported insertion. Use before, after, child_first, or child_last.");

    CommandHandle::get(sceneId)->addCommandNoMerge(new MoveEntityOrderCmd(project, sceneId, entity, target, type));
    return okResult("Moved entity order through the command history.");
}

ActionResult EditorActionExecutor::selectEntities(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    if (arguments.value("clear", false)) {
        project->clearSelectedEntities(sceneId);
        return okResult("Cleared selection.");
    }
    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");
    project->setSelectedEntity(sceneId, entity);
    return okResult("Selected entity.", Json{{"scene_id", sceneId}, {"entity_id", entity}});
}

ActionResult EditorActionExecutor::addComponent(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");
    ComponentType component;
    if (!parseComponentType(arguments.value("component", ""), component)) return failResult("Unknown component type.");
    if (hasComponent(sceneProject->scene, entity, component)) {
        return failResult("Entity already has component.");
    }
    CommandHandle::get(sceneId)->addCommandNoMerge(new AddComponentCmd(project, sceneId, entity, component));
    return okResult("Added component through the command history.");
}

ActionResult EditorActionExecutor::removeComponent(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");
    ComponentType component;
    if (!parseComponentType(arguments.value("component", ""), component)) return failResult("Unknown component type.");
    if (!hasComponent(sceneProject->scene, entity, component)) {
        return failResult("Entity does not have component.");
    }
    CommandHandle::get(sceneId)->addCommandNoMerge(new RemoveComponentCmd(project, sceneId, entity, component));
    return okResult("Removed component through the command history.");
}

ActionResult EditorActionExecutor::setComponentProperty(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");
    ComponentType component;
    if (!parseComponentType(arguments.value("component", ""), component)) return failResult("Unknown component type.");

    auto properties = Catalog::findEntityProperties(sceneProject->scene, entity, component);
    const std::string propertyName = arguments.value("property", "");
    auto it = properties.find(propertyName);
    if (it == properties.end() || !it->second.ref) return failResult("Property not found or not editable.");

    std::string error;
    std::unique_ptr<Command> cmd(buildPropertyCommand(project, sceneId, entity, component, propertyName, it->second, arguments, error));
    if (!cmd) return failResult(error);
    CommandHandle::get(sceneId)->addCommandNoMerge(cmd.release());
    return okResult("Set component property through the command history.");
}

ActionResult EditorActionExecutor::createScene(const Json& arguments) {
    SceneType type;
    if (!parseSceneType(arguments.value("type", ""), type)) {
        return failResult("Unsupported scene type. Use 3d, 2d, or ui.");
    }

    project->createNewScene(arguments.value("name", "Scene"), type);
    return okResult("Requested new scene creation.", Json{{"name", arguments.value("name", "Scene")},
                                                          {"type", sceneTypeName(type)}});
}

ActionResult EditorActionExecutor::renameScene(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject) return failResult("Scene not found.");

    CommandHandle::get(sceneId)->addCommandNoMerge(new SceneNameCmd(project, sceneId, arguments.value("new_name", "")));
    return okResult("Renamed scene through the command history.");
}

ActionResult EditorActionExecutor::saveScene(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    if (!project->getScene(sceneId)) return failResult("Scene not found.");

    project->saveScene(sceneId);
    return okResult("Requested scene save.", Json{{"scene_id", sceneId}});
}

ActionResult EditorActionExecutor::saveAllScenes() {
    project->saveAllScenes();
    return okResult("Requested save for all modified scenes.");
}

ActionResult EditorActionExecutor::setStartScene(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    if (!project->getScene(sceneId)) return failResult("Scene not found.");

    project->setStartSceneId(sceneId);
    project->saveProjectFile();
    return okResult("Set project startup scene.", Json{{"scene_id", sceneId}});
}

ActionResult EditorActionExecutor::setSceneProperty(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");

    const std::string property = arguments.value("property", "");
    Command* cmd = nullptr;
    if (property == "background_color") {
        Vector4 value = Catalog::getSceneProperty<Vector4>(sceneProject->scene, property);
        if (!parseVector4(arguments.value("vector4_value", Json::object()), value)) {
            return failResult("background_color requires vector4_value.");
        }
        cmd = new ScenePropertyCmd<Vector4>(sceneProject, property, value);
    } else if (property == "global_illumination_color") {
        Vector3 value = Catalog::getSceneProperty<Vector3>(sceneProject->scene, property);
        if (!parseVector3(arguments.value("vector3_value", Json::object()), value)) {
            return failResult("global_illumination_color requires vector3_value.");
        }
        cmd = new ScenePropertyCmd<Vector3>(sceneProject, property, value);
    } else if (property == "shadows_pcf" || property == "ssao_enabled" ||
               property == "ssao_debug" || property == "ssr_enabled") {
        if (!arguments.contains("bool_value") || !arguments["bool_value"].is_boolean()) {
            return failResult(property + " requires bool_value.");
        }
        cmd = new ScenePropertyCmd<bool>(sceneProject, property, arguments["bool_value"].get<bool>());
    } else if (property == "global_illumination_intensity" || property == "ssao_radius" ||
               property == "ssao_intensity" || property == "ssao_bias" ||
               property == "ssr_max_distance" || property == "ssr_thickness" ||
               property == "ssr_intensity" || property == "ssr_blur") {
        if (!arguments.contains("number_value") || !arguments["number_value"].is_number()) {
            return failResult(property + " requires number_value.");
        }
        cmd = new ScenePropertyCmd<float>(sceneProject, property, arguments["number_value"].get<float>());
    } else if (property == "ssr_max_steps" || property == "ssr_debug_mode") {
        if (!arguments.contains("int_value") || !arguments["int_value"].is_number_integer()) {
            return failResult(property + " requires int_value.");
        }
        cmd = new ScenePropertyCmd<int>(sceneProject, property, arguments["int_value"].get<int>());
    } else if (property == "light_state") {
        if (!arguments.contains("int_value") || !arguments["int_value"].is_number_integer()) {
            return failResult("light_state requires int_value.");
        }
        cmd = new ScenePropertyCmd<LightState>(sceneProject, property,
                                               static_cast<LightState>(arguments["int_value"].get<int>()));
    } else {
        return failResult("Unsupported scene property: " + property);
    }

    CommandHandle::get(sceneId)->addCommandNoMerge(cmd);
    return okResult("Set scene property through the command history.");
}

ActionResult EditorActionExecutor::controlPlayMode(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    if (!project->getScene(sceneId)) return failResult("Scene not found.");

    const std::string action = lower(arguments.value("action", ""));
    if (action == "start") project->start(sceneId);
    else if (action == "pause") project->pause(sceneId);
    else if (action == "resume") project->resume(sceneId);
    else if (action == "stop") project->stop(sceneId);
    else return failResult("Unsupported play mode action. Use start, pause, resume, or stop.");

    return okResult("Sent play mode command.", Json{{"scene_id", sceneId}, {"action", action}});
}

ActionResult EditorActionExecutor::addChildScene(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    const uint32_t childSceneId = static_cast<uint32_t>(arguments.value("child_scene_id", 0));
    if (!project->getScene(sceneId) || !project->getScene(childSceneId)) return failResult("Parent or child scene not found.");
    if (sceneId == childSceneId) return failResult("A scene cannot be its own child.");

    const bool startActive = arguments.value("start_active", true);
    if (startActive) {
        CommandHandle::get(sceneId)->addCommandNoMerge(new AddChildSceneCmd(project, sceneId, childSceneId));
    } else {
        auto* multiCmd = new MultiPropertyCmd();
        multiCmd->addCommand(std::make_unique<AddChildSceneCmd>(project, sceneId, childSceneId));
        multiCmd->addCommand(std::make_unique<SetChildSceneStartActiveCmd>(project, sceneId, childSceneId, false));
        CommandHandle::get(sceneId)->addCommandNoMerge(multiCmd);
    }
    return okResult("Added child scene through the command history.");
}

ActionResult EditorActionExecutor::removeChildScene(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    const uint32_t childSceneId = static_cast<uint32_t>(arguments.value("child_scene_id", 0));
    if (!project->getScene(sceneId)) return failResult("Scene not found.");
    if (!project->hasChildScene(sceneId, childSceneId)) return failResult("Child scene is not attached to this scene.");

    CommandHandle::get(sceneId)->addCommandNoMerge(new RemoveChildSceneCmd(project, sceneId, childSceneId));
    return okResult("Removed child scene through the command history.");
}

ActionResult EditorActionExecutor::setChildSceneStartActive(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    const uint32_t childSceneId = static_cast<uint32_t>(arguments.value("child_scene_id", 0));
    if (!project->getScene(sceneId)) return failResult("Scene not found.");
    if (!project->hasChildScene(sceneId, childSceneId)) return failResult("Child scene is not attached to this scene.");

    CommandHandle::get(sceneId)->addCommandNoMerge(
        new SetChildSceneStartActiveCmd(project, sceneId, childSceneId, arguments.value("start_active", true)));
    return okResult("Updated child scene Start active through the command history.");
}

ActionResult EditorActionExecutor::loadChildSceneInline(const Json& arguments) {
    const uint32_t childSceneId = static_cast<uint32_t>(arguments.value("child_scene_id", 0));
    if (!project->getScene(childSceneId)) return failResult("Child scene not found.");

    const bool load = arguments.value("load", true);
    if (load) {
        if (!project->loadChildSceneInline(childSceneId)) return failResult("Failed to load child scene inline.");
    } else {
        project->unloadChildSceneInline(childSceneId);
    }
    return okResult(load ? "Loaded child scene inline." : "Unloaded child scene inline.");
}

ActionResult EditorActionExecutor::createFolder(const Json& arguments) {
    fs::path parentRel = arguments.value("parent_path", std::string());
    fs::path parentFull = project->getProjectPath();
    if (!parentRel.empty()) {
        parentRel = parentRel.lexically_normal();
        if (!PathUtils::isSafeRelativePath(parentRel)) {
            return failResult("parent_path must be a safe project-relative directory.");
        }
        parentFull /= parentRel;
    }
    if (!fs::exists(parentFull) || !fs::is_directory(parentFull)) {
        return failResult("Parent directory does not exist.");
    }

    const std::string name = arguments.value("name", "");
    if (name.empty() || fs::path(name).filename().string() != name) {
        return failResult("Folder name must be a single safe path segment.");
    }
    project->getProjectCommandHistory()->addCommandNoMerge(new CreateDirCmd(name, parentFull.string()));
    if (resourcesWindow) resourcesWindow->refreshCurrentDirectory();
    return okResult("Created folder through the project command history.");
}

ActionResult EditorActionExecutor::renameResource(const Json& arguments) {
    std::string error;
    fs::path rel;
    if (!safeRelativePath(project, arguments, "path", rel, error, true)) return failResult(error);

    const std::string newName = arguments.value("new_name", "");
    if (newName.empty() || fs::path(newName).filename().string() != newName) {
        return failResult("new_name must be a single filename, not a path.");
    }

    fs::path full = project->getProjectPath() / rel;
    project->getProjectCommandHistory()->addCommandNoMerge(
        new RenameFileCmd(project, rel.filename().string(), newName, full.parent_path().string()));
    if (resourcesWindow) resourcesWindow->refreshCurrentDirectory();
    return okResult("Renamed resource through the project command history.");
}

ActionResult EditorActionExecutor::deleteResource(const Json& arguments) {
    std::string error;
    fs::path rel;
    if (!safeRelativePath(project, arguments, "path", rel, error, true)) return failResult(error);
    if (rel.empty() || rel == ".doriax" || rel.begin()->string() == ".doriax") {
        return failResult("Refusing to delete internal project metadata.");
    }

    fs::path full = project->getProjectPath() / rel;
    project->getProjectCommandHistory()->addCommandNoMerge(
        new DeleteFileCmd(project, std::vector<fs::path>{full}, project->getProjectPath()));
    if (resourcesWindow) resourcesWindow->refreshCurrentDirectory();
    return okResult("Deleted resource through the project command history.");
}

ActionResult EditorActionExecutor::importResourceFile(const Json& arguments) {
    fs::path source = arguments.value("source_path", "");
    if (source.empty() || source.is_relative() || !fs::exists(source)) {
        return failResult("source_path must be an existing absolute path.");
    }

    std::string error;
    fs::path targetRel;
    if (!safeRelativePath(project, arguments, "target_dir", targetRel, error, false)) return failResult(error);
    fs::path targetFull = project->getProjectPath() / targetRel;
    std::error_code ec;
    fs::create_directories(targetFull, ec);
    if (ec) return failResult("Failed to create destination directory: " + ec.message());

    project->getProjectCommandHistory()->addCommandNoMerge(
        new CopyFileCmd(project, std::vector<std::string>{source.string()}, targetFull.string(), true));
    if (resourcesWindow) resourcesWindow->refreshCurrentDirectory();
    return okResult("Imported resource file through the project command history.");
}

ActionResult EditorActionExecutor::linkMaterial(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");
    if (!hasComponent(sceneProject->scene, entity, ComponentType::MeshComponent)) return failResult("Entity has no Mesh component.");

    std::string error;
    fs::path rel;
    if (!safeRelativePath(project, arguments, "material_path", rel, error, true)) return failResult(error);
    if (!Util::isMaterialFile(rel.string())) return failResult("material_path must point to a .material file.");

    Material material;
    try {
        material = Stream::decodeMaterial(YAML::LoadFile((project->getProjectPath() / rel).string()));
    } catch (const std::exception& e) {
        return failResult(std::string("Failed to load material: ") + e.what());
    }
    material.name = rel.generic_string();

    const unsigned int submeshIndex = static_cast<unsigned int>(std::max(0, arguments.value("submesh_index", 0)));
    const std::string propertyName = "submeshes[" + std::to_string(submeshIndex) + "].material";
    if (!Catalog::getProperty(sceneProject->scene, entity, ComponentType::MeshComponent, propertyName).ref) {
        return failResult("Submesh material property not found.");
    }

    CommandHandle::get(sceneId)->addCommandNoMerge(
        new LinkMaterialCmd(project, sceneId, entity, ComponentType::MeshComponent, propertyName, submeshIndex, material));
    return okResult("Linked material through the command history.");
}

ActionResult EditorActionExecutor::unlinkMaterial(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");
    if (!hasComponent(sceneProject->scene, entity, ComponentType::MeshComponent)) return failResult("Entity has no Mesh component.");

    const unsigned int submeshIndex = static_cast<unsigned int>(std::max(0, arguments.value("submesh_index", 0)));
    const std::string propertyName = "submeshes[" + std::to_string(submeshIndex) + "].material";
    if (!Catalog::getProperty(sceneProject->scene, entity, ComponentType::MeshComponent, propertyName).ref) {
        return failResult("Submesh material property not found.");
    }

    CommandHandle::get(sceneId)->addCommandNoMerge(
        new UnlinkMaterialCmd(project, sceneId, ComponentType::MeshComponent, propertyName, submeshIndex, std::vector<Entity>{entity}));
    return okResult("Unlinked material through the command history.");
}

ActionResult EditorActionExecutor::createMaterialFile(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");
    if (!hasComponent(sceneProject->scene, entity, ComponentType::MeshComponent)) return failResult("Entity has no Mesh component.");

    std::string error;
    fs::path targetRel;
    if (!safeRelativePath(project, arguments, "target_dir", targetRel, error, false)) return failResult(error);
    fs::path targetFull = project->getProjectPath() / targetRel;
    std::error_code ec;
    fs::create_directories(targetFull, ec);
    if (ec) return failResult("Failed to create target directory: " + ec.message());

    const unsigned int submeshIndex = static_cast<unsigned int>(std::max(0, arguments.value("submesh_index", 0)));
    const std::string propertyName = "submeshes[" + std::to_string(submeshIndex) + "].material";
    PropertyData prop = Catalog::getProperty(sceneProject->scene, entity, ComponentType::MeshComponent, propertyName);
    if (!prop.ref) return failResult("Submesh material property not found.");

    Material material = *static_cast<Material*>(prop.ref);
    std::string payload = YAML::Dump(Stream::encodeMaterial(material));
    MaterialPayload sourceMaterial{0x4D54524C, sceneId, entity, submeshIndex};
    project->getProjectCommandHistory()->addCommandNoMerge(
        new CreateMaterialFileCmd(project, targetFull, payload.data(), payload.size(), &sourceMaterial));
    if (resourcesWindow) resourcesWindow->refreshCurrentDirectory();
    return okResult("Created material file through the project command history.");
}

ActionResult EditorActionExecutor::setTerrainTextures(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveTerrainEntity(project, sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Terrain entity not found.");

    struct TextureField { const char* arg; const char* property; bool height; };
    static const TextureField fields[] = {
        {"heightmap_path", "heightMap", true},
        {"blendmap_path", "blendMap", false},
        {"detail_red_path", "textureDetailRed", false},
        {"detail_green_path", "textureDetailGreen", false},
        {"detail_blue_path", "textureDetailBlue", false}
    };

    auto* multiCmd = new MultiPropertyCmd();
    int changed = 0;
    for (const TextureField& field : fields) {
        if (!arguments.contains(field.arg) || !arguments[field.arg].is_string()) continue;
        fs::path rel = arguments[field.arg].get<std::string>();
        if (!PathUtils::isSafeRelativePath(rel) || !fs::exists(project->getProjectPath() / rel)) {
            delete multiCmd;
            return failResult(std::string(field.arg) + " must be an existing safe project-relative path.");
        }
        auto onChanged = [project = this->project, sceneId, entity, height = field.height]() {
            SceneProject* sp = project ? project->getScene(sceneId) : nullptr;
            TerrainComponent* terrain = sp && sp->scene ? sp->scene->findComponent<TerrainComponent>(entity) : nullptr;
            if (!terrain) return;
            if (height) terrain->heightMapLoaded = false;
            terrain->needUpdateTerrain = height;
            terrain->needUpdateTexture = true;
        };
        multiCmd->addPropertyCmd<Texture>(project, sceneId, entity, ComponentType::TerrainComponent,
                                          field.property, Texture(rel.generic_string()), onChanged);
        changed++;
    }

    if (changed == 0) {
        delete multiCmd;
        return failResult("Provide at least one terrain texture path.");
    }
    CommandHandle::get(sceneId)->addCommandNoMerge(multiCmd);
    return okResult("Set terrain texture paths through the command history.");
}

ActionResult EditorActionExecutor::createTerrainBlendmap(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveTerrainEntity(project, sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Terrain entity not found.");

    int resolution = arguments.value("resolution", project->getTerrainEditorSettings().blendMapResolution);
    resolution = std::max(2, std::min(2048, resolution));
    TerrainBlendmapRequest request;
    request.resolution = resolution;
    request.red = arguments.value("red", 0.0f);
    request.green = arguments.value("green", 0.0f);
    request.blue = arguments.value("blue", 0.0f);

    TerrainBlendmapResult generated = AiTerrainCapability::createBlendmap(project, sceneId, entity, request);
    if (!generated.success) {
        return failResult(generated.error.empty() ? "Failed to create terrain blendmap." : generated.error);
    }

    auto onChanged = [project = this->project, sceneId, entity]() {
        SceneProject* sp = project ? project->getScene(sceneId) : nullptr;
        TerrainComponent* terrain = sp && sp->scene ? sp->scene->findComponent<TerrainComponent>(entity) : nullptr;
        if (!terrain) return;
        terrain->needUpdateTexture = true;
    };
    CommandHandle::get(sceneId)->addCommandNoMerge(
        new PropertyCmd<Texture>(project, sceneId, entity, ComponentType::TerrainComponent,
                                 "blendMap", Texture(generated.relativePath), onChanged));
    if (resourcesWindow) resourcesWindow->requestThumbnailGeneration(generated.fullPath, true);
    return okResult("Created terrain blendmap through the command history.",
                    Json{{"path", generated.relativePath}, {"resolution", resolution}});
}

ActionResult EditorActionExecutor::addTerrainCollision(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveTerrainEntity(project, sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Terrain entity not found.");

    Body3DComponent* body = sceneProject->scene->findComponent<Body3DComponent>(entity);
    const size_t shapeIdx = body ? body->numShapes : 0;

    Shape3D shape;
    shape.type = Shape3DType::HEIGHTFIELD;
    shape.source = Shape3DSource::ENTITY_HEIGHTFIELD;
    shape.sourceEntity = entity;
    shape.samplesSize = 256;

    auto* multiCmd = new MultiPropertyCmd();
    if (!body) {
        multiCmd->addCommand(std::make_unique<AddComponentCmd>(project, sceneId, entity, ComponentType::Body3DComponent));
    }
    multiCmd->addPropertyCmd<Shape3D>(project, sceneId, entity, ComponentType::Body3DComponent,
                                      "shapes[" + std::to_string(shapeIdx) + "]", shape);
    multiCmd->addPropertyCmd<size_t>(project, sceneId, entity, ComponentType::Body3DComponent,
                                     "numShapes", shapeIdx + 1);
    CommandHandle::get(sceneId)->addCommandNoMerge(multiCmd);
    return okResult("Added terrain heightfield collision through the command history.");
}

ActionResult EditorActionExecutor::createScript(const Json& arguments) {
    ScriptType type;
    if (!parseScriptType(arguments.value("type", ""), type)) {
        return failResult("Unsupported script type. Use lua, cpp_subclass, or cpp_script_class.");
    }

    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");

    const std::string className = sanitizeIdentifier(arguments.value("class_name", "NewScript"), "NewScript");
    fs::path targetRel = arguments.value("target_dir", std::string("scripts"));
    if (!PathUtils::isSafeRelativePath(targetRel)) return failResult("target_dir must be a safe project-relative directory.");
    fs::path targetFull = project->getProjectPath() / targetRel;
    std::error_code ec;
    fs::create_directories(targetFull, ec);
    if (ec) return failResult("Failed to create script directory: " + ec.message());

    fs::path sourceFull = PathUtils::uniqueChildPath(targetFull, className, scriptTypeExtension(type, false));
    fs::path headerFull;
    if (type != ScriptType::SCRIPT_LUA) {
        headerFull = PathUtils::uniqueChildPath(targetFull, className, scriptTypeExtension(type, true));
    }

    if (type == ScriptType::SCRIPT_LUA) {
        std::ofstream f(sourceFull, std::ios::trunc);
        if (!f) return failResult("Failed to create Lua script file.");
        f << "-- " << className << ".lua\n\n";
        f << "local " << className << " = {\n";
        f << "    properties = {\n";
        f << "        { name = \"speed\", displayName = \"Speed\", type = \"float\", default = 5.0 },\n";
        f << "        { name = \"isActive\", displayName = \"Is Active\", type = \"bool\", default = true }\n";
        f << "    }\n";
        f << "}\n\n";
        f << "function " << className << ":init()\n";
        f << "    RegisterEngineEvent(self, \"onUpdate\")\n";
        f << "end\n\n";
        f << "function " << className << ":onUpdate()\n";
        f << "    if not self.isActive then return end\n";
        f << "end\n\n";
        f << "return " << className << "\n";
    } else {
        std::string parentClass = "EntityHandle";
        if (type == ScriptType::SUBCLASS) {
            if (hasComponent(sceneProject->scene, entity, ComponentType::CameraComponent)) parentClass = "Camera";
            else if (hasComponent(sceneProject->scene, entity, ComponentType::MeshComponent) ||
                     hasComponent(sceneProject->scene, entity, ComponentType::ModelComponent)) parentClass = "Mesh";
            else if (hasComponent(sceneProject->scene, entity, ComponentType::LightComponent)) parentClass = "Light";
            else if (hasComponent(sceneProject->scene, entity, ComponentType::Transform)) parentClass = "Object";
        } else {
            parentClass = "ScriptBase";
        }

        std::ofstream h(headerFull, std::ios::trunc);
        std::ofstream c(sourceFull, std::ios::trunc);
        if (!h || !c) return failResult("Failed to create C++ script files.");
        h << "#pragma once\n\n";
        h << "#include \"" << parentClass << ".h\"\n";
        h << "#include \"Engine.h\"\n";
        h << "#include \"ScriptProperty.h\"\n\n";
        h << "class " << className << " : public doriax::" << parentClass << " {\n";
        h << "public:\n";
        h << "    DPROPERTY(\"Is Active\")\n";
        h << "    bool isActive = true;\n\n";
        h << "    " << className << "(doriax::Scene* scene, doriax::Entity entity);\n";
        h << "    ~" << className << "();\n\n";
        h << "    void onUpdate();\n";
        h << "};\n";

        c << "#include \"" << headerFull.filename().string() << "\"\n\n";
        c << "using namespace doriax;\n\n";
        c << className << "::" << className << "(Scene* scene, Entity entity): " << parentClass << "(scene, entity) {\n";
        c << "    REGISTER_ENGINE_EVENT(onUpdate);\n";
        c << "}\n\n";
        c << className << "::~" << className << "() {\n";
        c << "}\n\n";
        c << "void " << className << "::onUpdate() {\n";
        c << "    if (!isActive) return;\n";
        c << "}\n";
    }

    Json attachArgs = arguments;
    attachArgs["path"] = fs::relative(sourceFull, project->getProjectPath(), ec).generic_string();
    attachArgs["class_name"] = className;
    if (!headerFull.empty()) {
        attachArgs["header_path"] = fs::relative(headerFull, project->getProjectPath(), ec).generic_string();
    }
    ActionResult attached = attachScript(attachArgs);
    if (!attached.success) return attached;
    if (resourcesWindow) resourcesWindow->refreshCurrentDirectory();
    return okResult("Created and attached script.",
                    Json{{"path", attachArgs["path"]},
                         {"header_path", attachArgs.value("header_path", "")}});
}

ActionResult EditorActionExecutor::attachScript(const Json& arguments) {
    ScriptType type;
    if (!parseScriptType(arguments.value("type", ""), type)) {
        return failResult("Unsupported script type. Use lua, cpp_subclass, or cpp_script_class.");
    }

    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");

    std::string error;
    fs::path sourceRel;
    if (!safeRelativePath(project, arguments, "path", sourceRel, error, true)) return failResult(error);
    fs::path headerRel;
    if (type != ScriptType::SCRIPT_LUA) {
        if (!safeRelativePath(project, arguments, "header_path", headerRel, error, true)) return failResult(error);
    }

    ScriptEntry entry;
    entry.type = type;
    entry.enabled = true;
    entry.path = sourceRel.generic_string();
    entry.headerPath = headerRel.empty() ? "" : headerRel.generic_string();
    entry.className = sanitizeIdentifier(arguments.value("class_name", "NewScript"), "NewScript");

    const bool hasScript = hasComponent(sceneProject->scene, entity, ComponentType::ScriptComponent);
    std::vector<ScriptEntry> scripts;
    if (hasScript) {
        scripts = sceneProject->scene->getComponent<ScriptComponent>(entity).scripts;
    }
    scripts.push_back(entry);
    project->updateScriptProperties(sceneProject, entity, scripts);

    auto* multiCmd = new MultiPropertyCmd();
    if (!hasScript) {
        multiCmd->addCommand(std::make_unique<AddComponentCmd>(project, sceneId, entity, ComponentType::ScriptComponent));
    }
    multiCmd->addCommand(std::make_unique<PropertyCmd<std::vector<ScriptEntry>>>(
        project, sceneId, entity, ComponentType::ScriptComponent, "scripts", scripts));
    CommandHandle::get(sceneId)->addCommandNoMerge(multiCmd);
    return okResult("Attached script through the command history.");
}

ActionResult EditorActionExecutor::createBundleFromEntity(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");

    std::string error;
    fs::path bundleRel;
    if (!safeRelativePath(project, arguments, "bundle_path", bundleRel, error, false)) return failResult(error);
    if (!Util::isBundleFile(bundleRel.string())) return failResult("bundle_path must end with .bundle.");
    if (fs::exists(project->getProjectPath() / bundleRel)) return failResult("Bundle file already exists.");

    YAML::Node node = Stream::encodeEntitySelection(std::vector<Entity>{entity}, sceneProject->scene, project, sceneProject);
    if (!node || !node.IsMap()) return failResult("Failed to encode entity selection.");
    fs::create_directories((project->getProjectPath() / bundleRel).parent_path());

    CommandHandle::get(sceneId)->addCommandNoMerge(new CreateEntityBundleCmd(project, sceneId, bundleRel, node));
    if (resourcesWindow) resourcesWindow->refreshCurrentDirectory();
    return okResult("Created bundle from entity through the command history.");
}

ActionResult EditorActionExecutor::importBundleInstance(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");

    std::string error;
    fs::path bundleRel;
    if (!safeRelativePath(project, arguments, "bundle_path", bundleRel, error, true)) return failResult(error);
    if (!Util::isBundleFile(bundleRel.string())) return failResult("bundle_path must point to a .bundle file.");

    Entity parent = NULL_ENTITY;
    if (arguments.contains("parent_id")) {
        parent = resolveEntityByKeys(sceneProject, arguments, "parent_id", "parent_name");
        if (parent == NULL_ENTITY) return failResult("Parent entity not found.");
    }

    auto* cmd = new ImportEntityBundleCmd(project, sceneId, bundleRel, parent, true);
    CommandHandle::get(sceneId)->addCommandNoMerge(cmd);
    Json imported = Json::array();
    for (Entity importedEntity : cmd->getImportedEntities()) imported.push_back(importedEntity);
    return okResult("Imported bundle instance through the command history.", Json{{"imported_entities", imported}});
}

ActionResult EditorActionExecutor::addEntityToBundle(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");
    Entity bundleRoot = resolveEntityByKeys(sceneProject, arguments, "bundle_root_id", "bundle_root_name");
    if (bundleRoot == NULL_ENTITY) return failResult("Bundle root entity not found.");

    CommandHandle::get(sceneId)->addCommandNoMerge(new AddEntityToBundleCmd(project, sceneId, entity, bundleRoot));
    return okResult("Added entity to bundle through the command history.");
}

ActionResult EditorActionExecutor::removeEntityFromBundle(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");
    if (!project->isEntityInBundle(sceneId, entity)) return failResult("Entity is not part of a bundle.");

    Transform* transform = sceneProject->scene->findComponent<Transform>(entity);
    Entity parent = transform ? transform->parent : NULL_ENTITY;
    CommandHandle::get(sceneId)->addCommandNoMerge(new RemoveEntityFromBundleCmd(project, sceneId, entity, parent));
    return okResult("Removed entity from bundle through the command history.");
}

ActionResult EditorActionExecutor::makeBundleComponentUnique(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");
    ComponentType component;
    if (!parseComponentType(arguments.value("component", ""), component)) return failResult("Unknown component type.");

    CommandHandle::get(sceneId)->addCommandNoMerge(new ComponentToBundleLocalCmd(project, sceneId, entity, component));
    return okResult("Made bundle component unique through the command history.");
}

ActionResult EditorActionExecutor::revertBundleComponent(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");
    ComponentType component;
    if (!parseComponentType(arguments.value("component", ""), component)) return failResult("Unknown component type.");

    CommandHandle::get(sceneId)->addCommandNoMerge(new ComponentToBundleSharedCmd(project, sceneId, entity, component));
    return okResult("Reverted bundle component through the command history.");
}

ActionResult EditorActionExecutor::exportProject(const Json& arguments, const std::atomic<bool>* cancel) {
    if (cancel && cancel->load()) return failResult("Export cancelled.");
    ExportConfig config;
    config.targetDir = arguments.value("target_dir", "");
    config.assetsDir = arguments.value("assets_dir", project->getAssetsDir().generic_string());
    config.luaDir = arguments.value("lua_dir", project->getLuaDir().generic_string());
    config.startSceneId = static_cast<uint32_t>(arguments.value("start_scene_id", static_cast<int>(project->getStartSceneId())));
    std::string error;
    config.selectedPlatforms = parsePlatformList(arguments.value("platforms", "all"), error);
    if (!error.empty()) return failResult(error);

    Exporter exporter;
    const bool success = exporter.exportProject(project, config);
    ExportProgress progress = exporter.getProgress();
    if (!success) {
        return failResult(progress.errorMessage.empty() ? "Export failed." : progress.errorMessage);
    }
    return okResult("Exported project.", Json{{"target_dir", config.targetDir.generic_string()}});
}

ActionResult EditorActionExecutor::generateShaders(const Json& arguments, const std::atomic<bool>* cancel) {
    if (cancel && cancel->load()) return failResult("Shader generation cancelled.");
    ExportConfig config;
    config.targetDir = arguments.value("target_dir", "");
    config.shaderOutputDir = config.targetDir;
    std::string error;
    config.selectedPlatforms = parsePlatformList(arguments.value("platforms", "all"), error);
    if (!error.empty()) return failResult(error);
    if (!parseShaderOutputFormat(arguments.value("format", "header"), config.shaderOutputFormat)) {
        return failResult("Unsupported shader output format. Use binary, header, or json.");
    }
    for (const SceneProject& sceneProject : project->getScenes()) {
        config.selectedShaderKeys.insert(sceneProject.shaderKeys.begin(), sceneProject.shaderKeys.end());
    }
    if (config.selectedShaderKeys.empty()) {
        return failResult("No scene shader keys are available. Save or render scenes before generating shaders.");
    }

    Exporter exporter;
    const bool success = exporter.generateShaders(config);
    ExportProgress progress = exporter.getProgress();
    if (!success) {
        return failResult(progress.errorMessage.empty() ? "Shader generation failed." : progress.errorMessage);
    }
    return okResult("Generated shaders.", Json{{"target_dir", config.targetDir.generic_string()}});
}


ActionResult EditorActionExecutor::createTerrainHeightmap(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return failResult("Scene not found.");
    }
    if (sceneProject->sceneType != SceneType::SCENE_3D) {
        return failResult("Terrain heightmaps are only supported in 3D scenes.");
    }

    Entity entity = resolveTerrainEntity(project, sceneProject, arguments);
    if (entity == NULL_ENTITY) {
        return failResult("Terrain entity not found. Provide entity_id/entity_name or select a terrain.");
    }

    TerrainComponent* terrain = sceneProject->scene->findComponent<TerrainComponent>(entity);
    if (!terrain) {
        return failResult("Entity is not a terrain.");
    }

    int resolution = arguments.value("resolution", project->getTerrainEditorSettings().heightMapResolution);
    resolution = std::max(2, std::min(2048, resolution));

    std::string mode = arguments.value("mode", "middle");
    if (mode.empty()) mode = "middle";
    float baseHeight = arguments.value("base_height", mode == "flat" ? 0.0f : 0.5f);
    if (mode == "middle") {
        baseHeight = 0.5f;
    }
    float amplitude = arguments.value("amplitude", 0.35f);
    float frequency = arguments.value("frequency", 4.0f);
    int octaves = arguments.value("octaves", 4);

    uint32_t seed = 0;
    if (arguments.contains("seed") && arguments["seed"].is_number_integer()) {
        seed = static_cast<uint32_t>(arguments["seed"].get<int64_t>());
    } else if (arguments.contains("seed") && arguments["seed"].is_number_unsigned()) {
        seed = arguments["seed"].get<uint32_t>();
    } else {
        seed = std::random_device{}();
    }

    TerrainHeightmapRequest request;
    request.resolution = resolution;
    request.mode = mode;
    request.seed = seed;
    request.baseHeight = baseHeight;
    request.amplitude = amplitude;
    request.frequency = frequency;
    request.octaves = octaves;

    TerrainHeightmapResult generated = AiTerrainCapability::createHeightmap(project, sceneId, entity, request);
    if (!generated.success) {
        return failResult(generated.error.empty() ? "Failed to write generated terrain heightmap."
                                                  : generated.error);
    }
    Texture heightTexture = generated.texture;
    const std::string relativePath = generated.relativePath;

    auto onChanged = [project = this->project, sceneId, entity]() {
        SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
        if (!sceneProject || !sceneProject->scene) return;
        TerrainComponent* terrain = sceneProject->scene->findComponent<TerrainComponent>(entity);
        if (!terrain) return;
        terrain->heightMapLoaded = false;
        terrain->needUpdateTerrain = true;
        terrain->needUpdateTexture = true;
        terrain->heightMap.invalidateRender();
    };

    CommandHandle::get(sceneId)->addCommandNoMerge(
        new PropertyCmd<Texture>(project, sceneId, entity, ComponentType::TerrainComponent,
                                 "heightMap", heightTexture, onChanged));

    return okResult("Created terrain heightmap through the command history.",
                    Json{{"scene_id", sceneId},
                         {"entity_id", entity},
                         {"entity_name", sceneProject->scene->getEntityName(entity)},
                         {"path", relativePath},
                         {"resolution", resolution},
                         {"mode", mode},
                         {"seed", seed}});
}

ActionResult EditorActionExecutor::importProjectModel(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return failResult("Scene not found.");
    }
    if (sceneProject->sceneType != SceneType::SCENE_3D) {
        return failResult("Model import is only supported in 3D scenes.");
    }

    fs::path relPath(arguments.value("model_path", ""));
    if (!PathUtils::isSafeRelativePath(relPath) || !Util::isModelFile(relPath.string())) {
        return failResult("model_path must be a safe project-relative .gltf, .glb, or .obj file.");
    }
    fs::path fullPath = project->getProjectPath() / relPath;
    if (!fs::exists(fullPath)) {
        return failResult("Model file does not exist in the project.");
    }

    Vector3 position = Vector3::ZERO;
    if (arguments.contains("position")) parseVector3(arguments["position"], position);
    std::string entityName = arguments.value("entity_name", relPath.stem().string());
    if (entityName.empty()) entityName = "Model";

    CommandHandle::get(sceneId)->addCommandNoMerge(
        new ModelLoadCmd(project, sceneId, entityName, position, relPath.generic_string()));

    if (resourcesWindow) {
        resourcesWindow->requestThumbnailGeneration(fullPath, false);
    }

    return okResult("Imported project model through ModelLoadCmd.");
}

ActionResult EditorActionExecutor::searchCuratedAssets(const Json& arguments, const std::atomic<bool>* cancel) {
    if (!httpClient) {
        return failResult("HTTP client is unavailable.");
    }

    const std::string provider = lower(arguments.value("provider", ""));
    const std::string query = lower(arguments.value("query", ""));
    int maxResults = std::max(1, std::min(12, arguments.value("max_results", 6)));
    Json results = Json::array();

    if (provider == "polyhaven") {
        HttpResponse response = httpClient->get("https://api.polyhaven.com/assets?t=models",
            {"Accept: application/json", "User-Agent: DoriaxEditorAI/1.0"}, cancel);
        if (!response.error.empty()) return failResult(response.error);
        if (response.status < 200 || response.status >= 300) {
            return failResult("Poly Haven returned HTTP " + std::to_string(response.status));
        }
        Json root = Json::parse(response.body, nullptr, false);
        if (!root.is_object()) return failResult("Unexpected Poly Haven response.");
        for (auto it = root.begin(); it != root.end() && static_cast<int>(results.size()) < maxResults; ++it) {
            const std::string id = it.key();
            const Json& meta = it.value();
            std::string name = meta.value("name", id);
            std::string haystack = lower(id + " " + name + " " + meta.value("categories", Json::array()).dump() + " " + meta.value("tags", Json::array()).dump());
            if (haystack.find(query) == std::string::npos) continue;
            results.push_back({
                {"provider", "polyhaven"},
                {"asset_id", id},
                {"title", name},
                {"license", "CC0 / Poly Haven license terms"},
                {"source_url", "https://polyhaven.com/a/" + id},
                {"download_note", "Run files endpoint manually or provide a reviewed direct .glb/.gltf/.obj URL for download_curated_asset."}
            });
        }
        return okResult("Searched Poly Haven. API commercial usage may require custom licensing.", Json{{"results", results}});
    }

    if (provider == "sketchfab") {
        std::string url = "https://api.sketchfab.com/v3/search?type=models&downloadable=true&q=" +
                          HttpClient::urlEncode(query);
        HttpResponse response = httpClient->get(url, {"Accept: application/json"}, cancel);
        if (!response.error.empty()) return failResult(response.error);
        if (response.status < 200 || response.status >= 300) {
            return failResult("Sketchfab returned HTTP " + std::to_string(response.status) +
                              ". Search may require user authentication.");
        }
        Json root = Json::parse(response.body, nullptr, false);
        if (!root.is_object()) return failResult("Unexpected Sketchfab response.");
        const Json& items = root.value("results", Json::array());
        if (items.is_array()) {
            for (const auto& item : items) {
                if (static_cast<int>(results.size()) >= maxResults) break;
                results.push_back({
                    {"provider", "sketchfab"},
                    {"asset_id", item.value("uid", "")},
                    {"title", item.value("name", "")},
                    {"author", item.value("user", Json::object()).value("displayName", "")},
                    {"license", item.value("license", Json::object()).value("label", "")},
                    {"source_url", item.value("viewerUrl", "")},
                    {"download_note", "Sketchfab Download API requires end-user OAuth before download URLs can be used."}
                });
            }
        }
        return okResult("Searched Sketchfab metadata. Downloads require Sketchfab user OAuth.", Json{{"results", results}});
    }

    return failResult("Unsupported curated provider: " + provider);
}

ActionResult EditorActionExecutor::downloadCuratedAsset(const Json& arguments, const std::atomic<bool>* cancel) {
    if (!httpClient) {
        return failResult("HTTP client is unavailable.");
    }

    const std::string url = arguments.value("download_url", "");
    std::string title = arguments.value("title", "Asset");
    std::string slug = arguments.value("slug", "");
    if (slug.empty()) slug = PathUtils::slugify(title);
    slug = PathUtils::slugify(slug);

    std::string filename = filenameFromUrl(url, slug + ".glb");
    fs::path filenamePath(filename);
    if (!Util::isModelFile(filenamePath.string())) {
        return failResult("Only direct .gltf, .glb, and .obj downloads are supported in v1. Archive extraction is intentionally not enabled yet.");
    }

    fs::path stagingDir = project->getProjectInternalPath() / "ai" / "staging" / slug;
    fs::path importDir = project->getProjectPath() / project->getAssetsDir() / "ai_imports" / slug;
    fs::path stagingFile = stagingDir / filenamePath.filename();
    fs::path importedFile = PathUtils::uniqueChildPath(importDir, filenamePath.stem().string(), filenamePath.extension().string());

    HttpResponse response = httpClient->downloadToFile(url, stagingFile, {"Accept: */*"}, cancel);
    if (!response.error.empty()) return failResult(response.error);
    if (response.status < 200 || response.status >= 300) {
        return failResult("Download returned HTTP " + std::to_string(response.status));
    }

    std::error_code ec;
    fs::create_directories(importDir, ec);
    if (ec) return failResult("Failed to create import directory: " + ec.message());
    fs::copy_file(stagingFile, importedFile, fs::copy_options::overwrite_existing, ec);
    if (ec) return failResult("Failed to copy downloaded asset into project: " + ec.message());

    fs::path attributionPath = project->getProjectInternalPath() / "ai" / "asset_attributions.yaml";
    YAML::Node root;
    if (fs::exists(attributionPath)) {
        try {
            root = YAML::LoadFile(attributionPath.string());
        } catch (...) {
            root = YAML::Node(YAML::NodeType::Map);
        }
    }
    if (!root["assets"]) root["assets"] = YAML::Node(YAML::NodeType::Sequence);
    YAML::Node entry;
    entry["provider"] = arguments.value("provider", "");
    entry["asset_id"] = arguments.value("asset_id", "");
    entry["title"] = title;
    entry["author"] = arguments.value("author", "");
    entry["license"] = arguments.value("license", "");
    entry["source_url"] = arguments.value("source_url", "");
    entry["local_path"] = fs::relative(importedFile, project->getProjectPath()).generic_string();
    root["assets"].push_back(entry);
    fs::create_directories(attributionPath.parent_path(), ec);
    std::ofstream out(attributionPath);
    out << YAML::Dump(root);

    if (resourcesWindow) {
        resourcesWindow->refreshCurrentDirectory();
        resourcesWindow->requestThumbnailGeneration(importedFile, true);
    }

    Json data = {
        {"imported_path", fs::relative(importedFile, project->getProjectPath()).generic_string()},
        {"attribution_path", fs::relative(attributionPath, project->getProjectPath()).generic_string()}
    };

    if (arguments.contains("scene_id") || arguments.contains("entity_name") || arguments.contains("position")) {
        Json importArgs = {
            {"scene_id", resolveSceneId(project, arguments)},
            {"model_path", data["imported_path"].get<std::string>()},
            {"entity_name", arguments.value("entity_name", title)}
        };
        if (arguments.contains("position")) {
            importArgs["position"] = arguments["position"];
        }
        ActionResult importResult = importProjectModel(importArgs);
        data["import_result"] = importResult.message;
        if (!importResult.success) {
            return okResult("Downloaded asset and recorded attribution, but import was skipped: " + importResult.message, data);
        }
    }

    Out::success("AI imported curated asset '%s'", title.c_str());
    return okResult("Downloaded curated asset and recorded attribution.", data);
}

} // namespace doriax::editor::ai
