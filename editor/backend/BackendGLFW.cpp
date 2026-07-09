#include "Backend.h"
#include "EditorHost.h"
#include "AppSettings.h"

#include <GLFW/glfw3.h>

#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "nfd.hpp"
#include "nfd_glfw3.h"

#include <array>

static GLFWwindow* window = nullptr;
static GLFWcursor* invisibleCursor = nullptr;
static nfdwindowhandle_t nativeWindow;
static bool gameShowCursor = true;
static bool gameCursorInSceneRect = false;
static bool gameCursorHidden = false;

using namespace doriax;

editor::App editor::Backend::app;
std::string editor::Backend::title;

static void hideEditorCursor() {
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = false;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    if (invisibleCursor) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        glfwSetCursor(window, invisibleCursor);
    } else {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
    }
    gameCursorHidden = true;
}

static void showEditorCursor() {
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = false;

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    glfwSetCursor(window, nullptr);
    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    gameCursorHidden = false;
}

static void applyGameCursorVisibility(bool force = false) {
    const bool shouldHide = !gameShowCursor && gameCursorInSceneRect;
    if (!force && shouldHide == gameCursorHidden) {
        return;
    }

    if (shouldHide) {
        hideEditorCursor();
    } else {
        showEditorCursor();
    }
}

int editor::Backend::init(int argc, char* argv[]) {
    setEditorHost(&app);

    // Load settings early so the platform choice below can honor the persisted
    // multi-viewport preference (file I/O only, no window system needed).
    app.initializeSettings();

#ifdef __linux__
    // Dear ImGui multi-viewport needs to reposition OS windows, which the Wayland
    // backend forbids (glfwSetWindowPos is a no-op there). Only when the feature is
    // enabled, prefer X11 (works under XWayland) so dockable panels can be torn off.
    // Falls back to the default backend if X11 isn't available.
    if (AppSettings::getMultiViewportEnabled() && glfwPlatformSupported(GLFW_PLATFORM_X11))
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif

    // Initialize GLFW
    if (!glfwInit())
        return -1;

    if (NFD_Init() != NFD_OKAY) {
        printf("Error: NFD_Init failed: %s\n", NFD_GetError());
        return -1;
    }

    CameraRender render;

    int sampleCount = 1;

    glfwWindowHint(GLFW_SAMPLES, (sampleCount == 1) ? 0 : sampleCount);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // Get saved window dimensions from app
    int windowWidth = app.getInitialWindowWidth();
    int windowHeight = app.getInitialWindowHeight();

    // Create a windowed mode window and its OpenGL context
    window = glfwCreateWindow(windowWidth, windowHeight, "Doriax Engine", NULL, NULL);
    if (!window) {
        NFD_Quit();
        glfwTerminate();
        return -1;
    }

    // Apply saved window state
    if (app.getInitialWindowMaximized()) {
        glfwMaximizeWindow(window);
    }

    std::array<unsigned char, 16 * 16 * 4> cursorPixels = {};
    GLFWimage cursorImage;
    cursorImage.width = 16;
    cursorImage.height = 16;
    cursorImage.pixels = cursorPixels.data();
    invisibleCursor = glfwCreateCursor(&cursorImage, 0, 0);

    NFD_GetNativeWindowFromGLFWWindow(window, &nativeWindow);

    // Make the window's context current
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // Setup Dear ImGui context - MUST BE DONE BEFORE app.setup()
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    // Setup Platform/Renderer bindings - MUST BE DONE AFTER ImGui::CreateContext()
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 410");

    // Now we can safely call app.setup() which uses ImGui
    app.setup();
    app.engineInit(argc, argv);

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);

    app.engineViewLoaded();

    glfwSetDropCallback(window, [](GLFWwindow* window, int count, const char** paths) {
        std::vector<std::string> droppedPaths;
        for (int i = 0; i < count; i++) {
            droppedPaths.push_back(paths[i]);
        }
        app.handleExternalDrop(droppedPaths);
    });

    // Set window close callback to handle confirmation dialogs
    glfwSetWindowCloseCallback(window, [](GLFWwindow* w) {
        // Don't close window immediately - we'll handle this ourselves
        glfwSetWindowShouldClose(w, GLFW_FALSE);

        // Call the app's exit function which will handle unsaved changes
        app.exit();
    });

    // Main loop
    int currentSwapInterval = 1;
    bool prevFocused = true;
    while (!glfwWindowShouldClose(window)) {
        // Poll and handle events
        glfwPollEvents();

        // Skip presenting while iconified: SwapBuffers can block on Wayland and
        // stall clipboard + AI. Keep polling/updating so both keep working.
        const bool iconified = glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0;

        // With vsync on, SwapBuffers can block waiting for a frame callback the
        // compositor won't send while the window is unfocused/occluded, which
        // stalls the event loop that serves clipboard paste requests. Keep vsync
        // while focused (smooth, uncapped to refresh) and drop it when unfocused
        // so swap never blocks; a short sleep below then paces the idle loop.
        const bool focused = glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;
        if (focused != prevFocused) {
            const int desiredInterval = focused ? 1 : 0;
            if (desiredInterval != currentSwapInterval) {
                glfwSwapInterval(desiredInterval);
                currentSwapInterval = desiredInterval;
            }
            prevFocused = focused;
        }

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (!iconified) {
            app.engineRender();

            int display_w, display_h;
            glfwGetFramebufferSize(window, &display_w, &display_h);

            render.setClearColor(Vector4(0.45f, 0.55f, 0.60f, 1.00f));
            render.startRenderPass(display_w, display_h);
        } else {
            // engineRender() (skipped while minimized) is what normally drains
            // main-thread tasks. Keep draining them so AI-driven work, logging,
            // and async load callbacks still complete while minimized.
            app.processMainThreadTasks();
        }

        // Always run editor UI/update (AI agent pump, clipboard ownership via
        // continued event processing) even while the OS window is minimized.
        app.show();

        ImGui::Render();
        if (!iconified) {
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            render.endRenderPass();

            // Multi-viewport: render torn-off windows in their own GL contexts, then
            // restore the main context so sokol's next-frame pass runs on the right one.
            if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                GLFWwindow* backup_current_context = glfwGetCurrentContext();
                ImGui::UpdatePlatformWindows();
                ImGui::RenderPlatformWindowsDefault();
                glfwMakeContextCurrent(backup_current_context);
            }

            glfwSwapBuffers(window);

            // When unfocused vsync is off, so pace the loop to avoid a busy spin.
            if (!focused) {
                ImGui_ImplGlfw_Sleep(16);
            }
        } else {
            ImGui_ImplGlfw_Sleep(16);
        }
    }

    // Save window size and state before closing
    int width, height;
    glfwGetWindowSize(window, &width, &height);
    bool maximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED);

    // Save settings through app
    app.saveWindowSettings(width, height, maximized);

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    app.engineViewDestroyed();

    if (invisibleCursor) {
        glfwDestroyCursor(invisibleCursor);
        invisibleCursor = nullptr;
    }

    glfwDestroyWindow(window);
    NFD_Quit();
    glfwTerminate();

    app.engineShutdown();

    return 0;
}

