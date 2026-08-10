//
// (c) 2026 Eduardo Doria.
//
// X11 -> engine key translation. The engine's key constants are GLFW's (see
// core/Input.h), so this maps the keysym of the unshifted level-0 layout,
// which is how GLFW's X11 backend derives its key codes: it keeps letters on
// their printed key regardless of Shift or the active layout.

#ifndef KeyCodesLinux_h
#define KeyCodesLinux_h

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include "Input.h"

namespace doriax {

    inline int linuxKeyFromSym(KeySym key) {
        // Letters and digits share the ASCII range with the engine constants
        if (key >= XK_a && key <= XK_z) return D_KEY_A + int(key - XK_a);
        if (key >= XK_A && key <= XK_Z) return D_KEY_A + int(key - XK_A);
        if (key >= XK_0 && key <= XK_9) return D_KEY_0 + int(key - XK_0);
        if (key >= XK_F1 && key <= XK_F25) return D_KEY_F1 + int(key - XK_F1);

        switch (key) {
            case XK_space:          return D_KEY_SPACE;
            case XK_apostrophe:     return D_KEY_APOSTROPHE;
            case XK_comma:          return D_KEY_COMMA;
            case XK_minus:          return D_KEY_MINUS;
            case XK_period:         return D_KEY_PERIOD;
            case XK_slash:          return D_KEY_SLASH;
            case XK_semicolon:      return D_KEY_SEMICOLON;
            case XK_equal:          return D_KEY_EQUAL;
            case XK_bracketleft:    return D_KEY_LEFT_BRACKET;
            case XK_backslash:      return D_KEY_BACKSLASH;
            case XK_bracketright:   return D_KEY_RIGHT_BRACKET;
            case XK_grave:          return D_KEY_GRAVE_ACCENT;

            case XK_Escape:         return D_KEY_ESCAPE;
            case XK_Return:         return D_KEY_ENTER;
            case XK_Tab:            return D_KEY_TAB;
            case XK_BackSpace:      return D_KEY_BACKSPACE;
            case XK_Insert:         return D_KEY_INSERT;
            case XK_Delete:         return D_KEY_DELETE;
            case XK_Right:          return D_KEY_RIGHT;
            case XK_Left:           return D_KEY_LEFT;
            case XK_Down:           return D_KEY_DOWN;
            case XK_Up:             return D_KEY_UP;
            case XK_Page_Up:        return D_KEY_PAGE_UP;
            case XK_Page_Down:      return D_KEY_PAGE_DOWN;
            case XK_Home:           return D_KEY_HOME;
            case XK_End:            return D_KEY_END;
            case XK_Caps_Lock:      return D_KEY_CAPS_LOCK;
            case XK_Scroll_Lock:    return D_KEY_SCROLL_LOCK;
            case XK_Num_Lock:       return D_KEY_NUM_LOCK;
            case XK_Print:          return D_KEY_PRINT_SCREEN;
            case XK_Pause:          return D_KEY_PAUSE;

            case XK_KP_0:           return D_KEY_KP_0;
            case XK_KP_1:           return D_KEY_KP_1;
            case XK_KP_2:           return D_KEY_KP_2;
            case XK_KP_3:           return D_KEY_KP_3;
            case XK_KP_4:           return D_KEY_KP_4;
            case XK_KP_5:           return D_KEY_KP_5;
            case XK_KP_6:           return D_KEY_KP_6;
            case XK_KP_7:           return D_KEY_KP_7;
            case XK_KP_8:           return D_KEY_KP_8;
            case XK_KP_9:           return D_KEY_KP_9;
            // With NumLock off the keypad reports its navigation function
            case XK_KP_Insert:      return D_KEY_KP_0;
            case XK_KP_End:         return D_KEY_KP_1;
            case XK_KP_Down:        return D_KEY_KP_2;
            case XK_KP_Next:        return D_KEY_KP_3;
            case XK_KP_Left:        return D_KEY_KP_4;
            case XK_KP_Begin:       return D_KEY_KP_5;
            case XK_KP_Right:       return D_KEY_KP_6;
            case XK_KP_Home:        return D_KEY_KP_7;
            case XK_KP_Up:          return D_KEY_KP_8;
            case XK_KP_Prior:       return D_KEY_KP_9;
            case XK_KP_Delete:      return D_KEY_KP_DECIMAL;
            case XK_KP_Decimal:     return D_KEY_KP_DECIMAL;
            case XK_KP_Divide:      return D_KEY_KP_DIVIDE;
            case XK_KP_Multiply:    return D_KEY_KP_MULTIPLY;
            case XK_KP_Subtract:    return D_KEY_KP_SUBTRACT;
            case XK_KP_Add:         return D_KEY_KP_ADD;
            case XK_KP_Enter:       return D_KEY_KP_ENTER;
            case XK_KP_Equal:       return D_KEY_KP_EQUAL;

            case XK_Shift_L:        return D_KEY_LEFT_SHIFT;
            case XK_Control_L:      return D_KEY_LEFT_CONTROL;
            case XK_Alt_L:          return D_KEY_LEFT_ALT;
            case XK_Super_L:        return D_KEY_LEFT_SUPER;
            case XK_Shift_R:        return D_KEY_RIGHT_SHIFT;
            case XK_Control_R:      return D_KEY_RIGHT_CONTROL;
            case XK_Alt_R:
            case XK_ISO_Level3_Shift: return D_KEY_RIGHT_ALT;
            case XK_Super_R:        return D_KEY_RIGHT_SUPER;
            case XK_Menu:           return D_KEY_MENU;

            default:                return D_KEY_UNKNOWN;
        }
    }

    // GLFW-compatible modifier bits from an X event state mask
    inline int linuxKeyModifiers(unsigned int state) {
        int mods = 0;
        if (state & ShiftMask)   mods |= D_MODIFIER_SHIFT;
        if (state & ControlMask) mods |= D_MODIFIER_CONTROL;
        if (state & Mod1Mask)    mods |= D_MODIFIER_ALT;
        if (state & Mod4Mask)    mods |= D_MODIFIER_SUPER;
        if (state & LockMask)    mods |= D_MODIFIER_CAPS_LOCK;
        if (state & Mod2Mask)    mods |= D_MODIFIER_NUM_LOCK;
        return mods;
    }

}

#endif /* KeyCodesLinux_h */
