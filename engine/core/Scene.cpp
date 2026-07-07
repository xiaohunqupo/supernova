//
// (c) 2026 Eduardo Doria.
//

#include "Scene.h"

#include "object/Camera.h"
#include "Engine.h"

#include "object/Camera.h"

#include "subsystem/RenderSystem.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/UISystem.h"
#include "subsystem/ActionSystem.h"
#include "subsystem/AudioSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "util/Color.h"

using namespace doriax;

Scene::Scene() : EntityRegistry() {
    init();
}

Scene::Scene(EntityPool defaultPool) : EntityRegistry(defaultPool) {
    init();
}

void Scene::init(){
    registerSystem<ActionSystem>();
    registerSystem<MeshSystem>();
    registerSystem<UISystem>();
    registerSystem<RenderSystem>();
    registerSystem<PhysicsSystem>();
    registerSystem<AudioSystem>();

    camera = NULL_ENTITY;
    defaultCamera = NULL_ENTITY;

    backgroundColor = Vector4(0.0, 0.0, 0.0, 1.0); //sRGB
    shadowQuality = ShadowQuality::LOW;

    lightState = LightState::AUTO;
    globalIllumColor = Vector3(1.0, 1.0, 1.0);
    globalIllumIntensity = 0.1;

    // default 1.0 keeps 2D scenes visually unchanged until the user dims it
    ambientLight2DColor = Vector3(1.0, 1.0, 1.0);
    ambientLight2DIntensity = 1.0;
    shadow2DQuality = ShadowQuality::LOW;

    ssaoEnabled = false;
    ssaoRadius = 0.5;
    ssaoIntensity = 1.0;
    ssaoBias = 0.025;
    ssaoDebug = false;

    ssrEnabled = false;
    ssrMaxDistance = 8.0;
    ssrThickness = 0.5;
    ssrMaxSteps = 48;
    ssrIntensity = 1.0;
    ssrBlur = 0.0;
    ssrDebugMode = 0;

    fixedResolutionEnabled = false;
    fixedResolutionWidth = 640;
    fixedResolutionHeight = 360;
    fixedResolutionFilter = TextureFilter::NEAREST;

    uiEventState = UIEventState::NOT_SET;

    defaultMeshShader = "";
    defaultUIShader = "";
    defaultSkyShader = "";
    defaultPointsShader = "";
    defaultLinesShader = "";
}

Scene::~Scene(){
    destroy();
}

template<typename Component, typename Callback>
static void removeComponentSubscriptionsByTag(Scene* scene, const std::string& substring, Callback callback) {
    auto components = scene->getComponentArray<Component>();

    for (size_t i = 0; i < components->size(); i++) {
        callback(components->getComponentFromIndex(i), substring);
    }
}

void Scene::removeSubscriptionsByTag(const std::string& substring) {
    removeComponentSubscriptionsByTag<ActionComponent>(this, substring, [](ActionComponent& action, const std::string& tag) {
        action.onStart.removeByTagSubstring(tag);
        action.onPause.removeByTagSubstring(tag);
        action.onStop.removeByTagSubstring(tag);
        action.onStep.removeByTagSubstring(tag);
    });

    removeComponentSubscriptionsByTag<SoundComponent>(this, substring, [](SoundComponent& sound, const std::string& tag) {
        sound.onStart.removeByTagSubstring(tag);
        sound.onPause.removeByTagSubstring(tag);
        sound.onStop.removeByTagSubstring(tag);
    });

    removeComponentSubscriptionsByTag<UIComponent>(this, substring, [](UIComponent& ui, const std::string& tag) {
        ui.onGetFocus.removeByTagSubstring(tag);
        ui.onLostFocus.removeByTagSubstring(tag);
        ui.onPointerEnter.removeByTagSubstring(tag);
        ui.onPointerLeave.removeByTagSubstring(tag);
        ui.onPointerMove.removeByTagSubstring(tag);
        ui.onPointerDown.removeByTagSubstring(tag);
        ui.onPointerUp.removeByTagSubstring(tag);
        ui.onClick.removeByTagSubstring(tag);
        ui.onDoubleClick.removeByTagSubstring(tag);
        ui.onDragStart.removeByTagSubstring(tag);
        ui.onDrag.removeByTagSubstring(tag);
        ui.onDragEnd.removeByTagSubstring(tag);
    });

    removeComponentSubscriptionsByTag<ButtonComponent>(this, substring, [](ButtonComponent& button, const std::string& tag) {
        button.onPress.removeByTagSubstring(tag);
        button.onRelease.removeByTagSubstring(tag);
    });

    removeComponentSubscriptionsByTag<PanelComponent>(this, substring, [](PanelComponent& panel, const std::string& tag) {
        panel.onMove.removeByTagSubstring(tag);
        panel.onResize.removeByTagSubstring(tag);
    });

    removeComponentSubscriptionsByTag<ScrollbarComponent>(this, substring, [](ScrollbarComponent& scrollbar, const std::string& tag) {
        scrollbar.onChange.removeByTagSubstring(tag);
    });
}

