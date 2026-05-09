#include "ModelLoadCmd.h"

#include "Stream.h"
#include "util/ProjectUtils.h"
#include "subsystem/MeshSystem.h"
#include "io/FileData.h"
#include "Backend.h"

using namespace doriax;

editor::ModelLoadCmd::ModelLoadCmd(Project* project, uint32_t sceneId, Entity entity, const std::string& modelPath){
    this->project = project;
    this->sceneId = sceneId;
    this->entity = entity;
    this->modelPath = modelPath;

    this->wasModified = project->getScene(sceneId)->isModified;
}

editor::ModelLoadCmd::ModelLoadCmd(Project* project, uint32_t sceneId, const std::string& entityName, const Vector3& position, const std::string& modelPath){
    this->project = project;
    this->sceneId = sceneId;
    this->entity = NULL_ENTITY;
    this->modelPath = modelPath;

    this->wasModified = project->getScene(sceneId)->isModified;

    createEntityCmd = new CreateEntityCmd(project, sceneId, entityName, EntityCreationType::MODEL);
    createEntityCmd->addProperty<Vector3>(ComponentType::Transform, "position", position);
}

editor::ModelLoadCmd::~ModelLoadCmd(){
    if (cancelFlag) cancelFlag->store(true);
    if (asyncPending){
        Scene* scene = project->getScene(sceneId)->scene;
        scene->getSystem<MeshSystem>()->cancelAsyncModelLoad(entity, modelPath);
    }
    if (oldSubEntitiesDeleteCmd) {
        delete oldSubEntitiesDeleteCmd;
        oldSubEntitiesDeleteCmd = nullptr;
    }
    if (createEntityCmd) {
        delete createEntityCmd;
        createEntityCmd = nullptr;
    }
}

std::vector<Entity> editor::ModelLoadCmd::collectModelDeleteRoots(const ModelComponent& model) {
    std::vector<Entity> roots;
    if (model.skeleton != NULL_ENTITY) {
        roots.push_back(model.skeleton);
    }

    roots.insert(roots.end(), model.animations.begin(), model.animations.end());
    return roots;
}

bool editor::ModelLoadCmd::tryLoad(){
    Scene* scene = project->getScene(sceneId)->scene;
    std::shared_ptr<MeshSystem> meshSys = scene->getSystem<MeshSystem>();
    bool useAsync = MeshSystem::isAsyncModelLoading();
    std::string ext = FileData::getFilePathExtension(modelPath);
    if (ext == "obj"){
        return meshSys->loadOBJ(entity, modelPath, useAsync);
    }
    return meshSys->loadGLTF(entity, modelPath, useAsync, false, isNewModel);
}

void editor::ModelLoadCmd::finalizeLoad(){
    SceneProject* sceneProject = project->getScene(sceneId);
    Scene* scene = sceneProject->scene;

    ModelComponent& newModel = scene->getComponent<ModelComponent>(entity);
    newModel.needUpdateModel = false;

    std::vector<Entity> newSubEntities;
    ProjectUtils::collectModelEntities(scene, newModel, newSubEntities);
    for (const auto& e : newSubEntities){
        sceneProject->entities.push_back(e);
    }

    sceneProject->isModified = true;

    if (project->isEntityInBundle(sceneId, entity)){
        project->bundlePropertyChanged(sceneId, entity, ComponentType::ModelComponent, {"filename"});
    }
}

void editor::ModelLoadCmd::schedulePoll(){
    auto cancel = cancelFlag;
    Backend::getApp().enqueueMainThreadTask([this, cancel]() {
        if (cancel->load()) return;
        if (tryLoad()){
            asyncPending = false;
            finalizeLoad();
            return;
        }
        Scene* scene = project->getScene(sceneId)->scene;
        if (scene->getSystem<MeshSystem>()->isAsyncModelLoadPending(entity, modelPath)){
            schedulePoll();
        } else {
            // Terminal worker failure — drop pending state; rollback happens via the next undo
            asyncPending = false;
        }
    });
}

