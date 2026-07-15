---
name: self-contained-build
description: >
  Build fully self-contained, source-only projects/services: all third-party
  code vendored as committed text, compiled by the platform's first-party
  toolchain only — no package managers, no submodules, no downloads, no
  prebuilt binaries (including build tools). Use when the user asks for a
  "self-contained", "source-only", "no prebuilts", "no dependencies" project,
  wants to vendor/embed a third-party C/C++ library (FFmpeg-class), or wants
  a vendor library's build derived without running its configure/build system.
---

# Self-contained source-only builds

The exercise: a repo where `git clone` + the platform toolchain produces the
product. Every byte of the artifact traces to committed text. This skill
encodes the method proven by vlc-light-win64 (FFmpeg under MSVC, no
configure, no msys, no nasm) and its embedding into a host app.

## 0. Nail the purity contract FIRST (one round of questions, not ten)

Constraints arrive incrementally by default and each late one invalidates a
finished layer. Ask up front, get explicit answers:

- Do **build-time tool binaries** count as prebuilts? (shell, assembler,
  code generators — in the strict contract: yes, forbidden)
- May the **vendor's own build system run** anywhere, even CI? (strict: no —
  derive its outputs by reading it as text)
- Submodule acceptable, or **copied files only**? (strict: copied text,
  provenance recorded; no fetch from other repos at clone/build time)
- What scripting is allowed for the repo's own tooling? (strict: only the
  build system's language — e.g. `cmake -P` scripts, not Python)
- Priority: purity of the exercise beats delivery speed — confirm it.
- Performance concessions acceptable? (e.g. SIMD asm off to avoid an
  assembler binary; a hardware path can compensate later)

Write the agreed contract into the plan doc verbatim.

## 1. Vendor by closure, not by tree

Never copy the whole upstream tree; copy exactly what compiles plus what it
includes:

1. Derive the compiled-source list from the vendor's build description
   (section 2), commit it as text (`cmake/<lib>_sources.txt`).
2. Walk the transitive quoted-`#include` closure from those sources
   (includes template `.c`/tables, headers, `../` paths). Superset
   resolution (try includer dir, tree root, each sub-library root) is safe —
   it only copies a few extra same-named files.
3. Also copy: the vendor build-description inputs your generator reads
   (configure, Makefiles), the license text, registry/list source files.
4. Write `PROVENANCE.txt`: origin URL, tag, commit hash, file count,
   tool that produced the copy, "unmodified upstream text".
5. The vendor script (`tools/vendor_<lib>.cmake`, `cmake -P`) is the ONLY
   way content enters `third_party/`; re-running it against a new upstream
   checkout is the entire upgrade story. Exemplar:
   `vlc-light-win64/tools/vendor_ffmpeg.cmake`.

## 2. Derive the vendor's config — never run configure

Replace the vendor's configure step with committed text derived from source:

- **Parse the dependency graph as text**: autoconf-style `configure` files
  encode components as `NAME_select="a b"` / `NAME_deps=` lines; Makefiles
  encode sources as `OBJS-$(CONFIG_X) += a.o b.o` (mind `\` continuations,
  `$(if $(!CONFIG_X), ...)` expressions, `!CONFIG` negations, subdirectory
  Makefiles, `STLIBOBJS`). Compute the closure from your selected feature
  set; emit component headers, list sources, and the source list.
  Exemplar: `vlc-light-win64/tools/gen_ffmpeg_build.cmake`.
- **Hand-author the platform half** of the config header for your ONE fixed
  target (compiler version, OS, threading, C library facts). Iterate via CI
  compile errors — they point at exactly the missing/wrong define.
- **Define EVERY macro the tree references** — mature C codebases use config
  macros as C expressions (`if (HAVE_X)`) and paste-dispatch on them
  (`PREFIX_##CONFIG_X`), so "undefined evaluates to 0 in #if" is NOT enough:
  - scan all sources for referenced `HAVE_/CONFIG_/ARCH_` tokens
    (token-boundary matching) and zero-fill the disabled ones;
  - scan for token-paste sites (`CPUEXT(flags, LSX)` → `HAVE_LSX`) — pasted
    names never appear literally;
  - enumerate ALL declared components from registry files and define each
    0/1 — paste dispatch (`PCM_DECODER_##cf`) breaks on undefined macros;
  - scan arch/platform subdirectories too: non-arch code includes arch
    headers unconditionally.
- Value-type tunables (not 0/1) hide in configure's emit section — grep for
  `#define` writes with `$variables` (e.g. `SWS_MAX_FILTER_SIZE`).

## 3. Build layout that mirrors the vendor's include semantics

- One static lib per vendor sub-library; config/generated dir FIRST on the
  include path, then tree root.
- Give a sub-library its own directory on the include path ONLY if its
  subdirectory sources need parent-header resolution — and never for a dir
  containing headers that shadow system ones (`time.h` hijacking
  `#include <time.h>` is the canonical failure).
- Third-party compiles at `/W0`-equivalent; your code at normal warning
  levels. C17 + platform atomics flags as the vendor expects.
- Expose ONE interface target carrying include dirs + system link libs.

## 4. Iterate via CI in structural fixes

You often cannot run the target toolchain locally. Push, read the failing
round, and fix the CLASS of error at the generator/template level — never
whack-a-mole individual files. Expect this ladder (each was one round):
missing platform headers → macros-as-expressions → include-path mechanics →
header shadowing → value tunables → arch-header references → paste-derived
names → linker (wrong-arch dev prompt, CRT mismatch).

Known Windows/MSVC traps (each cost a round somewhere):
- default Developer Prompt targets x86 → every link fails LNK4272/LNK2001;
  guard at configure: fail if `ENV{LIB}` matches `[\\/]x86(;|$)`.
- static CRT must be set globally before any target or vendor libs default
  `/MD` → LNK2038.
- `WIN32_LEAN_AND_MEAN` drops COM/timeapi/mmreg headers; UNICODE affects
  resource-macro resolution; UCRT provides all C99 math (set the vendor's
  `HAVE_<mathfn>` to 1 or fallback definitions collide with intrinsics).
- CMake script gotchas: a variable named `on`/`off` is shadowed by boolean
  constants in `if()`; regex MATCHALL results containing `;` corrupt lists
  (pre-transform the delimiter); quoted-vs-variable IN_LIST semantics.

## 5. Design for embedding from day one

If the product has an engine, put it behind a C API in a static lib the
first week — it costs nothing and pays twice (shell + future hosts):

- Host communication via a registered **event callback** (opened / error /
  ended), never window messages to an HWND the engine happens to hold.
- Error strings copied out under a lock, never `c_str()` into mutable state.
- The engine owns its process-global init (log routing, timer resolution)
  behind `std::call_once` — undocumented shell-side contracts rot.
- Provide a `*_probe()` metadata call and a no-op **stub implementation**
  of the whole API (~40 lines) so hosts can build the feature out.
- Embedding into a host repo = the same vendor-script pattern, pointed at
  your repo: hosts copy the engine + its vendored deps as text.

## 6. Repo hygiene

- Plan doc records the constraint contract, rejected alternatives, and
  gaps-vs-incumbent honestly; README states the build in 3 commands and
  what is NOT OS-maintained anymore (licensing/patch-surface honesty).
- CI workflow on every push; artifact = the product.
- The only scripting language in the repo is the build system's own.
