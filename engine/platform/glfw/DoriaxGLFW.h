//
// (c) 2026 Eduardo Doria.
//

#ifndef DoriaxGLFW_h
#define DoriaxGLFW_h

#define GLFW_INCLUDE_NONE
#include "GLFW/glfw3.h"

#include "System.h"

class DoriaxGLFW: public doriax::System{

private:

    static int windowPosX;
    static int windowPosY;
    static int windowWidth;
    static int windowHeight;

    static int screenWidth;
    static int screenHeight;

    static double mousePosX;
    static double mousePosY;

    static int sampleCount;

    static GLFWwindow* window;
    static GLFWmonitor* monitor;

public:

    DoriaxGLFW();

    static int init(int argc, char **argv);

    virtual int getScreenWidth() override;
    virtual int getScreenHeight() override;

    virtual int getSampleCount() override;

    virtual bool isFullscreen() override;
    virtual void requestFullscreen() override;
    virtual void exitFullscreen() override;

    virtual void setMouseCursor(doriax::CursorType type) override;
    virtual void setShowCursor(bool showCursor) override;

    virtual std::string getAssetPath() override;
    virtual std::string getUserDataPath() override;
    virtual std::string getLuaPath() override;

};


#endif /* DoriaxGLFW_h */
