//
// (c) 2026 Eduardo Doria.
//

#ifndef MacViewMetal_h
#define MacViewMetal_h

#import <MetalKit/MetalKit.h>

// The Metal drawable for macOS games. It holds no input handling of its own:
// every event goes to MacInputRouter, shared with the OpenGL view.
@interface MacViewMetal : MTKView <NSTextInputClient>

@end

#endif /* MacViewMetal_h */
