#pragma once

#include "AiTypes.h"

#include <atomic>

namespace doriax::editor {
class Project;
class ResourcesWindow;
}

namespace doriax::editor::ai {

class HttpClient;

class EditorActionExecutor {
public:
    EditorActionExecutor(Project* project, ResourcesWindow* resourcesWindow, const HttpClient* httpClient);

    ActionResult execute(const std::string& name,
                         const Json& arguments,
                         const std::atomic<bool>* cancel = nullptr);

private:
    Project* project;
    ResourcesWindow* resourcesWindow;
    const HttpClient* httpClient;

    ActionResult getProjectSummary();
    ActionResult searchResources(const Json& arguments);
    ActionResult listSceneEntities(const Json& arguments);
    ActionResult inspectEntity(const Json& arguments);
    ActionResult inspectComponent(const Json& arguments);
    ActionResult listComponentTypes();
    ActionResult searchEngineApi(const Json& arguments);
    ActionResult createEntity(const Json& arguments);
    ActionResult setEntityTransform(const Json& arguments);
    ActionResult renameEntity(const Json& arguments);
    ActionResult deleteEntity(const Json& arguments);
    ActionResult duplicateEntity(const Json& arguments);
    ActionResult reparentEntity(const Json& arguments);
    ActionResult moveEntityOrder(const Json& arguments);
    ActionResult selectEntities(const Json& arguments);
    ActionResult addComponent(const Json& arguments);
    ActionResult removeComponent(const Json& arguments);
    ActionResult setComponentProperty(const Json& arguments);
    ActionResult createScene(const Json& arguments);
    ActionResult renameScene(const Json& arguments);
    ActionResult saveScene(const Json& arguments);
    ActionResult saveAllScenes();
    ActionResult setStartScene(const Json& arguments);
    ActionResult setSceneProperty(const Json& arguments);
    ActionResult controlPlayMode(const Json& arguments);
    ActionResult addChildScene(const Json& arguments);
    ActionResult removeChildScene(const Json& arguments);
    ActionResult setChildSceneStartActive(const Json& arguments);
    ActionResult loadChildSceneInline(const Json& arguments);
    ActionResult createFolder(const Json& arguments);
    ActionResult renameResource(const Json& arguments);
    ActionResult deleteResource(const Json& arguments);
    ActionResult importResourceFile(const Json& arguments);
    ActionResult linkMaterial(const Json& arguments);
    ActionResult unlinkMaterial(const Json& arguments);
    ActionResult createMaterialFile(const Json& arguments);
    ActionResult setTerrainTextures(const Json& arguments);
    ActionResult createTerrainBlendmap(const Json& arguments);
    ActionResult addTerrainCollision(const Json& arguments);
    ActionResult createScript(const Json& arguments);
    ActionResult attachScript(const Json& arguments);
    ActionResult updateScriptFile(const Json& arguments);
    ActionResult createBundleFromEntity(const Json& arguments);
    ActionResult importBundleInstance(const Json& arguments);
    ActionResult addEntityToBundle(const Json& arguments);
    ActionResult removeEntityFromBundle(const Json& arguments);
    ActionResult makeBundleComponentUnique(const Json& arguments);
    ActionResult revertBundleComponent(const Json& arguments);
    ActionResult exportProject(const Json& arguments, const std::atomic<bool>* cancel);
    ActionResult generateShaders(const Json& arguments, const std::atomic<bool>* cancel);
    ActionResult createTerrainHeightmap(const Json& arguments);
    ActionResult importProjectModel(const Json& arguments);
    ActionResult searchCuratedAssets(const Json& arguments, const std::atomic<bool>* cancel);
    ActionResult downloadCuratedAsset(const Json& arguments, const std::atomic<bool>* cancel);
};

} // namespace doriax::editor::ai