void Scene::setCamera(Camera* camera){
    setCamera(camera->getEntity());
}

void Scene::setCamera(Entity camera){
    if (CameraComponent* cameracomp = findComponent<CameraComponent>(camera)){
        if (camera != this->camera){
            this->camera = camera;
            if (defaultCamera != NULL_ENTITY){
                destroyEntity(defaultCamera);
                defaultCamera = NULL_ENTITY;
            }
            cameracomp->needUpdate = true;
            updateCameraSize();
        }
    }else{
        Log::error("Invalid camera entity: need CameraComponent");
    }
}

Entity Scene::getCamera() const{
    return camera;
}

Entity Scene::createDefaultCamera(){
    defaultCamera = createSystemEntity();
    addComponent<CameraComponent>(defaultCamera);
    addComponent<Transform>(defaultCamera);

    CameraComponent& camera = getComponent<CameraComponent>(defaultCamera);
    camera.type = CameraType::CAMERA_UI;
    camera.transparentSort = false;

    Transform& cameratransform = getComponent<Transform>(defaultCamera);
    cameratransform.position = Vector3(0.0, 0.0, 1.0);

    return defaultCamera;
}

void Scene::setBackgroundColor(Vector4 color){
    this->backgroundColor = color;
}

void Scene::setBackgroundColor(float red, float green, float blue){
    setBackgroundColor(Vector4(red, green, blue, 1.0));
}

void Scene::setBackgroundColor(float red, float green, float blue, float alpha){
    setBackgroundColor(Vector4(red, green, blue, alpha));
}

Vector4 Scene::getBackgroundColor() const{
    return backgroundColor;
}

void Scene::setShadowQuality(ShadowQuality quality){
    // uniform-driven (no shader variant), so no mesh reload is needed
    this->shadowQuality = quality;
}

ShadowQuality Scene::getShadowQuality() const{
    return this->shadowQuality;
}

void Scene::setSSAOEnabled(bool ssaoEnabled){
    if (this->ssaoEnabled != ssaoEnabled){
        this->ssaoEnabled = ssaoEnabled;
        // toggling SSAO changes the mesh shader variant (USE_SSAO), so recompile
        getSystem<RenderSystem>()->needReloadMeshes();
    }
}

bool Scene::isSSAOEnabled() const{
    return this->ssaoEnabled;
}

void Scene::setSSAORadius(float radius){
    this->ssaoRadius = radius;
}

float Scene::getSSAORadius() const{
    return this->ssaoRadius;
}

void Scene::setSSAOIntensity(float intensity){
    this->ssaoIntensity = intensity;
}

float Scene::getSSAOIntensity() const{
    return this->ssaoIntensity;
}

void Scene::setSSAOBias(float bias){
    this->ssaoBias = bias;
}

float Scene::getSSAOBias() const{
    return this->ssaoBias;
}

void Scene::setSSAODebug(bool debug){
    this->ssaoDebug = debug;
}

bool Scene::isSSAODebug() const{
    return this->ssaoDebug;
}

void Scene::setSSREnabled(bool ssrEnabled){
    if (this->ssrEnabled != ssrEnabled){
        this->ssrEnabled = ssrEnabled;
        // SSR requires the depth pre-pass, so meshes must (re)build their depth shader
        getSystem<RenderSystem>()->needReloadMeshes();
    }
}

bool Scene::isSSREnabled() const{
    return this->ssrEnabled;
}

void Scene::setSSRMaxDistance(float maxDistance){
    this->ssrMaxDistance = maxDistance;
}

float Scene::getSSRMaxDistance() const{
    return this->ssrMaxDistance;
}

void Scene::setSSRThickness(float thickness){
    this->ssrThickness = thickness;
}

