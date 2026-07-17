# Vendored libass + FreeType + FriBidi + HarfBuzz built directly by the VS
# toolchain - no meson, no configure, no shell, no nasm, no package manager.
# The configure steps are replaced by committed hand-authored config text in
# third_party/libass-config (see its PROVENANCE.txt): libass config.h,
# FreeType ftoption.h/ftmodule.h, FriBidi config.h + fribidi-config.h and
# pre-generated Unicode tables (*.tab.i, committed text). HarfBuzz builds as
# the upstream single-TU amalgamation (harfbuzz.cc) and needs no config
# header. SIMD asm is disabled everywhere (pure C; CONFIG_ASM=0).
#
# Style mirrors cmake/ffmpeg.cmake: one static lib per vendor library,
# config dir FIRST on the include path, third-party code at /W0 with
# /guard:cf (this stack parses untrusted subtitle+font data), /utf-8 for
# portable source text. CRT selection (static/dynamic) is global repo
# policy and must be set before these targets are created.

set(ASS_SRC ${CMAKE_SOURCE_DIR}/third_party/libass-src)
set(ASS_CFG ${CMAKE_SOURCE_DIR}/third_party/libass-config)

if(NOT EXISTS ${ASS_SRC}/libass/libass/ass.h)
  message(FATAL_ERROR "third_party/libass-src missing - run the libass kit's vendor_libass.cmake against this repo (see third_party/libass-src/PROVENANCE.txt)")
endif()

