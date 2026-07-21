# Butterscotch360-Next Xbox 360 Platform Migration Plan

## 1. Objective

`Butterscotch360-Next` is the user's direct GitHub fork of the official
`Butterscotch` repository. Use the latest upstream-derived code already present
in this fork as the only core baseline, then migrate the Xbox 360 XDK platform
implementation into it.

The target is not to update the old `Butterscotch-360` fork in place and it is
not necessary to create another fork. The target is to keep the current VM,
WAD17, runner, and data parsing implementations inherited from official
Butterscotch and add a maintainable Xbox 360 backend to this existing fork.

Baseline at audit time:

- Repository: `Butterscotch360-Next`
- Branch: `main`
- Commit: `f8de9da8982fca6550a2051da9e44c8c1644b375`
- Working tree: clean

### Current Implementation Status (2026-07-21)

- Active migration branch: `port/xbox360`.
- Phase 0, Phase 1, and Phase 2 are complete and have passed XDK Release builds.
- Phase 3 is partially complete: user surfaces, target switching, surface copy,
  dynamic sprites, GUI projection, lazy TXTR loading, and an Xbox texture cache
  are implemented. Full application-surface composition, camera matrices,
  shader emulation, and surface readback remain incomplete.
- Phase 4 has a working XAudio2 baseline. Embedded Chapter 4 audio, external
  OGG decoding, audio groups, gain, pitch, looping, and normal playback have
  run on hardware. Track-position seeking and long-run stress validation remain.
- Phase 5 texture diagnostics and tuning are partially complete. Static TXTR
  pages use `A4R4G4B4` and a 240 MiB frame-aware LRU cache on Xbox 360.
- Current hardware artifact:
  `Release/Butterscotch360-Next.xex`, 3,973,120 bytes, SHA-256
  `A49D42D17CDB3F2298679E110F69E0768B80CDFCE49C01EFC554EF19B16EEB2E`.
- Chapter 4 fixture: 132,001,470-byte `data.win`, SHA-256
  `07E2DF1088E56532B992FC9C59F88A4D66420ADA4DA9FF49BA6823A7F2CC3D47`.
- Known deferred issue: selecting the in-game return-to-title option hangs after
  its transition. Keep this separate from the resolved alpha fade/flash bug.

## 2. Source Repositories and Responsibilities

### Butterscotch360-Next

This is the user's direct fork of official Butterscotch, the target repository,
and the source of truth for all shared code:

- VM and bytecode execution
- GMS1 and GMS2/WAD17 support
- `DataWin` structures and parsing
- Runner lifecycle and rendering orchestration
- Renderer, audio, input, and filesystem interfaces
- Image decoding, including GameMaker QOI and BZip2+QOI

Do not replace shared Next files with copies from either older tree.

Git roles should be:

- `origin`: the user's `Butterscotch360-Next` fork
- `upstream`: the official Butterscotch repository, when configured
- `main`: tracks the current official-derived baseline
- `port/xbox360`: carries the Xbox 360 platform migration

### Butterscotch360-Refresh

Primary reference for the more complete Xbox 360 platform implementation:

- XDK solution and project configuration
- Xbox startup and diagnostic logging
- D3D9 renderer
- Application surface implementation
- XAudio2 backend and external OGG streaming
- Loading screen and diagnostic overlay
- Texture-page GPU cache
- Xbox-specific memory and game compatibility work

Its shared engine code is stale relative to Next and must not be copied over.

### Local modified Butterscotch-360

Reference for Xbox 360 behavior that has already run WAD17 more successfully:

- `ImageDecoder_decodeToRgba` usage in D3D9
- Binary filesystem read/write implementation
- Separate `xdk_gamepad` backend
- WAD17 test behavior and regression evidence

Its VM and runner changes are reference material only. Next already contains a
newer WAD17 implementation.

## 3. Migration Rules

1. Develop on a new `port/xbox360` branch created from Next `main`.
2. Never copy old shared files such as `vm.c`, `runner.c`, or `data_win.c` over Next.
3. Adapt platform code to Next interfaces instead of modifying Next interfaces to
   look like the old fork.
4. Keep commits small and grouped by subsystem.
5. Build and test both GMS1 and WAD17 after each functional stage.
6. Do not commit game data, XEX files, logs, Visual Studio caches, or build output.

## 4. Confirmed Compatibility Gaps

