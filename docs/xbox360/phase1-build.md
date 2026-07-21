# Xbox 360 Phase 1 Build

## Implemented Backends

### File System

`src/xbox360-xdk/xdk_file_system.cpp` assigns every current
`FileSystemVtable` callback by name:

- Text and whole-file binary read/write/delete
- Streaming open/close/read/write/tell/seek/size/rewrite
- Directory exists/create/delete/list
- Device-qualified Xbox paths and paths relative to the `data.win` directory
- Parent directory creation for write operations

The Phase 1 executable runs a self-cleaning contract probe in
`__butterscotch_phase1_probe`. It exercises all write, streaming, and directory
operations, then deletes the files and directory. The startup log reports
`filesystem=pass` or `filesystem=fail`.

### Input

`src/xbox360-xdk/xdk_gamepad.cpp` supports controller slot 0. The
`XdkInput_beginFrame` entry point performs this sequence once per game frame:

1. Clear keyboard and GameMaker gamepad transitions.
2. Poll XInput once.
3. Update raw GameMaker button, trigger, and stick state.
4. Apply the separate keyboard compatibility mapping.

Stick values remain raw in `GamepadSlot`; the current Next gamepad query code
applies the configured deadzone once. Vertical axes are inverted to match the
desktop GameMaker convention.

### Platform Callbacks

`src/xbox360-xdk/xdk_platform.cpp` supplies fixed 1280x720 window and
framebuffer size, always-focused behavior, and safe title, resize, and cursor
callbacks. `XdkPlatform_attachRunnerCallbacks` will be called by the Phase 2
runner startup path.

## Verification

- `Release|Xbox 360`: succeeded, zero failed projects
- XEX produced: `Release/Butterscotch360-Next.xex`
- All 19 current filesystem callbacks: statically confirmed assigned
- Desktop GCC regression build: no work required, existing executable still
  passes the `--help` smoke test
- Old `Butterscotch-360` working tree: read only and unchanged

## Hardware Verification

Verified on an Xbox 360 development kit on 2026-07-21. The XEX was launched
from `DEVKIT\\chapter4_windows\\Butterscotch360-Next.xex` and returned normally
after printing:

```text
Butterscotch360-Next: Phase 1 platform ready (1280x720, data.win=present, filesystem=pass, controllers=0)
[XAPI RETURN VALUE] 0
```

This confirms the fixed framebuffer callbacks, `data.win` discovery, the full
filesystem contract probe, and probe cleanup on hardware. The captured run had
no controller connected and correctly reported `controllers=0`; a follow-up
run with a controller in slot 0 correctly reported `controllers=1`, confirming
XInput device enumeration on hardware.
