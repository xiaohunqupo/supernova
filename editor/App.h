#ifndef EDITORAPP_H
#define EDITORAPP_H

#include "imgui.h"

#include "EditorHost.h"
#include "Project.h"

#include "window/Properties.h"
#include "window/Structure.h"
#include "window/OutputWindow.h"
#include "window/SceneWindow.h"
#include "window/ResourcesWindow.h"
#include "window/CodeEditor.h"
#include "window/AnimationWindow.h"
#include "window/TerrainEditWindow.h"

#include "window/LoadingWindow.h"

#include "window/dialog/ProjectSaveDialog.h"
#include "window/dialog/SceneSaveDialog.h"
#include "window/dialog/ExportWindow.h"
#include "window/dialog/ProjectSettingsWindow.h"

#include "render/SceneRender.h"

#include <mutex>
#include <queue>

namespace doriax::editor{

    enum class AlertType {
        Info,
        Confirm,
        ThreeButton
    };

    struct AlertData{
        bool needShow = false;
        std::string title;
        std::string message;
        AlertType type = AlertType::Info;
        std::function<void()> onYes = nullptr;
        std::function<void()> onNo = nullptr;
        std::function<void()> onCancel = nullptr;
    };

    enum class SaveDialogType {
        Scene,
        Project
    };

    struct SaveDialogQueueItem {
        SaveDialogType type;
        uint32_t sceneId;  // Only used for Scene dialogs
        std::function<void()> callback = nullptr;
    };

    class App : public EditorHost{
    private:
        Project project;

        std::mutex mainThreadTaskMutex;
        std::queue<std::function<void()>> mainThreadTasks;

        ImGuiID dockspace_id;
        ImGuiID dock_id_middle_top;

        static ImFont* codeFont;

        Structure* structureWindow;
        Properties* propertiesWindow;
        OutputWindow* outputWindow;
        SceneWindow* sceneWindow;
        CodeEditor* codeEditor;
        ResourcesWindow* resourcesWindow;
        AnimationWindow* animationWindow;
        TerrainEditWindow* terrainEditWindow;

        LoadingWindow* loadingWindow;

        bool isInitialized;
        bool dockspaceNeedsRebuild;

        AlertData alert;
        ProjectSaveDialog projectSaveDialog;
        SceneSaveDialog sceneSaveDialog;
        ExportWindow exportWindow;
        ProjectSettingsWindow projectSettingsWindow;

        std::queue<SaveDialogQueueItem> saveDialogQueue;
        bool saveDialogInProgress = false;
        std::filesystem::path lastResourcesProjectPath;

        std::vector<std::string> droppedExternalPaths;
        bool isDroppedExternalPaths;

        uint32_t lastActivatedScene;
        uint32_t pendingResizeScene = NULL_PROJECT_SCENE;

        enum class LastFocusedWindow {
            None,
            AnySceneWindow, // Represents any of the scene-related windows (Scene, Properties, Structure)
            Resources,
            Code
        } lastFocusedWindow;

        void saveFunc();
        void saveAllFunc(std::function<void(bool)> callback = nullptr);
        void saveAllAndProject(std::function<void()> onSuccess);
        void openProjectFunc();

        void showMenu();
        void showAlert();
        void showFooter();
        void showStyleEditor();
        void buildDockspace();
        void kewtStyleTheme();
        void processNextSaveDialog();
        bool popSaveDialogQueueItem();

        void closeWindow();

    public:
        void processMainThreadTasks();

    public:

        struct ThemeColors {
            static ImVec4 ButtonActivated;
            static ImVec4 FileCardBackground;
            static ImVec4 FileCardBackgroundHovered;
            static ImVec4 SubtleText;
            static ImVec4 filenameLabel;
            static ImVec4 ExtEntityButton;
            static ImVec4 ExtEntityButtonHovered;
            static ImVec4 ExtEntityButtonActive;
            static ImVec4 NestedHeader;
            static ImVec4 NestedHeaderHovered;
            static ImVec4 NestedHeaderActive;
        };

        App();

        void setup();

        void show();

        void engineInit(int argc, char** argv);
        void engineViewLoaded();
        void engineRender();
        void engineViewDestroyed();
        void engineShutdown();

        void addNewSceneToDock(uint32_t sceneId);
        void addNewCodeWindowToDock(fs::path path);
        void clearSceneWindowState(uint32_t sceneId);
        void prepareForProjectSwitch();

        void handleExternalDrop(const std::vector<std::string>& paths);
        void handleExternalDragEnter();
        void handleExternalDragLeave();

        void resetLastActivatedScene();
        bool shouldSyncEngineApi() const override;
        void updateResourcesPath();
        void requestDockspaceRebuild();
        void updateWindowTitle(const std::string& projectName) override;
        void stopTransientPreviews() override;
        void saveAllCodeEditors() override;

        void registerAlert(std::string title, std::string message);
        void registerConfirmAlert(std::string title, std::string message, std::function<void()> onYes, std::function<void()> onNo = nullptr);
        void registerThreeButtonAlert(std::string title, std::string message, std::function<void()> onYes, std::function<void()> onNo = nullptr, std::function<void()> onCancel = nullptr);
        void registerSaveSceneDialog(uint32_t sceneId, std::function<void()> callback = nullptr);
        void registerProjectSaveDialog(std::function<void()> callback = nullptr);

        // Thread-safe: schedules a task to run on the main/GL thread during the next frame.
        void enqueueMainThreadTask(std::function<void()> task);

        static std::filesystem::path getUserCacheBaseDir();
        static std::filesystem::path getUserShaderCacheDir();

        // Monospace font used by the code editor
        static ImFont* getCodeFont();

        // ImGui sizes fonts by their line box (ascent-descent) while CSS/VSCode use the em size.
        // JetBrains Mono's line box is (1020 - -300) / 1000upm = 1.32em; multiply a CSS-like
        // font size by this to get the equivalent ImGui font size.
        static constexpr float codeFontEmScale = 1.32f;

        // Tab notification helpers
        static void pushTabNotificationStyle();
        static void popTabNotificationStyle();

        Project* getProject();
        const Project* getProject() const;

        Properties* getPropertiesWindow() const;
        CodeEditor* getCodeEditor() const;
        ResourcesWindow* getResourcesWindow() const;
        AnimationWindow* getAnimationWindow() const;
        TerrainEditWindow* getTerrainEditWindow() const;

        // Window settings methods
        int getInitialWindowWidth() const;
        int getInitialWindowHeight() const;
        bool getInitialWindowMaximized() const;
        void saveWindowSettings(int width, int height, bool maximized);
        void initializeSettings();
        void exit();
    };

}

#endif /* EDITORAPP_H */