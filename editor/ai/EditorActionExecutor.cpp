#include "EditorActionExecutor.h"

#include "AiPathUtils.h"
#include "AiEngineApiContext.h"
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
#include "command/CommandHistory.h"
#include "component/CameraComponent.h"
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
#include "command/type/ForkShaderCmd.h"
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
#include "command/type/SetMainCameraCmd.h"
#include "command/type/MeshChangeCmd.h"
#include "command/type/UnlinkMaterialCmd.h"
#include "component/AnimationComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/TilemapComponent.h"
#include "subsystem/MeshSystem.h"
#include "util/ShapeParameters.h"
#include "texture/Texture.h"
#include "util/Util.h"
#include "window/ResourcesWindow.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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

// Maps a renderable component to its forkable built-in shader type.
bool shaderTypeForComponent(ComponentType component, ShaderType& out) {
    switch (component) {
        case ComponentType::MeshComponent:   out = ShaderType::MESH;   return true;
        case ComponentType::UIComponent:     out = ShaderType::UI;     return true;
        case ComponentType::PointsComponent: out = ShaderType::POINTS; return true;
        case ComponentType::LinesComponent:  out = ShaderType::LINES;  return true;
        case ComponentType::SkyComponent:    out = ShaderType::SKYBOX; return true;
        default: return false;
    }
}