### 4.1 Endianness

Xbox 360 uses a big-endian PowerPC CPU. Refresh previously treated `_XBOX` as
big-endian inside `binary_utils.h`, but Next only swaps when `IS_BIG_ENDIAN` is
defined.

The XDK project must explicitly define:

```text
IS_BIG_ENDIAN
STB_VORBIS_BIG_ENDIAN
```

Verify all raw WAD and bytecode reads use `BinaryUtils_*` helpers. Test embedded
and external OGG playback separately because audio byte order may require
additional platform handling.

### 4.2 Texture Decoding

Refresh D3D9 directly calls `stbi_load_from_memory`, which does not cover newer
GameMaker QOI and BZip2+QOI texture blobs.

The migrated renderer must use Next's:

```c
ImageDecoder_decodeToRgba(...)
```

Add `src/image/image_decoder.c` and its include directory to the XDK project.
Use Next's bzip2 decompression source set.

When `lazyLoadTextures` is enabled, D3D9 must call:

```c
DataWin_loadTxtrIfNeeded(dataWin, texturePageId);
```

After a non-mapped texture blob is decoded and uploaded, release `blobData` and
set it to `nullptr`, following the current GL renderer behavior.

### 4.3 Renderer Interface

Refresh predates several Next `RendererVtable` changes. Required adaptations
include:

- `applyProjection` now receives view and projection matrices.
- `beginGUI` receives the target surface ID.
- `setGuiProjection` is required.
- `drawTriangle` receives three colors.
- Blend mode state has getter callbacks and separate alpha factors.
- `drawTiled` is now `drawSpriteTiled`.
- `setRenderTarget` receives `implicitApplicationSurface`.
- `drawSurfaceTiled` is required.
- `shaderSetUniformFArray` is required.
- `surfaceGetTexture` is required.
- `shadersSupported` no longer receives a renderer.
- `setMatrix` is required.

Initialize the vtable by named fields. Do not use positional aggregate
initialization. Unsupported operations need safe stubs unless Next explicitly
documents the callback as optional.

Refresh references `Renderer.drawPhase`; Next no longer contains that field.
Remove the old phase-dependent game hacks and use Next render-target and pass
semantics.

### 4.4 Runner Lifecycle

Use the current Next lifecycle signatures:

```c
Runner_beginFrame(runner, gameW, gameH,
                  windowW, windowH, framebufferW, framebufferH);
Runner_drawViews(runner, gameW, gameH, debugShowCollisionMasks);
```

Refresh references `Runner.appSurfaceKeepWindowSize`, which has been removed.
Do not restore the field. Rework the behavior around current application
surface, viewport, display scale, and framebuffer semantics.

### 4.5 Filesystem Interface

Refresh only implements the first five `FileSystemVtable` callbacks. Next also
requires:

- Whole-file binary read and write
- Binary open, close, read, write, tell, seek, size, and rewrite
- Directory exists, create, delete, and list

Use the local modified tree's binary read/write functions as a starting point.
Implement streaming handles using XDK file handles or a small owned wrapper.
Implement directory enumeration with XDK-supported Win32 APIs.

This is required for `file_bin_*`, buffer save/load, save files, and filename
enumeration in WAD17 games.

### 4.6 Input

Use a separate `xdk_gamepad.cpp` backend, based on the local modified tree and
the current Next PS2 gamepad pattern:

1. Call `RunnerGamepad_beginFrame` once per game frame.
2. Poll XInput and update `GamepadSlot` state.
3. Preserve pressed/released transitions across catch-up frames correctly.
4. Keep keyboard emulation as a separate compatibility mapping.
5. Do not disable the GameMaker gamepad API by default.

Support at least controller slot 0 initially. Expand to four controllers after
the single-controller path is stable.

### 4.7 Audio

Refresh XAudio2 is the primary implementation source, but adapt it to Next:

- Set `AudioSystem.dw` during initialization.
- Implement `setMasterGainForListener` as supported behavior or a documented
  platform no-op.
- Retain streaming and embedded audio paths.
- Verify `STB_VORBIS_BIG_ENDIAN` behavior on hardware.
- Keep room-name and sound-name music special cases out of the initial backend
  commit; add them later as compatibility policy if still necessary.

### 4.8 XDK/MSVC Compatibility

