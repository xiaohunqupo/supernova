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
        int m_windowModeIndex = 0;
        int m_windowWidth = 0;
        int m_windowHeight = 0;
        bool m_windowResizable = true;
        char m_windowTitleBuffer[256] = {0};
        std::string m_windowTitleOriginal;
        uint32_t m_startSceneId = NULL_PROJECT_SCENE;
        fs::path m_assetsDir;
        fs::path m_luaDir;
        fs::path m_shadersDir;
        fs::path m_shaderSourcesDir;
        std::vector<CMakeKit> m_availableKits;
        int m_cmakeKitIndex = 0;

        void drawSettings();
        void drawGeneralSettings();
        void drawCanvasSettings();
        void drawWindowSettings();
        void drawDirectoriesSettings();
        void drawBuildSettings();
        void applySettings();

    public:
        ProjectSettingsWindow() = default;
        ~ProjectSettingsWindow() = default;

        void open(Project* project);
        void show();
        bool isOpen() const { return m_isOpen; }
    };

}
