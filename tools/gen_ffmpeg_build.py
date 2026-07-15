#!/usr/bin/env python3
"""Generate the FFmpeg build description from source text only.

Reads the vendored FFmpeg tree (third_party/ffmpeg): `configure` for the
component dependency graph (NAME_select= / NAME_deps= lines) and the
per-library Makefiles for object lists (OBJS / OBJS-$(CONFIG_X) lines).
FFmpeg's configure is never executed - this script derives what it would
have produced, for our fixed target (MSVC, win64, no asm), and emits:

  third_party/ffmpeg-config/config_components.h
  third_party/ffmpeg-config/libavcodec/{codec,parser,bsf}_list.c
  third_party/ffmpeg-config/libavformat/{demuxer,muxer,protocol}_list.c
  third_party/ffmpeg-config/config.h        (platform template + CONFIG_ flags)
  third_party/ffmpeg-config/libavutil/{avconfig.h,ffversion.h}
  cmake/ffmpeg_sources.txt

Run from the repo root:  python3 tools/gen_ffmpeg_build.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FF = ROOT / "third_party" / "ffmpeg-src"
OUT = ROOT / "third_party" / "ffmpeg-config"
LIBS = ["libavutil", "libavcodec", "libavformat", "libswresample", "libswscale"]

FFMPEG_VERSION = "n8.1.2"

# ---------------------------------------------------------------- selection

DECODERS = """
    h264 hevc mpeg2video mpeg4 msmpeg4v3 vp8 vp9
    aac mp3 ac3 eac3 flac vorbis opus
    pcm_s16le pcm_s24le pcm_s32le pcm_f32le
    subrip srt ass ssa movtext pgssub dvdsub
""".split()

PARSERS = """
    h264 hevc mpegvideo mpeg4video mpegaudio aac ac3 flac vorbis opus vp8 vp9
""".split()

DEMUXERS = "mov matroska avi srt ass".split()
MUXERS: list[str] = []
ENCODERS: list[str] = []
PROTOCOLS = ["file"]
BSFS = ["null"]
HWACCELS: list[str] = []  # d3d11va comes in the hw-decode phase

# Library/feature flags we deliberately enable besides the closure.
EXTRA_CONFIG = """
    avcodec avformat avutil swresample swscale
    static decoders demuxers parsers protocols bsfs
    safe_bitstream_reader swscale_alpha
