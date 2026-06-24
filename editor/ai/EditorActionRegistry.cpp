#include "EditorActionRegistry.h"

#include "AiPathUtils.h"

#include <sstream>

namespace doriax::editor::ai {

namespace {

Json objectSchema(std::initializer_list<std::pair<const char*, Json>> properties,
                  std::initializer_list<const char*> required = {}) {
    Json props = Json::object();
    for (const auto& item : properties) {
        props[item.first] = item.second;
    }
    Json req = Json::array();
    for (const char* key : required) {
        req.push_back(key);
    }
    return {
        {"type", "object"},
        {"additionalProperties", false},
        {"properties", props},
        {"required", req}
    };
}

Json stringSchema(const std::string& description) {
    return {{"type", "string"}, {"description", description}};
}

Json numberSchema(const std::string& description) {
    return {{"type", "number"}, {"description", description}};
}

Json integerSchema(const std::string& description) {
    return {{"type", "integer"}, {"description", description}};
}

Json vector3Schema(const std::string& description) {
    return {
        {"type", "object"},
        {"description", description},
        {"additionalProperties", false},
        {"properties", {
            {"x", numberSchema("X value")},
            {"y", numberSchema("Y value")},
            {"z", numberSchema("Z value")}
        }}
    };
}

const std::vector<ToolDefinition>& cachedTools() {
    static const std::vector<ToolDefinition> definitions = {
        {
            "get_project_summary",
            "Read a compact summary of the current project, selected scene, selected entities, and resource roots.",
            objectSchema({}),
            true
        },
        {
            "search_resources",
            "Search project files by name and optional file type. This only reads project paths.",
            objectSchema({
                {"query", stringSchema("Case-insensitive filename search text")},
                {"type", stringSchema("Optional type: model, image, material, scene, script, audio, font, any")},
                {"max_results", integerSchema("Maximum number of paths to return, 1-50")}
            }, {"query"}),
            true
        },
        {
            "list_scene_entities",
            "List entities in a scene by id/name/component summary.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")}
            }),
            true
        },
        {
            "create_entity",
            "Create an undoable entity in the selected scene using an allowed editor entity type.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"name", stringSchema("Entity display name")},
                {"type", stringSchema("Allowed type: empty, object, box, plane, wall, mirror, sphere, cylinder, capsule, torus, camera, point_light, directional_light, spot_light, sky, fog, model, terrain")},
                {"position", vector3Schema("Optional local position")}
            }, {"name", "type"}),
            false
        },
        {
            "set_entity_transform",
            "Set an entity transform through the undoable command system.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"entity_id", integerSchema("Entity id")},
                {"entity_name", stringSchema("Entity name, used only when entity_id is omitted")},
                {"position", vector3Schema("Optional local position")},
                {"scale", vector3Schema("Optional local scale")}
            }),
            false
        },
        {
            "import_project_model",
            "Import an existing project-relative glTF/GLB/OBJ model into a 3D scene using ModelLoadCmd.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"model_path", stringSchema("Project-relative model path, e.g. assets/models/dog.glb")},
                {"entity_name", stringSchema("Name for the created model entity")},
                {"position", vector3Schema("Optional drop position")}
            }, {"model_path"}),
            false
        },
        {
            "search_curated_assets",
            "Search curated model sources. Sketchfab search is metadata-only without user OAuth; Poly Haven API requires a unique User-Agent and may need licensing for commercial API use.",
            objectSchema({
                {"provider", stringSchema("polyhaven or sketchfab")},
                {"query", stringSchema("Search text")},
                {"max_results", integerSchema("Maximum number of assets to return, 1-12")}
            }, {"provider", "query"}),
            true
        },
        {
            "download_curated_asset",
            "Download a reviewed direct model URL into project assets, write attribution metadata, and optionally import it into the scene. Archives are rejected in v1 unless an extractor is added.",
            objectSchema({
                {"scene_id", integerSchema("Scene id. Omit to use the selected scene")},
                {"provider", stringSchema("Asset provider name")},
                {"asset_id", stringSchema("Provider asset id")},
                {"title", stringSchema("Asset title")},
                {"author", stringSchema("Author name")},
                {"license", stringSchema("License name or URL")},
                {"source_url", stringSchema("Provider asset page URL")},
                {"download_url", stringSchema("Direct HTTPS URL for a .glb, .gltf, or .obj file")},
                {"slug", stringSchema("Filesystem-safe asset slug")},
                {"entity_name", stringSchema("Optional imported entity name")},
                {"position", vector3Schema("Optional import position")}
            }, {"provider", "asset_id", "title", "source_url", "download_url"}),
            false
        }
    };
    return definitions;
}