// Picks the entity's first renderable component that supports custom shaders.
bool detectForkableComponent(EntityRegistry* registry, Entity entity, ComponentType& component, ShaderType& shaderType) {
    for (ComponentType candidate : {ComponentType::MeshComponent, ComponentType::UIComponent,
                                    ComponentType::PointsComponent, ComponentType::LinesComponent,
                                    ComponentType::SkyComponent}) {
        if (hasComponent(registry, entity, candidate)) {
            component = candidate;
            shaderTypeForComponent(candidate, shaderType);
            return true;
        }
    }
    return false;
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

std::string doriaxLuaScriptGuide(const std::string& className) {
    return "Doriax Lua scripts are returned tables, not Dori.Script objects. "
           "Use: local " + className + " = { properties = {} }; "
           "function " + className + ":init() ... end; return " + className + ". "
           "Editor-exposed properties go in the properties array as { name = \"speed\", displayName = \"Speed\", type = \"float\", default = 5.0 } "
           "(type strings: bool, int, float, string, vector2, vector3, vector4, color3, color4, entity); at runtime read/write them as self.<name>, e.g. self.speed. "
           "The engine injects self.scene and self.entity; self.entity is a numeric Entity id, not an object receiver. For an existing cube/shape, "
           "use local shape = Shape(self.scene, self.entity); shape:setColor(1.0, 0.0, 0.0, 1.0). "
           "Per-frame and input logic must be registered in :init() with the global RegisterEngineEvent(self, \"onUpdate\") and a matching function " + className + ":onUpdate() ... end; "
           "valid event names: onUpdate, onFixedUpdate, onDraw, onMouseDown, onMouseUp, onMouseMove, onKeyDown, onKeyUp, onTouchStart, onTouchMove, onTouchEnd. "
           "For component/UI events use the global RegisterEvent(self, eventObject, \"method\"), getting the event object from a wrapper, e.g. local b = Button(self.scene, self.entity); RegisterEvent(self, b:getButtonComponent().onPress, \"onPress\"). "
           "Engine state uses Lua properties, e.g. local dt = Engine.deltatime (not getDeltatime). No manual unregister is needed; the engine cleans up events when the script is destroyed. "
           "To print or log, use Log.print(\"text \" .. tostring(value)) (Log.warn/Log.error for severity), not bare print(). "
           "Use search_engine_api for real signatures before writing engine API code.";
}

std::string doriaxCppScriptGuide(ScriptType type, const std::string& parentClass) {
    std::ostringstream guide;
    guide << "Doriax C++ scripts compile against flat engine API headers copied to .doriax/engine-api. "
          << "Use quoted includes such as \"Mesh.h\", \"ScriptBase.h\", \"Engine.h\", and \"ScriptProperty.h\". "
          << "Never use #include <core/...> or #include \"core/...\". ";
    if (type == ScriptType::SCRIPT_CLASS) {
        guide << "cpp_script_class inherits doriax::ScriptBase for general entity logic. "
              << "Register frame callbacks with REGISTER_ENGINE_EVENT(onUpdate) in the constructor, "
              << "unregister with UNREGISTER_ENGINE_EVENT(onUpdate) in the destructor, and implement void onUpdate(). "
              << "ScriptBase exposes getScene()/getEntity(); construct a runtime wrapper such as Object(getScene(), getEntity()) to control the entity.";
    } else {
        guide << "cpp_subclass inherits doriax::" << parentClass
              << " and calls engine methods (e.g. setColor) directly on this. "
              << "Register frame callbacks with REGISTER_ENGINE_EVENT(onUpdate) in the constructor and unregister them in the destructor when using onUpdate(). "
              << "For mesh/cube color on a Mesh entity, prefer cpp_subclass extending Mesh (or Shape), not ScriptBase.";
    }
    guide << " To print or log, include \"Log.h\" and call Log::print(\"pos %f %f %f\", p.x, p.y, p.z) (Log::warn/Log::error for severity); Engine has no log method, and do not use printf or std::cout. "
          << "For component/UI events (button press, click, scrollbar change, etc.) subscribe in the constructor with REGISTER_COMPONENT_EVENT(Component, event, method) or its shortcuts REGISTER_UI_EVENT/REGISTER_BUTTON_EVENT/REGISTER_SCROLLBAR_EVENT/REGISTER_PANEL_EVENT, and pair each with its UNREGISTER_* in the destructor; search_engine_api for the exact macro and the component's event names. "
          << "Call search_engine_api for real signatures before writing engine API code. "
          << "DPROPERTY(\"Name\") exposes a public member as an editor property (optional; removing it clears stale editor properties); DPROPERTY(\"Name\", Type) forces the editor type such as Color3/Color4.";
    return guide.str();
}

std::string validateDoriaxLuaScriptContent(const std::string& content) {
    const std::string contentLower = lower(content);

    struct ForbiddenLuaPattern {
        const char* token;
        const char* message;
    };
    static const ForbiddenLuaPattern patterns[] = {
        {"dori.", "Doriax Lua scripts do not use a Dori namespace."},
        {"script:new", "Doriax Lua scripts do not derive from Script:new(). Return a plain module table instead."},
        {"on_start", "Doriax Lua scripts use init() for startup, not on_start()."},
        {"on_scene_start", "Doriax Lua scripts use init() for startup, not on_scene_start()."},
        {"self.entity:", "self.entity is a numeric Entity id, not an object receiver. Wrap it with a runtime class such as Shape(self.scene, self.entity)."},
        {"self.entity.", "self.entity is a numeric Entity id, not an object. Pass it to runtime wrappers; do not access methods/properties on it."},
        {"get_entity", "Doriax Lua scripts receive self.entity directly; there is no get_entity() helper."},
        {"getentity", "Doriax Lua scripts receive self.entity directly; there is no getEntity() helper."},
        {"get_component", "Doriax Lua scripts should use runtime wrappers returned by search_engine_api, not get_component()."},
        {"getcomponent", "Doriax Lua scripts should use runtime wrappers returned by search_engine_api, not getComponent()."},
        {"set_property", "Doriax Lua scripts should call runtime APIs, not editor-style set_property()."},
        {"setproperty", "Doriax Lua scripts should call runtime APIs, not editor-style setProperty()."},
        {"vector4.new", "Doriax Lua constructors use Vector4(...), not Vector4.new(...)."},
        {"engine.log", "Engine has no log method. To print, use Log.print(\"text \" .. tostring(value)) (Log.warn/Log.error for severity)."},
        {"engine:log", "Engine has no log method. To print, use Log.print(\"text \" .. tostring(value)) (Log.warn/Log.error for severity)."}
    };

    for (const auto& pattern : patterns) {
        if (contentLower.find(pattern.token) != std::string::npos) {
            return std::string(pattern.message) + " Call search_engine_api and rewrite using the Doriax Lua table pattern.";
        }
    }

    if (contentLower.find("return ") == std::string::npos) {
        return "Doriax Lua modules must return their script table.";
    }

    return {};
}

std::string validateDoriaxCppScriptContent(const std::string& content) {
    const std::string contentLower = lower(content);

    struct ForbiddenCppPattern {
        const char* token;
        const char* message;
    };
    static const ForbiddenCppPattern patterns[] = {
        {"<core/", "C++ scripts must use flat quoted headers such as \"Mesh.h\", not <core/...>."},
        {"\"core/", "C++ scripts must use flat quoted headers such as \"Mesh.h\", not \"core/...\"."},
        {"get_component<", "Doriax C++ scripts do not use get_component. Use cpp_subclass and inherit the runtime class (e.g. Mesh) instead."},
        {"oninit(", "Doriax C++ scripts use onUpdate() with REGISTER_ENGINE_EVENT, not onInit()."},
        {"void oninit", "Doriax C++ scripts use onUpdate() with REGISTER_ENGINE_EVENT, not onInit()."},
        {"on_start", "Doriax C++ scripts use onUpdate() with REGISTER_ENGINE_EVENT, not on_start()."},
        {"onstart(", "Doriax C++ scripts use onUpdate() with REGISTER_ENGINE_EVENT, not onStart()."},
        {"engine::log", "Engine has no log method. Include \"Log.h\" and use Log::print(\"value %f\", x) (Log::warn/Log::error for severity)."}
    };

    for (const auto& pattern : patterns) {
        if (contentLower.find(pattern.token) != std::string::npos) {
            return std::string(pattern.message) + " Call search_engine_api and rewrite using the Doriax C++ script pattern.";
        }
    }

    return {};
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

bool parseGeometryType(const std::string& name, int& out) {
    static const std::unordered_map<std::string, int> map = {
        {"plane", 0}, {"box", 1}, {"sphere", 2}, {"cylinder", 3},
        {"capsule", 4}, {"torus", 5}, {"wall", 6}
    };
    auto it = map.find(lower(name));
    if (it == map.end()) return false;
    out = it->second;
    return true;
}

bool parseGeometryTypeValue(const Json& args, ShapeParameters& shape, std::string& error) {
    if (!args.contains("geometry_type")) return true;

    int geometryType = shape.geometryType;
    if (args["geometry_type"].is_string()) {
        if (!parseGeometryType(args["geometry_type"].get<std::string>(), geometryType)) {
            error = "Unsupported geometry_type. Use plane, box, sphere, cylinder, capsule, torus, or wall.";
            return false;
        }
    } else if (args["geometry_type"].is_number_integer()) {
        geometryType = args["geometry_type"].get<int>();
        if (geometryType < 0 || geometryType > 6) {
            error = "geometry_type integer must be between 0 and 6.";
            return false;
        }
    } else {
        error = "geometry_type must be a string or integer.";
        return false;
    }

    shape.geometryType = geometryType;
    return true;
}

float clampedFloatArg(const Json& args, const char* key, float current, float minValue, float maxValue) {
    if (!args.contains(key) || !args[key].is_number()) return current;
    float value = args[key].get<float>();
    if (!std::isfinite(value)) return current;
    return std::clamp(value, minValue, maxValue);
}

unsigned int clampedUIntArg(const Json& args, const char* key, unsigned int current,
                            unsigned int minValue, unsigned int maxValue) {
    if (!args.contains(key) || !args[key].is_number_integer()) return current;
    int64_t value = args[key].get<int64_t>();
    if (value < static_cast<int64_t>(minValue)) return minValue;
    if (value > static_cast<int64_t>(maxValue)) return maxValue;
    return static_cast<unsigned int>(value);
}

void applyShapeParameters(const Json& args, ShapeParameters& shape) {
    constexpr float kMinSize = 0.1f;
    constexpr float kMaxSize = 100.0f;
    constexpr unsigned int kMaxSegments = 100;

    shape.planeWidth = clampedFloatArg(args, "plane_width", shape.planeWidth, kMinSize, kMaxSize);
    shape.planeDepth = clampedFloatArg(args, "plane_depth", shape.planeDepth, kMinSize, kMaxSize);
    shape.planeTiles = clampedUIntArg(args, "plane_tiles", shape.planeTiles, 1, kMaxSegments);
    shape.wallWidth = clampedFloatArg(args, "wall_width", shape.wallWidth, kMinSize, kMaxSize);
    shape.wallHeight = clampedFloatArg(args, "wall_height", shape.wallHeight, kMinSize, kMaxSize);
    shape.wallTiles = clampedUIntArg(args, "wall_tiles", shape.wallTiles, 1, kMaxSegments);
    shape.boxWidth = clampedFloatArg(args, "box_width", shape.boxWidth, kMinSize, kMaxSize);
    shape.boxHeight = clampedFloatArg(args, "box_height", shape.boxHeight, kMinSize, kMaxSize);
    shape.boxDepth = clampedFloatArg(args, "box_depth", shape.boxDepth, kMinSize, kMaxSize);
    shape.boxTiles = clampedUIntArg(args, "box_tiles", shape.boxTiles, 1, kMaxSegments);
    shape.sphereRadius = clampedFloatArg(args, "sphere_radius", shape.sphereRadius, kMinSize, kMaxSize);
    shape.sphereSlices = clampedUIntArg(args, "sphere_slices", shape.sphereSlices, 3, kMaxSegments);
    shape.sphereStacks = clampedUIntArg(args, "sphere_stacks", shape.sphereStacks, 3, kMaxSegments);
    shape.cylinderBaseRadius = clampedFloatArg(args, "cylinder_base_radius", shape.cylinderBaseRadius, kMinSize, kMaxSize);
    shape.cylinderTopRadius = clampedFloatArg(args, "cylinder_top_radius", shape.cylinderTopRadius, kMinSize, kMaxSize);
    shape.cylinderHeight = clampedFloatArg(args, "cylinder_height", shape.cylinderHeight, kMinSize, kMaxSize);
    shape.cylinderSlices = clampedUIntArg(args, "cylinder_slices", shape.cylinderSlices, 3, kMaxSegments);
    shape.cylinderStacks = clampedUIntArg(args, "cylinder_stacks", shape.cylinderStacks, 1, kMaxSegments);
    shape.capsuleBaseRadius = clampedFloatArg(args, "capsule_base_radius", shape.capsuleBaseRadius, kMinSize, kMaxSize);
    shape.capsuleTopRadius = clampedFloatArg(args, "capsule_top_radius", shape.capsuleTopRadius, kMinSize, kMaxSize);
    shape.capsuleHeight = clampedFloatArg(args, "capsule_height", shape.capsuleHeight, kMinSize, kMaxSize);
    shape.capsuleSlices = clampedUIntArg(args, "capsule_slices", shape.capsuleSlices, 3, kMaxSegments);
    shape.capsuleStacks = clampedUIntArg(args, "capsule_stacks", shape.capsuleStacks, 1, kMaxSegments);
    shape.torusRadius = clampedFloatArg(args, "torus_radius", shape.torusRadius, kMinSize, kMaxSize);
    shape.torusRingRadius = clampedFloatArg(args, "torus_ring_radius", shape.torusRingRadius, kMinSize, kMaxSize);
    shape.torusSides = clampedUIntArg(args, "torus_sides", shape.torusSides, 3, kMaxSegments);
    shape.torusRings = clampedUIntArg(args, "torus_rings", shape.torusRings, 3, kMaxSegments);
}

void updateMeshShape(MeshComponent& meshComp, MeshSystem* meshSys, const ShapeParameters& shapeParams) {
    switch (shapeParams.geometryType) {
        case 0:
            meshSys->createPlane(meshComp, shapeParams.planeWidth, shapeParams.planeDepth, shapeParams.planeTiles);
            break;
        case 1:
            meshSys->createBox(meshComp, shapeParams.boxWidth, shapeParams.boxHeight, shapeParams.boxDepth, shapeParams.boxTiles);
            break;
        case 2:
            meshSys->createSphere(meshComp, shapeParams.sphereRadius, shapeParams.sphereSlices, shapeParams.sphereStacks);
            break;
        case 3:
            meshSys->createCylinder(meshComp, shapeParams.cylinderBaseRadius, shapeParams.cylinderTopRadius,
                                  shapeParams.cylinderHeight, shapeParams.cylinderSlices, shapeParams.cylinderStacks);
            break;
        case 4:
            meshSys->createCapsule(meshComp, shapeParams.capsuleBaseRadius, shapeParams.capsuleTopRadius,
                                   shapeParams.capsuleHeight, shapeParams.capsuleSlices, shapeParams.capsuleStacks);
            break;
        case 5:
            meshSys->createTorus(meshComp, shapeParams.torusRadius, shapeParams.torusRingRadius,
                                 shapeParams.torusSides, shapeParams.torusRings);
            break;
        case 6:
            meshSys->createWall(meshComp, shapeParams.wallWidth, shapeParams.wallHeight, shapeParams.wallTiles);
            break;
    }
}

bool isReadableTextResource(const std::string& ext) {
    static const std::set<std::string> allowed = {
        ".lua", ".cpp", ".h", ".hpp", ".material", ".scene", ".yaml", ".yml",
        ".json", ".txt", ".glsl", ".vert", ".frag", ".hlsl", ".md", ".csv", ".ini", ".cfg"
    };
    return allowed.count(ext) > 0;
}

Json sceneReadableProperties(SceneProject* sceneProject) {
    Scene* scene = sceneProject->scene;
    Json props = Json::object();
    props["background_color"] = {{"type", "vector4"}, {"value", vector4Json(Catalog::getSceneProperty<Vector4>(scene, "background_color"))}};
    props["shadows_pcf"] = {{"type", "bool"}, {"value", Catalog::getSceneProperty<bool>(scene, "shadows_pcf")}};
    props["global_illumination_color"] = {{"type", "vector3"}, {"value", vector3Json(Catalog::getSceneProperty<Vector3>(scene, "global_illumination_color"))}};
    props["global_illumination_intensity"] = {{"type", "float"}, {"value", Catalog::getSceneProperty<float>(scene, "global_illumination_intensity")}};
    props["light_state"] = {{"type", "int"}, {"value", static_cast<int>(Catalog::getSceneProperty<LightState>(scene, "light_state"))}};
    props["ssao_enabled"] = {{"type", "bool"}, {"value", Catalog::getSceneProperty<bool>(scene, "ssao_enabled")}};
    props["ssao_radius"] = {{"type", "float"}, {"value", Catalog::getSceneProperty<float>(scene, "ssao_radius")}};
    props["ssao_intensity"] = {{"type", "float"}, {"value", Catalog::getSceneProperty<float>(scene, "ssao_intensity")}};
    props["ssao_bias"] = {{"type", "float"}, {"value", Catalog::getSceneProperty<float>(scene, "ssao_bias")}};
    props["ssao_debug"] = {{"type", "bool"}, {"value", Catalog::getSceneProperty<bool>(scene, "ssao_debug")}};
    props["ssr_enabled"] = {{"type", "bool"}, {"value", Catalog::getSceneProperty<bool>(scene, "ssr_enabled")}};
    props["ssr_max_distance"] = {{"type", "float"}, {"value", Catalog::getSceneProperty<float>(scene, "ssr_max_distance")}};
    props["ssr_thickness"] = {{"type", "float"}, {"value", Catalog::getSceneProperty<float>(scene, "ssr_thickness")}};
    props["ssr_intensity"] = {{"type", "float"}, {"value", Catalog::getSceneProperty<float>(scene, "ssr_intensity")}};
    props["ssr_blur"] = {{"type", "float"}, {"value", Catalog::getSceneProperty<float>(scene, "ssr_blur")}};
    props["ssr_max_steps"] = {{"type", "int"}, {"value", Catalog::getSceneProperty<int>(scene, "ssr_max_steps")}};
    props["ssr_debug_mode"] = {{"type", "int"}, {"value", Catalog::getSceneProperty<int>(scene, "ssr_debug_mode")}};
    return props;
}

bool readWholeFile(const fs::path& path, std::string& content) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool writeWholeFile(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << content;
    out.close();
    return static_cast<bool>(out);
}

void refreshMaterialEdit(Project* project, ResourcesWindow* resourcesWindow, const fs::path& fullPath) {
    if (project) {
        project->refreshLinkedMaterials(true);
    }
    if (resourcesWindow) {
        resourcesWindow->notifyResourceFileChanged(fullPath);
        resourcesWindow->refreshCurrentDirectory();
    }
}

bool normalizeMaterialPayload(const fs::path& rel, const std::string& content,
                              std::string& normalized, std::string& error) {
    try {
        YAML::Node node = YAML::Load(content);
        if (!node || !node.IsMap()) {
            error = "Material content must be a YAML map.";
            return false;
        }

        Material material = Stream::decodeMaterial(node);
        material.name = rel.lexically_normal().generic_string();
        normalized = YAML::Dump(Stream::encodeMaterial(material));
        return true;
    } catch (const std::exception& e) {
        error = std::string("Material content is invalid: ") + e.what();
        return false;
    }
}

uint32_t findSceneByPath(Project* project, const fs::path& relPath) {
    if (!project) return NULL_PROJECT_SCENE;
    const fs::path normalized = relPath.lexically_normal();
    for (const SceneProject& scene : project->getScenes()) {
        if (scene.filepath.lexically_normal() == normalized) {
            return scene.id;
        }
    }
    return NULL_PROJECT_SCENE;
}

class ReplaceMaterialFileCmd : public Command {
public:
    ReplaceMaterialFileCmd(Project* project, ResourcesWindow* resourcesWindow,
                           fs::path fullPath, std::string newContent)
        : project(project)
        , resourcesWindow(resourcesWindow)
        , fullPath(std::move(fullPath))
        , newContent(std::move(newContent)) {}

    bool execute() override {
        if (!capturedOldContent) {
            oldExists = fs::exists(fullPath) && !fs::is_directory(fullPath);
            if (oldExists && !readWholeFile(fullPath, oldContent)) {
                return false;
            }
            capturedOldContent = true;
        }

        if (!writeWholeFile(fullPath, newContent)) {
            return false;
        }

        refreshMaterialEdit(project, resourcesWindow, fullPath);
        return true;
    }

    void undo() override {
        if (oldExists) {
            writeWholeFile(fullPath, oldContent);
        } else {
            std::error_code ec;
            fs::remove(fullPath, ec);
        }
        refreshMaterialEdit(project, resourcesWindow, fullPath);
    }

    bool mergeWith(Command*) override {
        return false;
    }

private:
    Project* project;
    ResourcesWindow* resourcesWindow;
    fs::path fullPath;
    std::string newContent;
    std::string oldContent;
    bool oldExists = false;
    bool capturedOldContent = false;
};

class RemoveProjectSceneCmd : public Command {
public:
    RemoveProjectSceneCmd(Project* project, uint32_t sceneId)
        : project(project)
        , sceneId(sceneId) {}

    bool execute() override {
        if (!project || project->isAnyScenePlaying() || project->getScenes().size() <= 1) {
            return false;
        }

        uint32_t targetSceneId = sceneId;
        SceneProject* sceneProject = project->getScene(targetSceneId);
        if (!sceneProject && !scenePath.empty()) {
            targetSceneId = findSceneByPath(project, scenePath);
            sceneProject = project->getScene(targetSceneId);
        }
        if (!sceneProject || sceneProject->filepath.empty()) {
            return false;
        }
        if (project->hasSceneUnsavedChanges(sceneProject->id)) {
            return false;
        }

        sceneId = targetSceneId;
        scenePath = sceneProject->filepath.lexically_normal();
        sceneName = sceneProject->name;
        wasOpened = sceneProject->opened;
        previousSelectedScene = project->getSelectedSceneId();

        project->removeScene(sceneId);
        if (project->getScene(sceneId) || findSceneByPath(project, scenePath) != NULL_PROJECT_SCENE) {
            return false;
        }

        if (!project->saveProjectFile()) {
            restoreScene();
            return false;
        }

        removed = true;
        return true;
    }

    void undo() override {
        if (!removed) return;
        restoreScene();
    }

    bool mergeWith(Command*) override {
        return false;
    }

private:
    void restoreScene() {
        if (!project || scenePath.empty()) return;
        if (findSceneByPath(project, scenePath) == NULL_PROJECT_SCENE) {
            project->loadScene(scenePath, wasOpened, true, wasOpened);
        }

        uint32_t restoredSceneId = findSceneByPath(project, scenePath);
        if (previousSelectedScene != NULL_PROJECT_SCENE && project->getScene(previousSelectedScene)) {
            project->setSelectedSceneId(previousSelectedScene);
        } else if (restoredSceneId != NULL_PROJECT_SCENE) {
            project->setSelectedSceneId(restoredSceneId);
        }
        project->saveProjectFile();
    }

    Project* project;
    uint32_t sceneId;
    fs::path scenePath;
    std::string sceneName;
    bool wasOpened = false;
    bool removed = false;
    uint32_t previousSelectedScene = NULL_PROJECT_SCENE;
};

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
    if (name == "search_engine_api") return searchEngineApi(arguments);
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
    if (name == "update_script_file") return updateScriptFile(arguments);
    if (name == "create_bundle_from_entity") return createBundleFromEntity(arguments);
    if (name == "import_bundle_instance") return importBundleInstance(arguments);
    if (name == "add_entity_to_bundle") return addEntityToBundle(arguments);
    if (name == "remove_entity_from_bundle") return removeEntityFromBundle(arguments);
    if (name == "make_bundle_component_unique") return makeBundleComponentUnique(arguments);
    if (name == "revert_bundle_component") return revertBundleComponent(arguments);
    if (name == "export_project") return exportProject(arguments, cancel);
    if (name == "generate_shaders") return generateShaders(arguments, cancel);
    if (name == "fork_shader") return forkShader(arguments);
    if (name == "write_shader_file") return writeShaderFile(arguments);
    if (name == "create_terrain_heightmap") return createTerrainHeightmap(arguments);
    if (name == "import_project_model") return importProjectModel(arguments);
    if (name == "search_curated_assets") return searchCuratedAssets(arguments, cancel);
    if (name == "download_curated_asset") return downloadCuratedAsset(arguments, cancel);
    if (name == "read_resource_file") return readResourceFile(arguments);
    if (name == "set_main_camera") return setMainCamera(arguments);
    if (name == "open_scene") return openScene(arguments);
    if (name == "select_scene") return selectScene(arguments);
    if (name == "regenerate_mesh_geometry") return regenerateMeshGeometry(arguments);
    if (name == "delete_scene") return deleteScene(arguments);
    if (name == "save_project") return saveProject();
    if (name == "copy_resource") return copyResource(arguments);
    if (name == "update_material_file") return updateMaterialFile(arguments);
    if (name == "set_component_properties") return setComponentProperties(arguments);
    if (name == "inspect_scene") return inspectScene(arguments);
    if (name == "add_tilemap_tile") return addTilemapTile(arguments);
    if (name == "remove_tilemap_tile") return removeTilemapTile(arguments);
    if (name == "duplicate_tilemap_tile") return duplicateTilemapTile(arguments);
    if (name == "add_animation_action") return addAnimationAction(arguments);
    if (name == "remove_animation_action") return removeAnimationAction(arguments);
    if (name == "set_keyframe_times") return setKeyframeTimes(arguments);
    if (name == "undo_editor") return undoEditor(arguments);
    if (name == "redo_editor") return redoEditor(arguments);

    return failResult("Unknown action.");
}

ActionResult EditorActionExecutor::getProjectSummary() {
    Json data;
    data["name"] = project->getName();
    data["path"] = project->getProjectPath().string();
    data["selected_scene_id"] = project->getSelectedSceneId();
    data["start_scene_id"] = project->getStartSceneId();
    data["assets_dir"] = project->getAssetsDir().generic_string();
    data["scenes"] = Json::array();
    for (const auto& scene : project->getScenes()) {
        data["scenes"].push_back({
            {"id", scene.id},
            {"name", scene.name},
            {"type", sceneTypeName(scene.sceneType)},
            {"opened", scene.opened},
            {"modified", scene.isModified},
            {"main_camera", scene.mainCamera},
            {"filepath", scene.filepath.generic_string()},
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

ActionResult EditorActionExecutor::searchEngineApi(const Json& arguments) {
    const std::string query = arguments.value("query", "");
    const std::string parent = arguments.value("parent", "");
    int maxResults = arguments.value("max_results", 16);
    maxResults = std::max(1, std::min(50, maxResults));

    Json data = AiEngineApiContext::search(query, parent, maxResults);
    return okResult("Searched engine API.", data);
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

    std::vector<Entity> selected;
    if (arguments.contains("entity_ids") && arguments["entity_ids"].is_array()) {
        for (const Json& idNode : arguments["entity_ids"]) {
            Json tmp = Json::object();
            tmp["entity_id"] = idNode;
            Entity entity = resolveEntity(sceneProject, tmp);
            if (entity == NULL_ENTITY) return failResult("Entity not found in entity_ids.");
            selected.push_back(entity);
        }
    } else if (arguments.contains("entity_names") && arguments["entity_names"].is_array()) {
        for (const Json& nameNode : arguments["entity_names"]) {
            Json tmp = Json::object();
            tmp["entity_name"] = nameNode;
            Entity entity = resolveEntity(sceneProject, tmp);
            if (entity == NULL_ENTITY) return failResult("Entity not found in entity_names.");
            selected.push_back(entity);
        }
    } else {
        Entity entity = resolveEntity(sceneProject, arguments);
        if (entity == NULL_ENTITY) return failResult("Entity not found.");
        selected.push_back(entity);
    }

    if (selected.empty()) return failResult("No entities to select.");
    project->clearSelectedEntities(sceneId);
    for (Entity entity : selected) {
        project->addSelectedEntity(sceneId, entity);
    }

    Json ids = Json::array();
    for (Entity entity : selected) ids.push_back(entity);
    return okResult("Updated entity selection.", Json{{"scene_id", sceneId}, {"entity_ids", ids}});
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
    if (!validateCustomShaderValue(propertyName, arguments, error)) return failResult(error);

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

    const std::string requestedName = arguments.value("name", "Scene");
    project->createNewScene(requestedName, type);
    const uint32_t sceneId = project->getSelectedSceneId();
    SceneProject* created = project->getScene(sceneId);
    const std::string actualName = created ? created->name : requestedName;
    return okResult("Created new scene.", Json{{"scene_id", sceneId},
                                               {"name", actualName},
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

    std::string parentClass = "EntityHandle";
    if (type == ScriptType::SUBCLASS) {
        if (hasComponent(sceneProject->scene, entity, ComponentType::CameraComponent)) parentClass = "Camera";
        else if (hasComponent(sceneProject->scene, entity, ComponentType::MeshComponent) ||
                 hasComponent(sceneProject->scene, entity, ComponentType::ModelComponent)) parentClass = "Mesh";
        else if (hasComponent(sceneProject->scene, entity, ComponentType::LightComponent)) parentClass = "Light";
        else if (hasComponent(sceneProject->scene, entity, ComponentType::Transform)) parentClass = "Object";
    } else if (type != ScriptType::SCRIPT_LUA) {
        parentClass = "ScriptBase";
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
        std::ofstream h(headerFull, std::ios::trunc);
        std::ofstream c(sourceFull, std::ios::trunc);
        if (!h || !c) return failResult("Failed to create C++ script files.");
        h << "#pragma once\n\n";
        if (type == ScriptType::SUBCLASS) {
            h << "#include \"Shape.h\"\n";
            h << "#include \"" << parentClass << ".h\"\n";
        } else {
            h << "#include \"ScriptBase.h\"\n";
        }
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
        c << "    UNREGISTER_ENGINE_EVENT(onUpdate);\n";
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
    Json data = {
        {"path", attachArgs["path"]},
        {"header_path", attachArgs.value("header_path", "")},
        {"class_name", className},
        {"type", arguments.value("type", "")}
    };
    if (type == ScriptType::SCRIPT_LUA) {
        data["lua_script_guide"] = doriaxLuaScriptGuide(className);
        data["lua_startup_method"] = "init";
        data["lua_runtime_context"] = Json{{"scene", "self.scene"}, {"entity", "self.entity"}};
        data["lua_cube_color_example"] =
            "local shape = Shape(self.scene, self.entity)\n"
            "shape:setColor(1.0, 0.0, 0.0, 1.0)";
        data["next_step"] =
            "If the user asked for behavior, call search_engine_api for required runtime wrappers/methods, then update_script_file with complete Lua contents.";
    } else {
        data["cpp_script_guide"] = doriaxCppScriptGuide(type, parentClass);
        data["cpp_startup_method"] = "onUpdate with REGISTER_ENGINE_EVENT in constructor and UNREGISTER_ENGINE_EVENT in destructor";
        if (parentClass == "Mesh") {
            data["recommended_type"] = "cpp_subclass";
            data["cpp_cube_color_example"] =
                "#include \"" + className + ".h\"\n\n"
                "using namespace doriax;\n\n"
                + className + "::" + className + "(Scene* scene, Entity entity): Mesh(scene, entity) {\n"
                "    REGISTER_ENGINE_EVENT(onUpdate);\n"
                "    setColor(1.0f, 0.0f, 0.0f, 1.0f);\n"
                "}\n\n"
                + className + "::~" + className + "() {\n"
                "    UNREGISTER_ENGINE_EVENT(onUpdate);\n"
                "}\n";
        }
        data["next_step"] =
            "If the user asked for behavior, call search_engine_api for required methods, then update_script_file with complete .h and .cpp contents using flat headers (e.g. \"Mesh.h\").";
    }
    return okResult("Created and attached script.", data);
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

ActionResult EditorActionExecutor::updateScriptFile(const Json& arguments) {
    std::string error;
    fs::path rel;
    if (!safeRelativePath(project, arguments, "path", rel, error, true)) {
        return failResult(error);
    }

    const std::string ext = lower(rel.extension().string());
    if (ext != ".lua" && ext != ".cpp" && ext != ".h" && ext != ".hpp") {
        return failResult("path must point to a .lua, .cpp, .h, or .hpp script file.");
    }

    const std::string content = arguments.value("content", "");
    constexpr size_t kMaxScriptBytes = 2 * 1024 * 1024;
    if (content.size() > kMaxScriptBytes) {
        return failResult("Script content is too large for an AI edit.");
    }
    if (ext == ".lua") {
        std::string validationError = validateDoriaxLuaScriptContent(content);
        if (!validationError.empty()) {
            return failResult(validationError);
        }
    } else if (ext == ".cpp" || ext == ".h" || ext == ".hpp") {
        std::string validationError = validateDoriaxCppScriptContent(content);
        if (!validationError.empty()) {
            return failResult(validationError);
        }
    }

    const fs::path fullPath = project->getProjectPath() / rel;
    std::ofstream out(fullPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        return failResult("Failed to open script file for writing.");
    }
    out << content;
    out.close();
    if (!out) {
        return failResult("Failed to write script file.");
    }

    auto normalizeScriptPath = [this](const std::string& value) {
        fs::path path(value);
        if (path.is_absolute() && project) {
            std::error_code ec;
            fs::path relPath = fs::relative(path, project->getProjectPath(), ec);
            if (!ec) {
                path = relPath;
            }
        }
        return path.lexically_normal().generic_string();
    };

    const std::string relString = rel.lexically_normal().generic_string();
    int refreshedComponents = 0;
    for (auto& sceneProject : project->getScenes()) {
        if (!sceneProject.scene) continue;

        for (Entity entity : sceneProject.entities) {
            ScriptComponent* scriptComponent = sceneProject.scene->findComponent<ScriptComponent>(entity);
            if (!scriptComponent) continue;

            bool matchesFile = false;
            for (const ScriptEntry& scriptEntry : scriptComponent->scripts) {
                if (!scriptEntry.path.empty() && normalizeScriptPath(scriptEntry.path) == relString) {
                    matchesFile = true;
                    break;
                }
                if (!scriptEntry.headerPath.empty() && normalizeScriptPath(scriptEntry.headerPath) == relString) {
                    matchesFile = true;
                    break;
                }
            }
            if (!matchesFile) continue;

            std::vector<ScriptEntry> scripts = scriptComponent->scripts;
            if (ext == ".h" || ext == ".hpp") {
                project->updateScriptProperties(&sceneProject, entity, scripts, content, fullPath.string());
            } else {
                project->updateScriptProperties(&sceneProject, entity, scripts);
            }
            PropertyCmd<std::vector<ScriptEntry>> propertyCmd(
                project, sceneProject.id, entity, ComponentType::ScriptComponent, "scripts", scripts);
            propertyCmd.execute();
            refreshedComponents++;
        }
    }

    if (resourcesWindow) {
        resourcesWindow->refreshCurrentDirectory();
    }
    Out::info("AI updated script file: %s", relString.c_str());

    return okResult("Updated script file.",
                    Json{{"path", relString},
                         {"bytes", content.size()},
                         {"refreshed_script_components", refreshedComponents}});
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

bool EditorActionExecutor::validateCustomShaderValue(const std::string& propertyName, const Json& args, std::string& error) const {
    if (propertyName != "customShader") {
        return true;
    }
    const std::string value = args.value("string_value", "");
    if (value.empty()) {
        return true; // empty resets to the built-in shader
    }

    Util::CustomShaderPaths paths = Util::resolveCustomShaderPaths(value);
    const fs::path projectPath = project->getProjectPath();
    if (!fs::exists(projectPath / paths.vert) || !fs::exists(projectPath / paths.frag)) {
        error = "customShader references missing files (" + paths.vert + ", " + paths.frag +
                "). Use fork_shader to create a fork, or write_shader_file to author the .vert/.frag, before setting customShader.";
        return false;
    }
    return true;
}

ActionResult EditorActionExecutor::forkShader(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");
    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");

    // Resolve which renderable component to fork for, either explicit or auto-detected.
    ComponentType component;
    ShaderType shaderType;
    const std::string componentArg = arguments.value("component", "");
    if (!componentArg.empty()) {
        if (!parseComponentType(componentArg, component)) return failResult("Unknown component type.");
        if (!shaderTypeForComponent(component, shaderType)) {
            return failResult("Component does not support custom shaders. Use Mesh/UI/Points/Lines/Sky.");
        }
        if (!hasComponent(sceneProject->scene, entity, component)) {
            return failResult("Entity has no " + componentArg + ".");
        }
    } else if (!detectForkableComponent(sceneProject->scene, entity, component, shaderType)) {
        return failResult("Entity has no shader-capable component (Mesh/UI/Points/Lines/Sky).");
    }

    // Re-forking would orphan the previous fork's files; require an explicit reset first.
    if (std::string* current = Catalog::getPropertyRef<std::string>(sceneProject->scene, entity, component, "customShader")) {
        if (!current->empty()) {
            return failResult("Component already uses a custom shader (" + *current +
                              "). Edit it with write_shader_file, or clear customShader to reset before forking again.");
        }
    }

    const std::string desiredName = sceneProject->scene->getEntityName(entity);
    auto* forkCmd = new ForkShaderCmd(project, sceneId, entity, component, shaderType, desiredName);
    if (!forkCmd->isValid()) {
        delete forkCmd;
        return failResult("Failed to plan the shader fork (built-in source unavailable?).");
    }

    const std::string base = forkCmd->getBase();
    CommandHandle::get(sceneId)->addCommandNoMerge(forkCmd);

    // The command writes the files as part of execute(); confirm so the AI gets accurate feedback.
    if (!fs::exists(project->getProjectPath() / (base + ".vert")) ||
        !fs::exists(project->getProjectPath() / (base + ".frag"))) {
        return failResult("Shader fork did not write its source files.");
    }

    if (resourcesWindow) {
        resourcesWindow->refreshCurrentDirectory();
    }
    Out::info("AI forked shader: %s", base.c_str());

    return okResult("Forked shader through the command history.",
                    Json{{"custom_shader", base},
                         {"vert_path", base + ".vert"},
                         {"frag_path", base + ".frag"}});
}

ActionResult EditorActionExecutor::writeShaderFile(const Json& arguments) {
    std::string error;
    fs::path rel;
    // A shader source may be edited (forked entry point) or newly created (shared include).
    if (!safeRelativePath(project, arguments, "path", rel, error, false)) {
        return failResult(error);
    }

    const std::string ext = lower(rel.extension().string());
    if (ext != ".vert" && ext != ".frag" && ext != ".glsl") {
        return failResult("path must point to a .vert, .frag, or .glsl shader file.");
    }

    const std::string content = arguments.value("content", "");
    constexpr size_t kMaxShaderBytes = 2 * 1024 * 1024;
    if (content.size() > kMaxShaderBytes) {
        return failResult("Shader content is too large for an AI edit.");
    }

    const fs::path fullPath = project->getProjectPath() / rel;
    std::error_code ec;
    fs::create_directories(fullPath.parent_path(), ec);
    std::ofstream out(fullPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        return failResult("Failed to open shader file for writing.");
    }
    out << content;
    out.close();
    if (!out) {
        return failResult("Failed to write shader file.");
    }

    // Editing a forked source (or shared include) invalidates compiled shaders so the
    // viewport recompiles them; mirrors CodeEditor saving a .vert/.frag/.glsl.
    project->invalidateCustomShaders();

    if (resourcesWindow) {
        resourcesWindow->refreshCurrentDirectory();
    }
    Out::info("AI wrote shader file: %s", rel.generic_string().c_str());

    return okResult("Wrote shader file.",
                    Json{{"path", rel.generic_string()},
                         {"bytes", content.size()}});
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

ActionResult EditorActionExecutor::readResourceFile(const Json& arguments) {
    std::string error;
    fs::path rel;
    if (!safeRelativePath(project, arguments, "path", rel, error, true)) {
        return failResult(error);
    }

    const std::string ext = lower(rel.extension().string());
    if (!isReadableTextResource(ext)) {
        return failResult("path extension is not supported for read_resource_file.");
    }

    const fs::path fullPath = project->getProjectPath() / rel;
    std::ifstream in(fullPath, std::ios::binary);
    if (!in) return failResult("Failed to open resource file.");

    constexpr size_t kMaxBytes = 512 * 1024;
    std::string content;
    content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (content.size() > kMaxBytes) {
        return failResult("Resource file is too large to read (max 512 KB).");
    }

    return okResult("Read resource file.",
                    Json{{"path", rel.lexically_normal().generic_string()},
                         {"bytes", content.size()},
                         {"content", content}});
}

ActionResult EditorActionExecutor::setMainCamera(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");

    Entity cameraEntity = NULL_ENTITY;
    if (arguments.value("clear", false)) {
        cameraEntity = NULL_ENTITY;
    } else {
        cameraEntity = resolveEntity(sceneProject, arguments);
        if (cameraEntity == NULL_ENTITY) return failResult("Camera entity not found.");
        if (!sceneProject->scene->findComponent<CameraComponent>(cameraEntity)) {
            return failResult("Entity does not have a Camera component.");
        }
    }

    CommandHandle::get(sceneId)->addCommandNoMerge(new SetMainCameraCmd(project, sceneId, cameraEntity));
    return okResult("Set main camera through the command history.",
                    Json{{"scene_id", sceneId}, {"main_camera", cameraEntity}});
}

ActionResult EditorActionExecutor::openScene(const Json& arguments) {
    std::string error;
    fs::path rel;
    if (!safeRelativePath(project, arguments, "scene_path", rel, error, true)) {
        return failResult(error);
    }
    if (!Util::isSceneFile(rel.string())) {
        return failResult("scene_path must point to a .scene file.");
    }
    if (project->isAnyScenePlaying()) {
        return failResult("Cannot open a scene while play mode is active.");
    }

    const bool closePrevious = arguments.value("close_previous", false);
    project->openScene(rel, closePrevious);
    return okResult("Requested scene open.",
                    Json{{"scene_path", rel.lexically_normal().generic_string()},
                         {"selected_scene_id", project->getSelectedSceneId()}});
}

ActionResult EditorActionExecutor::selectScene(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject) return failResult("Scene not found.");
    if (!sceneProject->opened) return failResult("Scene is not open. Use open_scene first.");

    project->setSelectedSceneId(sceneId);
    return okResult("Selected scene tab.", Json{{"scene_id", sceneId}, {"scene_name", sceneProject->name}});
}

ActionResult EditorActionExecutor::regenerateMeshGeometry(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");

    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");
    if (!hasComponent(sceneProject->scene, entity, ComponentType::MeshComponent)) {
        return failResult("Entity has no Mesh component.");
    }

    MeshComponent meshComp = sceneProject->scene->getComponent<MeshComponent>(entity);
    bool hasGenerated = false;
    for (unsigned int i = 0; i < meshComp.numSubmeshes; ++i) {
        if (meshComp.submeshes[i].generated) {
            hasGenerated = true;
            break;
        }
    }
    if (!hasGenerated) {
        return failResult("Mesh entity has no generated procedural geometry to regenerate.");
    }

    ShapeParameters shapeParams;
    std::string geometryError;
    if (!parseGeometryTypeValue(arguments, shapeParams, geometryError)) {
        return failResult(geometryError);
    }
    applyShapeParameters(arguments, shapeParams);

    std::shared_ptr<MeshSystem> meshSys = sceneProject->scene->getSystem<MeshSystem>();
    if (!meshSys) return failResult("Mesh system is unavailable.");
    updateMeshShape(meshComp, meshSys.get(), shapeParams);
    CommandHandle::get(sceneId)->addCommandNoMerge(new MeshChangeCmd(project, sceneId, entity, meshComp));
    return okResult("Regenerated mesh geometry through the command history.",
                    Json{{"scene_id", sceneId}, {"entity_id", entity}, {"geometry_type", shapeParams.geometryType}});
}

ActionResult EditorActionExecutor::deleteScene(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject) return failResult("Scene not found.");
    if (project->getScenes().size() <= 1) {
        return failResult("Cannot delete the last scene in the project.");
    }
    if (project->isAnyScenePlaying()) {
        return failResult("Cannot delete a scene while play mode is active.");
    }
    if (sceneProject->filepath.empty()) {
        return failResult("Cannot delete an unsaved scene through AI because it cannot be restored on undo.");
    }
    if (project->hasSceneUnsavedChanges(sceneId)) {
        return failResult("Save the scene and its dependent resources before deleting it through AI.");
    }

    const std::string sceneName = sceneProject->name;
    const fs::path scenePath = sceneProject->filepath.lexically_normal();
    project->getProjectCommandHistory()->addCommandNoMerge(new RemoveProjectSceneCmd(project, sceneId));
    if (project->getScene(sceneId) || findSceneByPath(project, scenePath) != NULL_PROJECT_SCENE) {
        return failResult("Failed to remove scene.");
    }
    return okResult("Removed scene from project through the project command history.",
                    Json{{"removed_scene_id", sceneId}, {"removed_scene_name", sceneName},
                         {"scene_path", scenePath.generic_string()},
                         {"selected_scene_id", project->getSelectedSceneId()}});
}

ActionResult EditorActionExecutor::saveProject() {
    if (!project->saveProject(false)) {
        return failResult("Failed to save project.");
    }
    return okResult("Saved project.", Json{{"path", project->getProjectPath().string()}});
}

ActionResult EditorActionExecutor::copyResource(const Json& arguments) {
    std::string error;
    fs::path sourceRel;
    fs::path targetRel;
    if (!safeRelativePath(project, arguments, "source_path", sourceRel, error, true)) return failResult(error);
    if (!safeRelativePath(project, arguments, "target_dir", targetRel, error, false)) return failResult(error);

    const fs::path sourceFull = project->getProjectPath() / sourceRel;
    const fs::path targetFull = project->getProjectPath() / targetRel;
    if (!fs::exists(sourceFull)) return failResult("source_path does not exist.");
    std::error_code ec;
    fs::create_directories(targetFull, ec);
    if (ec) return failResult("Failed to create target_dir: " + ec.message());

    project->getProjectCommandHistory()->addCommandNoMerge(
        new CopyFileCmd(project, std::vector<std::string>{sourceFull.string()}, targetFull.string(), true));
    if (resourcesWindow) resourcesWindow->refreshCurrentDirectory();
    return okResult("Copied resource through the project command history.",
                    Json{{"source_path", sourceRel.generic_string()},
                         {"target_dir", targetRel.generic_string()}});
}

ActionResult EditorActionExecutor::updateMaterialFile(const Json& arguments) {
    std::string error;
    fs::path rel;
    if (!safeRelativePath(project, arguments, "path", rel, error, true)) return failResult(error);
    if (!Util::isMaterialFile(rel.string())) {
        return failResult("path must point to a .material file.");
    }

    const std::string content = arguments.value("content", "");
    constexpr size_t kMaxBytes = 512 * 1024;
    if (content.size() > kMaxBytes) {
        return failResult("Material content is too large for an AI edit.");
    }

    const fs::path fullPath = project->getProjectPath() / rel;
    std::string normalizedContent;
    std::string materialError;
    if (!normalizeMaterialPayload(rel, content, normalizedContent, materialError)) {
        return failResult(materialError);
    }

    project->getProjectCommandHistory()->addCommandNoMerge(
        new ReplaceMaterialFileCmd(project, resourcesWindow, fullPath, normalizedContent));

    std::string writtenContent;
    if (!readWholeFile(fullPath, writtenContent) || writtenContent != normalizedContent) {
        return failResult("Failed to update material file through the project command history.");
    }

    Out::info("AI updated material file: %s", rel.generic_string().c_str());
    return okResult("Updated material file through the project command history.",
                    Json{{"path", rel.lexically_normal().generic_string()}, {"bytes", normalizedContent.size()}});
}

ActionResult EditorActionExecutor::setComponentProperties(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");

    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");
    if (!arguments.contains("properties") || !arguments["properties"].is_array() || arguments["properties"].empty()) {
        return failResult("properties must be a non-empty array.");
    }

    auto* multiCmd = new MultiPropertyCmd();
    int applied = 0;
    for (const Json& item : arguments["properties"]) {
        if (!item.is_object()) return failResult("Each properties entry must be an object.");
        ComponentType component;
        if (!parseComponentType(item.value("component", ""), component)) {
            delete multiCmd;
            return failResult("Unknown component in properties array.");
        }
        const std::string propertyName = item.value("property", "");
        if (propertyName.empty()) {
            delete multiCmd;
            return failResult("Each properties entry requires property.");
        }

        auto props = Catalog::findEntityProperties(sceneProject->scene, entity, component);
        auto it = props.find(propertyName);
        if (it == props.end()) {
            delete multiCmd;
            return failResult("Property not found: " + propertyName);
        }

        std::string validationError;
        if (!validateCustomShaderValue(propertyName, item, validationError)) {
            delete multiCmd;
            return failResult(validationError);
        }

        std::string buildError;
        Command* cmd = buildPropertyCommand(project, sceneId, entity, component, propertyName, it->second, item, buildError);
        if (!cmd) {
            delete multiCmd;
            return failResult(buildError.empty() ? ("Failed to build property command for " + propertyName) : buildError);
        }
        multiCmd->addCommand(std::unique_ptr<Command>(cmd));
        ++applied;
    }

    multiCmd->setNoMerge();
    CommandHandle::get(sceneId)->addCommandNoMerge(multiCmd);
    return okResult("Set component properties through the command history.",
                    Json{{"scene_id", sceneId}, {"entity_id", entity}, {"applied_count", applied}});
}

ActionResult EditorActionExecutor::inspectScene(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");

    Json childScenes = Json::array();
    for (uint32_t childId : project->getChildScenes(sceneId)) {
        SceneProject* child = project->getScene(childId);
        childScenes.push_back({
            {"scene_id", childId},
            {"name", child ? child->name : ""},
            {"start_active", project->isChildSceneStartActive(sceneId, childId)}
        });
    }

    Json data = {
        {"scene_id", sceneId},
        {"name", sceneProject->name},
        {"type", sceneTypeName(sceneProject->sceneType)},
        {"opened", sceneProject->opened},
        {"modified", sceneProject->isModified},
        {"filepath", sceneProject->filepath.generic_string()},
        {"main_camera", sceneProject->mainCamera},
        {"entity_count", sceneProject->entities.size()},
        {"selected_entities", project->getSelectedEntities(sceneId)},
        {"child_scenes", childScenes},
        {"properties", sceneReadableProperties(sceneProject)}
    };
    return okResult("Inspected scene.", data);
}

ActionResult EditorActionExecutor::addTilemapTile(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");

    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");

    TilemapComponent* tilemap = sceneProject->scene->findComponent<TilemapComponent>(entity);
    if (!tilemap) return failResult("Entity has no Tilemap component.");

    const int rectIndex = arguments.value("rect_index", 0);
    if (rectIndex < 0 || static_cast<unsigned int>(rectIndex) >= tilemap->numTilesRect) {
        return failResult("rect_index is out of range.");
    }

    const unsigned int freeSlot = tilemap->numTiles;
    if (freeSlot >= tilemap->tiles.size()) {
        return failResult("Tilemap tile array is full.");
    }

    const TileRectData& rectData = tilemap->tilesRect[rectIndex];
    Vector2 position(rectData.rect.getX(), rectData.rect.getY());
    if (arguments.contains("position")) {
        if (!parseVector2(arguments["position"], position)) return failResult("position must be a vector2 object.");
    }
    float tileW = rectData.rect.getWidth();
    float tileH = rectData.rect.getHeight();
    if (arguments.contains("width") && arguments["width"].is_number()) tileW = arguments["width"].get<float>();
    if (arguments.contains("height") && arguments["height"].is_number()) tileH = arguments["height"].get<float>();

    const std::string tilePrefix = "tiles[" + std::to_string(freeSlot) + "]";
    auto* multiCmd = new MultiPropertyCmd();
    multiCmd->addPropertyCmd<std::string>(project, sceneId, entity, ComponentType::TilemapComponent,
                                          tilePrefix + ".name", rectData.name);
    multiCmd->addPropertyCmd<int>(project, sceneId, entity, ComponentType::TilemapComponent,
                                  tilePrefix + ".rectId", rectIndex);
    multiCmd->addPropertyCmd<Vector2>(project, sceneId, entity, ComponentType::TilemapComponent,
                                      tilePrefix + ".position", position);
    multiCmd->addPropertyCmd<float>(project, sceneId, entity, ComponentType::TilemapComponent,
                                    tilePrefix + ".width", tileW);
    multiCmd->addPropertyCmd<float>(project, sceneId, entity, ComponentType::TilemapComponent,
                                    tilePrefix + ".height", tileH);
    multiCmd->addPropertyCmd<unsigned int>(project, sceneId, entity, ComponentType::TilemapComponent,
                                           "numTiles", freeSlot + 1);
    multiCmd->setNoMerge();
    CommandHandle::get(sceneId)->addCommandNoMerge(multiCmd);
    return okResult("Added tilemap tile through the command history.",
                    Json{{"scene_id", sceneId}, {"entity_id", entity}, {"tile_index", freeSlot}});
}

ActionResult EditorActionExecutor::removeTilemapTile(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");

    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");

    const unsigned int tileIndex = static_cast<unsigned int>(std::max(0, arguments.value("tile_index", 0)));
    Command* cmd = ProjectUtils::buildDeleteTileCmd(project, sceneId, entity, tileIndex);
    if (!cmd) return failResult("Failed to remove tile. Check tile_index and Tilemap component.");
    CommandHandle::get(sceneId)->addCommandNoMerge(cmd);
    return okResult("Removed tilemap tile through the command history.",
                    Json{{"scene_id", sceneId}, {"entity_id", entity}, {"tile_index", tileIndex}});
}

ActionResult EditorActionExecutor::duplicateTilemapTile(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");

    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");

    const unsigned int tileIndex = static_cast<unsigned int>(std::max(0, arguments.value("tile_index", 0)));
    TilemapComponent* tilemap = sceneProject->scene->findComponent<TilemapComponent>(entity);
    const unsigned int newIndex = tilemap ? tilemap->numTiles : 0;
    Command* cmd = ProjectUtils::buildDuplicateTileCmd(project, sceneId, entity, tileIndex);
    if (!cmd) return failResult("Failed to duplicate tile. Check tile_index and Tilemap capacity.");
    CommandHandle::get(sceneId)->addCommandNoMerge(cmd);
    return okResult("Duplicated tilemap tile through the command history.",
                    Json{{"scene_id", sceneId}, {"entity_id", entity},
                         {"source_tile_index", tileIndex}, {"new_tile_index", newIndex}});
}

ActionResult EditorActionExecutor::addAnimationAction(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");

    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");

    AnimationComponent* anim = sceneProject->scene->findComponent<AnimationComponent>(entity);
    if (!anim) return failResult("Entity has no Animation component.");

    ActionFrame frame;
    frame.startTime = arguments.value("start_time", 0.0f);
    frame.duration = arguments.value("duration", 1.0f);
    frame.track = static_cast<uint32_t>(std::max(0, arguments.value("track", 0)));
    if (arguments.contains("action_entity_id")) {
        frame.action = static_cast<Entity>(std::max(0, arguments.value("action_entity_id", 0)));
    } else if (arguments.contains("action_entity_name")) {
        Json tmp = Json::object();
        tmp["entity_name"] = arguments["action_entity_name"];
        frame.action = resolveEntity(sceneProject, tmp);
    }

    std::vector<ActionFrame> actions = anim->actions;
    for (const auto& existing : actions) {
        if (existing.track != frame.track) continue;
        const float startA = existing.startTime;
        const float endA = existing.startTime + existing.duration;
        const float startB = frame.startTime;
        const float endB = frame.startTime + frame.duration;
        if (std::max(startA, startB) < std::min(endA, endB)) {
            return failResult("New action overlaps an existing action on the same track.");
        }
    }
    actions.push_back(frame);

    CommandHandle::get(sceneId)->addCommandNoMerge(new PropertyCmd<std::vector<ActionFrame>>(
        project, sceneId, entity, ComponentType::AnimationComponent, "actions", actions));
    return okResult("Added animation action through the command history.",
                    Json{{"scene_id", sceneId}, {"entity_id", entity}, {"action_index", actions.size() - 1}});
}

ActionResult EditorActionExecutor::removeAnimationAction(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");

    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");

    AnimationComponent* anim = sceneProject->scene->findComponent<AnimationComponent>(entity);
    if (!anim) return failResult("Entity has no Animation component.");

    const size_t actionIndex = static_cast<size_t>(std::max(0, arguments.value("action_index", 0)));
    if (actionIndex >= anim->actions.size()) return failResult("action_index is out of range.");

    std::vector<ActionFrame> actions = anim->actions;
    actions.erase(actions.begin() + static_cast<std::ptrdiff_t>(actionIndex));
    CommandHandle::get(sceneId)->addCommandNoMerge(new PropertyCmd<std::vector<ActionFrame>>(
        project, sceneId, entity, ComponentType::AnimationComponent, "actions", actions));
    return okResult("Removed animation action through the command history.",
                    Json{{"scene_id", sceneId}, {"entity_id", entity}, {"removed_action_index", actionIndex}});
}

ActionResult EditorActionExecutor::setKeyframeTimes(const Json& arguments) {
    uint32_t sceneId = resolveSceneId(project, arguments);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) return failResult("Scene not found.");

    Entity entity = resolveEntity(sceneProject, arguments);
    if (entity == NULL_ENTITY) return failResult("Entity not found.");

    KeyframeTracksComponent* keyframes = sceneProject->scene->findComponent<KeyframeTracksComponent>(entity);
    if (!keyframes) return failResult("Entity has no KeyframeTracks component.");

    std::vector<float> times;
    if (arguments.contains("times") && arguments["times"].is_array()) {
        for (const Json& value : arguments["times"]) {
            if (!value.is_number()) return failResult("times must contain numbers.");
            times.push_back(value.get<float>());
        }
    } else if (arguments.value("append", false)) {
        times = keyframes->times;
        times.push_back(arguments.value("time", times.empty() ? 0.0f : times.back()));
    } else {
        return failResult("Provide times array or append=true with time.");
    }

    CommandHandle::get(sceneId)->addCommandNoMerge(new PropertyCmd<std::vector<float>>(
        project, sceneId, entity, ComponentType::KeyframeTracksComponent, "times", times));
    return okResult("Set keyframe times through the command history.",
                    Json{{"scene_id", sceneId}, {"entity_id", entity}, {"count", times.size()}});
}

ActionResult EditorActionExecutor::undoEditor(const Json& arguments) {
    const std::string scope = lower(arguments.value("scope", "scene"));
    if (scope == "project") {
        CommandHistory* history = project->getProjectCommandHistory();
        if (!history || !history->canUndo()) return failResult("Nothing to undo in project history.");
        history->undo();
        return okResult("Undid last project-level command.");
    }

    uint32_t sceneId = resolveSceneId(project, arguments);
    CommandHistory* history = CommandHandle::get(sceneId);
    if (!history || !history->canUndo()) return failResult("Nothing to undo in scene history.");
    history->undo();
    return okResult("Undid last scene command.", Json{{"scene_id", sceneId}});
}

ActionResult EditorActionExecutor::redoEditor(const Json& arguments) {
    const std::string scope = lower(arguments.value("scope", "scene"));
    if (scope == "project") {
        CommandHistory* history = project->getProjectCommandHistory();
        if (!history || !history->canRedo()) return failResult("Nothing to redo in project history.");
        history->redo();
        return okResult("Redid last project-level command.");
    }

    uint32_t sceneId = resolveSceneId(project, arguments);
    CommandHistory* history = CommandHandle::get(sceneId);
    if (!history || !history->canRedo()) return failResult("Nothing to redo in scene history.");
    history->redo();
    return okResult("Redid last scene command.", Json{{"scene_id", sceneId}});
}

} // namespace doriax::editor::ai
