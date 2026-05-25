#pragma once

#include "Project.h"
#include "shader/ShaderBuilder.h"
#include "ShaderDataSerializer.h"

#include <filesystem>
#include <set>
#include <vector>
#include <string>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>

namespace doriax::editor {

    namespace fs = std::filesystem;

    struct ExportConfig {
        fs::path targetDir;
        fs::path assetsDir;
        fs::path luaDir;
        uint32_t startSceneId = 0;
        std::set<ShaderKey> selectedShaderKeys;
        std::set<::doriax::Platform> selectedPlatforms;
        fs::path shaderOutputDir;
    };

    struct ExportProgress {
        std::string currentStep;
        float overallProgress = 0.0f;
        bool started = false;
        bool finished = false;
        bool failed = false;
        std::string errorMessage;
    };

    class Exporter {
    private:
        Project* project = nullptr;
        ExportConfig config;
        ExportProgress progress;
        mutable std::mutex progressMutex;
        std::atomic<bool> cancelRequested{false};

        std::thread exportThread;

        ShaderBuilder shaderBuilder;

        void setProgress(const std::string& step, float value);
        void setError(const std::string& message);
        void runExport();

        bool isCancelled() const;

        void collectSelectedShaderKeys();

        fs::path getExportProjectRoot() const;
        fs::path getShaderOutputDir() const;
        static bool shouldSkipExportSupportFile(const fs::path& relativePath);

        bool checkTargetDir();
        bool clearGenerated();
        bool loadAndSaveAllScenes();
        bool copyGenerated();
        bool copyAssets();
        bool copyLua();
        bool copyCppScripts();
        bool copyEngine();
        bool buildAndSaveShaders();

    public:
        Exporter();
        ~Exporter();

        void startExport(Project* project, const ExportConfig& config);
        bool exportProject(Project* project, const ExportConfig& config);
        bool generateShaders(const ExportConfig& config);
        void cancelExport();
        ExportProgress getProgress() const;
        bool isRunning() const;

        static std::string getShaderDisplayName(ShaderType type, uint32_t properties);
        static std::string getPlatformName(::doriax::Platform platform);
    };

}
