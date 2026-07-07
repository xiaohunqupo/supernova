#ifndef DoriaxWeb_h
#define DoriaxWeb_h

#include <emscripten/html5.h>

#include "System.h"

class DoriaxWeb: public doriax::System{

private:

    static std::string canvas;

    static int syncWaitTime;
    static bool enabledIDB;

    static int screenWidth;
    static int screenHeight;

    static double mousePosX;
    static double mousePosY;

    static int sampleCount;

    static EM_BOOL key_callback(int eventType, const EmscriptenKeyboardEvent *e, void *userData);
    static EM_BOOL mouse_callback(int eventType, const EmscriptenMouseEvent *e, void *userData);
    static EM_BOOL wheel_callback(int eventType, const EmscriptenWheelEvent *e, void *userData);
    static EM_BOOL touch_callback(int emsc_type, const EmscriptenTouchEvent* emsc_event, void* user_data);
    static EM_BOOL resize_callback(int event_type, const EmscriptenUiEvent* ui_event, void* user_data);

    static EM_BOOL canvas_resize(int eventType, const void *reserved, void *userData);
    static EM_BOOL webgl_context_callback(int emsc_type, const void* reserved, void* user_data);

    static EM_BOOL renderLoop(double time, void* userdata);

    static wchar_t toCodepoint(const std::string &u);
    static std::string toUTF8(wchar_t cp);
    static int doriax_mouse_button(int button);
    static int doriax_legacy_input(int code);
    static int doriax_input(const char code[32]);

public:

    DoriaxWeb();

    static void setEnabledIDB(bool enabledIDB);

    static int init(int argc, char **argv);
    static void changeCanvasSize(int width, int height);

    virtual int getScreenWidth() override;
    virtual int getScreenHeight() override;

    virtual int getSampleCount() override;

    virtual bool isFullscreen() override;
    virtual void requestFullscreen() override;
    virtual void exitFullscreen() override;

    virtual void setMouseCursor(doriax::CursorType type) override;
    virtual void setShowCursor(bool showCursor) override;
    virtual void setMouseLocked(bool mouseLocked) override;

    virtual std::string getUserDataPath() override;

    virtual bool syncFileSystem() override;

    virtual std::string getStringForKey(const char *key, const std::string& defaultValue) override;
    virtual void setStringForKey(const char* key, const std::string& value) override;
    virtual void removeKey(const char *key) override;

    virtual void initializeCrazyGamesSDK() override;
    virtual void showCrazyGamesAd(const std::string& type) override;
    virtual void happytimeCrazyGames() override;
    virtual void gameplayStartCrazyGames() override;
    virtual void gameplayStopCrazyGames() override;
    virtual void loadingStartCrazyGames() override;
    virtual void loadingStopCrazyGames() override;

};


#endif /* DoriaxWeb_h */
