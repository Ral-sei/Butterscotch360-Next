#ifndef BS_XBOX360_AUDIO_H
#define BS_XBOX360_AUDIO_H

// Butterscotch Xbox 360 port - XAudio2 audio backend.
//
// Implements the public AudioSystemVtable (src/audio_system.h) on top of the
// XDK's XAudio2. Xenon has no runtime audio decoder for Ogg Vorbis, so this
// backend decodes to 16-bit PCM itself: embedded AUDO entries are RIFF WAVE
// (PCM) already, external .ogg/.wav files go through the vendored stb_vorbis
// or the WAV parser. Xbox 360 is big-endian, so 16-bit PCM read from little-
// endian files is byte-swapped before it is submitted to a source voice (see
// "Audio Data and Endianness" in the XDK docs); stb_vorbis already produces
// host-order shorts and needs no swap.
//
// Port-owned. No public source is touched.

#include "common.h"
#include "audio_system.h"

#include <stdint.h>
#include <stdbool.h>

// Instance/stream id spaces. These mirror the conventions the other backends
// use (src/audio/miniaudio/ma_audio_system.h) so GML builtins that compare an
// id against a sound index keep working.
#define X360_MAX_SOUND_INSTANCES      96
#define X360_SOUND_INSTANCE_ID_BASE   100000
#define X360_MAX_AUDIO_STREAMS        16
#define X360_AUDIO_STREAM_INDEX_BASE  300000
#define X360_MAX_LISTENERS            4

// A decoded, big-endian, 16-bit interleaved PCM buffer. Reference counted so a
// decode cache and live sound instances can share one copy.
typedef struct X360PcmBlock {
    int32_t refs;
    uint32_t bytes;         // total bytes of PCM data
    uint32_t totalSamples;  // per-channel sample count
    uint16_t channels;
    uint32_t sampleRate;
    uint8_t *data;
} X360PcmBlock;

typedef struct {
    bool active;
    int32_t soundIndex;   // SOND index, or a stream index (>= X360_AUDIO_STREAM_INDEX_BASE)
    int32_t instanceId;   // unique id returned to GML
    int32_t priority;
    bool loop;
    bool paused;
    bool ended;           // buffer queue drained; reaped on the next update

    // Gain fading (seconds).
    float currentGain;
    float targetGain;
    float startGain;
    float fadeTimeRemaining;
    float fadeTotalTime;

    float sondVolume;     // SOND resource volume
    float pitch;          // frequency ratio applied to the voice

    // Spatial state (simple distance attenuation + equal-power pan).
    bool spatial;
    float spatialX, spatialY, spatialZ;
    float falloffRef, falloffMax, falloffFactor;
    float spatialGain;

    X360PcmBlock *pcm;    // shared decoded PCM (retained by this instance)
    uint32_t playOffset;  // first sample played (for seek); 0 normally
    uint32_t samplesPlayedAtStart;

    void *stream;         // X360StreamPlay* for an Ogg stream instance, else NULL
    void *voice;          // IXAudio2SourceVoice*
} X360SoundInstance;

typedef struct {
    bool active;
    char *filePath;       // resolved, owned
    float initialGain;
    float initialPitch;
    float lengthSeconds;

    // Compressed file bytes are loaded once and shared by every play of this
    // stream. Ogg streams are decoded in chunks by the instance (see
    // X360StreamPlay in xbox360_audio.c); they never allocate a full PCM copy.
    uint8_t *fileData;
    uint32_t fileSize;
    bool probed;          // fileData/format fields below are valid
    bool isOgg;
    int channels;
    uint32_t sampleRate;
    uint32_t totalFrames;

    // Non-Ogg (WAV) streams keep the simple whole-file decode path.
    X360PcmBlock *pcm;
} X360AudioStream;

typedef struct {
    AudioEntry *entry;
    X360PcmBlock *pcm;
} X360AudoCacheEntry;

typedef struct {
    AudioSystem base;

    void *xaudio;         // IXAudio2*
    void *mastering;      // IXAudio2MasteringVoice*
    FileSystem *fileSystem;
    bool initialized;
    float masterGain;
    float listenerX, listenerY, listenerZ;
    float listenerGains[X360_MAX_LISTENERS];

    X360SoundInstance instances[X360_MAX_SOUND_INSTANCES];
    int32_t nextInstanceCounter;

    X360AudioStream streams[X360_MAX_AUDIO_STREAMS];

    // Decoded PCM cache for embedded AUDO entries (stb_ds array).
    X360AudoCacheEntry *audoCache;
} X360AudioSystem;

X360AudioSystem *X360AudioSystem_create(void);

// Hardware smoke test for the backend (enabled with -DBS_X360_AUDIO_SELFTEST).
// Plays an embedded group-0 sound, checks that the voice advances, exercises
// gain/pitch/pause/resume/seek/stop, and logs PASS/FAIL. Never required by the
// runtime.
bool X360AudioSystem_runSelftest(X360AudioSystem *audio);

#endif // BS_XBOX360_AUDIO_H
