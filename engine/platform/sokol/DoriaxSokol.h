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

    // sokol_app has no maximize/resize/resizable control; only the title
    // is overridable, the rest keep the base no-ops
    virtual void setWindowTitle(const std::string& title) override;
    virtual void quit() override;

    virtual void setMouseCursor(doriax::CursorType type) override;
    virtual void setMouseMode(doriax::MouseMode mode) override;

    virtual std::string getAssetPath() override;
    virtual std::string getUserDataPath() override;
    virtual std::string getLuaPath() override;

};

#endif /* DoriaxSokol_h */