""".split()

# HAVE_ flags that gate Makefile objects on our target.
HAVE_FLAGS = {"W32THREADS", "THREADS"}

# ------------------------------------------------------------ configure text

def parse_configure_graph(text: str) -> dict[str, set[str]]:
    graph: dict[str, set[str]] = {}
    for m in re.finditer(
        r'^([a-zA-Z0-9_]+)_(select|deps|suggest)=(["\'])(.*?)\3', text, re.M
    ):
        name, kind, _, deps = m.groups()
        if kind == "suggest":
            continue
        graph.setdefault(name, set()).update(deps.split())
    return graph


def closure(roots: set[str], graph: dict[str, set[str]]) -> set[str]:
    enabled: set[str] = set()
    stack = list(roots)
    while stack:
        n = stack.pop()
        if n in enabled:
            continue
        enabled.add(n)
        stack.extend(graph.get(n, ()))
    return enabled


# --------------------------------------------------------------- makefiles

ARCH_DIRS = {"x86", "aarch64", "arm", "riscv", "loongarch", "ppc", "mips",
             "wasm", "neon", "e2k", "avr32", "bfin", "sh4", "sparc"}


def flag_on(kind: str, negate: str, flag: str, enabled: set[str]) -> bool:
    if kind == "HAVE":
        on = flag in HAVE_FLAGS
    else:
        on = flag.lower() in enabled
    return not on if negate else on


def parse_makefile_objs(lib: str, enabled: set[str]) -> list[str]:
    """All .o entries active under `enabled`, from the lib's Makefile and
    any non-arch subdirectory Makefiles (aac/, opus/, ...)."""
    makefiles = [FF / lib / "Makefile"]
    for sub in sorted((FF / lib).iterdir()):
        if sub.is_dir() and sub.name not in ARCH_DIRS and (sub / "Makefile").exists():
            makefiles.append(sub / "Makefile")

    objs: list[str] = []
    for mk in makefiles:
        text = re.sub(r"\\\n", " ", mk.read_text())
        for line in text.splitlines():
            m = re.match(
                r"^(OBJS|STLIBOBJS)(?:-\$\((!?)(CONFIG|HAVE)_([A-Z0-9_]+)\))?"
                r"\s*[+:]?=\s*(.*)$", line)
            if not m:
                continue
            _, negate, kind, flag, rhs = m.groups()
            if flag and not flag_on(kind, negate, flag, enabled):
                continue
            # expand $(if $(!CONFIG_X), a b) / $(if $(CONFIG_X), a b)
            def expand_if(mm: re.Match) -> str:
                on = flag_on(mm.group(2), mm.group(1), mm.group(3), enabled)
                return mm.group(4) if on else ""
            rhs = re.sub(
                r"\$\(if \$\((!?)(CONFIG|HAVE)_([A-Z0-9_]+)\),([^)]*)\)",
                expand_if, rhs)
            for tok in rhs.split():
                if "$" in tok:
                    print(f"  warn: skipping unexpanded token '{tok}' in {mk}")
                    continue
                if tok.endswith(".o"):
                    objs.append(tok)
    return objs


# ------------------------------------------------------------- known lists

def declared(pattern: str, path: Path) -> set[str]:
    return set(re.findall(pattern, path.read_text()))


def main() -> int:
    configure = (FF / "configure").read_text()
    graph = parse_configure_graph(configure)

    comps = {
        "decoder": DECODERS, "encoder": ENCODERS, "parser": PARSERS,
        "demuxer": DEMUXERS, "muxer": MUXERS, "protocol": PROTOCOLS,
        "bsf": BSFS, "hwaccel": HWACCELS,
    }

    # validate requested names against the source tree's declarations
    decl = {
        "decoder": declared(r"ff_([a-z0-9_]+)_decoder;", FF / "libavcodec/allcodecs.c"),
        "encoder": declared(r"ff_([a-z0-9_]+)_encoder;", FF / "libavcodec/allcodecs.c"),
        "parser": declared(r"ff_([a-z0-9_]+)_parser;", FF / "libavcodec/parsers.c"),
        "demuxer": declared(r"ff_([a-z0-9_]+)_demuxer;", FF / "libavformat/allformats.c"),
        "muxer": declared(r"ff_([a-z0-9_]+)_muxer;", FF / "libavformat/allformats.c"),
        "protocol": declared(r"ff_([a-z0-9_]+)_protocol;", FF / "libavformat/protocols.c"),
        "bsf": declared(r"ff_([a-z0-9_]+)_bsf;", FF / "libavcodec/bitstream_filters.c"),
        "hwaccel": declared(r"ff_([a-z0-9_]+)_hwaccel;", FF / "libavcodec/hwaccels.h"),
    }
    bad = []
    for kind, names in comps.items():
        for n in names:
            if n not in decl[kind]:
                bad.append(f"{n}_{kind}")
    if bad:
        print("ERROR: unknown components:", " ".join(bad))
        return 1

    roots = {f"{n}_{kind}" for kind, names in comps.items() for n in names}
    roots.update(EXTRA_CONFIG)
    enabled = closure(roots, graph)

    comp_suffixes = tuple(
        f"_{k}" for k in ("decoder", "encoder", "parser", "demuxer", "muxer",
                          "protocol", "bsf", "hwaccel", "filter", "indev", "outdev")
    )
    components = sorted(n for n in enabled if n.endswith(comp_suffixes))
    subsystems = sorted(n for n in enabled if not n.endswith(comp_suffixes))

    # FFmpeg uses config macros as C expressions (`if (HAVE_X)`), so every
    # macro referenced anywhere in code we might include must be defined.
    # Scan the tree (excluding arch dirs) and zero-fill whatever we don't
    # explicitly enable.
    macro_re = re.compile(r"\b((?:HAVE|CONFIG|ARCH)_[A-Z0-9_]+)\b")
    referenced: set[str] = set()
    # scan arch dirs too: non-arch code includes arch headers
    # unconditionally (e.g. swscale/utils.c -> loongarch/cpu.h), and
    # zero-filling extra macros is harmless
    for top in LIBS + ["compat"]:
        for p in (FF / top).rglob("*"):
            if p.suffix not in (".c", ".h") or not p.is_file():
                continue
            referenced |= set(macro_re.findall(p.read_text(errors="ignore")))
    referenced.discard("HAVE_AV_CONFIG_H")

    # CPUEXT(flags, LSX) token-pastes HAVE_##LSX(_EXTERNAL/_INLINE/_FAST):
    # those names never appear literally, so derive them from paste sites.
    paste_re = re.compile(r"CPUEXT(?:_SUFFIX)?\(\s*flags\s*,\s*([A-Za-z0-9_]+)\s*\)")
    for top in LIBS + ["compat"]:
        for p in (FF / top).rglob("*.h"):
            for name in paste_re.findall(p.read_text(errors="ignore")):
                base = name.upper()
                for suffix in ("", "_EXTERNAL", "_INLINE", "_FAST", "_SLOW"):
                    referenced.add(f"HAVE_{base}{suffix}")

    COMP_SUFFIX_U = tuple(s.upper() for s in comp_suffixes)
    ref_components = {m for m in referenced
                      if m.startswith("CONFIG_") and m.endswith(COMP_SUFFIX_U)}
    ref_other = referenced - ref_components

    # ------------------------------------------------------------- sources
    sources: list[str] = []
    missing: list[str] = []
    for lib in LIBS:
        for o in parse_makefile_objs(lib, enabled):
            c = f"{lib}/{o[:-2]}.c"
            if (FF / c).exists():
                if c not in sources:
                    sources.append(c)
            else:
                missing.append(f"{lib}/{o}")
    sources.sort()

    # --------------------------------------------------------------- emit
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "libavcodec").mkdir(exist_ok=True)
    (OUT / "libavformat").mkdir(exist_ok=True)
    (OUT / "libavutil").mkdir(exist_ok=True)

    gen_note = "/* Generated by tools/gen_ffmpeg_build.py - do not edit */\n"

    enabled_comp_macros = {f"CONFIG_{c.upper()}" for c in components}
    # every component declared in the registries must exist as a macro:
    # sources paste names like CONFIG_PCM_##id##_DECODER that never appear
    # literally, and an undefined macro breaks the paste dispatch
    all_comp_macros = set(ref_components)
    for kind, names in decl.items():
        for n in names:
            all_comp_macros.add(f"CONFIG_{n.upper()}_{kind.upper()}")
    with open(OUT / "config_components.h", "w") as f:
        f.write(gen_note)
        f.write("#ifndef FFMPEG_CONFIG_COMPONENTS_H\n#define FFMPEG_CONFIG_COMPONENTS_H\n")
        for m in sorted(enabled_comp_macros):
            f.write(f"#define {m} 1\n")
        f.write("/* all other declared/referenced components: off */\n")
        for m in sorted(all_comp_macros - enabled_comp_macros):
            f.write(f"#ifndef {m}\n#define {m} 0\n#endif\n")
        f.write("#endif\n")

    def write_list(path: Path, typ: str, var: str, kind: str, names: list[str]):
        with open(path, "w") as f:
            f.write(gen_note)
            f.write(f"static const {typ} * const {var}[] = {{\n")
            for n in names:
                f.write(f"    &ff_{n}_{kind},\n")
            f.write("    NULL };\n")

    write_list(OUT / "libavcodec/codec_list.c", "FFCodec", "codec_list",
               "decoder", DECODERS)
    write_list(OUT / "libavcodec/parser_list.c", "AVCodecParser", "parser_list",
               "parser", PARSERS)
    write_list(OUT / "libavcodec/bsf_list.c", "FFBitStreamFilter", "bitstream_filters",
               "bsf", BSFS)
    write_list(OUT / "libavformat/demuxer_list.c", "FFInputFormat", "demuxer_list",
               "demuxer", DEMUXERS)
    write_list(OUT / "libavformat/muxer_list.c", "FFOutputFormat", "muxer_list",
               "muxer", MUXERS)
    write_list(OUT / "libavformat/protocol_list.c", "URLProtocol", "url_protocols",
               "protocol", PROTOCOLS)

    template = (ROOT / "tools" / "config_msvc_win64.h").read_text()
    template_defined = set(
        re.findall(r"#define ((?:HAVE|CONFIG|ARCH)_[A-Z0-9_]+)", template))
    subsystem_macros = {f"CONFIG_{s.upper()}" for s in subsystems}
    with open(OUT / "config.h", "w") as f:
        f.write(gen_note)
        f.write(template)
        f.write("\n/* --- subsystem flags from the configure dependency graph --- */\n")
        for m in sorted(subsystem_macros - template_defined):
            f.write(f"#define {m} 1\n")
        f.write("\n/* --- every other macro referenced by the tree: off --- */\n")
        for m in sorted(ref_other - template_defined - subsystem_macros):
            f.write(f"#ifndef {m}\n#define {m} 0\n#endif\n")
        f.write("#endif /* FFMPEG_CONFIG_H */\n")

    with open(OUT / "libavutil/avconfig.h", "w") as f:
        f.write(gen_note)
        f.write("#ifndef AVUTIL_AVCONFIG_H\n#define AVUTIL_AVCONFIG_H\n"
                "#define AV_HAVE_BIGENDIAN 0\n"
                "#define AV_HAVE_FAST_UNALIGNED 1\n#endif\n")

    with open(OUT / "libavutil/ffversion.h", "w") as f:
        f.write(gen_note)
        f.write("#ifndef AVUTIL_FFVERSION_H\n#define AVUTIL_FFVERSION_H\n"
                f'#define FFMPEG_VERSION "{FFMPEG_VERSION}"\n#endif\n')

    with open(ROOT / "cmake" / "ffmpeg_sources.txt", "w") as f:
        f.write("# Generated by tools/gen_ffmpeg_build.py - do not edit\n")
        for s in sources:
            f.write(s + "\n")

    print(f"components: {len(components)}  subsystems: {len(subsystems)}  "
          f"sources: {len(sources)}")
    if missing:
        print("objects with no .c (excluded):")
        for m in missing:
            print("  ", m)
    return 0


if __name__ == "__main__":
    sys.exit(main())
