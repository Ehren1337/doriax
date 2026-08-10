//
// (c) 2026 Eduardo Doria.
//

#ifndef DoriaxMac_h
#define DoriaxMac_h

// Entry point of the macOS OpenGL backend for exported games; Metal games run
// on engine/platform/apple, which MTKView ties to Metal. The window is
// WindowMac and the doriax::System surface is SystemMac, both shared with the
// editor, so what is left here is the frame loop and the build settings.
class DoriaxMac {
public:

    static int init(int argc, char **argv);
};

#endif /* DoriaxMac_h */
