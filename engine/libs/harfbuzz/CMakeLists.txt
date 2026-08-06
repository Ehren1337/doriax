# HarfBuzz 14.3.0 - https://github.com/harfbuzz/harfbuzz/
#
# Built from the amalgamated harfbuzz.cc, which includes every source it needs.
# Only text shaping is vendored: the subset/repacker sources are not here, and the
# FreeType, Glib, Graphite2 and CoreText integrations compile to nothing because
# their HAVE_* defines are unset. Glyphs are rasterized by stb_truetype, so the
# OpenType font funcs (hb_ot_font_set_funcs) are the only backend in use.

add_library(

    harfbuzz

    STATIC

    harfbuzz.cc

)

target_include_directories(harfbuzz PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(
    harfbuzz

    PRIVATE

    HB_NO_MT                # no shared faces across threads, fonts are per scene
    HB_NO_AAT               # Apple morx/kerx tables, OpenType only
    HB_NO_COLOR             # COLR/CBDT, the atlas is single channel
    HB_NO_DRAW              # outline extraction, stb_truetype rasterizes (implies HB_NO_PAINT)
    HB_NO_STYLE
    HB_NO_META
    HB_NO_BUFFER_SERIALIZE
    HB_NO_BUFFER_VERIFY
)

# Keep every hb_* symbol internal. They would otherwise be exported by libdoriax and
# interpose on the system HarfBuzz that GTK/Pango loads through the file dialog, which
# then calls this differently configured build and crashes.
if(NOT MSVC)
    target_compile_options(harfbuzz PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
endif()

if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
	target_compile_options(harfbuzz PRIVATE -pthread)
endif()
