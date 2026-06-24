#include "EditorActionExecutor.h"

#include "AiPathUtils.h"
#include "EditorActionRegistry.h"
#include "HttpClient.h"

#include "Out.h"
#include "Project.h"
#include "Catalog.h"
#include "command/CommandHandle.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "command/type/ObjectTransformCmd.h"
#include "util/Util.h"
#include "window/ResourcesWindow.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <fstream>
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
        {"camera", EntityCreationType::CAMERA},
        {"point_light", EntityCreationType::POINT_LIGHT},
        {"directional_light", EntityCreationType::DIRECTIONAL_LIGHT},
        {"spot_light", EntityCreationType::SPOT_LIGHT},
        {"sky", EntityCreationType::SKY},
        {"fog", EntityCreationType::FOG},
        {"model", EntityCreationType::MODEL},
        {"terrain", EntityCreationType::TERRAIN}
    };
    auto it = map.find(lower(typeName));
    if (it == map.end()) return false;
    type = it->second;
    return true;
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
    if (name == "create_entity") return createEntity(arguments);
    if (name == "set_entity_transform") return setEntityTransform(arguments);
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

    auto* cmd = new CreateEntityCmd(project, sceneId, arguments.value("name", "Entity"), type);
    if (arguments.contains("position")) {
        Vector3 position = Vector3::ZERO;
        if (parseVector3(arguments["position"], position)) {
            cmd->addProperty<Vector3>(ComponentType::Transform, "position", position);
        }
    }
    CommandHandle::get(sceneId)->addCommandNoMerge(cmd);

    return okResult("Created entity through the command history.");
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
    Vector3 scale = transform->scale;
    if (arguments.contains("position")) parseVector3(arguments["position"], position);
    if (arguments.contains("scale")) parseVector3(arguments["scale"], scale);

    CommandHandle::get(sceneId)->addCommandNoMerge(
        new ObjectTransformCmd(project, sceneId, entity, position, transform->rotation, scale));

    return okResult("Updated entity transform through the command history.");
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
