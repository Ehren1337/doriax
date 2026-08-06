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

    typedef struct GamepadState{
        bool connected = false;
        unsigned char buttons[GLFW_GAMEPAD_BUTTON_LAST + 1] = {0};
        float axes[GLFW_GAMEPAD_AXIS_LAST + 1] = {0.0f};
    } GamepadState;

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

    static GamepadState gamepads[GLFW_JOYSTICK_LAST + 1];

    static void pollGamepads();

public:

    DoriaxGLFW();

    static int init(int argc, char **argv);

    virtual int getScreenWidth() override;
    virtual int getScreenHeight() override;

    virtual int getSampleCount() override;

    virtual bool isFullscreen() override;
    virtual void requestFullscreen() override;
    virtual void exitFullscreen() override;

    virtual bool isWindowMaximized() override;
    virtual void maximizeWindow() override;
    virtual void restoreWindow() override;
    virtual void setWindowSize(int width, int height) override;
    virtual bool isWindowResizable() override;
    virtual void setWindowResizable(bool resizable) override;
    virtual void setWindowTitle(const std::string& title) override;
    virtual void quit() override;

    virtual void setMouseCursor(doriax::CursorType type) override;
    virtual void setMouseMode(doriax::MouseMode mode) override;
    virtual void setMousePosition(float x, float y) override;

    virtual std::string getAssetPath() override;
    virtual std::string getUserDataPath() override;
    virtual std::string getLuaPath() override;

};


#endif /* DoriaxGLFW_h */
