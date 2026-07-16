#pragma once

#include "App.h"
#include "System.h"

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
        static void setMouseMode(MouseMode mode);
        // Temporarily hands the OS cursor back to the editor (visible + free) while a
        // play session is paused, loading, or its window is unfocused, without
        // discarding the game's requested mouse mode. The mode is reapplied when the
        // suspension is lifted (resume/refocus). Driven from the main loop.
        static void setMouseControlSuspended(bool suspended);
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