float Scene::getSSRThickness() const{
    return this->ssrThickness;
}

void Scene::setSSRMaxSteps(int maxSteps){
    this->ssrMaxSteps = maxSteps;
}

int Scene::getSSRMaxSteps() const{
    return this->ssrMaxSteps;
}

void Scene::setSSRIntensity(float intensity){
    this->ssrIntensity = intensity;
}

float Scene::getSSRIntensity() const{
    return this->ssrIntensity;
}

void Scene::setSSRBlur(float blur){
    this->ssrBlur = blur;
}

float Scene::getSSRBlur() const{
    return this->ssrBlur;
}

void Scene::setSSRDebugMode(int mode){
    this->ssrDebugMode = mode;
}

int Scene::getSSRDebugMode() const{
    return this->ssrDebugMode;
}

void Scene::setFixedResolutionEnabled(bool fixedResolutionEnabled){
    if (this->fixedResolutionEnabled != fixedResolutionEnabled){
        this->fixedResolutionEnabled = fixedResolutionEnabled;
        // the offscreen target renders with PIP_RTT; (re)bake it on all
        // renderables so a runtime toggle works in exported builds too
        getSystem<RenderSystem>()->needReloadMeshes();
        getSystem<RenderSystem>()->needReloadUIs();
        getSystem<RenderSystem>()->needReloadSky();
        getSystem<RenderSystem>()->needReloadPoints();
        getSystem<RenderSystem>()->needReloadLines();
    }
}

bool Scene::isFixedResolutionEnabled() const{
    return this->fixedResolutionEnabled;
}

void Scene::setFixedResolutionWidth(unsigned int width){
    this->fixedResolutionWidth = width;
}

unsigned int Scene::getFixedResolutionWidth() const{
    return this->fixedResolutionWidth;
}

void Scene::setFixedResolutionHeight(unsigned int height){
    this->fixedResolutionHeight = height;
}

unsigned int Scene::getFixedResolutionHeight() const{
    return this->fixedResolutionHeight;
}

void Scene::setFixedResolutionSize(unsigned int width, unsigned int height){
    this->fixedResolutionWidth = width;
    this->fixedResolutionHeight = height;
}

void Scene::setFixedResolutionFilter(TextureFilter filter){
    this->fixedResolutionFilter = filter;
}

TextureFilter Scene::getFixedResolutionFilter() const{
    return this->fixedResolutionFilter;
}

void Scene::setLightState(LightState state){
    if (this->lightState != state){
        this->lightState = state;
        getSystem<RenderSystem>()->needReloadMeshes();
    }
}

LightState Scene::getLightState() const{
    return this->lightState;
}

void Scene::setGlobalIllumination(float intensity, Vector3 color){
    this->globalIllumIntensity = intensity;
    this->globalIllumColor = Color::sRGBToLinear(color);
}

void Scene::setGlobalIllumination(float intensity){
    this->globalIllumIntensity = intensity;
}

void Scene::setGlobalIllumination(Vector3 color){
    this->globalIllumColor = Color::sRGBToLinear(color);
}

float Scene::getGlobalIlluminationIntensity() const{
    return this->globalIllumIntensity;
}

Vector3 Scene::getGlobalIlluminationColor() const{
    return Color::linearTosRGB(this->globalIllumColor);
}

Vector3 Scene::getGlobalIlluminationColorLinear() const{
    return this->globalIllumColor;
}

void Scene::setAmbientLight2D(float intensity, Vector3 color){
    this->ambientLight2DIntensity = intensity;
    this->ambientLight2DColor = Color::sRGBToLinear(color);
}

void Scene::setAmbientLight2D(float intensity){
    this->ambientLight2DIntensity = intensity;
}

void Scene::setAmbientLight2D(Vector3 color){
    this->ambientLight2DColor = Color::sRGBToLinear(color);
}

float Scene::getAmbientLight2DIntensity() const{
    return this->ambientLight2DIntensity;
}

Vector3 Scene::getAmbientLight2DColor() const{
    return Color::linearTosRGB(this->ambientLight2DColor);
}

Vector3 Scene::getAmbientLight2DColorLinear() const{
    return this->ambientLight2DColor;
}

void Scene::setShadow2DQuality(ShadowQuality quality){
    this->shadow2DQuality = quality;
}

ShadowQuality Scene::getShadow2DQuality() const{
    return this->shadow2DQuality;
}

