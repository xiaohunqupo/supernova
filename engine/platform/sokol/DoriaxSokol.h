#ifndef DoriaxSokol_h
#define DoriaxSokol_h

#include "System.h"

class DoriaxSokol: public doriax::System{

public:

    DoriaxSokol();

    virtual int getScreenWidth() override;
    virtual int getScreenHeight() override;

    virtual sg_environment getSokolEnvironment() override;
    virtual sg_swapchain getSokolSwapchain() override;

    virtual bool isFullscreen() override;
    virtual void requestFullscreen() override;
    virtual void exitFullscreen() override;

    virtual void setMouseCursor(doriax::CursorType type) override;
    virtual void setShowCursor(bool showCursor) override;

    virtual std::string getAssetPath() override;
    virtual std::string getUserDataPath() override;
    virtual std::string getLuaPath() override;

};

#endif /* DoriaxSokol_h */
