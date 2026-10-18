# Butterscotch Xbox 360 port - build scripts

These scripts build the Xbox 360 port. They are **port-owned**: they do not read
or write the public `CMakeLists.txt`, do not reuse another project's scripts, and
add nothing to a protected path. See `docs/xbox360-platform-plan.md` for the
rules this follows, `docs/xbox360-progress.md` for current status and evidence,
and `docs/xbox360-renderer-design.md` for the D3D9 renderer design.

## Layout

| File | Role |
| --- | --- |
| `env.sh` | Resolves and validates `OXDK_DIR`, `OXDK_LLVM`, `XDK_DIR`; checks host tools. Sourced by `build.sh`. |
| `build.sh` | The single build entry point. Sources `env.sh`, then drives CMake or the OXDK-native Makefile. |
| `toolchain.cmake` | The port's CMake toolchain: pins OXDK's clang, targets `powerpc-unknown-none-elf` with the Xenon ABI, and replaces the link rule with the wrapper below. |
| `oxdklink_wrapper.py` | Stages each link input, clears `.deplibs` on the copy, then calls OXDK's `oxdklink.py`. |
| `compile_probe_shaders.py` | Calls the installed Xbox 360 `fxc.exe`; generates build-local shader headers for the standalone D3D9 probe. |
| `compile_shaders.py` | Generic `fxc` driver: compiles each `NAME:PROFILE` into a build-local header. Used for the runtime renderer's precompiled shaders. |
| `precompile_shaders.py` | Offline user-shader precompile: compiles a title's HLSL9 shaders (from a `data.win`'s SHDR, or a directory of source pairs) with Xbox `fxc` into the `data.x360shaders` sidecar. |

The build projects live in `port/xbox360/` (`CMakeLists.txt`, `Makefile`), the
port's own shims in `port/xbox360/shim/`, and platform sources in `src/xbox360/`.

## What gets built

Three CMake modes:

- **default (runtime)** - `build/xbox360/default.xex`. The full runtime with the
  port's D3D9 `RendererVtable` (`src/xbox360/xbox360_renderer.c`) and XAudio2
  audio backend (`src/xbox360/xbox360_audio.c`): it boots a `data.win`, runs the
  frame loop, composites the application surface to the 1280x720 back buffer, and
  presents. It also shows the port loading/status screen during init and a
  debug stats overlay toggled with LB+RB. It links
  `d3d9`/`xgraphics`/`xaudio2`/`xmcore` and needs the XDK `fxc.exe` to build its
  precompiled shaders (Xenon has no runtime HLSL compiler).
- **`--minimal`** - a toolchain probe that links nothing but `xtl.h`. Use it to
  tell a toolchain problem from a runtime problem.
- **`--d3d-probe`** - standalone 1280×720 D3D9 device/Clear/Present probe,
  `build/xbox360-d3d/d3d-probe.xex`. **Not** a `RendererVtable` implementation.
  `--d3d-shader-probe` adds a yellow triangle drawn with XDK `fxc`-compiled
  vertex/pixel shaders and a vertex declaration. Both variants use `d3d9.lib`.
  `--real-resource-probe` additionally reads `game:\data.win`, parses the first
  valid TPAG/TXTR entry, decodes it with the existing image decoder, and uploads
  the real RGBA page through `XGTileTextureLevel`; it requires the shader probe.
  `--probe-tpag=N` selects a specific TPAG index so a non-full-page sub-rect can
  be verified (`src=(2,2 155x14)` on page 1 in the DELTARUNE data.win).

**User shaders (offline precompile).** Xenon has no runtime HLSL compiler, so a
title's `hlsl9_*` shaders are translated ahead of time. `precompile_shaders.py`
compiles them with the Xbox 360 `fxc` into a big-endian sidecar
(`data.x360shaders`); `src/xbox360/xbox360_shadersets.c` loads it at runtime and
implements `shaderIsCompiled` / `gpuSetShader` / `shaderGetUniform` /
`shader_get_sampler_index` / `shader_set_uniform_*` without `d3dx9`. If the
sidecar is missing or a shader was not precompiled, that shader falls back to the
built-in one (a `shader_is_compiled` query reports false). The sidecar is a build
product and is not committed.

