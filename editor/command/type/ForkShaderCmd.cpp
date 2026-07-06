#include "ForkShaderCmd.h"

#include "command/type/PropertyCmd.h"
#include "command/type/ScenePropertyCmd.h"

#include <filesystem>
#include <system_error>

using namespace doriax;

editor::ForkShaderCmd::ForkShaderCmd(Project* project, uint32_t sceneId, Entity entity, ComponentType cpType,
                                     ShaderType shaderType, const std::string& desiredName) {
    this->project = project;
    this->plan = ProjectUtils::prepareShaderFork(project, shaderType, desiredName);

    if (plan.valid) {
        propertyCmd = std::make_unique<PropertyCmd<std::string>>(
            project, sceneId, entity, cpType, "customShader", plan.base);
    }
}

editor::ForkShaderCmd::ForkShaderCmd(Project* project, SceneProject* sceneProject, ShaderType shaderType,
                                     const std::string& scenePropertyName, const std::string& desiredName) {
    this->project = project;
    this->plan = ProjectUtils::prepareShaderFork(project, shaderType, desiredName);

    if (plan.valid) {
        propertyCmd = std::make_unique<ScenePropertyCmd<std::string>>(
            project, sceneProject->id, scenePropertyName, plan.base);
    }
}

bool editor::ForkShaderCmd::execute() {
    if (!plan.valid)
        return false;

    if (!ProjectUtils::writeShaderFork(plan))
        return false;

    if (propertyCmd && !propertyCmd->execute()) {
        std::error_code ec;
        std::filesystem::remove(plan.vertPath, ec);
        std::filesystem::remove(plan.fragPath, ec);
        return false;
    }

    return true;
}

void editor::ForkShaderCmd::undo() {
    if (propertyCmd)
        propertyCmd->undo();

    std::error_code ec;
    std::filesystem::remove(plan.vertPath, ec);
    std::filesystem::remove(plan.fragPath, ec);
}

bool editor::ForkShaderCmd::mergeWith(Command* otherCommand) {
    return false;
}
