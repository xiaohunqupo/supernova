#pragma once

#include "Project.h"
#include "imgui.h"

#include <string>
#include <filesystem>

namespace doriax::editor {

    namespace fs = std::filesystem;

    class ProjectSettingsWindow {
    private:
        bool m_isOpen = false;
        Project* m_project = nullptr;

        // UI state
        int m_canvasWidth = 0;
        int m_canvasHeight = 0;
        int m_scalingModeIndex = 0;
        int m_textureStrategyIndex = 0;
        bool m_vsyncEnabled = true;
        int m_startSceneIndex = 0;
        fs::path m_assetsDir;
        fs::path m_luaDir;
        fs::path m_shadersDir;
        fs::path m_shaderSourcesDir;
        std::vector<CMakeKit> m_availableKits;
        int m_cmakeKitIndex = 0;

        void drawSettings();

    public:
        ProjectSettingsWindow() = default;
        ~ProjectSettingsWindow() = default;

        void open(Project* project);
        void show();
        bool isOpen() const { return m_isOpen; }
    };

}
