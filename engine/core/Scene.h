//
// (c) 2026 Eduardo Doria.
//

#ifndef SCENE_H
#define SCENE_H

#include "Entity.h"
#include "SubSystem.h"
#include "EntityRegistry.h"
#include "Export.h"
#include "render/Render.h"
#include <string>
#include <vector>


namespace doriax{

    class Camera;

    enum class LightState {
        OFF,
        ON,
        AUTO
    };

    // PCF filter quality of shadow edges, uniform-driven (no shader rebuild).
    // 3D shadows (shadowQuality): NONE = 1 tap, LOW = 3x3, MEDIUM = 5x5, HIGH = 7x7.
    // 2D shadows (shadow2DQuality): NONE = 1 tap, LOW = 5, MEDIUM = 9, HIGH = 13 taps
    // along the 1D polar map, smoothing the penumbra set by Light2D shadowSoftness.
    enum class ShadowQuality {
        NONE,   // 1 tap, no filtering (hard edges)
        LOW,
        MEDIUM,
        HIGH
    };

    enum class UIEventState {
        NOT_SET,
        ENABLED,
        DISABLED
    };

    class DORIAX_API Scene : public EntityRegistry {
    private:

        Entity camera;
        Entity defaultCamera;

        Vector4 backgroundColor;
        ShadowQuality shadowQuality;

        LightState lightState;
        Vector3 globalIllumColor;
        float globalIllumIntensity;

        Vector3 ambientLight2DColor;
        float ambientLight2DIntensity;
        ShadowQuality shadow2DQuality;

        bool ssaoEnabled;
        float ssaoRadius;
        float ssaoIntensity;
        float ssaoBias;
        bool ssaoDebug;

        bool ssrEnabled;
        float ssrMaxDistance;   // max ray length in view-space units
        float ssrThickness;     // depth-compare tolerance (view-space units)
        int ssrMaxSteps;        // linear march sample count
        float ssrIntensity;     // reflection strength multiplier
        float ssrBlur;          // glossy blur amount [0..1] (0 = sharp/mirror)
        int ssrDebugMode;       // 0=off,1=reflection,2=normal,3=roughness,4=metallic,5=albedo,6=IBL specular

        // fixed game resolution: when enabled and this scene is the Engine main
        // scene, the main camera renders into an offscreen framebuffer of this
        // size, upscaled to the view rect by RenderSystem
        bool fixedResolutionEnabled;
        unsigned int fixedResolutionWidth;
        unsigned int fixedResolutionHeight;
        TextureFilter fixedResolutionFilter;

        UIEventState uiEventState;

        // scene-wide custom shaders, used when a component's customShader is empty
        // (project-relative base or "a.vert|b.frag"; empty = engine built-in)
        std::string defaultMeshShader;
        std::string defaultUIShader;
        std::string defaultSkyShader;
        std::string defaultPointsShader;
        std::string defaultLinesShader;

        std::vector<std::pair<std::string, std::shared_ptr<SubSystem>>> systems;

        void init();
        Entity createDefaultCamera();

    protected:

        void onComponentAdded(Entity entity, ComponentId componentId) override {
            for (auto const& pair : systems) {
                pair.second->onComponentAdded(entity, componentId);
            }
        }

        void onComponentRemoved(Entity entity, ComponentId componentId) override {
            for (auto const& pair : systems) {
                pair.second->onComponentRemoved(entity, componentId);
            }
        }

        void onEntityDestroyed(Entity entity, Signature signature) override {
            for (auto const& pair : systems) {
                for (ComponentId componentId = 0; componentId < signature.size(); ++componentId) {
                    if (signature.test(componentId)) {
                        pair.second->onComponentRemoved(entity, componentId);
                    }
                }
            }
        }

    public:

        Scene();
        Scene(EntityPool defaultPool);
        virtual ~Scene();

        void load();
        void destroy();
        void draw();
        void update(double dt);
        void fixedUpdate(double dt);

        void removeSubscriptionsByTag(const std::string& substring);

        void updateCameraSize();

        void setCamera(Camera* camera);
        void setCamera(Entity camera);
        Entity getCamera() const;

        void setBackgroundColor(Vector4 color);
        void setBackgroundColor(float red, float green, float blue);
        void setBackgroundColor(float red, float green, float blue, float alpha);
        Vector4 getBackgroundColor() const;