editor::App& editor::Backend::getApp() {
    return app;
}

void editor::Backend::disableMouseCursor() {
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.MouseDrawCursor = false;

    if (invisibleCursor) {
        glfwSetCursor(window, invisibleCursor);
    }

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
}

void editor::Backend::enableMouseCursor() {
    applyGameCursorVisibility(true);
}

void editor::Backend::setShowCursor(bool showCursor) {
    gameShowCursor = showCursor;
    applyGameCursorVisibility();
}

void editor::Backend::setMouseLocked(bool mouseLocked) {
    if (mouseLocked) {
        disableMouseCursor();
    } else {
        enableMouseCursor();
    }
}

void editor::Backend::setGameCursorInSceneRect(bool inSceneRect) {
    gameCursorInSceneRect = inSceneRect;
    applyGameCursorVisibility();
}

void editor::Backend::closeWindow() {
    glfwSetWindowShouldClose(window, GLFW_TRUE);
}

bool editor::Backend::isRunningOnWayland() {
    return glfwGetPlatform() == GLFW_PLATFORM_WAYLAND;
}

void editor::Backend::updateWindowTitle(const std::string& projectName) {
    if (projectName.empty()) {
        title = "Empty project - Doriax Engine";
    } else {
        title = projectName + " - Doriax Engine";
    }
    if (window){
        glfwSetWindowTitle(window, title.c_str());
    }
}

void* editor::Backend::getNFDWindowHandle() {
    return &nativeWindow;
}