function(ass_thirdparty_defaults tgt)
  set_target_properties(${tgt} PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
  if(MSVC)
    # third-party code: silence warnings; CFG hardening to match the repo;
    # /utf-8: these trees contain UTF-8 source and string literals.
    target_compile_options(${tgt} PRIVATE /W0 /guard:cf /utf-8)
  endif()
endfunction()

# --- FreeType (20 TUs of the documented modular build; INSTALL.ANY) -------
set(ASS_FT_SOURCES
  src/base/ftsystem.c src/base/ftinit.c src/base/ftdebug.c src/base/ftbase.c
  src/base/ftglyph.c src/base/ftbitmap.c src/base/ftstroke.c
  src/base/ftsynth.c src/base/ftmm.c src/base/fttype1.c
  src/truetype/truetype.c src/cff/cff.c src/type1/type1.c src/sfnt/sfnt.c
  src/psaux/psaux.c src/pshinter/pshinter.c src/psnames/psnames.c
  src/smooth/smooth.c src/raster/raster.c src/autofit/autofit.c)
list(TRANSFORM ASS_FT_SOURCES PREPEND ${ASS_SRC}/freetype/)
add_library(ass_freetype STATIC ${ASS_FT_SOURCES})
ass_thirdparty_defaults(ass_freetype)
target_compile_definitions(ass_freetype PRIVATE FT2_BUILD_LIBRARY
  _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
target_include_directories(ass_freetype BEFORE PRIVATE
  ${ASS_CFG}/freetype              # our freetype/config/ftoption.h+ftmodule.h
  ${ASS_SRC}/freetype/include)     # upstream public headers (ftoption/ftmodule removed)

# --- FriBidi (committed generated tables; meson fribidi_sources list) -----
set(ASS_FRIBIDI_SOURCES
  fribidi.c fribidi-arabic.c fribidi-bidi.c fribidi-bidi-types.c
  fribidi-char-sets.c fribidi-char-sets-cap-rtl.c fribidi-char-sets-cp1255.c
  fribidi-char-sets-cp1256.c fribidi-char-sets-iso8859-6.c
  fribidi-char-sets-iso8859-8.c fribidi-char-sets-utf8.c
  fribidi-deprecated.c fribidi-joining.c fribidi-joining-types.c
  fribidi-mirroring.c fribidi-brackets.c fribidi-run.c fribidi-shape.c)
list(TRANSFORM ASS_FRIBIDI_SOURCES PREPEND ${ASS_SRC}/fribidi/lib/)
add_library(ass_fribidi STATIC ${ASS_FRIBIDI_SOURCES})
ass_thirdparty_defaults(ass_fribidi)
target_compile_definitions(ass_fribidi PRIVATE
  HAVE_CONFIG_H           # picks up libass-config/fribidi/config.h
  FRIBIDI_LIB_STATIC      # no dllexport/dllimport on the API
  _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
target_include_directories(ass_fribidi BEFORE PRIVATE
  ${ASS_CFG}/fribidi               # config.h, fribidi-config.h
  ${ASS_SRC}/fribidi/lib)          # headers + committed *.tab.i tables

# --- HarfBuzz (upstream single-TU amalgamation, C++17) --------------------
add_library(ass_harfbuzz STATIC ${ASS_SRC}/harfbuzz/src/harfbuzz.cc)
set_target_properties(ass_harfbuzz PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
if(MSVC)
  target_compile_options(ass_harfbuzz PRIVATE /W0 /guard:cf /utf-8)
endif()
# see libass-config/harfbuzz/DEFINES.txt for the rationale; every optional
# backend (glib/icu/graphite/uniscribe/gdi/directwrite/freetype) stays off
# because its HAVE_* macro is absent. Do NOT define HB_NO_MT.
target_compile_definitions(ass_harfbuzz PRIVATE
  HB_NO_PRAGMA_GCC_DIAGNOSTIC NDEBUG)

# --- libass (core + DirectWrite provider only) ----------------------------
set(ASS_LIBASS_SOURCES
  c/c_be_blur.c c/c_blend_bitmaps.c c/c_blur.c c/c_rasterizer.c
  ass.c ass_arabic_charmap.c ass_bitmap.c ass_bitmap_engine.c ass_blur.c
  ass_cache.c ass_drawing.c ass_filesystem.c ass_font.c ass_fontselect.c
  ass_library.c ass_outline.c ass_parse.c ass_rasterizer.c ass_render.c
  ass_render_api.c ass_shaper.c ass_string.c ass_strtod.c ass_utils.c
  ass_directwrite.c)
list(TRANSFORM ASS_LIBASS_SOURCES PREPEND ${ASS_SRC}/libass/libass/)
add_library(ass_libass STATIC ${ASS_LIBASS_SOURCES})
ass_thirdparty_defaults(ass_libass)
target_compile_definitions(ass_libass PRIVATE
  FRIBIDI_LIB_STATIC      # consumer side of the static FriBidi API
  _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
  # NOTE: no WIN32_LEAN_AND_MEAN - ass_directwrite.c/dwrite_c.h need the
  # COM pieces of windows.h that it would strip.
target_include_directories(ass_libass BEFORE PRIVATE
  ${ASS_CFG}/libass                # config.h (hand-authored)
  ${ASS_SRC}/libass                # tree root
  ${ASS_SRC}/libass/libass         # "ass_*.h" from c/ subdir sources
  ${ASS_CFG}/freetype
  ${ASS_SRC}/freetype/include      # <ft2build.h>, <freetype/...>
  ${ASS_CFG}/fribidi
  ${ASS_SRC}/fribidi/lib           # <fribidi.h>
  ${ASS_SRC}/harfbuzz/src)         # <hb.h>

# --- interface target the engine links ------------------------------------
add_library(libass INTERFACE)
# engine includes the public API as: #include <ass/ass.h> is NOT the layout
# here; use #include <ass.h> (dir libass-src/libass/libass is exported).
target_include_directories(libass INTERFACE ${ASS_SRC}/libass/libass)
target_link_libraries(libass INTERFACE
  ass_libass ass_harfbuzz ass_fribidi ass_freetype
  # DirectWrite itself is loaded at runtime via LoadLibrary("dwrite.dll")
  # inside ass_directwrite.c; the Win32 desktop path uses GDI directly.
  gdi32 ole32)
