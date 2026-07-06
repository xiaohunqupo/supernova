#pragma once

#include "command/Command.h"
#include "util/ProjectUtils.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <memory>
#include <string>

namespace doriax::editor {

    // Forks a built-in shader and points a component's customShader (or a scene's default
    // shader property) at the new fork as a single undoable step: execute() writes the
    // .vert/.frag files and sets the property; undo() restores the property and deletes
    // the forked files.
    class ForkShaderCmd : public Command {
    private:
        Project* project;
        ProjectUtils::ShaderForkPlan plan;
        std::unique_ptr<Command> propertyCmd;  // sets customShader (or scene property) to plan.base

    public:
        ForkShaderCmd(Project* project, uint32_t sceneId, Entity entity, ComponentType cpType,
                      ShaderType shaderType, const std::string& desiredName);

        // scene-level variant: sets a scene default shader property (e.g. "default_mesh_shader")
        ForkShaderCmd(Project* project, SceneProject* sceneProject, ShaderType shaderType,
                      const std::string& scenePropertyName, const std::string& desiredName);

        bool isValid() const { return plan.valid; }
        const std::string& getBase() const { return plan.base; }

        bool execute() override;
        void undo() override;
        bool mergeWith(Command* otherCommand) override;
    };

}
