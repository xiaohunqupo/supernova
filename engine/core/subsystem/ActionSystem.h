//
// (c) 2026 Eduardo Doria.
//

#ifndef ACTIONSYSTEM_H
#define ACTIONSYSTEM_H

#include "SubSystem.h"

#include "component/Transform.h"
#include "component/SpriteComponent.h"
#include "component/MeshComponent.h"
#include "component/UILayoutComponent.h"
#include "component/UIComponent.h"
#include "component/ActionComponent.h"
#include "component/TimedActionComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "component/PositionActionComponent.h"
#include "component/RotationActionComponent.h"
#include "component/ScaleActionComponent.h"
#include "component/ColorActionComponent.h"
#include "component/AlphaActionComponent.h"
#include "component/PointsComponent.h"
#include "component/ParticlesComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/AnimationComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/TranslateTracksComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/MorphTracksComponent.h"

namespace doriax{

	class DORIAX_API ActionSystem : public SubSystem {

    private:

		void actionStateChange(Entity entity, ActionComponent& action);

		void actionComponentStart(ActionComponent& action);
		void actionComponentStop(ActionComponent& action);
		void actionComponentPause(ActionComponent& action);
		void actionUpdate(double dt, ActionComponent& action);

		void actionDestroy(ActionComponent& action);

		void animationUpdate(double dt, Entity entity, ActionComponent& action, AnimationComponent& animcomp);
		void animationDestroy(AnimationComponent& animcomp);

		// Sprite action functions
		void setSpriteTextureRect(MeshComponent& mesh, SpriteComponent& sprite, SpriteAnimationComponent& spriteanim);
		void spriteActionStart(MeshComponent& mesh, SpriteComponent& sprite, SpriteAnimationComponent& spriteanim);
		void spriteActionStop(MeshComponent& mesh, SpriteComponent& sprite, SpriteAnimationComponent& spriteanim);
		void spriteActionUpdate(double dt, Entity entity, ActionComponent& action, MeshComponent& mesh, SpriteComponent& sprite, SpriteAnimationComponent& spriteanim);

		void timedActionStop(TimedActionComponent& timedaction);
		void timedActionUpdate(double dt, Entity entity, ActionComponent& action, TimedActionComponent& timedaction);

		void positionActionUpdate(double dt, ActionComponent& action, TimedActionComponent& timedaction, PositionActionComponent& posaction, Transform& transform);
		void rotationActionUpdate(double dt, ActionComponent& action, TimedActionComponent& timedaction, RotationActionComponent& rotaction, Transform& transform);
		void scaleActionUpdate(double dt, ActionComponent& action, TimedActionComponent& timedaction, ScaleActionComponent& scaleaction, Transform& transform);

		void colorActionMeshUpdate(double dt, ActionComponent& action, TimedActionComponent& timedaction, ColorActionComponent& coloraction, MeshComponent& mesh);
		void colorActionUIUpdate(double dt, ActionComponent& action, TimedActionComponent& timedaction, ColorActionComponent& coloraction, UIComponent& ui);
		void alphaActionMeshUpdate(double dt, ActionComponent& action, TimedActionComponent& timedaction, AlphaActionComponent& alphaaction, MeshComponent& mesh);
		void alphaActionUIUpdate(double dt, ActionComponent& action, TimedActionComponent& timedaction, AlphaActionComponent& alphaaction, UIComponent& ui);

		//Particle helpers functions
		float getParticleInverseScale(float value);
		Vector3 getParticleDisplayScale(Vector3 scale, Transform* targetTransform);
		bool isParticleWorldSpace(ParticlesComponent& particles, Transform* targetTransform);
		void updateParticleTargetTransform(Transform& transform);
		void updateParticleTargetCache(ParticlesComponent& particles, Transform* targetTransform);
		Vector3 samplePositionInitializer(ParticlePositionInitializer& init);
		int sampleBurstCount(ParticleBurst& burst);
		void sortParticleBursts(ParticlesComponent& particles);
		void sortParticleColorGradient(ParticlesComponent& particles);
		Vector3 sampleParticleColorGradient(const ParticleColorGradient& gradient, float time, float life);
		float getParticleCycleDuration(ParticlesComponent& particles);
		Vector3 getParticleSimulationPosition(ParticlesComponent& particles, Transform* targetTransform, Vector3 position);
		Vector3 getParticleSimulationDirection(ParticlesComponent& particles, Transform* targetTransform, Vector3 direction);
		Quaternion getParticleSimulationRotation(ParticlesComponent& particles, Transform* targetTransform, Quaternion rotation);
		Vector3 getParticleDisplayPosition(ParticlesComponent& particles, Transform* targetTransform, Vector3 position);
		Quaternion getParticleDisplayRotation(ParticlesComponent& particles, Transform* targetTransform, Quaternion rotation);
		Vector3 getParticleDisplayScale(ParticlesComponent& particles, Transform* targetTransform, Vector3 scale);
		void syncParticleInstance(size_t idx, ParticlesComponent& particles, InstancedMeshComponent& instmesh, Transform* targetTransform);
		void syncParticlePoint(size_t idx, ParticlesComponent& particles, PointsComponent& points, Transform* targetTransform);