Do not reuse the complete Refresh `compat_msvc.h`. It overrides utility macros
that Next now implements itself.

Create a reduced compatibility header containing only what VS2010/XDK lacks,
such as:

- CRT name mappings
- Missing C99 math functions and constants
- `snprintf`/`vsnprintf` handling
- `nullptr`/stdbool support where needed
- Xbox abort and diagnostic hooks
- Platform time support

Compile shared `.c` files as C++ only if required by the XDK compiler, and use
Next's explicit pointer casts rather than redefining allocation macros.

## 5. Build Project Requirements

Rebuild the XDK project source list from the Next tree. Do not copy the Refresh
list unchanged.

Include at minimum:

- Current root `src/*.c` engine sources required by the runner
- `src/gettime.c`
- `src/image/image_decoder.c`
- `src/debug_font/debug_font.c` if the loading/diagnostic UI uses it
- Xbox platform `.cpp` files
- One STB implementation translation unit
- Next base64, MD5, SHA1, and bzip2 sources required by the current core

Do not include Refresh-only bzip2 compression sources that no longer exist in
Next. `sprite_shaders.hlsl` is currently not part of the Refresh build and can
remain excluded until precompiled shader integration is deliberately added.

Recommended Xbox build definitions:

```text
_XBOX
XBOX
IS_BIG_ENDIAN
ENABLE_WAD14
ENABLE_WAD16
ENABLE_WAD17
BZ_NO_STDIO
STB_VORBIS_BIG_ENDIAN
```

Do not enable `NO_RVALUE_INT64` for the WAD17 target.

## 6. Implementation Phases

### Phase 0: Branch and Build Skeleton - Complete

- Create `port/xbox360` from Next `main`.
- Add the XDK solution and project.
- Add the reduced compatibility header and platform header shims.
- Align the project source list with Next.
- Add endian definitions.
- Build with a noop audio backend if necessary.

Exit criteria:

- XDK Release compilation reaches platform code without shared-core compile
  errors.
- No Next shared source file has been replaced by an old copy.

Result: `port/xbox360`, the VS2010/XDK solution and project, reduced
compatibility shims, endian definitions, and the Next-aligned source list are
in place. Release builds succeed without shared-core compile errors.

### Phase 1: Filesystem and Input - Complete for One Controller

- Implement the complete XDK filesystem vtable.
- Add the separate XDK gamepad backend.
- Add fixed 720p window/framebuffer callbacks.
- Keep diagnostic logging minimal and reliable.

Exit criteria:

- Files can be read and written next to `data.win`.
- `file_bin_*` calls do not use null callbacks.
- Keyboard-style and GameMaker gamepad input both work.

Result: filesystem probes pass on hardware, `data.win` and adjacent files are
accessible, binary callbacks are populated, and controller count correctly
changes from zero when disconnected to one when connected. Four-controller
expansion remains optional follow-up work.

### Phase 2: Minimal D3D9 Renderer - Complete

- Port Refresh D3D9 resource setup and sprite batching.
- Rebuild the vtable against Next.
- Implement or safely stub every required callback.
- Use Next matrix and render lifecycle semantics.
- Use Next `ImageDecoder`.

Exit criteria:

- A GMS1 game reaches and renders its first room.
- The WAD17 test game decodes its first texture pages and reaches its first room.

Result: Deltarune Chapter 4 reaches and renders its rooms on hardware. All 43
`2zoq` pages validate and decode. Sprite rotation, semi-transparent rendering,
and fade behavior have been corrected. The D3D alpha path clamps before integer
packing and supports separate alpha blend factors.

### Phase 3: Surfaces and WAD17 Rendering - In Progress

- Adapt application surface handling.
- Implement user surfaces, target switching, surface copy, and surface textures.
- Implement matrix updates and GUI projection.
- Add lazy texture loading and the GPU page cache.

Exit criteria:

- GMS1 regression remains functional.
- WAD17 rooms, layers, GUI, surfaces, and dynamic sprites render correctly.
- Room transitions do not show stale or one-frame incorrect surfaces.

Implemented so far:

- Basic user surfaces, render-target switching, copy, resize, free, and texture
  handles.
- Dynamic sprites created from surfaces.
- GUI projection and the current Runner draw lifecycle.
- Lazy TXTR loading and release of uploaded compressed blobs.
- Xbox-only static texture conversion to linear `A4R4G4B4`.
- A 240 MiB frame-aware LRU that protects pages used in the current frame and
  backs off failed allocations instead of retrying every draw call.

