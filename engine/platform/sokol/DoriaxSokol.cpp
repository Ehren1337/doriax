#include "DoriaxSokol.h"

#include "sokol_gfx.h"
#include "sokol_app.h"
#include "sokol_glue.h"

DoriaxSokol::DoriaxSokol(){

}

int DoriaxSokol::getScreenWidth(){
    return sapp_width();
}

int DoriaxSokol::getScreenHeight(){
    return sapp_height();
}

sg_environment DoriaxSokol::getSokolEnvironment(){
    return sglue_environment();
}

sg_swapchain DoriaxSokol::getSokolSwapchain(){
    return sglue_swapchain();
}

bool DoriaxSokol::isFullscreen(){
    return sapp_is_fullscreen();
}

void DoriaxSokol::requestFullscreen(){
    if (!sapp_is_fullscreen())
        sapp_toggle_fullscreen();
}

void DoriaxSokol::exitFullscreen(){
    if (sapp_is_fullscreen())
        sapp_toggle_fullscreen();
}

void DoriaxSokol::setWindowTitle(const std::string& title){
    sapp_set_window_title(title.c_str());
}

void DoriaxSokol::quit(){
    sapp_request_quit();
}

void DoriaxSokol::setMouseCursor(doriax::CursorType type){
    if (type == doriax::CursorType::ARROW){
        sapp_set_mouse_cursor(SAPP_MOUSECURSOR_ARROW);
    }else if (type == doriax::CursorType::IBEAM){
        sapp_set_mouse_cursor(SAPP_MOUSECURSOR_IBEAM);
    }else if (type == doriax::CursorType::CROSSHAIR){
        sapp_set_mouse_cursor(SAPP_MOUSECURSOR_CROSSHAIR);
    }else if (type == doriax::CursorType::POINTING_HAND){
        sapp_set_mouse_cursor(SAPP_MOUSECURSOR_POINTING_HAND);
    }else if (type == doriax::CursorType::RESIZE_EW){
        sapp_set_mouse_cursor(SAPP_MOUSECURSOR_RESIZE_EW);
    }else if (type == doriax::CursorType::RESIZE_NS){
        sapp_set_mouse_cursor(SAPP_MOUSECURSOR_RESIZE_NS);
    }else if (type == doriax::CursorType::RESIZE_NWSE){
        sapp_set_mouse_cursor(SAPP_MOUSECURSOR_RESIZE_NWSE);
    }else if (type == doriax::CursorType::RESIZE_NESW){
        sapp_set_mouse_cursor(SAPP_MOUSECURSOR_RESIZE_NESW);
    }else if (type == doriax::CursorType::RESIZE_ALL){
        sapp_set_mouse_cursor(SAPP_MOUSECURSOR_RESIZE_ALL);
    }else if (type == doriax::CursorType::NOT_ALLOWED){
        sapp_set_mouse_cursor(SAPP_MOUSECURSOR_NOT_ALLOWED);
    }
}

void DoriaxSokol::setMouseMode(doriax::MouseMode mode){
    switch (mode){
        case doriax::MouseMode::CAPTURED:
            sapp_lock_mouse(true);
            break;
        case doriax::MouseMode::HIDDEN:
            sapp_lock_mouse(false);
            sapp_show_mouse(false);
            break;
        case doriax::MouseMode::NORMAL:
        case doriax::MouseMode::CONFINED:
            // sokol_app has no cursor confinement, so CONFINED behaves as NORMAL.
            sapp_lock_mouse(false);
            sapp_show_mouse(true);
            break;
    }
}

std::string DoriaxSokol::getAssetPath(){
    return "assets";
}

std::string DoriaxSokol::getUserDataPath(){
    return ".";
}

std::string DoriaxSokol::getLuaPath(){
    return "lua";
}
