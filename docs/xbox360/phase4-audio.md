# Xbox 360 XAudio2 Baseline

## Implemented

- Added the XDK XAudio2 backend under `src/xbox360-xdk` and rebuilt the current
  `AudioSystemVtable` using named assignments.
- Supports embedded and compressed AUDO entries, external OGG files, WAV and
  OGG decoding, looping, pause/resume, gain fades, pitch, master gain, streams,
  and synchronous audio-group loading.
- Preserves Xbox 360 big-endian PCM handling and uses the portable
  `stb_vorbis` conversion path.
- Uses the current GameMaker audio flag rules. Compressed AUDO resources are no
  longer misclassified as external files.
- Audio groups are stored at their actual group index, including the current
  AGRP path field when present.
- `audio_sound_length` now reads real WAV/OGG duration instead of returning the
  old fixed `0.001` seconds, and master gain correctly accepts zero.
- The first 32 voice starts are logged with resource name, index, duration,
  sample rate, channels, and loop state for hardware diagnosis.

## Remaining Phase 4 Work

- `audio_sound_set_track_position` does not seek yet.
- Long-run memory, many simultaneous voices, external streams, audio groups,
  and endian playback still require hardware validation.
- Per-listener gain maps to the single XAudio2 mastering voice.

This is a hardware-testable generic baseline, not a claim that the complete
Phase 4 validation matrix has passed.

## Current Artifact

```text
Release/Butterscotch360-Next.xex
size:   3973120 bytes
SHA256: A49D42D17CDB3F2298679E110F69E0768B80CDFCE49C01EFC554EF19B16EEB2E
```
