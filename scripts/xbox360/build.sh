#!/bin/sh
# Butterscotch Xbox 360 port - single build entry point.
#
#   scripts/xbox360/build.sh [options]
#
#     --clean              remove the build directory first
#     --build-type=TYPE    CMake build type (default: Release)
#     --backend=cmake|make cmake (default) drives port/xbox360 with the port's
#                          own toolchain; make runs the OXDK-native minimal
#                          probe in port/xbox360/Makefile
#     -j N, --jobs=N       parallelism (default: number of CPUs)
#     -h, --help           this text
#
# It sources scripts/xbox360/env.sh (which validates OXDK_DIR / OXDK_LLVM /
# XDK_DIR), then builds. It never reads or writes the public CMakeLists.txt and
# never touches a protected path.
#
# Run it from any directory. It is a POSIX shell script: run it under Linux,
# macOS or WSL, not from a Windows command prompt.

set -e

usage() {
    cat <<'USAGE'
Butterscotch Xbox 360 port - build entry point.

  scripts/xbox360/build.sh [options]

    --clean              remove the build directory first
    --build-type=TYPE    CMake build type (default: Release)
    --minimal            build only the minimal XEX (toolchain probe)
    --d3d-probe          build the standalone D3D9 display probe, not the runtime
    --d3d-shader-probe   additionally draw a triangle via XDK fxc shader (requires FXC)
    --real-resource-probe use deployed game:\data.win TXTR/TPAG data (requires shader probe)
    --probe-tpag=N       force TPAG index N in the real-resource probe (default: first valid)
    --frames=N           runtime/probe test frames before exit (default: 120; 0 = until exit)
    --input-log          log controller connect/disconnect and button edges
    --renderer-selftest  run the renderer surface/EDRAM self-test at startup
    --shader-selftest    build the precompiled-shader self-test + test sidecar
    --audio-selftest     run the XAudio2 audio backend self-test at startup
    --dump-textures      write data.x360tex while pages load (disables autogen)
    --texture-autogen    generate data.x360tex on first boot, reuse it afterwards (default)
    --no-texture-autogen disable first-boot texture generation
    --texture-workers=N  worker threads for texture generation (default 2, 1 = sequential)
    --debug-overlay      start with the port debug stats overlay enabled
                         (LB+RB toggles it at runtime)
    --render-stats       log draws/quads/culled/tiled per frame every 300 frames
    --gml-profiler       enable the GML profiler overlay page (LB+RB twice)
    --backend=cmake|make cmake (default) drives port/xbox360 with the port's
                         own toolchain; make runs the OXDK-native minimal
                         probe in port/xbox360/Makefile
    -j N, --jobs=N       parallelism (default: number of CPUs)
    -h, --help           this text

Sources scripts/xbox360/env.sh first, which validates OXDK_DIR / OXDK_LLVM /
XDK_DIR. Run under Linux, macOS or WSL.
USAGE
}

for _bs_x360_arg in "$@"; do
    case "$_bs_x360_arg" in
        -h|--help) usage; exit 0 ;;
    esac
done
unset _bs_x360_arg

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
PORT_DIR=$REPO/port/xbox360

# Shown on the loading screen's credits line.
BS_X360_COMMIT_HASH=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)
BS_X360_COMMIT_DATE=$(git -C "$REPO" log -1 --format=%cs 2>/dev/null || echo unknown)

# shellcheck source=env.sh
. "$HERE/env.sh" || exit 1

BUILD_TYPE=Release
JOBS=
CLEAN=0
BACKEND=cmake
MINIMAL=OFF
D3D_PROBE=OFF
D3D_SHADER_PROBE=OFF
REAL_RESOURCE_PROBE=OFF
PROBE_TPAG=-1
FRAMES=120
INPUT_LOG=OFF
RENDERER_SELFTEST=OFF
SHADER_SELFTEST=OFF
AUDIO_SELFTEST=OFF
DUMP_TEXTURES=OFF
TEXTURE_AUTOGEN=ON
TEXTURE_WORKERS=2
DEBUG_OVERLAY=OFF
RENDER_STATS=OFF
GML_PROFILER=OFF

while [ $# -gt 0 ]; do
    case "$1" in
        --clean)            CLEAN=1 ;;
        --minimal)          MINIMAL=ON ;;
        --d3d-probe)        D3D_PROBE=ON ;;
        --d3d-shader-probe) D3D_SHADER_PROBE=ON ;;
        --real-resource-probe) REAL_RESOURCE_PROBE=ON ;;
        --probe-tpag=*)     PROBE_TPAG=${1#--probe-tpag=} ;;
        --frames=*)         FRAMES=${1#--frames=} ;;
        --input-log)        INPUT_LOG=ON ;;
        --renderer-selftest) RENDERER_SELFTEST=ON ;;
        --shader-selftest)  SHADER_SELFTEST=ON ;;
        --audio-selftest)   AUDIO_SELFTEST=ON ;;
        --dump-textures)    DUMP_TEXTURES=ON ;;
        --texture-autogen)  TEXTURE_AUTOGEN=ON ;;
        --no-texture-autogen) TEXTURE_AUTOGEN=OFF ;;
        --texture-workers=*) TEXTURE_WORKERS=${1#--texture-workers=} ;;
        --debug-overlay)    DEBUG_OVERLAY=ON ;;
        --render-stats)     RENDER_STATS=ON ;;
        --gml-profiler)     GML_PROFILER=ON ;;
        --backend=*)        BACKEND=${1#--backend=} ;;
        --build-type=*)     BUILD_TYPE=${1#--build-type=} ;;
        -j)                 shift; JOBS=$1 ;;
        -j*)                JOBS=${1#-j} ;;
        --jobs=*)           JOBS=${1#--jobs=} ;;
        -h|--help)          usage; exit 0 ;;
        *)
            printf 'build.sh: unknown argument: %s\n' "$1" >&2
            exit 1 ;;
    esac
    shift
