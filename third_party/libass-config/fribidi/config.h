/* Hand-authored FriBidi config for MSVC / win64 (replaces meson config.h).
 * FriBidi 1.0.16.  Complete HAVE_* set referenced by lib/: HAVE_CONFIG_H,
 * HAVE_FRIBIDI_CONFIG_H (not used - lib uses DONT_HAVE_FRIBIDI_CONFIG_H),
 * HAVE_FRIBIDI_CUSTOM_H, HAVE_MEMORY_H, HAVE_STDLIB_H, HAVE_STRINGIZE,
 * HAVE_STRINGS_H, HAVE_STRING_H (+ STDC_HEADERS).
 * Build with -DHAVE_CONFIG_H so common.h picks this up. */
#ifndef FRIBIDI_MSVC_CONFIG_H
#define FRIBIDI_MSVC_CONFIG_H

#define HAVE_STRINGIZE 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define STDC_HEADERS 1
/* HAVE_STRINGS_H, HAVE_MEMORY_H: not present with MSVC; guarded by
 * !STDC_HEADERS respectively, so left undefined. Also fine on glibc. */
/* HAVE_FRIBIDI_CUSTOM_H: not used */

#endif
