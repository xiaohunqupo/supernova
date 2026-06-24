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
    ActionResult createEntity(const Json& arguments);
    ActionResult setEntityTransform(const Json& arguments);
    ActionResult importProjectModel(const Json& arguments);
    ActionResult searchCuratedAssets(const Json& arguments, const std::atomic<bool>* cancel);
    ActionResult downloadCuratedAsset(const Json& arguments, const std::atomic<bool>* cancel);
};

} // namespace doriax::editor::ai