void Scene::setDefaultMeshShader(const std::string& path){
    if (this->defaultMeshShader != path){
        this->defaultMeshShader = path;
        getSystem<RenderSystem>()->needReloadMeshes();
    }
}

const std::string& Scene::getDefaultMeshShader() const{
    return this->defaultMeshShader;
}

void Scene::setDefaultUIShader(const std::string& path){
    if (this->defaultUIShader != path){
        this->defaultUIShader = path;
        getSystem<RenderSystem>()->needReloadUIs();
    }
}

const std::string& Scene::getDefaultUIShader() const{
    return this->defaultUIShader;
}

void Scene::setDefaultSkyShader(const std::string& path){
    if (this->defaultSkyShader != path){
        this->defaultSkyShader = path;
        getSystem<RenderSystem>()->needReloadSky();
    }
}

const std::string& Scene::getDefaultSkyShader() const{
    return this->defaultSkyShader;
}

void Scene::setDefaultPointsShader(const std::string& path){
    if (this->defaultPointsShader != path){
        this->defaultPointsShader = path;
        getSystem<RenderSystem>()->needReloadPoints();
    }
}

const std::string& Scene::getDefaultPointsShader() const{
    return this->defaultPointsShader;
}

void Scene::setDefaultLinesShader(const std::string& path){
    if (this->defaultLinesShader != path){
        this->defaultLinesShader = path;
        getSystem<RenderSystem>()->needReloadLines();
    }
}

const std::string& Scene::getDefaultLinesShader() const{
    return this->defaultLinesShader;
}

void Scene::setDefaultCustomShader(ShaderType type, const std::string& path){
    switch (type){
        case ShaderType::MESH:   setDefaultMeshShader(path);   break;
        case ShaderType::UI:     setDefaultUIShader(path);     break;
        case ShaderType::SKYBOX: setDefaultSkyShader(path);    break;
        case ShaderType::POINTS: setDefaultPointsShader(path); break;
        case ShaderType::LINES:  setDefaultLinesShader(path);  break;
        default: break; // internal pass types cannot have scene defaults
    }
}

const std::string& Scene::getDefaultCustomShader(ShaderType type) const{
    static const std::string empty;
    switch (type){
        case ShaderType::MESH:   return this->defaultMeshShader;
        case ShaderType::UI:     return this->defaultUIShader;
        case ShaderType::SKYBOX: return this->defaultSkyShader;
        case ShaderType::POINTS: return this->defaultPointsShader;
        case ShaderType::LINES:  return this->defaultLinesShader;
        default: return empty;
    }
}

bool Scene::canReceiveUIEvents(){
    switch (this->uiEventState) {
        case UIEventState::ENABLED:
            return true;
        case UIEventState::DISABLED:
            return false;
        case UIEventState::NOT_SET:
        default:
            Signature cameraSignature = getSignature(getCamera());
            if (cameraSignature.test(getComponentId<CameraComponent>())){
                return getComponent<CameraComponent>(getCamera()).type == CameraType::CAMERA_UI;
            }

            return false;
    }
}

UIEventState Scene::getEnableUIEvents() const{
    return this->uiEventState;
}

void Scene::setEnableUIEvents(UIEventState enableUIEvents){
    this->uiEventState = enableUIEvents;
}

void Scene::enableUIEvents(){
    this->uiEventState = UIEventState::ENABLED;
}

bool Scene::isEnableUIEvents() const{
    return this->uiEventState != UIEventState::DISABLED;
}

void Scene::setEnableUIEvents(bool enableUIEvents){
    this->uiEventState = enableUIEvents ? UIEventState::ENABLED : UIEventState::DISABLED;
}

void Scene::load(){
    if (camera == NULL_ENTITY){
        camera = createDefaultCamera();
    }

    for (auto const &pair: systems) {
        if (Engine::isViewLoaded()){
            pair.second->load();
        }
    }
}

void Scene::destroy(){
    for (auto const& pair : systems){
        pair.second->destroy();
    }
}

void Scene::draw(){
    for (auto const& pair : systems){
        pair.second->draw();
    }
}


void Scene::update(double dt){
    for (auto const& pair : systems){
        pair.second->update(dt);
    }
}

void Scene::fixedUpdate(double dt){
    for (auto const& pair : systems){
        pair.second->fixedUpdate(dt);
    }
}

void Scene::updateCameraSize(){
    getSystem<RenderSystem>()->updateCameraSize(getCamera());
}