Remaining:

- Full application-surface composition and validation.
- Camera/view matrix semantics beyond the current 2D transform path.
- Custom GameMaker shaders and surface pixel readback.
- Broader room-transition and dynamic-surface regression coverage.

### Phase 4: XAudio2 - Baseline Implemented, Validation In Progress

- Port the generic XAudio2 backend.
- Verify embedded audio, external OGG, streaming, looping, pause/resume, gain,
  pitch, track position, and audio groups.
- Verify memory is released when instances and streams stop.

Exit criteria:

- GMS1 and WAD17 audio both work without speed or endian errors.
- Long-running music does not continuously grow memory usage.

Result so far: Chapter 4 audio plays normally on hardware, including its loaded
audio group. Embedded and external WAV/OGG paths, looping, pause/resume, gain,
pitch, duration, and streaming callbacks are implemented. Track-position seek,
many-voice stress, and long-duration memory testing remain.

### Phase 5: Refresh Enhancements - Partially Started

- Loading screen
- Diagnostic overlay
- Memory statistics
- Texture cache tuning
- Startup and room transition diagnostics
- NXTale or game-specific compatibility fixes that remain necessary

Keep generic platform behavior separate from game-specific compatibility
commits.

Completed from this phase: startup/room diagnostics, texture allocation error
reporting, exact `2zoq` memory sizing, 16-bit static texture upload, cache
tuning, and retry throttling. Loading UI, a dedicated diagnostic overlay, and
general memory-stat presentation remain.

## 7. Validation Matrix

Run the same scenarios after every major phase:

| Test | Required checks | Current status |
| --- | --- | --- |
| GMS1 baseline | Boot, first room, movement, room transition, save/load, audio | Desktop regression previously passed; Xbox hardware coverage still required |
| WAD17 baseline | Boot, VM execution, layers, textures, surfaces, input, save/load | Chapter 4 boots and plays through tested church/arena rooms; save/load and return-to-title remain incomplete |
| Input | Keyboard mapping, GameMaker gamepad API, analog sticks, triggers | Slot 0 and keyboard mapping work on hardware; four-slot coverage pending |
| Rendering | Sprites, rotation, blend modes, GUI, application surface, user surfaces | Sprites, rotation, alpha fades, GUI, and basic user surfaces work; full application surface and shaders pending |
| Audio | Embedded audio, external OGG, looping, pause/resume, pitch and gain | Normal Chapter 4 playback and audio groups work; seek and long-run stress pending |
| Memory | Startup peak, room transition peak, texture eviction, long-run stability | 16-bit TXTR plus 240 MiB LRU is smooth in tested rooms; extended play and dynamic-surface peaks pending |

Record the exact test game build and `data.win` hash so Refresh, the local
modified build, and Next can be compared consistently.

## 8. Recommended Commit Sequence

```text
xbox360: add XDK build skeleton
xbox360: add VS2010 compatibility and endian configuration
xbox360: add complete filesystem backend
xbox360: add XInput gamepad backend
xbox360: add minimal D3D9 renderer
xbox360: adapt renderer surfaces and matrices
xbox360: add WAD17 texture decoding and lazy loading
xbox360: add XAudio2 backend
xbox360: add loading and diagnostic overlays
xbox360: add targeted game compatibility fixes
```

## 9. Next Implementation Priorities

1. Complete and validate application-surface composition, target restoration,
   and user-surface orientation across room and GUI passes.
2. Run the GMS1 hardware regression matrix, including save/load and room
   transitions, without changing the current Next VM or parser baseline.
3. Stress XAudio2 over long sessions and implement
   `audio_sound_set_track_position` seeking.
4. Validate the 240 MiB texture cache through later Chapter 4 rooms, watching
   title memory, `CreateTexture` failures, gradient banding, and dynamic-surface
   peaks.
5. Expand XInput from slot 0 to four controllers if required.
6. Add a loading/diagnostic overlay after the rendering and memory behavior is
   stable.

Deferred by current hardware-test decision: diagnose the return-to-title hang.
Do not let that issue block the other Phase 3 and Phase 4 validation work.

Continue to treat the local modified `Butterscotch-360` tree as read-only
reference material. Do not replace current shared core files with old-fork
copies.
