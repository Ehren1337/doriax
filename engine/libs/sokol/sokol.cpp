#include "Log.h"
#define SOKOL_LOG(s) { SOKOL_ASSERT(s); doriax::Log::error(s); }

#define SOKOL_IMPL

#if defined(DORIAX_ANDROID)
#define SOKOL_EXTERNAL_GL_LOADER
#include <GLES3/gl31.h>
#endif

#if defined(_WIN32)
#undef SOKOL_LOG
#define SOKOL_LOG(s) OutputDebugStringA(s)
#endif
/* this is only needed for the debug-inspection headers */
#define SOKOL_TRACE_HOOKS
/* sokol 3D-API defines are provided by build options */

// sokol_app (and the sokol_glue/sokol_fetch helpers that go with it) is no
// longer compiled: every platform now has its own native application backend
// under engine/platform, which owns the window, the event loop and the
// graphics device.

#include "sokol_gfx.h"
#include "sokol_time.h"
//#include "sokol_audio.h"
//#include "sokol_log.h"