        void setShadowQuality(ShadowQuality quality);
        ShadowQuality getShadowQuality() const;

        void setSSAOEnabled(bool ssaoEnabled);
        bool isSSAOEnabled() const;

        void setSSAORadius(float radius);
        float getSSAORadius() const;

        void setSSAOIntensity(float intensity);
        float getSSAOIntensity() const;

        void setSSAOBias(float bias);
        float getSSAOBias() const;

        void setSSAODebug(bool debug);
        bool isSSAODebug() const;

        void setSSREnabled(bool ssrEnabled);
        bool isSSREnabled() const;

        void setSSRMaxDistance(float maxDistance);
        float getSSRMaxDistance() const;

        void setSSRThickness(float thickness);
        float getSSRThickness() const;

        void setSSRMaxSteps(int maxSteps);
        int getSSRMaxSteps() const;

        void setSSRIntensity(float intensity);
        float getSSRIntensity() const;

        void setSSRBlur(float blur);
        float getSSRBlur() const;

        void setSSRDebugMode(int mode);
        int getSSRDebugMode() const;

        void setFixedResolutionEnabled(bool fixedResolutionEnabled);
        bool isFixedResolutionEnabled() const;

        void setFixedResolutionWidth(unsigned int width);
        unsigned int getFixedResolutionWidth() const;

        void setFixedResolutionHeight(unsigned int height);
        unsigned int getFixedResolutionHeight() const;

        void setFixedResolutionSize(unsigned int width, unsigned int height);

        void setFixedResolutionFilter(TextureFilter filter);
        TextureFilter getFixedResolutionFilter() const;

        void setLightState(LightState state);
        LightState getLightState() const;

        void setGlobalIllumination(float intensity, Vector3 color);
        void setGlobalIllumination(float intensity);
        void setGlobalIllumination(Vector3 color);

        float getGlobalIlluminationIntensity() const;
        Vector3 getGlobalIlluminationColor() const;
        Vector3 getGlobalIlluminationColorLinear() const;

        void setAmbientLight2D(float intensity, Vector3 color);
        void setAmbientLight2D(float intensity);
        void setAmbientLight2D(Vector3 color);

        float getAmbientLight2DIntensity() const;
        Vector3 getAmbientLight2DColor() const;
        Vector3 getAmbientLight2DColorLinear() const;

        void setShadow2DQuality(ShadowQuality quality);
        ShadowQuality getShadow2DQuality() const;

        void setDefaultMeshShader(const std::string& path);
        const std::string& getDefaultMeshShader() const;

        void setDefaultUIShader(const std::string& path);
        const std::string& getDefaultUIShader() const;

        void setDefaultSkyShader(const std::string& path);
        const std::string& getDefaultSkyShader() const;

        void setDefaultPointsShader(const std::string& path);
        const std::string& getDefaultPointsShader() const;

        void setDefaultLinesShader(const std::string& path);
        const std::string& getDefaultLinesShader() const;

        void setDefaultCustomShader(ShaderType type, const std::string& path);
        const std::string& getDefaultCustomShader(ShaderType type) const;

        bool canReceiveUIEvents();

        UIEventState getEnableUIEvents() const;
        void setEnableUIEvents(UIEventState enableUIEvents);
        void enableUIEvents();

        bool isEnableUIEvents() const;
        void setEnableUIEvents(bool enableUIEvents);

        // System methods

        template<typename T>
        std::shared_ptr<T> registerSystem(){
            const char* typeName = typeid(T).name();

            auto it = std::find_if(systems.begin(), systems.end(), [&](const auto& pair) { return pair.first == typeName; });

            assert(it == systems.end() && "Registering system more than once");

            auto system = std::make_shared<T>(this);
            systems.push_back(std::make_pair(typeName, system));
            return system;
        }

        template<typename T>
        std::shared_ptr<T> getSystem(){
            const char* typeName = typeid(T).name();

            auto it = std::find_if(systems.begin(), systems.end(), [&](const auto& pair) { return pair.first == typeName; });

            assert(it != systems.end() && "System not found.");

            return std::dynamic_pointer_cast<T>(it->second);
        }
    };

}

#endif //SCENE_H