		int findUnusedParticle(ParticlesComponent& particles);

		float getFloatInitializerValue(float& min, float& max);
		Vector3 getVector3InitializerValue(Vector3& min, Vector3& max, bool linearSort);
		Quaternion getQuaternionInitializerValue(Quaternion& min, Quaternion& max, bool shortestPath);
		Rect getSpriteInitializerValue(std::vector<int>& frames, SpriteComponent& sprite);
		Rect getSpriteInitializerValue(std::vector<int>& frames, PointsComponent& points);
		void applyParticleInitializers(size_t idx, ParticlesComponent& particles, InstancedMeshComponent& instmesh, SpriteComponent* sprite, Transform* targetTransform);
		void applyParticleInitializers(size_t idx, ParticlesComponent& particles, PointsComponent& points, Transform* targetTransform);

		float getTimeFromParticleTime(float& time, float& fromTime, float& toTime);
		bool getParticleModifierValue(float& particleTime, float& fromTime, float& toTime, Ease& function, float& value);
		float getFloatModifierValue(float& value, float& fromValue, float& toValue);
		Vector3 getVector3ModifierValue(float& value, Vector3& fromValue, Vector3& toValue);
		Quaternion getQuaternionModifierValue(float& value, Quaternion& fromValue, Quaternion& toValue, bool shortestPath);
		Rect getSpriteModifierValue(float& value, std::vector<int>& frames, SpriteComponent& sprite);
		Rect getSpriteModifierValue(float& value, std::vector<int>& frames, PointsComponent& points);
		void applyParticleModifiers(size_t idx, ParticlesComponent& particles, InstancedMeshComponent& instmesh, SpriteComponent* sprite, Transform* targetTransform);
		void applyParticleModifiers(size_t idx, ParticlesComponent& particles, PointsComponent& points, Transform* targetTransform);
		void advanceParticle(size_t idx, float dt, ParticlesComponent& particles, InstancedMeshComponent& instmesh, SpriteComponent* sprite, Transform* targetTransform);
		void advanceParticle(size_t idx, float dt, ParticlesComponent& particles, PointsComponent& points, Transform* targetTransform);

		void particleActionStart(ParticlesComponent& particles, InstancedMeshComponent& instmesh, MeshComponent& mesh);
		void particleActionStart(ParticlesComponent& particles, PointsComponent& points);
		void particlesActionUpdate(double dt, Entity entity, Entity target, ActionComponent& action, ParticlesComponent& particles, InstancedMeshComponent& instmesh);
		void particlesActionUpdate(double dt, Entity entity, Entity target, ActionComponent& action, ParticlesComponent& particles, PointsComponent& points);

		//Keyframe
		void keyframeUpdate(double dt, ActionComponent& action, KeyframeTracksComponent& keyframe);
		void translateTracksUpdate(KeyframeTracksComponent& keyframe, TranslateTracksComponent& translatetracks, Transform& transform);
		void scaleTracksUpdate(KeyframeTracksComponent& keyframe, ScaleTracksComponent& scaletracks, Transform& transform);
		void rotateTracksUpdate(KeyframeTracksComponent& keyframe, RotateTracksComponent& rotatetracks, Transform& transform);
		void morphTracksUpdate(KeyframeTracksComponent& keyframe, MorphTracksComponent& morpthtracks, MeshComponent& mesh);

		void processRunningAction(double dt, Entity entity, ActionComponent& action);

	public:
		ActionSystem(Scene* scene);

		void actionStart(Entity entity);
		void actionStop(Entity entity);
		void actionPause(Entity entity);
		void particleActionReset(Entity entity);

		void updateAnimationPreview(double dt, Entity entity);
		void updateActionPreview(double dt, Entity entity);

		float getDuration(Entity entity);
	
		void load() override;
		void resetRunningActions();
		void draw() override;
		void destroy() override;
		void update(double dt) override;

		void onComponentAdded(Entity entity, ComponentId componentId) override;
		void onComponentRemoved(Entity entity, ComponentId componentId) override;
	};

}

#endif //ACTIONSYSTEM_H