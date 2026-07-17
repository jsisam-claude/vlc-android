/* Hand-authored libass config for MSVC / win64 (replaces meson's config.h).
 * Target: MSVC 2022+ (C17), Windows 10+ x64, static lib, no asm, no nasm.
 * Derived from libass 0.17.5 meson.build configuration_data() keys; the
 * complete referenced-macro set in libass/*.c,h is:
 *   ARCH_AARCH64 ARCH_X86 CONFIG_ASM CONFIG_CORETEXT CONFIG_DIRECTWRITE
 *   CONFIG_FONTCONFIG CONFIG_ICONV CONFIG_LARGE_TILES CONFIG_SOURCEVERSION
 *   CONFIG_UNIBREAK HAVE_STRDUP HAVE_STRNDUP
 * CONFIG_ASM / CONFIG_LARGE_TILES / ARCH_* are used as #if expressions and
 * must be defined 0, not left undefined. */
#ifndef LIBASS_MSVC_CONFIG_H
#define LIBASS_MSVC_CONFIG_H

/* font provider: DirectWrite only (Win32 desktop path, loaded via
 * LoadLibrary inside ass_directwrite.c; links gdi32). */
#define CONFIG_DIRECTWRITE 1
/* CONFIG_FONTCONFIG, CONFIG_CORETEXT intentionally undefined (#ifdef use) */

/* optional deps not vendored: no iconv (input must be UTF-8/UTF-16/UTF-32
 * as handled internally), no libunibreak, no libpng. (#ifdef use) */

/* pure C build - x86 asm needs nasm, which the contract forbids */
#define CONFIG_ASM 0
#define ARCH_X86 0
#define ARCH_AARCH64 0

/* default tile size (meson default large-tiles=false -> 0; #if use) */
#define CONFIG_LARGE_TILES 0

#define CONFIG_SOURCEVERSION "vendored source, release 0.17.5, commit 4a05d8127f525943ebf45fdc6497c9e665947f0d"

/* UCRT declares strdup (with _CRT_NONSTDC_NO_WARNINGS) but not strndup;
 * ass_compat.h maps strndup to ass_strndup_fallback when undefined. */
#define HAVE_STRDUP 1
/* HAVE_STRNDUP intentionally undefined */
/* HAVE_FSTAT: set by meson but never referenced in 0.17.5 sources; omitted */

#endif /* LIBASS_MSVC_CONFIG_H */