`--shader-selftest` builds the pair of deterministic test shaders in
`src/xbox360/shaders/selftest/` into `generated/data.x360shaders` and enables
`X360Renderer_runShaderSelftest`, which runs once at startup and logs
`ShaderSelfTest: PASS/FAIL` over KDNet. **Deploy that sidecar next to `data.win`
on the console before launching** (see below).

**Texture compression (DXT + `data.x360tex`).** Static TXTR pages are uploaded as
`D3DFMT_DXT1` (no alpha, 4bpp) or `D3DFMT_DXT5` (alpha, 8bpp) instead of RGBA8.
The runtime path decodes a page and compresses it with `XGCompressSurface`, then
tiles it with `XGTileTextureLevel`. A `data.x360tex` sidecar can hold the
precompressed pages: when it exists and matches the deployed `data.win`
(gameID + file size), pages are uploaded straight from it; pages missing from
the sidecar fall back to runtime compression, and compression failures fall back
to RGBA8. Before compression, fully transparent texels are recoloured from their
4x4 block's visible average (GameMaker pages often use pure green as the chroma
background), which removes green fringes around blue sprite edges. The sidecar
is version 3; v2 caches are rejected and regenerated. Dynamic pages, surface
resolve textures and the debug UI font stay RGBA8. Generate a sidecar on a devkit
with `--dump-textures` (it writes `data.x360tex` in the game directory as pages
load), or let the title generate a complete one on first boot. First-boot generation is the default: a missing or
incomplete sidecar makes the title decode/compress every static page one at a
time, with a loading-screen progress bar and `Hold START to skip`. The next boot
reuses a complete sidecar. Use `--no-texture-autogen` for a load-only build, or
`--dump-textures` for the old partial dump mode (which disables autogen). The
sidecar is a build/deployment product and is not committed.

The real-resource probe links `xgraphics.lib` and adds only the probe-specific
sources it needs (`src/binary_reader.c`, `src/data_win.c`, `src/log.c`,
`src/stb_ds.c`, `src/image/image_decoder.c`, `src/xbox360/stb_impl.c`,
`src/xbox360/xbox360_log.c`, plus the vendored `bzip2` decoder). The default
runtime source list is unchanged, so these dependencies stay confined to the
probe. The probe also shares the runtime's C99/string compatibility defines
(`BS_X360_COMPAT_DEFINES`); without them the public headers pull the host's
`<strings.h>` instead of the XDK's.

## Requirements

- Linux, macOS or WSL (not a Windows command prompt).
- `cmake`, `ninja`, `python3`, `make` on `PATH`.
- An OXDK checkout (`OXDK_DIR`), a patched clang (`OXDK_LLVM`), and an extracted
  Xbox 360 XDK (`XDK_DIR`). `env.sh` verifies all three and refuses to build if
  the compiler lacks the Xenon ABI feature.
- For the runtime and for `--d3d-shader-probe`: Windows Xbox 360 SDK
  `fxc.exe`, available to the WSL host (set `XEDK` to its `/mnt/...` directory).
  The runtime renderer and the shader probe need it; `--minimal` and the bare
  `--d3d-probe` do not.

`env.sh` reads each path from the environment first and otherwise searches the
usual OXDK locations. Nothing here points at another project's tree.

## Usage

