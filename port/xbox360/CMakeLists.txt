# Butterscotch Xbox 360 port - port-owned CMake project.
#
# Configured only with scripts/xbox360/toolchain.cmake:
#
#     cmake -S port/xbox360 -B build/xbox360 -G Ninja \
#           -DCMAKE_TOOLCHAIN_FILE=scripts/xbox360/toolchain.cmake
#
# (scripts/xbox360/build.sh does exactly this.)
#
# It deliberately does NOT add_subdirectory() the public project and does NOT
# include the public CMakeLists.txt. The source list is enumerated here on
# purpose, so the port can be built without touching any protected path. When
# upstream moves or renames a file, this list must be updated by hand; a build
# failure is the intended signal, not a silent re-glob.

cmake_minimum_required(VERSION 3.21)
project(butterscotch_xbox360 C)

# The port uses CMAKE_SYSTEM_NAME=Generic (there is no Xenon platform module),
# and Generic supplies no per-configuration flags: CMAKE_C_FLAGS_RELEASE is
# empty, so clang would compile the whole runtime at -O0. The runtime is a
# bytecode interpreter plus a software-heavy 2D renderer; -O0 made a 770-instance
# room roughly 2-3x slower than the pre-refactor build. OXDK's own xbox360.mk
# also defaults to -O2. Set the optimization level explicitly, and keep strict
# aliasing off because the public runtime does type punning in several places.
add_compile_options(-fno-strict-aliasing)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2 -DNDEBUG)
elseif(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    add_compile_options(-Os -DNDEBUG)
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    add_compile_options(-O2 -g -DNDEBUG)
else()
    add_compile_options(-O0 -g)
endif()

if(NOT DEFINED BS_X360_XDK_INCLUDE_DIRS)
    message(FATAL_ERROR
        "Configure with -DCMAKE_TOOLCHAIN_FILE=scripts/xbox360/toolchain.cmake")
endif()

get_filename_component(BS_X360_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

option(BS_X360_MINIMAL
    "Build only the minimal XEX (toolchain probe) instead of the runtime" OFF)
option(BS_X360_D3D_PROBE
    "Build the D3D9 display probe (not a runtime renderer)" OFF)
option(BS_X360_D3D_SHADER_PROBE
    "Include a precompiled-shader triangle in the D3D9 probe" OFF)
option(BS_X360_REAL_RESOURCE_PROBE
    "Use the deployed data.win TXTR/TPAG resource in the D3D9 probe" OFF)
option(BS_X360_RENDERER_SELFTEST
    "Run the renderer surface/EDRAM self-test at startup (runtime)" OFF)
option(BS_X360_SHADER_SELFTEST
    "Build the precompiled-shader self-test and its test sidecar (runtime)" OFF)
option(BS_X360_AUDIO_SELFTEST
    "Run the XAudio2 audio backend self-test at startup (runtime)" OFF)
option(BS_X360_DUMP_TEXTURES
    "Write data.x360tex as texture pages are loaded (devkit sidecar generation)" OFF)
option(BS_X360_TEXTURE_AUTOGEN
    "Generate data.x360tex on first boot when missing/incomplete" ON)
set(BS_X360_TEXTURE_WORKERS "2" CACHE STRING
    "Worker threads for first-boot texture generation (1 = sequential)")
option(BS_X360_DEBUG_OVERLAY
    "Start with the port debug stats overlay enabled (LB+RB toggles it)" OFF)
option(BS_X360_RENDER_STATS
    "Log per-frame draw statistics (draws/quads/culled/tiled) every 300 frames" OFF)
option(BS_X360_GML_PROFILER
    "Enable the GML profiler overlay page (ENABLE_VM_GML_PROFILER)" OFF)
if(BS_X360_D3D_SHADER_PROBE AND NOT BS_X360_D3D_PROBE)
    message(FATAL_ERROR "--d3d-shader-probe requires --d3d-probe")
endif()
if(BS_X360_MINIMAL AND BS_X360_D3D_PROBE)
    message(FATAL_ERROR "--minimal and --d3d-probe are mutually exclusive")
endif()

# Runtime test knobs. The default is a short smoke run; a hardware input test
# needs a long run (so there is time to inject a button) and input logging.
set(BS_X360_TEST_FRAMES "120" CACHE STRING "Frames the runtime runs before exiting")
option(BS_X360_INPUT_LOG "Log controller connect/disconnect and button edges" OFF)

# Shared include directories. The XDK headers and the port shims come from the
# toolchain's flag set (they must precede the host's /usr/include); these are
# the project's own and vendored trees, which have no name collisions.
set(BS_X360_INCLUDE_DIRS
    "${BS_X360_REPO}/src"
    "${BS_X360_REPO}/src/debug_font"
    "${BS_X360_REPO}/src/video"
    "${BS_X360_REPO}/src/image"
    "${BS_X360_REPO}/vendor/stb/ds"
    "${BS_X360_REPO}/vendor/stb/image"
    "${BS_X360_REPO}/vendor/stb/vorbis"
    "${BS_X360_REPO}/vendor/miniz"
    "${BS_X360_REPO}/vendor/bzip2"
    "${BS_X360_REPO}/vendor/md5"
    "${BS_X360_REPO}/vendor/base64"
    "${BS_X360_REPO}/vendor/sha1"
    "${BS_X360_REPO}/vendor/box2d/include")

set(BS_X360_COMMIT_HASH "unknown" CACHE STRING "Git commit hash")
set(BS_X360_COMMIT_DATE "unknown" CACHE STRING "Git commit date")

# The XDK's CRT is MSVC's, and its math/string headers are missing several C99
# entry points. These switches either select a public fallback (the NO_* macros
# the public compat headers already honour) or redirect a name (snprintf).
set(BS_X360_COMPAT_DEFINES
    NO_STRINGS_H NO_STRCASECMP NO_STRTOK_R
    NO_ISNAN NO_ISINF NO_ROUND NO_ROUNDF NO_LROUND NO_FMIN NO_FMAX NO_LOG2
    snprintf=_snprintf)

set(BS_X360_BUILD_DEFINES
    PLATFORM_XBOX360
    ENABLE_WAD14 ENABLE_WAD16 ENABLE_WAD17
    ENABLE_PHYSICS
    MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO
    BUTTERSCOTCH_VIDEO_NULL
    BOX2D_DISABLE_SIMD B2_SINGLE_THREADED)

# --- source lists -----------------------------------------------------------
#
# Every file is named. The only entries omitted from src/ are the ones the port
# replaces or does not use, each with its reason:
#
#   gettime.c            no Xbox 360 branch; src/xbox360/xbox360_time.c supplies nowNanos()
#   loop.c               the port owns main(); the platformdefs.h hook set is not used
#   overlay_file_system.c normalises paths to '/', which the XDK CRT rejects (measured:
#                        fopen("game:\\x") works, fopen("game:/x") fails). Replaced by
#                        src/xbox360/xbox360_file_system.c, which is backslash-native.
set(BS_X360_PUBLIC_SOURCES
    src/binary_reader.c
    src/data_win.c
    src/data_win_print.c
    src/debug_overlay.c
    src/event_table.c
    src/gml_array.c
    src/gml_method.c
    src/ini.c
    src/input_recording.c
    src/instance.c
    src/int_int_hashmap.c
    src/int_rvalue_hashmap.c
    src/json_reader.c
    src/json_writer.c
    src/log.c
    src/noop_audio_system.c
    src/noop_file_system.c
    src/noop_renderer.c
    src/profiler.c
    src/random.c
    src/runner.c
    src/runner_gamepad.c
    src/runner_keyboard.c
    src/runner_mouse.c
    src/spatial_grid.c
    src/stb_ds.c
    src/string_builder.c
    src/vm.c
    src/vm_builtins.c
    src/debug_font/debug_font.c
    src/debug_font/debug_font_4bpp.c
    src/physics/physics.c
    src/physics/physics_engine.c
    src/physics/physics_joints.c
    src/physics/physics_particles.c
    src/physics/physics_box2d_timer.c
    src/video/null_video.c
    src/image/image_decoder.c)

set(BS_X360_PORT_SOURCES
    src/xbox360/main.c
    src/xbox360/xbox360_compat.c
    src/xbox360/xbox360_d3d_device.c
    src/xbox360/xbox360_file_system.c
    src/xbox360/xbox360_input.c
    src/xbox360/xbox360_log.c
    src/xbox360/xbox360_renderer.c
    src/xbox360/xbox360_overlay.c
    src/xbox360/xbox360_audio.c
    src/xbox360/xbox360_shadersets.c
    src/xbox360/xbox360_textures.c
    src/xbox360/xbox360_time.c
    src/xbox360/stb_impl.c
    src/xbox360/stb_vorbis_impl.c)

set(BS_X360_VENDOR_SOURCES
    vendor/miniz/miniz.c
    vendor/miniz/miniz_tdef.c
    vendor/miniz/miniz_tinfl.c
    vendor/bzip2/bz_internal_error.c
    vendor/bzip2/bzlib.c
    vendor/bzip2/crctable.c
    vendor/bzip2/decompress.c
    vendor/bzip2/huffman.c
    vendor/bzip2/randtable.c
    vendor/md5/md5.c
    vendor/base64/base64.c
    vendor/sha1/sha1.c)

# box2d is a stable third-party tree; globbing it is what the public build does.
file(GLOB BS_X360_BOX2D_SOURCES CONFIGURE_DEPENDS "${BS_X360_REPO}/vendor/box2d/src/*.c")
list(REMOVE_ITEM BS_X360_BOX2D_SOURCES "${BS_X360_REPO}/vendor/box2d/src/timer.c")

# The lists above are written relative to the repository root; resolve them,
# because this project lives in port/xbox360.
set(BS_X360_RUNTIME_SOURCES "")
foreach(_bs_x360_src IN LISTS
        BS_X360_PUBLIC_SOURCES BS_X360_PORT_SOURCES BS_X360_VENDOR_SOURCES)
    list(APPEND BS_X360_RUNTIME_SOURCES "${BS_X360_REPO}/${_bs_x360_src}")
endforeach()
list(APPEND BS_X360_RUNTIME_SOURCES ${BS_X360_BOX2D_SOURCES})

# OXDK's builtins.c supplies the 64-bit integer/float helpers clang calls into
# (__divdi3, __udivdi3, __floatundidf, ...). The XDK ships none of them; OXDK's
# makefile builds this per title for the same reason. It lives in the OXDK tree,
# so it is added here rather than copied.
list(APPEND BS_X360_RUNTIME_SOURCES
    "${BS_X360_OXDK360_DIR}/oxdk360/builtins/builtins.c")

# --- target -----------------------------------------------------------------

if(BS_X360_MINIMAL)
    add_executable(butterscotch "${BS_X360_REPO}/src/xbox360/main_minimal.c")
    target_compile_definitions(butterscotch PRIVATE PLATFORM_XBOX360)
elseif(BS_X360_D3D_PROBE)
    add_executable(butterscotch
        "${BS_X360_REPO}/src/xbox360/main_d3d_probe.c"
        "${BS_X360_REPO}/src/xbox360/xbox360_d3d_device.c")
    target_compile_definitions(butterscotch PRIVATE
        ${BS_X360_COMPAT_DEFINES}
        BS_X360_TEST_FRAMES=${BS_X360_TEST_FRAMES})
    if(BS_X360_D3D_SHADER_PROBE)
        if(NOT DEFINED BS_X360_FXC OR NOT EXISTS "${BS_X360_FXC}")
            message(FATAL_ERROR "Shader probe requires -DBS_X360_FXC=<installed Xbox 360 fxc.exe>")
        endif()
        set(_bs_shader_out "${CMAKE_CURRENT_BINARY_DIR}/generated")
        set(_bs_shader_headers
            "${_bs_shader_out}/probe_vs.h" "${_bs_shader_out}/probe_ps.h")
        add_custom_command(OUTPUT ${_bs_shader_headers}
            COMMAND "${BS_X360_PYTHON}" "${BS_X360_REPO}/scripts/xbox360/compile_probe_shaders.py"
                    --fxc "${BS_X360_FXC}"
                    --source "${BS_X360_REPO}/src/xbox360/shaders"
                    --output "${_bs_shader_out}"
            DEPENDS "${BS_X360_REPO}/scripts/xbox360/compile_probe_shaders.py"
                    "${BS_X360_REPO}/src/xbox360/shaders/probe_vs.hlsl"
                    "${BS_X360_REPO}/src/xbox360/shaders/probe_ps.hlsl"
            VERBATIM)
        add_custom_target(bs_x360_probe_shaders DEPENDS ${_bs_shader_headers})
        add_dependencies(butterscotch bs_x360_probe_shaders)
        # XDK vertex declaration helpers call the __IsIntConst intrinsic;
        # OXDK's own builtins supplies it for clang/Xenon (as in the runtime).
        target_sources(butterscotch PRIVATE
            "${BS_X360_OXDK360_DIR}/oxdk360/builtins/builtins.c")
        target_include_directories(butterscotch PRIVATE "${_bs_shader_out}")
        target_compile_definitions(butterscotch PRIVATE BS_X360_D3D_SHADER_PROBE)
        if(BS_X360_REAL_RESOURCE_PROBE)
            target_compile_definitions(butterscotch PRIVATE BS_X360_REAL_RESOURCE_PROBE)
        endif()
        if(DEFINED BS_X360_PROBE_TPAG_INDEX)
            target_compile_definitions(butterscotch PRIVATE
                BS_X360_PROBE_TPAG_INDEX=${BS_X360_PROBE_TPAG_INDEX})
        endif()
        target_sources(butterscotch PRIVATE
            "${BS_X360_REPO}/src/binary_reader.c"
            "${BS_X360_REPO}/src/data_win.c"
            "${BS_X360_REPO}/src/log.c"
            "${BS_X360_REPO}/src/stb_ds.c"
            "${BS_X360_REPO}/src/xbox360/stb_impl.c"
            "${BS_X360_REPO}/src/xbox360/xbox360_log.c"
            "${BS_X360_REPO}/src/image/image_decoder.c"
            "${BS_X360_REPO}/vendor/bzip2/bz_internal_error.c"
            "${BS_X360_REPO}/vendor/bzip2/bzlib.c"
            "${BS_X360_REPO}/vendor/bzip2/crctable.c"
            "${BS_X360_REPO}/vendor/bzip2/decompress.c"
            "${BS_X360_REPO}/vendor/bzip2/huffman.c"
            "${BS_X360_REPO}/vendor/bzip2/randtable.c")
        target_include_directories(butterscotch PRIVATE
            "${BS_X360_REPO}/src"
            "${BS_X360_REPO}/src/image"
            "${BS_X360_REPO}/vendor/bzip2"
            "${BS_X360_REPO}/vendor/stb/ds"
            "${BS_X360_REPO}/vendor/stb/image")
    endif()
else()
    add_executable(butterscotch ${BS_X360_RUNTIME_SOURCES})
    target_compile_definitions(butterscotch PRIVATE
        ${BS_X360_COMPAT_DEFINES}
        ${BS_X360_BUILD_DEFINES}
        BS_X360_TEST_FRAMES=${BS_X360_TEST_FRAMES}
        $<$<BOOL:${BS_X360_INPUT_LOG}>:BS_X360_INPUT_LOG>
        $<$<BOOL:${BS_X360_RENDERER_SELFTEST}>:BS_X360_RENDERER_SELFTEST>
        $<$<BOOL:${BS_X360_SHADER_SELFTEST}>:BS_X360_SHADER_SELFTEST>
        $<$<BOOL:${BS_X360_AUDIO_SELFTEST}>:BS_X360_AUDIO_SELFTEST>
        $<$<BOOL:${BS_X360_DUMP_TEXTURES}>:BS_X360_DUMP_TEXTURES>
        $<$<BOOL:${BS_X360_TEXTURE_AUTOGEN}>:BS_X360_TEXTURE_AUTOGEN>
        BS_X360_TEXTURE_WORKERS=${BS_X360_TEXTURE_WORKERS}
        $<$<BOOL:${BS_X360_DEBUG_OVERLAY}>:BS_X360_DEBUG_OVERLAY>
        $<$<BOOL:${BS_X360_RENDER_STATS}>:BS_X360_RENDER_STATS>
        $<$<BOOL:${BS_X360_GML_PROFILER}>:ENABLE_VM_GML_PROFILER>
        BUTTERSCOTCH_COMMIT_HASH="${BS_X360_COMMIT_HASH}"
        BUTTERSCOTCH_COMMIT_DATE="${BS_X360_COMMIT_DATE}")
    target_include_directories(butterscotch PRIVATE ${BS_X360_INCLUDE_DIRS})

    # The renderer draws exclusively through precompiled Xenon shaders. Xenon
    # has no runtime HLSL compiler, so they are built here with the installed
    # XDK fxc and kept in the build directory (never committed).
    if(NOT DEFINED BS_X360_FXC OR NOT EXISTS "${BS_X360_FXC}")
        message(FATAL_ERROR
            "The runtime renderer needs the Xbox 360 XDK fxc. Set XEDK to the "
            "installed Windows SDK or pass -DBS_X360_FXC=<fxc.exe>.")
    endif()
    set(_bs_render_out "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(_bs_render_headers
        "${_bs_render_out}/renderer_vs.h" "${_bs_render_out}/renderer_ps.h")
    add_custom_command(OUTPUT ${_bs_render_headers}
        COMMAND "${BS_X360_PYTHON}" "${BS_X360_REPO}/scripts/xbox360/compile_shaders.py"
                --fxc "${BS_X360_FXC}"
                --source "${BS_X360_REPO}/src/xbox360/shaders"
                --output "${_bs_render_out}"
                --shader renderer_vs:vs_3_0
                --shader renderer_ps:ps_3_0
        DEPENDS "${BS_X360_REPO}/scripts/xbox360/compile_shaders.py"
                "${BS_X360_REPO}/src/xbox360/shaders/renderer_vs.hlsl"
                "${BS_X360_REPO}/src/xbox360/shaders/renderer_ps.hlsl"
        VERBATIM)
    add_custom_target(bs_x360_renderer_shaders DEPENDS ${_bs_render_headers})
    add_dependencies(butterscotch bs_x360_renderer_shaders)
    target_include_directories(butterscotch PRIVATE "${_bs_render_out}")
    # XDK vertex declaration helpers call __IsIntConst; OXDK supplies it.
    target_sources(butterscotch PRIVATE
        "${BS_X360_OXDK360_DIR}/oxdk360/builtins/builtins.c")

    # Shader self-test: build a small deterministic sidecar from the test HLSL
    # pair so the offline precompile path can be verified on a devkit. It is
    # named data.x360shaders because that is the path the runtime loads; deploy
    # it next to data.win before launching.
    if(BS_X360_SHADER_SELFTEST)
        set(_bs_shader_selftest "${CMAKE_CURRENT_BINARY_DIR}/generated/data.x360shaders")
        add_custom_command(OUTPUT "${_bs_shader_selftest}"
            COMMAND "${BS_X360_PYTHON}" "${BS_X360_REPO}/scripts/xbox360/precompile_shaders.py"
                    --fxc "${BS_X360_FXC}"
                    --hlsl-dir "${BS_X360_REPO}/src/xbox360/shaders/selftest"
                    --output "${_bs_shader_selftest}"
            DEPENDS "${BS_X360_REPO}/scripts/xbox360/precompile_shaders.py"
                    "${BS_X360_REPO}/src/xbox360/shaders/selftest/selftest.v.hlsl"
                    "${BS_X360_REPO}/src/xbox360/shaders/selftest/selftest.f.hlsl"
            VERBATIM)
        add_custom_target(bs_x360_shader_selftest ALL DEPENDS "${_bs_shader_selftest}")
        add_dependencies(butterscotch bs_x360_shader_selftest)
    endif()
endif()

# SUFFIX is set on the target as well as in the toolchain: with a Generic
# system, CMake does not keep CMAKE_EXECUTABLE_SUFFIX for the target file name.
set_target_properties(butterscotch PROPERTIES OUTPUT_NAME default SUFFIX ".xex")
if(BS_X360_D3D_PROBE)
    set_target_properties(butterscotch PROPERTIES OUTPUT_NAME d3d-probe)
endif()
