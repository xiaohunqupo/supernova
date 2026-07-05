#pragma once

#include "App.h"

namespace doriax::editor{
    class Backend{
    private:
        static App app;
        static std::string title;

    public:
        static int init(int argc, char **argv);

        static App& getApp();

        static void disableMouseCursor();
        static void enableMouseCursor();
        static void setShowCursor(bool showCursor);
        static void setGameCursorInSceneRect(bool inSceneRect);
        static void closeWindow();

        // True when the windowing platform can't reposition OS windows at runtime
        // (Wayland). In that case toggling multi-viewport needs an app restart to
        // take effect, since the backend is chosen at startup from the saved setting.
        static bool isRunningOnWayland();

        static void updateWindowTitle(const std::string& projectName);

        static void* getNFDWindowHandle();
    };

}
