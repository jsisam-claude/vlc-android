#!/usr/bin/env python3
"""Fail the build when the app passes a libvlc option no built module defines.

Why this exists
---------------
libvlc's second ``config_LoadCmdLine`` pass (src/libvlc.c) treats a command-line
option that no *loaded* module defines as fatal: ``libvlc_new()`` returns NULL
and the Android app dies with "can't create LibVLC instance". The diagnostic
goes to stderr via ``fputs``, which Android discards, and the early log buffer
is dropped before the Android logger is attached -- so the failure is silent.

This fork prunes contribs, which removes modules, which removes the options
those modules defined. The app layer is upstream code and keeps passing them.
That combination has already shipped twice:

  * ``--hrtf-file``  -- defined only by the spatialaudio module, disabled in
    aa07a05. Crashed the app on first launch on a device.
  * ``--soundfont``  -- defined only by the fluidsynth codec, disabled by
    ``--disable-fluidsynth`` / ``--disable-fluidlite``. Latent: it fires for any
    user who selects a MIDI soundfont.

Neither is detectable by compiling, by unit tests, or by starting the app
without the right preference set. This script closes the class by checking the
two lists against each other at build time.

What it does
------------
1. Collects every ``--option`` literal the app source can emit.
2. Collects every option *defined* by the VLC core plus the modules actually
   linked into the generated ``libvlcjni-modules.c``.
3. Reports any app option whose every definer is absent from the build.

Failure mode is deliberately narrow. An option is only an ERROR when it is
defined somewhere in the VLC tree but every definer is a module that was not
built -- that is the exact signature of the bug, and it cannot be a false
positive from a parsing gap. Options this script cannot resolve at all are
reported as warnings and never fail the build, so a new ``add_*`` macro spelling
degrades to noise rather than a broken build.

Usage
-----
    tools/check-libvlc-options.py [--vlc DIR] [--app DIR] [--modules FILE] [-v]

All paths default to this fork's layout. Exits 1 on an error-class finding.
"""

import argparse
import os
import re
import sys

# add_string("x", ...), add_integer(CFG_PREFIX "x", ...), add_bool(...), and the
# rest of the family. Group 1 is an optional macro prefix, group 2 the literal.
ADD_RE = re.compile(
    r'\badd_(?:string|integer|bool|float|loadfile|savefile|directory|module'
    r'|module_list|module_cat|module_list_cat|password|key|font|rgb'
    r'|integer_with_range|float_with_range|obsolete_string|obsolete_integer'
    r'|obsolete_bool|obsolete_float|deprecated_alias)\s*\(\s*'
    r'(?:([A-Z_][A-Z0-9_]*)\s+)?"([^"]+)"')

CFG_PREFIX_RE = re.compile(r'^\s*#\s*define\s+([A-Z_][A-Z0-9_]*)\s+"([^"]*)"', re.M)

# lib<name>_plugin_la_SOURCES = a.c b.c \  (continuation lines included)
SOURCES_RE = re.compile(
    r'^lib([A-Za-z0-9_]+)_plugin_la_SOURCES\s*[+]?=\s*((?:[^\n\\]*\\\n)*[^\n]*)',
    re.M)

ENTRY_RE = re.compile(r'vlc_entry__([A-Za-z0-9_]+)\s*,')

# "--foo", "--foo=...", "--no-foo". Interpolation ("--foo=$bar") is truncated at
# the '=' so only the option name is compared.
OPT_RE = re.compile(r'"(--[a-zA-Z][a-zA-Z0-9-]*)(=[^"]*)?"')

# Options the app emits that libvlc's core accepts positionally or that are
# intentionally passed through unvalidated.
IGNORE = {'--help', '--version', '--longhelp', '--full-help'}


def read(path):
    with open(path, encoding='utf-8', errors='replace') as fh:
        return fh.read()


def built_modules(modules_c):
    """Plugin names linked into the APK, from the generated module list."""
    return set(ENTRY_RE.findall(read(modules_c)))


def source_to_plugin(vlc):
    """Map a VLC-relative source path to the plugin name that compiles it.

    VLC's per-directory ``Makefile.am`` files are ``include``d from
    ``modules/Makefile.am``, so the paths inside them are relative to the
    *including* directory, not to the fragment's own directory --
    ``modules/codec/Makefile.am`` says ``codec/subsdec.c``, meaning
    ``modules/codec/subsdec.c``. Resolving a token against the fragment's own
    directory silently maps nothing, which would make every option defined in
    such a module look unowned. So try the fragment's directory and each
    ancestor up to the tree root, and keep the first that exists on disk.
    """
    mapping = {}
    for root, _dirs, files in os.walk(vlc):
        if 'Makefile.am' not in files:
            continue
        am = os.path.join(root, 'Makefile.am')
        bases, cursor = [], root
        while True:
            bases.append(os.path.relpath(cursor, vlc))
            if os.path.normpath(cursor) == os.path.normpath(vlc):
                break
            cursor = os.path.dirname(cursor)
        for plugin, body in SOURCES_RE.findall(read(am)):
            for token in body.replace('\\\n', ' ').split():
                if not token.endswith(('.c', '.cpp', '.m', '.mm', '.cc')):
                    continue
                for base in bases:
                    rel = os.path.normpath(os.path.join(base, token))
                    if os.path.isfile(os.path.join(vlc, rel)):
                        mapping.setdefault(rel, set()).add(plugin)
                        break
    return mapping