```sh
# Configure your shell (paths are machine-specific):
export OXDK_DIR=/path/to/OXDK
export XDK_DIR=/path/to/XDK          # holds lib/xbox/xboxkrnl.lib
export OXDK_LLVM=/path/to/oxdk-llvm/build

# CMake backend (default) -> build/xbox360/default.xex  (runtime + D3D9 renderer)
export XEDK='/mnt/d/Microsoft Xbox 360 SDK'   # WSL path to the installed XDK
scripts/xbox360/build.sh

# Runtime + a one-shot surface/EDRAM self-test at startup (logs PASS/FAIL over KDNet)
scripts/xbox360/build.sh --renderer-selftest --frames=300

# Runtime + the offline-shader self-test: builds generated/data.x360shaders from
# the test HLSL pair and logs "ShaderSelfTest: PASS/FAIL" at startup. Deploy the
# sidecar next to data.win on the console (see "Deploying to a console").
scripts/xbox360/build.sh --shader-selftest --frames=1800

# Runtime + the XAudio2 audio self-test: plays an embedded WAVE sound, an
# external Ogg sound and an audio stream, and logs "AudioSelfTest: PASS/FAIL".
scripts/xbox360/build.sh --audio-selftest --frames=1800

# Runtime with texture-page dumping: writes data.x360tex in the game directory
# as pages load. Run it through the content once, then deploy the sidecar.
scripts/xbox360/build.sh --dump-textures --frames=600

# First-boot autogeneration is the default: if data.x360tex is missing or
# incomplete, generate every static page on the console (progress bar, Hold
# START to skip), then reuse the complete sidecar on later boots.
scripts/xbox360/build.sh --frames=0

# Disable first-boot generation (load an existing sidecar, otherwise runtime
# compression):
scripts/xbox360/build.sh --no-texture-autogen --frames=0

# Runtime with the debug stats overlay enabled at startup (LB+RB cycles it).
scripts/xbox360/build.sh --debug-overlay --frames=0

# Minimal toolchain probe -> build/xbox360/default.xex
scripts/xbox360/build.sh --minimal

# OXDK-native minimal probe -> build/xbox360-make/default.xex
scripts/xbox360/build.sh --backend=make

# D3D9 display/programmable pipeline probe; deployed separately from default.xex
scripts/xbox360/build.sh --d3d-probe --frames=1800
export XEDK='/mnt/d/Microsoft Xbox 360 SDK'  # WSL path to the installed XDK
scripts/xbox360/build.sh --d3d-probe --d3d-shader-probe --frames=1800

# Real-resource probe: needs a data.win next to the XEX on the console.
# Deploy it to DEVKIT:\chapter1_windows\data.win before launching.
scripts/xbox360/build.sh --d3d-probe --d3d-shader-probe --real-resource-probe \
    --frames=1800
# Force a specific TPAG index to check a non-full-page sub-rect:
scripts/xbox360/build.sh --d3d-probe --d3d-shader-probe --real-resource-probe \
    --probe-tpag=1 --frames=1800

# Other options
scripts/xbox360/build.sh --clean -j8 --build-type=Release
scripts/xbox360/build.sh --help

# Hardware input test: run long enough to inject a button, and log the edges
scripts/xbox360/build.sh --clean --frames=40000 --input-log
```

## Loading screen and debug overlay

The runtime now shows a PS2-style loading/status screen while `data.win` is
parsed, and a runtime debug overlay:

- The D3D device is created before `DataWin_parse` (`X360Renderer_initDevice`),
  so the screen covers the whole parse. It shows `Butterscotch360-Next`, the
  game name, per-chunk progress (`Loading SHDR... (12/31)`), a progress bar,
  `Memory: used/total MB`, resource counts, and the build commit/date.
- Status screens follow the init steps: `Creating VM...`, `Initializing file
  system...`, `Initializing audio...`, `Creating runner...`, `Initializing first
  room...`, `Reticulating splines...`.
- **LB+RB on controller 0** cycles the debug overlay: stats -> stats+profiler ->
  off. The combo is read from the raw XInput button state, consumed by the port,
  and not injected into the game's key state. `--debug-overlay` starts it enabled;
  it is off by default. XBDM's `controller.py` cannot reliably hold both
  shoulders in one state, so use a physical controller to verify the combo.
- The overlay is drawn through the renderer's built-in shader and embedded debug
  font. It is not part of the public `RendererVtable`; `beginOverlay` forces the
  built-in shader/state and `endOverlay` restores the game's state.
- `GlobalMemoryStatus` supplies the memory figure and is labelled `approx`; it
  is not a precise dynamic tracker. The profiler state only has data when the
  build defines `ENABLE_VM_GML_PROFILER`.

## Deploying to a console

OXDK ships `xbdm.py`. From the OXDK checkout:

```sh
python3 "$OXDK_DIR/xbox360/tools/xbdm/xbdm.py" <host> deploy \
    build/xbox360/default.xex 'Hdd1:\butterscotch\default.xex'
python3 "$OXDK_DIR/xbox360/tools/xbdm/xbdm.py" <host> cmd \
    'magicboot title="Hdd1:\butterscotch\default.xex" directory="Hdd1:\butterscotch"'
python3 "$OXDK_DIR/xbox360/tools/xbdm/xbdm.py" <host> listen   # debug output
```