done

if [ -z "$JOBS" ]; then
    JOBS=$( (getconf _NPROCESSORS_ONLN 2>/dev/null) || echo 4 )
fi

# The partial dump build writes pages as they load; it is mutually exclusive
# with the first-boot generator.
if [ "$DUMP_TEXTURES" = ON ]; then
    TEXTURE_AUTOGEN=OFF
fi

case "$BACKEND" in
    cmake)
        if [ "$MINIMAL" = ON ] && [ "$D3D_PROBE" = ON ]; then
            printf 'build.sh: --minimal and --d3d-probe are mutually exclusive\n' >&2
            exit 1
        fi
        if [ "$D3D_SHADER_PROBE" = ON ] && [ "$D3D_PROBE" != ON ]; then
            printf 'build.sh: --d3d-shader-probe requires --d3d-probe\n' >&2
            exit 1
        fi
        if [ "$REAL_RESOURCE_PROBE" = ON ] && [ "$D3D_SHADER_PROBE" != ON ]; then
            printf 'build.sh: --real-resource-probe requires --d3d-shader-probe\n' >&2
            exit 1
        fi
        BUILD_DIR=$REPO/build/xbox360
        XEX_NAME=default.xex
        # The runtime now includes the D3D9 renderer: d3d9 + xgraphics, and its
        # precompiled shaders need the XDK fxc (Xenon has no runtime compiler).
        # The audio backend links XAudio2 (xaudio2); xmcore supplies the
        # XLFQueue* lock-free queue functions that XAudio2 depends on.
        XDK_LIBS=xapilib,d3d9,xgraphics,xaudio2,xmcore,libcMT
        if [ "$D3D_PROBE" = ON ]; then
            BUILD_DIR=$REPO/build/xbox360-d3d
            XEX_NAME=d3d-probe.xex
            XDK_LIBS=xapilib,d3d9,xgraphics,libcMT
        fi
        FXC_PATH=
        NEEDS_FXC=0
        if [ "$D3D_SHADER_PROBE" = ON ]; then
            NEEDS_FXC=1
        fi
        if [ "$D3D_PROBE" != ON ] && [ "$MINIMAL" != ON ]; then
            NEEDS_FXC=1
        fi
        if [ "$NEEDS_FXC" = 1 ]; then
            if [ -n "${XEDK:-}" ]; then
                FXC_PATH=$XEDK/bin/win32/fxc.exe
            fi
            if [ -z "$FXC_PATH" ] || [ ! -f "$FXC_PATH" ]; then
                printf 'build.sh: set XEDK to the installed Windows Xbox 360 SDK (fxc.exe required)\n' >&2
                exit 1
            fi
        fi
        if [ "$CLEAN" = 1 ]; then
            rm -rf "$BUILD_DIR"
        fi
        cmake -S "$PORT_DIR" -B "$BUILD_DIR" -G Ninja \
              -DCMAKE_TOOLCHAIN_FILE="$HERE/toolchain.cmake" \
              -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
              -DBS_X360_MINIMAL="$MINIMAL" \
              -DBS_X360_D3D_PROBE="$D3D_PROBE" \
              -DBS_X360_D3D_SHADER_PROBE="$D3D_SHADER_PROBE" \
              -DBS_X360_REAL_RESOURCE_PROBE="$REAL_RESOURCE_PROBE" \
              -DBS_X360_PROBE_TPAG_INDEX="$PROBE_TPAG" \
              -DBS_X360_FXC="$FXC_PATH" \
              -DBS_X360_XDK_LIBS="$XDK_LIBS" \
              -DBS_X360_TEST_FRAMES="$FRAMES" \
              -DBS_X360_COMMIT_HASH="$BS_X360_COMMIT_HASH" \
              -DBS_X360_COMMIT_DATE="$BS_X360_COMMIT_DATE" \
              -DBS_X360_INPUT_LOG="$INPUT_LOG" \
              -DBS_X360_RENDERER_SELFTEST="$RENDERER_SELFTEST" \
              -DBS_X360_SHADER_SELFTEST="$SHADER_SELFTEST" \
              -DBS_X360_AUDIO_SELFTEST="$AUDIO_SELFTEST" \
              -DBS_X360_DUMP_TEXTURES="$DUMP_TEXTURES" \
              -DBS_X360_TEXTURE_AUTOGEN="$TEXTURE_AUTOGEN" \
              -DBS_X360_TEXTURE_WORKERS="$TEXTURE_WORKERS" \
              -DBS_X360_DEBUG_OVERLAY="$DEBUG_OVERLAY" \
              -DBS_X360_RENDER_STATS="$RENDER_STATS" \
              -DBS_X360_GML_PROFILER="$GML_PROFILER"
        cmake --build "$BUILD_DIR" --parallel "$JOBS"
        printf 'XEX: %s/%s\n' "$BUILD_DIR" "$XEX_NAME"
        ;;
    make)
        if [ "$D3D_PROBE" = ON ] || [ "$D3D_SHADER_PROBE" = ON ]; then
            printf 'build.sh: D3D probes require --backend=cmake\n' >&2
            exit 1
        fi
        BUILD_DIR=$REPO/build/xbox360-make
        if [ "$CLEAN" = 1 ]; then
            rm -rf "$BUILD_DIR"
        fi
        make -C "$PORT_DIR" -j"$JOBS" BS_BUILD="$BUILD_DIR"
        printf 'XEX: %s/default.xex\n' "$BUILD_DIR"
        ;;
    *)
        printf 'build.sh: unknown backend: %s\n' "$BACKEND" >&2
        exit 1 ;;
esac
