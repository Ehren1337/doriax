//
// (c) 2026 Eduardo Doria.
//

#ifndef DoriaxMac_h
#define DoriaxMac_h

// Entry point of the macOS backend for exported games, on both graphic
// backends: SOKOL_METAL picks the drawable, a MacViewMetal or a MacViewGL. The
// window is WindowMac and the doriax::System surface is SystemMac, both shared
// with the editor, so what is left here is the frame loop and the build settings.
class DoriaxMac {
public:

    static int init(int argc, char **argv);
};

#endif /* DoriaxMac_h */
