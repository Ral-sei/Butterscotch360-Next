# Xbox 360 Phase 2 Build

## Implemented

- Added the XDK D3D9 backend under `src/xbox360-xdk` without replacing the
  shared renderer, runner, VM, or parser.
- Rebuilt the current `RendererVtable` using named assignments.
- Added D3D9 device creation, sprite batching, primitive and text drawing,
  point-filtered texture upload, blend state, basic user surfaces, texture
  handles, and dynamic sprites.
- Texture pages are decoded through the current Next `ImageDecoder` and are
  uploaded to the GPU on first use.
- Xbox texture parsing now uses `lazyLoadTextures`. Each page is read when it
  is first referenced, and its compressed blob is released after a successful
  GPU upload, matching the current GL backend lifecycle.
- GameMaker 2022.5+ `2zoq` pages use their declared decompressed length instead
  of allocating the QOI worst case. For a 2048x2048 page this removes an
  unnecessary allocation of roughly 20 MiB; the Chapter 4 fixture's largest
  declared stream is 2,137,795 bytes.
- Sprite, sprite-part, text, and surface rotation use the same negative
  GameMaker angle convention as the current GL backend.
- D3D vertex and clear alpha are clamped as floating-point values before being
  packed to eight bits. Values slightly above 1 or below 0 no longer wrap at
  the integer conversion boundary, which previously made fades and other
  semi-transparent effects restart or flash. Separate alpha blending is also
  enabled so extended color/alpha blend factors apply to render targets.
- The Xbox backend uploads original TXTR pages as linear `A4R4G4B4` textures;
  runtime surfaces and dynamically-created sprites remain 32-bit. This halves
  each 2048x2048 static page from 16 MiB to 8 MiB. The Chapter 4 arena's
  approximately 296 MiB same-frame RGBA working set therefore fits in about
  148 MiB, avoiding frame-to-frame eviction thrashing.
- Original TXTR pages use a 240 MiB frame-aware LRU
  cache. Pages used in the current frame are protected; older room/menu pages
  are released before a new static texture is created. Dynamic runtime textures
  are not included in this eviction pool.
- A failed page is retried at most once every 30 rendered frames. This prevents
  repeated decode/upload attempts and diagnostic logging from collapsing the
  frame rate when allocation cannot succeed immediately.
- Added safe callbacks for every required current renderer operation. Custom
  GameMaker shader uniforms and surface pixel readback remain unsupported.
- Replaced the Phase 1 probe-only entry point with the current Next
  `VM_create`, `Runner_create`, and render lifecycle. The filesystem contract
  probe still runs before game startup.

The application surface currently uses the renderer contract's direct-host
sentinel. User surfaces are backed by D3D9 render-target textures. Full
application-surface composition, camera-matrix projection, shader emulation,
and surface readback remain Phase 3 work.

## Verification

- `Release|Xbox 360`: succeeded with one existing numeric-conversion warning
  in shared `text_utils.h` and no errors.
- Read-only validation of `datach4.win` found 43 `2zoq` pages. All 43 BZip2
  streams decoded to `fioq`, matched their declared lengths, and matched the
  dimensions in their outer headers.
- Current hardware-test XEX: 3,973,120 bytes, SHA-256
  `A49D42D17CDB3F2298679E110F69E0768B80CDFCE49C01EFC554EF19B16EEB2E`.
- The current desktop GCC rebuild could not be rerun because the local MSYS2
  compiler exits without diagnostics even for an unchanged bzip2 source file;
  the independent Chapter 4 BZip2 validation above passed.
- Desktop read-only WAD17 smoke test: reached frame 2 and transitioned through
  `room_gms_debug_failsafe`, `ROOM_INITIALIZE`, and `room_intro_ch2`.
- Old `Butterscotch-360` status remained stable; its game data and source tree
  were not modified.

## Hardware Verification

Tested on the Xbox 360 target with Deltarune Chapter 4 `data.win` (132,001,470
bytes, SHA-256
`07E2DF1088E56532B992FC9C59F88A4D66420ADA4DA9FF49BA6823A7F2CC3D47`):

- Video is visible and audio plays at the expected speed and ordering.
- Controller enumeration reports zero when disconnected and one when connected.
- All 43 Chapter 4 `2zoq` texture pages decode on hardware; the previous
  `BZ_MEM_ERROR` failures are resolved.
- Static `A4R4G4B4` pages with the 240 MiB cache run smoothly through the tested
  church and arena sections without the earlier RGBA allocation failures and
  frame-to-frame texture thrashing.
- Sprite rotation direction matches the current GL backend.
- Semi-transparent effects no longer flash, and fades no longer wrap from
  opaque back to transparent when GameMaker supplies alpha values outside the
  nominal `[0,1]` range.

## Known Issues

- Selecting the in-game option to return to the title screen currently hangs
  after the transition. This is intentionally deferred; do not treat it as an
  alpha/fade regression.
- Full application-surface composition, camera-matrix projection, custom shader
  emulation, and surface pixel readback remain Phase 3 work.
- Static texture pages use four bits per color and alpha channel. Watch for
  unacceptable gradient banding or transparency precision loss in later rooms.

## Hardware Retest

Deploy `Release/Butterscotch360-Next.xex` beside the same `data.win`. A healthy
startup should include:

```text
Butterscotch360-Next: Phase 2/4 startup (D3D9 + XAudio2)
Butterscotch360-Next: loaded ...
Butterscotch360-Next: Phase 2/4 ready (room=..., textures=..., audio=ready, controllers=...)
Butterscotch360-Next: D3D9 texture page=... size=...x...
```

The first room must remain visible for more than one frame. Any `Failed to
decode TXTR page` or `CreateTexture failed` line should be retained. The latter
includes the page dimensions and HRESULT so GPU-memory failures can be
distinguished from image-decoder failures.

For the Chapter 4 room transition test, `texture cache evict` lines are normal.
They should be followed by successful `D3D9 texture page` loads. Continuous
`CreateTexture failed ... hr=0x8007000E` lines indicate that the cache budget
still exceeds the title's available unified memory.
