/* Hand-authored FFmpeg config for MSVC / win64 / no-asm.
 * This replaces what ./configure would detect, for our one fixed target:
 * MSVC 2022+ (C17, /experimental:c11atomics), Windows 10+ x64, static
 * lib, pure C (no inline asm, no external asm, no nasm).
 * Undefined HAVE_/CONFIG_ macros evaluate to 0 in #if - only truths are
 * listed. Wrong guesses surface as compile/link errors and get fixed here.
 * Note: tools/gen_ffmpeg_build.py appends the CONFIG_ subsystem flags and
 * closes the include guard. */
#ifndef FFMPEG_CONFIG_H
#define FFMPEG_CONFIG_H

#define FFMPEG_CONFIGURATION "vlc-light-win64: msvc, win64, no-asm, source-only build"
#define FFMPEG_LICENSE "LGPL version 2.1 or later"
#define CC_IDENT "MSVC"
#define FFMPEG_DATADIR "."
#define AVCONV_DATADIR "."
#define CONFIG_THIS_YEAR 2026
#define BUILDSUF ""
#define SLIBSUF ".dll"
#define EXTERN_PREFIX ""
#define EXTERN_ASM

/* keyword mapping: MSVC C has __restrict, not C99 restrict */
#define av_restrict __restrict
#ifndef restrict
#define restrict __restrict
#endif

/* architecture: built as generic C - all arch-specific paths off */
#define ARCH_X86 0
#define ARCH_X86_64 0
#define HAVE_INLINE_ASM 0
#define HAVE_X86ASM 0

#define AV_HAVE_BIGENDIAN 0
#define AV_HAVE_FAST_UNALIGNED 1
#define HAVE_FAST_UNALIGNED 1
#define HAVE_SIMD_ALIGN_16 1
#define HAVE_SIMD_ALIGN_32 1
#define HAVE_SIMD_ALIGN_64 0

/* threading: Win32 threads */
#define HAVE_THREADS 1
#define HAVE_W32THREADS 1
#define HAVE_PTHREADS 0

/* headers present with MSVC + Windows SDK */
#define HAVE_WINDOWS_H 1
#define HAVE_DIRECT_H 1
#define HAVE_IO_H 1
#define HAVE_MALLOC_H 1
#define HAVE_UNISTD_H 0
#define HAVE_SYS_TIME_H 0
#define HAVE_SYS_MMAN_H 0
#define HAVE_SYS_RESOURCE_H 0
#define HAVE_SYS_UN_H 0
#define HAVE_ARPA_INET_H 0
#define HAVE_POLL_H 0
#define HAVE_PTHREAD_H 0
#define HAVE_STDATOMIC_H 1

/* win32 API functions used by the libraries */
#define HAVE_ALIGNED_MALLOC 1
#define HAVE_COMMANDLINETOARGVW 1
#define HAVE_GETMODULEHANDLE 1
#define HAVE_GETPROCESSAFFINITYMASK 1
#define HAVE_GETPROCESSMEMORYINFO 1
#define HAVE_GETPROCESSTIMES 1
#define HAVE_GETSYSTEMTIMEASFILETIME 1
#define HAVE_GETSTDHANDLE 1
#define HAVE_LOADLIBRARY 1
#define HAVE_MAPVIEWOFFILE 1
#define HAVE_PEEKNAMEDPIPE 1
#define HAVE_SETCONSOLETEXTATTRIBUTE 1
#define HAVE_SETCONSOLECTRLHANDLER 1
#define HAVE_SLEEP 1
#define HAVE_VIRTUALALLOC 1
#define HAVE_WGLGETPROCADDRESS 0

/* libc-ish functions: MSVC universal CRT */
#define HAVE_LIBC_MSVCRT 1
#define HAVE_ISATTY 1
#define HAVE_KBHIT 1
#define HAVE_SETMODE 1
#define HAVE_ACCESS 1
#define HAVE_SNPRINTF 1
#define HAVE_STRERROR_R 0
#define HAVE_GETTIMEOFDAY 0
#define HAVE_NANOSLEEP 0
#define HAVE_USLEEP 0
#define HAVE_MKSTEMP 0
#define HAVE_GETRUSAGE 0
#define HAVE_SYSCTL 0
#define HAVE_MMAP 0
#define HAVE_GETENV 1
#define HAVE_DOS_PATHS 1

/* general build shape */
#define CONFIG_STATIC 1
#define CONFIG_SHARED 0
#define CONFIG_SMALL 0
#define CONFIG_GPL 0
#define CONFIG_NONFREE 0
#define CONFIG_VERSION3 0
#define CONFIG_HARDCODED_TABLES 0
#define CONFIG_MEMORY_POISONING 0
#define CONFIG_RUNTIME_CPUDETECT 0
#define CONFIG_AUTODETECT 0
#define CONFIG_NETWORK 0
#define CONFIG_AVDEVICE 0
#define CONFIG_AVFILTER 0
#define CONFIG_POSTPROC 0
#define CONFIG_MUXERS 0
#define CONFIG_ENCODERS 0
#define CONFIG_FILTERS 0
#define CONFIG_HWACCELS 0
#define CONFIG_DEVICES 0
#define CONFIG_DOC 0
#define CONFIG_FTRAPV 0

/* external libraries: none - source-only build */
#define CONFIG_ZLIB 0
#define CONFIG_BZLIB 0
#define CONFIG_LZMA 0
#define CONFIG_ICONV 0
#define CONFIG_OPENSSL 0
#define CONFIG_GNUTLS 0
#define CONFIG_LIBXML2 0
#define CONFIG_SECURETRANSPORT 0
#define CONFIG_SCHANNEL 0
#define CONFIG_MEDIAFOUNDATION 0
#define CONFIG_D3D11VA 0
#define CONFIG_D3D12VA 0
#define CONFIG_DXVA2 0
#define CONFIG_VULKAN 0
#define CONFIG_CUDA 0
#define CONFIG_OPENCL 0