bool hasString(const Json& args, const char* key) {
    return args.contains(key) && args[key].is_string() && !args[key].get<std::string>().empty();
}

bool hasObject(const Json& args, const char* key) {
    return args.contains(key) && args[key].is_object();
}

ValidationResult ok() {
    return {true, {}};
}

ValidationResult fail(const std::string& error) {
    return {false, error};
}

} // namespace

std::vector<ToolDefinition> EditorActionRegistry::tools() {
    return cachedTools();
}

bool EditorActionRegistry::hasTool(const std::string& name) {
    for (const auto& tool : cachedTools()) {
        if (tool.name == name) return true;
    }
    return false;
}

bool EditorActionRegistry::isReadOnly(const std::string& name) {
    return getTool(name).readOnly;
}

ToolDefinition EditorActionRegistry::getTool(const std::string& name) {
    for (const auto& tool : cachedTools()) {
        if (tool.name == name) return tool;
    }
    return {};
}

ValidationResult EditorActionRegistry::validate(const std::string& name, const Json& arguments) {
    if (!hasTool(name)) {
        return fail("Unknown editor action: " + name);
    }
    if (!arguments.is_object()) {
        return fail("Action arguments must be a JSON object.");
    }

    if (name == "search_resources") {
        return hasString(arguments, "query") ? ok() : fail("search_resources requires query.");
    }
    if (name == "create_entity") {
        if (!hasString(arguments, "name") || !hasString(arguments, "type")) {
            return fail("create_entity requires name and type.");
        }
        return ok();
    }
    if (name == "set_entity_transform") {
        if (!arguments.contains("entity_id") && !hasString(arguments, "entity_name")) {
            return fail("set_entity_transform requires entity_id or entity_name.");
        }
        if (!hasObject(arguments, "position") && !hasObject(arguments, "scale")) {
            return fail("set_entity_transform requires position or scale.");
        }
        return ok();
    }
    if (name == "import_project_model") {
        if (!hasString(arguments, "model_path")) {
            return fail("import_project_model requires model_path.");
        }
        if (!PathUtils::isSafeRelativePath(arguments["model_path"].get<std::string>())) {
            return fail("model_path must be a safe project-relative path.");
        }
        return ok();
    }
    if (name == "search_curated_assets") {
        return hasString(arguments, "provider") && hasString(arguments, "query")
            ? ok()
            : fail("search_curated_assets requires provider and query.");
    }
    if (name == "download_curated_asset") {
        if (!hasString(arguments, "provider") || !hasString(arguments, "asset_id") ||
            !hasString(arguments, "title") || !hasString(arguments, "source_url") ||
            !hasString(arguments, "download_url")) {
            return fail("download_curated_asset requires provider, asset_id, title, source_url, and download_url.");
        }
        const std::string url = arguments["download_url"].get<std::string>();
        if (url.rfind("https://", 0) != 0) {
            return fail("download_url must use HTTPS.");
        }
        return ok();
    }

    return ok();
}

std::string EditorActionRegistry::describe(const std::string& name, const Json& arguments) {
    std::ostringstream out;
    if (name == "get_project_summary") {
        return "Read project summary";
    }
    if (name == "search_resources") {
        return "Search resources for \"" + arguments.value("query", "") + "\"";
    }
    if (name == "list_scene_entities") {
        return arguments.contains("scene_id")
            ? "List entities in scene " + std::to_string(arguments.value("scene_id", 0))
            : "List entities in the selected scene";
    }
    if (name == "create_entity") {
        out << "Create " << arguments.value("type", "entity")
            << " entity \"" << arguments.value("name", "Entity") << "\"";
        return out.str();
    }
    if (name == "set_entity_transform") {
        if (arguments.contains("entity_id")) {
            out << "Set transform for entity " << arguments["entity_id"].get<uint64_t>();
        } else {
            out << "Set transform for entity \"" << arguments.value("entity_name", "") << "\"";
        }
        return out.str();
    }
    if (name == "import_project_model") {
        return "Import model " + arguments.value("model_path", "");
    }
    if (name == "search_curated_assets") {
        return "Search " + arguments.value("provider", "curated assets") +
               " for \"" + arguments.value("query", "") + "\"";
    }
    if (name == "download_curated_asset") {
        return "Download curated asset \"" + arguments.value("title", "Asset") + "\"";
    }
    return name;
}

} // namespace doriax::editor::ai