bool editor::ModelLoadCmd::execute(){
    if (createEntityCmd) {
        if (!createEntityCmd->execute()) {
            return false;
        }
        entity = createEntityCmd->getEntity();
    }

    SceneProject* sceneProject = project->getScene(sceneId);
    Scene* scene = sceneProject->scene;

    Signature signature = scene->getSignature(entity);
    if (!signature.test(scene->getComponentId<Transform>())){
        Log::error("Entity %lu does not have a Transform component", entity);
        return false;
    }
    if (!signature.test(scene->getComponentId<MeshComponent>())){
        Log::error("Entity %lu does not have a MeshComponent", entity);
        return false;
    }
    if (!signature.test(scene->getComponentId<ModelComponent>())){
        Log::error("Entity %lu does not have a ModelComponent", entity);
        return false;
    }

    Transform& transform = scene->getComponent<Transform>(entity);
    MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
    ModelComponent& model = scene->getComponent<ModelComponent>(entity);

    isNewModel = model.filename.empty();

    // Save old component state
    oldTransform = Stream::encodeTransform(transform);
    oldMesh = Stream::encodeMeshComponent(mesh, false, false);
    oldModel = Stream::encodeModelComponent(model);

    std::vector<Entity> oldSubEntityRoots = collectModelDeleteRoots(model);
    if (!oldSubEntityRoots.empty()) {
        oldSubEntitiesDeleteCmd = new DeleteEntityCmd(project, sceneId, oldSubEntityRoots, true);
        if (!oldSubEntitiesDeleteCmd->execute()) {
            delete oldSubEntitiesDeleteCmd;
            oldSubEntitiesDeleteCmd = nullptr;
            return false;
        }
    }

    // Clear stale model data before loading new model
    model.skeleton = NULL_ENTITY;
    model.bonesIdMapping.clear();
    model.bonesNameMapping.clear();
    model.animations.clear();

    if (tryLoad()){
        finalizeLoad();
        return true;
    }

    if (MeshSystem::isAsyncModelLoading()){
        // Load is in progress on a worker thread — accept the command and finalize when ready
        asyncPending = true;
        cancelFlag = std::make_shared<std::atomic<bool>>(false);
        schedulePoll();
        return true;
    }

    // Synchronous failure — restore old state
    scene->getComponent<Transform>(entity) = Stream::decodeTransform(oldTransform);
    scene->getComponent<MeshComponent>(entity) = Stream::decodeMeshComponent(oldMesh);
    scene->getComponent<ModelComponent>(entity) = Stream::decodeModelComponent(oldModel);
    if (oldSubEntitiesDeleteCmd) {
        oldSubEntitiesDeleteCmd->undo();
        delete oldSubEntitiesDeleteCmd;
        oldSubEntitiesDeleteCmd = nullptr;
    }
    if (createEntityCmd) {
        createEntityCmd->undo();
    }
    return false;
}

void editor::ModelLoadCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);
    Scene* scene = sceneProject->scene;

    if (asyncPending) {
        // Async load still in flight — cancel it; no sub-entities have been created yet
        if (cancelFlag) cancelFlag->store(true);
        scene->getSystem<MeshSystem>()->cancelAsyncModelLoad(entity, modelPath);
        asyncPending = false;
    } else {
        ModelComponent& model = scene->getComponent<ModelComponent>(entity);
        std::vector<Entity> newSubEntityRoots = collectModelDeleteRoots(model);
        if (!newSubEntityRoots.empty()) {
            DeleteEntityCmd newSubEntitiesDeleteCmd(project, sceneId, newSubEntityRoots, true);
            newSubEntitiesDeleteCmd.execute();
        }
    }

    // Restore old components
    scene->getComponent<Transform>(entity) = Stream::decodeTransform(oldTransform);
    scene->getComponent<MeshComponent>(entity) = Stream::decodeMeshComponent(oldMesh);
    scene->getComponent<ModelComponent>(entity) = Stream::decodeModelComponent(oldModel);

    if (oldSubEntitiesDeleteCmd) {
        oldSubEntitiesDeleteCmd->undo();
        delete oldSubEntitiesDeleteCmd;
        oldSubEntitiesDeleteCmd = nullptr;
    }

    sceneProject->isModified = wasModified;

    if (project->isEntityInBundle(sceneId, entity)){
        project->bundlePropertyChanged(sceneId, entity, ComponentType::ModelComponent, {"filename"});
    }

    if (createEntityCmd) {
        createEntityCmd->undo();
        entity = NULL_ENTITY;
    }
}

bool editor::ModelLoadCmd::mergeWith(editor::Command* otherCommand){
    return false;
}