The minimal XEX writes one line with `OutputDebugStringA` and exits.

**User-shader sidecar.** The runtime loads `data.x360shaders` through the port
file system (relative to the directory `data.win` came from). For a real title,
generate it once against the title's `data.win` and deploy it alongside:

```sh
# Compile the title's HLSL9 shaders (needs the Xbox 360 fxc):
python3 scripts/xbox360/precompile_shaders.py \
    --fxc "/mnt/d/Microsoft Xbox 360 SDK/bin/win32/fxc.exe" \
    --datawin /path/to/data.win \
    --output build/xbox360/generated/data.x360shaders

python3 "$OXDK_DIR/xbox360/tools/xbdm/xbdm.py" <host> deploy \
    build/xbox360/generated/data.x360shaders \
    'Hdd1:\butterscotch\data.x360shaders'
```

`--shader-selftest` builds that file from the test HLSL pair instead. Shaders a
title stores as declarations only (GMS 2.3+ `hlsl9` slots) are skipped; that
shader then falls back to the built-in one.

**Texture sidecar.** A `--dump-textures` build writes `data.x360tex` next to
`data.win` as pages load. Copy it back to the PC and deploy it with normal
builds:

```sh
python3 "$OXDK_DIR/xbox360/tools/xbdm/xbdm.py" <host> pull \
    'Hdd1:\butterscotch\data.x360tex' build/xbox360/data.x360tex
python3 "$OXDK_DIR/xbox360/tools/xbdm/xbdm.py" <host> deploy \
    build/xbox360/data.x360tex 'Hdd1:\butterscotch\data.x360tex'
```

The loader validates the sidecar against the deployed `data.win` (gameID and
file size); a mismatch is ignored and every page falls back to runtime
compression. The dump build overwrites the sidecar instead of reading it, so do
not run a dump build when you want to test an existing sidecar.

## Design notes

- **Two backends, one purpose.** The CMake path is what the port will grow into;
  the Makefile path drives OXDK's own `xbox360.mk` directly and is the fastest way
  to tell a toolchain problem from a CMake problem.
- **`.deplibs` on a copy.** clang in MS compatibility mode records
  `#pragma comment(lib, ...)` in a `.deplibs` section, which ld.lld would try to
  honour. The wrapper strips it on a staged copy so the real object keeps its
  timestamp and incremental builds stay incremental.
- **`-target` is spelled out.** CMake does not expand `CMAKE_C_COMPILER_TARGET`
  into the compile rule, so the toolchain puts `-target powerpc-unknown-none-elf`
  in the flags directly. Without it clang compiles for the host and the XDK
  headers fail with `__stdcall`/builtin conflicts.
- **Header order.** OXDK's `header-shim` must precede the XDK headers. The
  toolchain therefore supplies the include directories through the flag set and
  the project does not call `target_include_directories`, which would put the XDK
  tree first.
- **No public code.** The port enumerates its own source list. If upstream moves
  a file, the port fails to build; that failure is the signal to update the list,
  not a reason to edit the public build.
- **Backslash paths.** The XDK CRT rejects forward slashes (measured on a devkit:
  `fopen("game:\\x")` works, `fopen("game:/x")` fails), so the public
  `OverlayFileSystem` cannot reach console storage. The port supplies
  `src/xbox360/xbox360_file_system.c`, which keeps OverlayFileSystem's two-area
  semantics but builds backslash paths.
- **Two input contracts.** `src/xbox360/xbox360_input.c` polls XInput once per
  frame and drives both `RunnerGamepadState` (the GameMaker gamepad API) and a
  VK mapping into `RunnerKeyboardState`, so both pad-native and keyboard-driven
  games work. Neither public mapping table is touched.