def defined_options(vlc, src2plugin):
    """option name -> set of plugin names defining it ('' means the core)."""
    defined = {}
    core = os.path.join('src', 'libvlc-module.c')
    for root, dirs, files in os.walk(vlc):
        dirs[:] = [d for d in dirs if d not in ('.git', 'test', 'contrib', 'extras')]
        for name in files:
            if not name.endswith(('.c', '.cpp', '.m', '.mm', '.h', '.hpp', '.cc')):
                continue
            path = os.path.join(root, name)
            rel = os.path.relpath(path, vlc)
            text = read(path)
            if 'add_' not in text:
                continue
            macros = dict(CFG_PREFIX_RE.findall(text))
            # The core module's options are always present; everything else is
            # attributed to whichever plugin(s) compile the file.
            owners = {''} if rel == core else src2plugin.get(rel, set())
            for prefix, literal in ADD_RE.findall(text):
                option = (macros.get(prefix, '') if prefix else '') + literal
                if not option or option.startswith('-'):
                    continue
                defined.setdefault(option, set()).update(owners or {'?' + rel})
    return defined


def app_options(app):
    """option name -> sorted list of 'file:line' sites that can emit it."""
    found = {}
    roots = [os.path.join(app, 'application'), os.path.join(app, 'libvlcjni')]
    for base in roots:
        for root, dirs, files in os.walk(base):
            dirs[:] = [d for d in dirs if d not in ('.git', 'build', 'vlc')]
            for name in files:
                if not name.endswith(('.kt', '.java')):
                    continue
                path = os.path.join(root, name)
                for lineno, line in enumerate(read(path).splitlines(), 1):
                    for opt, _eq in OPT_RE.findall(line):
                        if opt in IGNORE:
                            continue
                        site = '%s:%d' % (os.path.relpath(path, app), lineno)
                        found.setdefault(opt, set()).add(site)
    return {k: sorted(v) for k, v in found.items()}


def resolve(option, defined):
    """Definers of an option, accounting for libvlc's --no- boolean prefix."""
    for candidate in (option[2:], option[2:].replace('no-', '', 1)):
        if candidate in defined:
            return defined[candidate]
    return None


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--app', default=here, help='vlc-android checkout')
    ap.add_argument('--vlc', default=os.path.join(here, 'libvlcjni', 'vlc'),
                    help='VLC core source tree')
    ap.add_argument('--modules', default=None,
                    help='generated libvlcjni-modules.c (default: autodetect '
                         'under the VLC build dirs)')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    if not os.path.isdir(args.vlc):
        print('check-libvlc-options: no VLC source tree at %s, skipping'
              % args.vlc, file=sys.stderr)
        return 0

    modules_c = args.modules
    if not modules_c:
        for entry in sorted(os.listdir(args.vlc)):
            candidate = os.path.join(args.vlc, entry, 'ndk', 'libvlcjni-modules.c')
            if entry.startswith('build-android-') and os.path.isfile(candidate):
                modules_c = candidate
                break
    if not modules_c or not os.path.isfile(modules_c):
        print('check-libvlc-options: no libvlcjni-modules.c yet, skipping '
              '(run after the native build)', file=sys.stderr)
        return 0

    built = built_modules(modules_c)
    src2plugin = source_to_plugin(args.vlc)
    defined = defined_options(args.vlc, src2plugin)
    emitted = app_options(args.app)

    errors, unknown, unresolved_owner = [], [], []
    for option in sorted(emitted):
        owners = resolve(option, defined)
        if owners is None:
            unknown.append(option)
            continue
        real = {o for o in owners if not o.startswith('?')}
        if '' in owners:
            continue                      # core option, always present
        if not real:
            # Defined somewhere, but this script could not attribute the file to
            # a plugin. Not evidence of a bug -- warn, never fail.
            unresolved_owner.append((option, sorted(owners)))
        elif not (real & built):
            errors.append((option, sorted(real)))

    print('check-libvlc-options: %d options emitted by the app, %d modules built, '
          '%d options defined in the VLC tree'
          % (len(emitted), len(built), len(defined)))

    if args.verbose:
        for option in sorted(emitted):
            owners = resolve(option, defined) or set()
            where = 'core' if '' in owners else ','.join(sorted(owners)) or 'UNRESOLVED'
            print('  %-44s %s' % (option, where))

    for option in unknown:
        print('check-libvlc-options: WARNING: %s is not defined anywhere in the '
              'VLC tree (%s) -- not failing the build; likely a gap in this '
              "script's parsing, not proof the option is valid"
              % (option, emitted[option][0]), file=sys.stderr)
    for option, owners in unresolved_owner:
        print('check-libvlc-options: WARNING: %s is defined in %s but that file '
              'maps to no plugin -- cannot tell whether it is built, so not '
              'failing the build'
              % (option, ', '.join(o.lstrip('?') for o in owners)), file=sys.stderr)

    if not errors:
        print('check-libvlc-options: OK, every resolvable option has a built definer')
        return 0

    print('', file=sys.stderr)
    print('check-libvlc-options: FAILED -- %d option(s) would make libvlc_new() '
          'return NULL at runtime:' % len(errors), file=sys.stderr)
    for option, owners in errors:
        print('', file=sys.stderr)
        print('  %s' % option, file=sys.stderr)
        print('    defined only by: %s (not built)' % ', '.join(owners), file=sys.stderr)
        for site in emitted[option]:
            print('    emitted at:      %s' % site, file=sys.stderr)
    print('', file=sys.stderr)
    print('  Either stop emitting the option, or re-enable its module in '
          'libvlcjni/buildsystem/compile-libvlc.sh.', file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