- **D3D9 renderer.** `src/xbox360/xbox360_renderer.c` implements the public
  `RendererVtable` with precompiled Xenon shaders and XGraphics tiling. Xenon
  cannot texture from a render target, so each GML surface owns a lockable
  resolve texture filled by `Resolve`; EDRAM render targets are created on
  demand and cached, with LRU eviction under EDRAM pressure (RTs are not
  released on every unbind). A full-size RT that still does not fit falls back
  to predicated tiling (up to 15 tiles, 1024x1024 each); `TilingSelfTest` covers
  a 2048x2048 surface on hardware. EDRAM content is restored from the resolve
  texture when a cached RT is re-bound after Present. The
  port's `main()` composites the application surface onto the back buffer and
  calls `X360Renderer_present()` after the GUI pass, except while a room change
  is pending (then the last presented frame stays on screen). It also sets
  `D3DRS_HALFPIXELOFFSET` for the GL/D3D10 pixel-centre convention and paces
  the loop to `Runner_getEffectiveGameSpeed` instead of relying on Present's
  60 Hz vblank. See `docs/xbox360-renderer-design.md` sections 7 and 10.
- **User shaders are precompiled offline.** Xenon cannot compile HLSL at
  runtime, so `precompile_shaders.py` wraps the Xbox 360 `fxc` and writes a
  big-endian sidecar (`data.x360shaders`); `src/xbox360/xbox360_shadersets.c`
  loads it and backs `shader_is_compiled` / `shader_get_uniform` /
  `shader_set_uniform_*`. `discard` is stripped (Xenon's microcode compiler
  rejects it) and alpha testing becomes the matching D3D render state; shaders
  that read `transpose(gm_Matrices...)` are flagged so the runtime uploads
  transposed matrices. Anything not precompiled falls back to the built-in
  shader. Nothing proprietary (microcode or generated headers) is committed.
- **Audio decodes to PCM, then hands it to XAudio2.** Xenon's XAudio2 decodes
  neither Ogg nor the target's `AUDO` blobs, so `src/xbox360/xbox360_audio.c`
  decodes embedded RIFF WAVE (16-bit PCM) itself and external/streamed `.ogg`
  with the vendored `stb_vorbis` (`src/xbox360/stb_vorbis_impl.c`), then submits
  each sound to its own `IXAudio2SourceVoice`. Ogg streams created with
  `audio_create_stream` are decoded in chunks by a worker thread and fed to
  XAudio2 as a queued buffer set (~4 s of lookahead), so starting a long track
  no longer decodes the whole file on the game thread or allocates its full PCM
  copy. Xbox 360 is big-endian, so WAV data is byte-swapped and stb_vorbis'
  host-order output is submitted as-is. A failed
  `XAudio2Create`/`CreateMasteringVoice` leaves the backend silent rather than
  crashing or faking success. `--audio-selftest` exercises embedded, external
  and streamed playback and logs `AudioSelfTest: PASS/FAIL`.

## Port shims and compatibility defines

`_WIN32` must be defined for the XDK's CRT headers, which makes the public code
take Windows code paths. The port therefore supplies a small `windows.h` shim
that routes the Windows types at `<xtl.h>` and stubs the few Win32 APIs Xenon
does not provide (`CreateFileMappingA`, `MapViewOfFile`, `UnmapViewOfFile`,
`GetModuleHandleW`) so they degrade to "not available". `math.h` is a thin
`#include_next` shim that adds only what the XDK's MSVC math header lacks
(`remainderf`).

The XDK CRT is also missing several C99 entry points. The port sets the `NO_*`
switches the public compat headers already honour (`NO_ISNAN`, `NO_ISINF`,
`NO_ROUND`, `NO_ROUNDF`, `NO_LROUND`, `NO_FMIN`, `NO_FMAX`, `NO_LOG2`,
`NO_STRINGS_H`, `NO_STRCASECMP`, `NO_STRTOK_R`) and redirects `snprintf` to
`_snprintf`. `src/xbox360/xbox360_compat.c` defines `getenv`, which the XDK
declares but does not implement.

OXDK's `oxdk360/builtins/builtins.c` is compiled into the runtime: it supplies
the 64-bit integer/float helpers clang calls into that the XDK does not ship.

## Differences from other ports' scripts

Butterscotch is C and its first target has no graphics and no audio, so this
script set is deliberately smaller than a C++ title's would be: no libc++
headers, no compiled libc++ runtime, no fake linker or `pkg-config` shims for
third-party `configure` scripts. Those are added only if and when a dependency
needs them.
