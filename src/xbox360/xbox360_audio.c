// Butterscotch Xbox 360 port - XAudio2 audio backend implementation.
//
// See xbox360_audio.h for the design summary. Nothing here touches a public
// source file; the backend plugs into the public AudioSystemVtable.

#include "xbox360_audio.h"

#include "data_win.h"
#include "log.h"
#include "utils.h"
#include "stb_ds.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <xtl.h>
#include <xaudio2.h>

// Declarations only; the implementation is compiled once in
// src/xbox360/stb_vorbis_impl.c.
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

// Highest frequency ratio we allow on a source voice. GameMaker's pitch is a
// multiplier around 1.0; the target data stays inside 0.25..4.0.
#define X360_MAX_FREQ_RATIO 4.0f

// Streaming playback state (functions live with the streaming implementation
// below). The struct is defined here because instance helpers need its format
// fields and instance teardown releases it.
typedef enum {
    X360_STREAM_SLOT_FREE = 0,
    X360_STREAM_SLOT_DECODING,
    X360_STREAM_SLOT_FILLED,
    X360_STREAM_SLOT_SUBMITTED,
} X360StreamSlotState;

typedef struct {
    uint8_t *data;
    uint32_t frames;
    uint32_t startFrame;
    X360StreamSlotState state;
    bool endOfStream;
} X360StreamSlot;

#define X360_STREAM_SLOTS       12
#define X360_STREAM_SLOT_FRAMES 16384 // ~0.34 s at 48 kHz
#define X360_STREAM_MAX_QUEUED  X360_STREAM_SLOTS

typedef struct X360StreamPlay {
    X360AudioStream *stream; // borrowed from the audio system's stream table
    stb_vorbis *vorbis;
    int channels;
    uint32_t sampleRate;
    uint32_t totalFrames;

    uint32_t nextFrame;      // next source frame the decoder will produce
    uint32_t startFrame;     // frame the current voice started at (for position)
    uint64_t samplesAtStart; // SamplesPlayed right after the voice was started
    bool loop;

    volatile LONG stop;
    volatile LONG eof;
    HANDLE thread;
    HANDLE wake;
    CRITICAL_SECTION lock;

    X360StreamSlot slots[X360_STREAM_SLOTS];
    // Ring of submitted slots, oldest first. Used to recycle buffers once the
    // voice has played them (pCurrentBufferContext marks the in-flight one).
    X360StreamSlot *fifo[X360_STREAM_SLOTS];
    uint32_t fifoHead;
    uint32_t fifoTail;
} X360StreamPlay;

static void x360StreamPlayDestroy(X360StreamPlay *p);

// ===[ Small helpers ]===

static uint16_t x360Rd16le(const uint8_t *p) {
    return (uint16_t) (p[0] | (p[1] << 8));
}

static uint32_t x360Rd32le(const uint8_t *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

// Xbox 360 is big-endian; little-endian file data must be swapped to the byte
// order XAudio2 expects. stb_vorbis already emits host-order shorts, so its
// output is not passed through here.
static void x360Swap16(uint8_t *data, uint32_t bytes) {
    for (uint32_t i = 0; i + 1 < bytes; i += 2) {
        uint8_t t = data[i];
        data[i] = data[i + 1];
        data[i + 1] = t;
    }
}

static X360PcmBlock *x360PcmCreate(uint32_t bytes, uint16_t channels, uint32_t sampleRate) {
    X360PcmBlock *b = (X360PcmBlock *) safeCalloc(1, sizeof(X360PcmBlock));
    b->refs = 1;
    b->bytes = bytes;
    b->channels = channels;
    b->sampleRate = sampleRate;
    b->totalSamples = (channels > 0) ? (bytes / (uint32_t) channels / 2u) : 0;
    b->data = (uint8_t *) safeMalloc(bytes > 0 ? bytes : 1);
    return b;
}

static void x360PcmRetain(X360PcmBlock *b) {
    if (b != nullptr) b->refs++;
}

static void x360PcmRelease(X360PcmBlock *b) {
    if (b == nullptr) return;
    if (--b->refs > 0) return;
    free(b->data);
    free(b);
}

// ===[ Decoders ]===

// Parse a RIFF WAVE container holding 16-bit PCM and return it as a
// big-endian PCM block. Returns NULL for anything unsupported (the target data
// is all 16-bit PCM, but non-PCM/8-bit/24-bit is rejected rather than guessed).
static X360PcmBlock *x360DecodeWav(const uint8_t *d, uint32_t n) {
    if (n < 12 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WAVE", 4) != 0) return nullptr;

    uint16_t tag = 0, channels = 0, bits = 0;
    uint32_t sampleRate = 0;
    const uint8_t *pcm = nullptr;
    uint32_t pcmBytes = 0;

    uint32_t p = 12;
    while (p + 8 <= n) {
        uint32_t chunkSize = x360Rd32le(d + p + 4);
        const uint8_t *chunk = d + p + 8;
        uint32_t avail = n - (p + 8);
        if (chunkSize > avail) chunkSize = avail; // tolerate a truncated final chunk
        if (memcmp(d + p, "fmt ", 4) == 0 && chunkSize >= 16) {
            tag = x360Rd16le(chunk);
            channels = x360Rd16le(chunk + 2);
            sampleRate = x360Rd32le(chunk + 4);
            bits = x360Rd16le(chunk + 14);
            if (tag == WAVE_FORMAT_EXTENSIBLE && chunkSize >= 40) {
                tag = x360Rd16le(chunk + 24); // SubFormat GUID's first WORD
            }
        } else if (memcmp(d + p, "data", 4) == 0) {
            pcm = chunk;
            pcmBytes = chunkSize;
        }
        p += 8 + chunkSize + (chunkSize & 1);
    }

    if (tag != WAVE_FORMAT_PCM || bits != 16 || channels == 0 || sampleRate == 0 || pcm == nullptr || pcmBytes == 0) {
        logWarn("X360Audio: unsupported WAVE (tag=%u bits=%u ch=%u)\n", tag, bits, channels);
        return nullptr;
    }

    X360PcmBlock *b = x360PcmCreate(pcmBytes, channels, sampleRate);
    memcpy(b->data, pcm, pcmBytes);
    x360Swap16(b->data, b->bytes);
    return b;
}

// Decode an Ogg Vorbis stream from memory. stb_vorbis returns host-order
// 16-bit samples, which is already what XAudio2 wants on big-endian Xenon.
static X360PcmBlock *x360DecodeOgg(const uint8_t *d, uint32_t n) {
    int channels = 0, sampleRate = 0;
    short *output = nullptr;
    int frames = stb_vorbis_decode_memory(d, (int) n, &channels, &sampleRate, &output);
    if (frames <= 0 || output == nullptr || channels <= 0 || sampleRate <= 0) {
        if (output != nullptr) free(output);
        logWarn("X360Audio: Ogg decode failed (%d)\n", frames);
        return nullptr;
    }
    X360PcmBlock *b = (X360PcmBlock *) safeCalloc(1, sizeof(X360PcmBlock));
    b->refs = 1;
    b->channels = (uint16_t) channels;
    b->sampleRate = (uint32_t) sampleRate;
    b->totalSamples = (uint32_t) frames;
    b->bytes = (uint32_t) frames * (uint32_t) channels * 2u;
    b->data = (uint8_t *) output; // take ownership of stb_vorbis' buffer
    return b;
}

// Header-only WAV probe. Used for stream length queries so asking for the
// length of a long file does not decode (or even copy) its PCM.
static bool x360ProbeWav(const uint8_t *d, uint32_t n, int *outChannels,
                         uint32_t *outSampleRate, uint32_t *outFrames) {
    if (n < 12 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WAVE", 4) != 0) return false;

    uint16_t tag = 0, channels = 0, bits = 0;
    uint32_t sampleRate = 0, dataBytes = 0;
    bool haveData = false;

    uint32_t p = 12;
    while (p + 8 <= n) {
        uint32_t chunkSize = x360Rd32le(d + p + 4);
        const uint8_t *chunk = d + p + 8;
        uint32_t avail = n - (p + 8);
        if (chunkSize > avail) chunkSize = avail;
        if (memcmp(d + p, "fmt ", 4) == 0 && chunkSize >= 16) {
            tag = x360Rd16le(chunk);
            channels = x360Rd16le(chunk + 2);
            sampleRate = x360Rd32le(chunk + 4);
            bits = x360Rd16le(chunk + 14);
            if (tag == WAVE_FORMAT_EXTENSIBLE && chunkSize >= 40) tag = x360Rd16le(chunk + 24);
        } else if (memcmp(d + p, "data", 4) == 0) {
            dataBytes = chunkSize;
            haveData = true;
        }
        p += 8 + chunkSize + (chunkSize & 1);
    }

    if (tag != WAVE_FORMAT_PCM || bits != 16 || channels == 0 || sampleRate == 0 || !haveData) return false;
    *outChannels = (int) channels;
    *outSampleRate = sampleRate;
    *outFrames = dataBytes / ((uint32_t) channels * 2u);
    return true;
}

static X360PcmBlock *x360DecodeAudio(const uint8_t *d, uint32_t n) {
    if (n >= 4 && memcmp(d, "RIFF", 4) == 0) return x360DecodeWav(d, n);
    if (n >= 4 && memcmp(d, "OggS", 4) == 0) return x360DecodeOgg(d, n);
    logWarn("X360Audio: unknown audio container (magic %.4s)\n", (const char *) d);
    return nullptr;
}

// ===[ Embedded AUDO cache ]===

static X360PcmBlock *x360AudoPcm(X360AudioSystem *s, AudioEntry *entry) {
    for (int32_t i = 0; i < (int32_t) arrlen(s->audoCache); i++) {
        if (s->audoCache[i].entry == entry) return s->audoCache[i].pcm;
    }
    if (entry == nullptr || entry->data == nullptr || entry->dataSize == 0) return nullptr;

    X360PcmBlock *pcm = x360DecodeAudio(entry->data, entry->dataSize);
    X360AudoCacheEntry e = {.entry = entry, .pcm = pcm};
    arrput(s->audoCache, e);
    return pcm;
}

// ===[ File-based sounds/streams ]===

// Return the file name to open for a SOND/stream entry, appending ".ogg" when
// the entry has no extension (matching the other backends). Caller frees.
static char *x360AudioFileName(const char *file) {
    if (file == nullptr || file[0] == '\0') return nullptr;
    if (strchr(file, '.') != nullptr) return safeStrdup(file);
    size_t len = strlen(file);
    char *out = (char *) safeMalloc(len + 5);
    memcpy(out, file, len);
    memcpy(out + len, ".ogg", 5);
    return out;
}

// Read and decode an audio file (used for external SOND entries and streams).
// readFileBinary resolves relative paths through the port file system and
// accepts the absolute paths createStream stores.
static X360PcmBlock *x360DecodeFile(X360AudioSystem *s, const char *file) {
    char *name = x360AudioFileName(file);
    if (name == nullptr) return nullptr;

    uint8_t *bytes = nullptr;
    int32_t size = 0;
    bool ok = s->fileSystem->vtable->readFileBinary(s->fileSystem, name, &bytes, &size);
    if (!ok || bytes == nullptr || size <= 0) {
        logWarn("X360Audio: could not read audio file '%s'\n", name);
        free(name);
        if (bytes != nullptr) free(bytes);
        return nullptr;
    }
    free(name);

    X360PcmBlock *pcm = x360DecodeAudio(bytes, (uint32_t) size);
    free(bytes);
    return pcm;
}

// ===[ Instance bookkeeping ]===

static bool x360ValidInstanceId(int32_t id) {
    return id >= X360_SOUND_INSTANCE_ID_BASE && id < X360_AUDIO_STREAM_INDEX_BASE;
}

static X360SoundInstance *x360FindInstanceById(X360AudioSystem *s, int32_t instanceId) {
    for (int32_t i = 0; i < X360_MAX_SOUND_INSTANCES; i++) {
        if (s->instances[i].active && s->instances[i].instanceId == instanceId) return &s->instances[i];
    }
    return nullptr;
}

static X360SoundInstance *x360FindFreeSlot(X360AudioSystem *s) {
    for (int32_t i = 0; i < X360_MAX_SOUND_INSTANCES; i++) {
        if (!s->instances[i].active) return &s->instances[i];
    }
    // No free slot: evict the lowest-priority slot whose buffer queue drained.
    X360SoundInstance *best = nullptr;
    for (int32_t i = 0; i < X360_MAX_SOUND_INSTANCES; i++) {
        X360SoundInstance *inst = &s->instances[i];
        if (inst->ended || inst->paused) {
            if (best == nullptr || best->priority > inst->priority) best = inst;
        }
    }
    return best;
}

static void x360ReleaseInstance(X360SoundInstance *inst) {
    if (inst->voice != nullptr) {
        IXAudio2SourceVoice_DestroyVoice((IXAudio2SourceVoice *) inst->voice);
        inst->voice = nullptr;
    }
    if (inst->stream != nullptr) {
        x360StreamPlayDestroy((X360StreamPlay *) inst->stream);
        inst->stream = nullptr;
    }
    x360PcmRelease(inst->pcm);
    inst->pcm = nullptr;
    inst->active = false;
    inst->ended = false;
    inst->paused = false;
}

static void x360InstanceApplyVolume(X360AudioSystem *s, X360SoundInstance *inst) {
    if (inst->voice == nullptr) return;
    float gain = inst->currentGain * AudioSystem_soundGroupGain(&s->base, inst->soundIndex) * inst->spatialGain;
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    IXAudio2Voice_SetVolume((IXAudio2Voice *) inst->voice, gain, XAUDIO2_COMMIT_NOW);
}

// Simple equal-power pan for a mono spatial source; stereo sources are left
// centred (only attenuated). This is a deliberate first-pass approximation of
// GameMaker's 3D audio: no Doppler and no HRTF.
static void x360InstanceApplyPan(X360AudioSystem *s, X360SoundInstance *inst) {
    if (inst->voice == nullptr || !inst->spatial) return;
    int channels = 0;
    if (inst->pcm != nullptr) {
        channels = inst->pcm->channels;
    } else if (inst->stream != nullptr) {
        channels = ((X360StreamPlay *) inst->stream)->channels;
    }
    if (channels != 1) return;
    float dx = inst->spatialX - s->listenerX;
    float dy = inst->spatialY - s->listenerY;
    float dz = inst->spatialZ - s->listenerZ;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    float pan = (dist > 0.0001f) ? (dx / dist) : 0.0f;
    if (pan < -1.0f) pan = -1.0f;
    if (pan > 1.0f) pan = 1.0f;
    float angle = (pan + 1.0f) * 0.7853981634f; // (pan+1)*pi/4
    float levels[2] = {cosf(angle), sinf(angle)};
    IXAudio2Voice_SetOutputMatrix((IXAudio2Voice *) inst->voice, nullptr, 1, 2, levels, XAUDIO2_COMMIT_NOW);
}

// GameMaker falloff: full volume inside `ref`, then inverse rolloff out to
// `max`. Mirrors the arguments of audio_play_sound_at.
static void x360InstanceSpatialFromListener(X360AudioSystem *s, X360SoundInstance *inst) {
    if (!inst->spatial) {
        inst->spatialGain = 1.0f;
        return;
    }
    float dx = inst->spatialX - s->listenerX;
    float dy = inst->spatialY - s->listenerY;
    float dz = inst->spatialZ - s->listenerZ;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);

    float ref = inst->falloffRef > 0.0f ? inst->falloffRef : 0.0001f;
    float max = inst->falloffMax > ref ? inst->falloffMax : ref;
    float factor = inst->falloffFactor > 0.0f ? inst->falloffFactor : 1.0f;

    float gain = 1.0f;
    if (dist > ref) {
        gain = ref / (ref + factor * (dist - ref));
    }
    if (dist > max) gain = 0.0f;
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    inst->spatialGain = gain;
}

// Submit the PCM (from `offset` samples) to a freshly created voice and start it.
static bool x360InstanceStartVoice(X360AudioSystem *s, X360SoundInstance *inst, uint32_t offset) {
    X360PcmBlock *pcm = inst->pcm;
    if (pcm == nullptr || pcm->channels == 0 || pcm->totalSamples == 0) return false;
    if (offset >= pcm->totalSamples) offset = 0;

    uint32_t frameBytes = (uint32_t) pcm->channels * 2u;
    uint32_t startByte = offset * frameBytes;
    uint32_t bytes = pcm->bytes - startByte;
    uint32_t samples = pcm->totalSamples - offset;
    if (bytes == 0 || samples == 0) return false;

    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = pcm->channels;
    wfx.nSamplesPerSec = pcm->sampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD) frameBytes;
    wfx.nAvgBytesPerSec = pcm->sampleRate * frameBytes;
    wfx.cbSize = 0;

    IXAudio2SourceVoice *voice = nullptr;
    HRESULT hr = IXAudio2_CreateSourceVoice((IXAudio2 *) s->xaudio, &voice, &wfx, 0, X360_MAX_FREQ_RATIO,
                                            nullptr, nullptr, nullptr);
    if (FAILED(hr) || voice == nullptr) {
        logWarn("X360Audio: CreateSourceVoice failed (0x%08X)\n", (unsigned) hr);
        return false;
    }
    inst->voice = voice;

    XAUDIO2_BUFFER buf;
    memset(&buf, 0, sizeof(buf));
    buf.AudioBytes = bytes;
    buf.pAudioData = pcm->data + startByte;
    buf.Flags = XAUDIO2_END_OF_STREAM;
    if (inst->loop) {
        buf.LoopBegin = 0;
        buf.LoopLength = samples;
        buf.LoopCount = XAUDIO2_LOOP_INFINITE;
    }

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(voice, &buf, nullptr);
    if (FAILED(hr)) {
        logWarn("X360Audio: SubmitSourceBuffer failed (0x%08X)\n", (unsigned) hr);
        IXAudio2SourceVoice_DestroyVoice(voice);
        inst->voice = nullptr;
        return false;
    }

    inst->playOffset = offset;
    if (inst->pitch > 0.0f) IXAudio2SourceVoice_SetFrequencyRatio(voice, inst->pitch, XAUDIO2_COMMIT_NOW);
    x360InstanceSpatialFromListener(s, inst);
    x360InstanceApplyVolume(s, inst);
    x360InstanceApplyPan(s, inst);

    hr = IXAudio2SourceVoice_Start(voice, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        logWarn("X360Audio: Start failed (0x%08X)\n", (unsigned) hr);
        IXAudio2SourceVoice_DestroyVoice(voice);
        inst->voice = nullptr;
        return false;
    }

    XAUDIO2_VOICE_STATE st;
    IXAudio2SourceVoice_GetState(voice, &st, 0);
    inst->samplesPlayedAtStart = (uint32_t) st.SamplesPlayed;
    inst->paused = false;
    inst->ended = false;
    return true;
}

// ===[ Streaming Ogg playback ]===
//
// A stream used to be decoded in one shot the first time it was played, which
// blocks the game thread for seconds on a multi-minute track (and allocates
// tens of MB of PCM). Ogg streams are now decoded in fixed-size chunks by a
// worker thread and fed to XAudio2 as a buffer queue, so starting a track costs
// one chunk instead of the whole file.

// Reads the compressed file once and caches format/length in the stream
// resource. Ogg playback opens its own decoder per instance over these bytes.
static bool x360StreamLoadFile(X360AudioSystem *s, X360AudioStream *stream) {
    if (stream->probed) return true;

    char *name = x360AudioFileName(stream->filePath);
    if (name == nullptr) return false;

    uint8_t *bytes = nullptr;
    int32_t size = 0;
    bool ok = s->fileSystem->vtable->readFileBinary(s->fileSystem, name, &bytes, &size);
    free(name);
    if (!ok || bytes == nullptr || size <= 0) {
        logWarn("X360Audio: could not read stream file '%s'\n", stream->filePath);
        if (bytes != nullptr) free(bytes);
        return false;
    }

    stream->fileData = bytes;
    stream->fileSize = (uint32_t) size;
    stream->probed = true;

    if (size >= 4 && memcmp(bytes, "OggS", 4) == 0) {
        int error = 0;
        stb_vorbis *v = stb_vorbis_open_memory(bytes, size, &error, nullptr);
        if (v == nullptr) {
            logWarn("X360Audio: could not parse Ogg stream '%s' (error %d)\n", stream->filePath, error);
            return false;
        }
        stb_vorbis_info info = stb_vorbis_get_info(v);
        if (info.channels <= 0 || info.sample_rate <= 0) {
            logWarn("X360Audio: Ogg stream '%s' has invalid format\n", stream->filePath);
            stb_vorbis_close(v);
            return false;
        }
        stream->isOgg = true;
        stream->channels = info.channels;
        stream->sampleRate = (uint32_t) info.sample_rate;
        stream->totalFrames = stb_vorbis_stream_length_in_samples(v);
        stb_vorbis_close(v);
        stream->lengthSeconds = stream->sampleRate > 0
            ? (float) stream->totalFrames / (float) stream->sampleRate : 0.0f;
        logInfo("X360Audio: stream '%s' is %.2f s %d ch %u Hz (chunked)\n",
                stream->filePath, (double) stream->lengthSeconds,
                stream->channels, (unsigned) stream->sampleRate);
        return true;
    }

    int channels = 0;
    uint32_t rate = 0, frames = 0;
    if (x360ProbeWav(bytes, (uint32_t) size, &channels, &rate, &frames)) {
        stream->isOgg = false;
        stream->channels = channels;
        stream->sampleRate = rate;
        stream->totalFrames = frames;
        stream->lengthSeconds = rate > 0 ? (float) frames / (float) rate : 0.0f;
        return true;
    }

    logWarn("X360Audio: unsupported stream container '%s'\n", stream->filePath);
    return false;
}

static IXAudio2SourceVoice *x360StreamCreateVoice(X360AudioSystem *s, X360StreamPlay *p) {
    uint32_t frameBytes = (uint32_t) p->channels * 2u;
    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD) p->channels;
    wfx.nSamplesPerSec = p->sampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD) frameBytes;
    wfx.nAvgBytesPerSec = p->sampleRate * frameBytes;
    wfx.cbSize = 0;

    IXAudio2SourceVoice *voice = nullptr;
    HRESULT hr = IXAudio2_CreateSourceVoice((IXAudio2 *) s->xaudio, &voice, &wfx, 0,
                                            X360_MAX_FREQ_RATIO, nullptr, nullptr, nullptr);
    if (FAILED(hr) || voice == nullptr) {
        logWarn("X360Audio: CreateSourceVoice (stream) failed (0x%08X)\n", (unsigned) hr);
        return nullptr;
    }
    return voice;
}

// Decodes one chunk into a slot. Only the decode thread (or the game thread
// before the thread exists / after it was joined) may call this, because it
// touches the stb_vorbis state and p->nextFrame.
static bool x360StreamDecodeSlot(X360StreamPlay *p, X360StreamSlot *slot) {
    slot->frames = 0;
    slot->endOfStream = false;

    int got = stb_vorbis_get_samples_short_interleaved(
        p->vorbis, p->channels, (short *) slot->data,
        X360_STREAM_SLOT_FRAMES * p->channels);
    if (got < 0) {
        logWarn("X360Audio: stream decode failed\n");
        return false;
    }

    if (got > 0) {
        slot->startFrame = p->nextFrame;
        slot->frames = (uint32_t) got;
        p->nextFrame += (uint32_t) got;
    }

    bool atEnd = (got == 0) || (got < X360_STREAM_SLOT_FRAMES) ||
                 (p->totalFrames > 0 && p->nextFrame >= p->totalFrames);

    if (atEnd) {
        if (p->loop) {
            // The partial chunk (if any) plays first; the next decode starts
            // from the beginning, so the loop point stays gapless.
            if (stb_vorbis_seek_start(p->vorbis)) {
                p->nextFrame = 0;
            } else {
                logWarn("X360Audio: stream loop seek failed\n");
                return false;
            }
        } else {
            slot->endOfStream = (got > 0);
            InterlockedExchange(&p->eof, 1);
        }
    }
    return true;
}

static DWORD WINAPI x360StreamDecodeThread(void *arg) {
    X360StreamPlay *p = (X360StreamPlay *) arg;
    for (;;) {
        if (InterlockedCompareExchange(&p->stop, 0, 0) != 0) break;

        X360StreamSlot *slot = nullptr;
        EnterCriticalSection(&p->lock);
        for (int32_t i = 0; i < X360_STREAM_SLOTS; i++) {
            if (p->slots[i].state == X360_STREAM_SLOT_FREE) {
                p->slots[i].state = X360_STREAM_SLOT_DECODING;
                slot = &p->slots[i];
                break;
            }
        }
        LeaveCriticalSection(&p->lock);

        if (slot == nullptr) {
            WaitForSingleObject(p->wake, 50);
            continue;
        }

        bool ok = x360StreamDecodeSlot(p, slot);

        EnterCriticalSection(&p->lock);
        slot->state = (ok && slot->frames > 0) ? X360_STREAM_SLOT_FILLED : X360_STREAM_SLOT_FREE;
        LeaveCriticalSection(&p->lock);

        if (!ok) {
            InterlockedExchange(&p->eof, 1);
            break;
        }
        // A non-looping stream that reached the end has nothing left to decode.
        if (!p->loop && InterlockedCompareExchange(&p->eof, 0, 0) != 0) break;

        // Stay polite: one 0.34 s chunk is only needed every ~340 ms, so a
        // 1 ms yield between chunks avoids a CPU burst competing with the game.
        Sleep(1);
    }
    return 0;
}

static X360StreamPlay *x360StreamPlayCreate(X360AudioStream *stream, bool loop) {
    X360StreamPlay *p = (X360StreamPlay *) safeCalloc(1, sizeof(X360StreamPlay));
    p->stream = stream;
    p->loop = loop;
    p->channels = stream->channels;
    p->sampleRate = stream->sampleRate;
    p->totalFrames = stream->totalFrames;

    int error = 0;
    p->vorbis = stb_vorbis_open_memory(stream->fileData, (int) stream->fileSize, &error, nullptr);
    if (p->vorbis == nullptr) {
        logWarn("X360Audio: could not open Ogg decoder (error %d)\n", error);
        free(p);
        return nullptr;
    }

    InitializeCriticalSection(&p->lock);
    p->wake = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (p->wake == nullptr) {
        logWarn("X360Audio: CreateEvent for stream failed\n");
        stb_vorbis_close(p->vorbis);
        DeleteCriticalSection(&p->lock);
        free(p);
        return nullptr;
    }

    size_t slotBytes = (size_t) X360_STREAM_SLOT_FRAMES * (size_t) p->channels * 2u;
    for (int32_t i = 0; i < X360_STREAM_SLOTS; i++) {
        p->slots[i].data = (uint8_t *) safeMalloc(slotBytes);
    }
    return p;
}

static void x360StreamPlayDestroy(X360StreamPlay *p) {
    if (p == nullptr) return;

    InterlockedExchange(&p->stop, 1);
    if (p->wake != nullptr) SetEvent(p->wake);
    if (p->thread != nullptr) {
        WaitForSingleObject(p->thread, INFINITE);
        CloseHandle(p->thread);
    }
    if (p->vorbis != nullptr) stb_vorbis_close(p->vorbis);
    for (int32_t i = 0; i < X360_STREAM_SLOTS; i++) {
        free(p->slots[i].data);
    }
    if (p->wake != nullptr) CloseHandle(p->wake);
    DeleteCriticalSection(&p->lock);
    free(p);
}

static bool x360StreamSubmitSlot(X360StreamPlay *p, X360StreamSlot *slot,
                                 IXAudio2SourceVoice *voice) {
    XAUDIO2_BUFFER buf;
    memset(&buf, 0, sizeof(buf));
    buf.AudioBytes = slot->frames * (uint32_t) p->channels * 2u;
    buf.pAudioData = slot->data;
    buf.pContext = slot;
    if (slot->endOfStream) buf.Flags = XAUDIO2_END_OF_STREAM;

    HRESULT hr = IXAudio2SourceVoice_SubmitSourceBuffer(voice, &buf, nullptr);
    if (FAILED(hr)) {
        logWarn("X360Audio: stream SubmitSourceBuffer failed (0x%08X)\n", (unsigned) hr);
        return false;
    }
    slot->state = X360_STREAM_SLOT_SUBMITTED;
    return true;
}

// (Re)starts a stream instance at `offset`: resets the queue, decodes the first
// chunk, creates the voice and starts the worker. The caller must have already
// destroyed the previous voice/worker for this instance.
static bool x360StreamBegin(X360AudioSystem *s, X360SoundInstance *inst, uint32_t offset) {
    X360StreamPlay *p = (X360StreamPlay *) inst->stream;
    if (p == nullptr || s->xaudio == nullptr) return false;

    for (int32_t i = 0; i < X360_STREAM_SLOTS; i++) {
        p->slots[i].state = X360_STREAM_SLOT_FREE;
        p->slots[i].frames = 0;
        p->slots[i].endOfStream = false;
    }
    p->fifoHead = 0;
    p->fifoTail = 0;
    p->thread = nullptr;
    InterlockedExchange(&p->stop, 0);
    InterlockedExchange(&p->eof, 0);

    if (offset > 0) {
        if (!stb_vorbis_seek(p->vorbis, offset)) {
            logWarn("X360Audio: stream seek to frame %u failed\n", (unsigned) offset);
            return false;
        }
    } else {
        stb_vorbis_seek_start(p->vorbis);
    }
    p->nextFrame = offset;
    p->startFrame = offset;

    X360StreamSlot *first = &p->slots[0];
    first->state = X360_STREAM_SLOT_DECODING;
    if (!x360StreamDecodeSlot(p, first) || first->frames == 0) {
        first->state = X360_STREAM_SLOT_FREE;
        return false;
    }
    first->state = X360_STREAM_SLOT_FILLED;

    IXAudio2SourceVoice *voice = x360StreamCreateVoice(s, p);
    if (voice == nullptr) return false;
    inst->voice = voice;

    if (!x360StreamSubmitSlot(p, first, voice)) {
        IXAudio2SourceVoice_DestroyVoice(voice);
        inst->voice = nullptr;
        return false;
    }
    p->fifo[p->fifoTail % X360_STREAM_SLOTS] = first;
    p->fifoTail++;

    if (inst->pitch > 0.0f) {
        IXAudio2SourceVoice_SetFrequencyRatio(voice, inst->pitch, XAUDIO2_COMMIT_NOW);
    }
    x360InstanceSpatialFromListener(s, inst);
    x360InstanceApplyVolume(s, inst);
    x360InstanceApplyPan(s, inst);

    HRESULT hr = IXAudio2SourceVoice_Start(voice, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        logWarn("X360Audio: stream Start failed (0x%08X)\n", (unsigned) hr);
        IXAudio2SourceVoice_DestroyVoice(voice);
        inst->voice = nullptr;
        return false;
    }

    XAUDIO2_VOICE_STATE st;
    IXAudio2SourceVoice_GetState(voice, &st, 0);
    p->samplesAtStart = st.SamplesPlayed;

    DWORD threadId = 0;
    p->thread = CreateThread(nullptr, 512 * 1024, x360StreamDecodeThread, p,
                             CREATE_SUSPENDED, &threadId);
    if (p->thread != nullptr) {
        XSetThreadProcessor(p->thread, 2); // keep 4/5 free for XAudio2
        ResumeThread(p->thread);
    } else {
        // The game thread will decode in x360StreamPump instead. Playback
        // still works, just with less lookahead.
        logWarn("X360Audio: stream CreateThread failed; decoding on the game thread\n");
    }

    inst->paused = false;
    inst->ended = false;
    return true;
}

// Called once per frame for a streaming instance: recycles played buffers,
// submits decoded ones and, if CreateThread failed, decodes a little inline.
static void x360StreamPump(X360AudioSystem *s, X360SoundInstance *inst) {
    (void) s;
    X360StreamPlay *p = (X360StreamPlay *) inst->stream;
    if (p == nullptr) return;

    if (p->thread == nullptr) {
        for (int32_t n = 0; n < 2; n++) {
            if (!p->loop && InterlockedCompareExchange(&p->eof, 0, 0) != 0) break;
            X360StreamSlot *slot = nullptr;
            for (int32_t i = 0; i < X360_STREAM_SLOTS; i++) {
                if (p->slots[i].state == X360_STREAM_SLOT_FREE) {
                    p->slots[i].state = X360_STREAM_SLOT_DECODING;
                    slot = &p->slots[i];
                    break;
                }
            }
            if (slot == nullptr) break;
            bool ok = x360StreamDecodeSlot(p, slot);
            slot->state = (ok && slot->frames > 0) ? X360_STREAM_SLOT_FILLED : X360_STREAM_SLOT_FREE;
            if (!ok) break;
            if (!p->loop && InterlockedCompareExchange(&p->eof, 0, 0) != 0) break;
        }
    }

    if (inst->voice == nullptr) return;
    IXAudio2SourceVoice *voice = (IXAudio2SourceVoice *) inst->voice;

    XAUDIO2_VOICE_STATE st;
    IXAudio2SourceVoice_GetState(voice, &st, XAUDIO2_VOICE_NOSAMPLESPLAYED);

    EnterCriticalSection(&p->lock);

    // Recycle buffers that the voice has finished playing. pCurrentBufferContext
    // points at the buffer being processed, so everything older is done.
    if (st.BuffersQueued == 0) {
        while (p->fifoHead != p->fifoTail) {
            p->fifo[p->fifoHead % X360_STREAM_SLOTS]->state = X360_STREAM_SLOT_FREE;
            p->fifo[p->fifoHead % X360_STREAM_SLOTS] = nullptr;
            p->fifoHead++;
        }
    } else if (st.pCurrentBufferContext != nullptr) {
        X360StreamSlot *current = (X360StreamSlot *) st.pCurrentBufferContext;
        bool found = false;
        for (uint32_t i = p->fifoHead; i != p->fifoTail; i++) {
            if (p->fifo[i % X360_STREAM_SLOTS] == current) {
                found = true;
                break;
            }
        }
        if (found) {
            while (p->fifoHead != p->fifoTail &&
                   p->fifo[p->fifoHead % X360_STREAM_SLOTS] != current) {
                p->fifo[p->fifoHead % X360_STREAM_SLOTS]->state = X360_STREAM_SLOT_FREE;
                p->fifo[p->fifoHead % X360_STREAM_SLOTS] = nullptr;
                p->fifoHead++;
            }
        }
    }

    uint32_t queued = st.BuffersQueued;
    for (int32_t i = 0; i < X360_STREAM_SLOTS && queued < X360_STREAM_MAX_QUEUED; i++) {
        X360StreamSlot *slot = &p->slots[i];
        if (slot->state != X360_STREAM_SLOT_FILLED) continue;
        if (!x360StreamSubmitSlot(p, slot, voice)) break;
        p->fifo[p->fifoTail % X360_STREAM_SLOTS] = slot;
        p->fifoTail++;
        queued++;
    }

    LeaveCriticalSection(&p->lock);
    SetEvent(p->wake);
}

// ===[ Sound resolution ]===

// Resolve a SOND index (or a stream index) to a retained PCM block plus the
// resource's volume/pitch. Returns NULL when the sound is unavailable.
static X360PcmBlock *x360PcmForSoundIndex(X360AudioSystem *s, int32_t soundIndex,
                                          float *outVolume, float *outPitch) {
    *outVolume = 1.0f;
    *outPitch = 1.0f;

    if (soundIndex >= X360_AUDIO_STREAM_INDEX_BASE) {
        int32_t slot = soundIndex - X360_AUDIO_STREAM_INDEX_BASE;
        if (slot < 0 || slot >= X360_MAX_AUDIO_STREAMS || !s->streams[slot].active) return nullptr;
        X360AudioStream *stream = &s->streams[slot];
        if (stream->pcm == nullptr) {
            stream->pcm = x360DecodeFile(s, stream->filePath);
            if (stream->pcm != nullptr) {
                logInfo("X360Audio: decoded stream %d ('%s') %.2f s %u ch %u Hz\n",
                        soundIndex, stream->filePath,
                        stream->pcm->sampleRate ? (double) stream->pcm->totalSamples / (double) stream->pcm->sampleRate : 0.0,
                        (unsigned) stream->pcm->channels, (unsigned) stream->pcm->sampleRate);
            }
        }
        if (stream->pcm == nullptr) return nullptr;
        *outVolume = stream->initialGain;
        *outPitch = stream->initialPitch;
        x360PcmRetain(stream->pcm);
        return stream->pcm;
    }

    DataWin *dw = s->base.audioGroups[0];
    if (dw == nullptr || soundIndex < 0 || (uint32_t) soundIndex >= dw->sond.count) {
        logWarn("X360Audio: invalid sound index %d\n", soundIndex);
        return nullptr;
    }
    Sound *sound = &dw->sond.sounds[soundIndex];

    bool isRegular = (sound->flags & AUDIO_ENTRY_FLAG_REGULAR) == AUDIO_ENTRY_FLAG_REGULAR;
    bool isEmbedded = (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) != 0;
    bool isCompressed = (sound->flags & AUDIO_ENTRY_FLAG_IS_COMPRESSED) != 0;
    bool inAudo = !isRegular || isEmbedded || isCompressed;

    *outVolume = sound->volume > 0.0f ? sound->volume : 1.0f;
    // GameMaker stores the default pitch as 0.0; the desktop backends rely on
    // their audio library ignoring a non-positive pitch. Match that here.
    *outPitch = sound->pitch > 0.0f ? sound->pitch : 1.0f;

    if (inAudo) {
        int32_t group = sound->audioGroup;
        if (group < 0 || (uint32_t) group >= arrlen(s->base.audioGroups) || s->base.audioGroups[group] == nullptr) {
            logWarn("X360Audio: audio group %d for '%s' is not loaded\n", group, sound->name);
            return nullptr;
        }
        DataWin *audioGroup = s->base.audioGroups[group];
        if (sound->audioFile < 0 || (uint32_t) sound->audioFile >= audioGroup->audo.count) {
            logWarn("X360Audio: invalid AUDO index %d for '%s'\n", sound->audioFile, sound->name);
            return nullptr;
        }
        X360PcmBlock *pcm = x360AudoPcm(s, &audioGroup->audo.entries[sound->audioFile]);
        if (pcm != nullptr) x360PcmRetain(pcm);
        return pcm;
    }

    return x360DecodeFile(s, sound->file);
}

// ===[ Vtable: lifecycle ]===

static void x360Init(AudioSystem *audio, DataWin *dataWin, FileSystem *fileSystem) {
    X360AudioSystem *s = (X360AudioSystem *) audio;
    s->base.dw = dataWin;
    s->fileSystem = fileSystem;
    s->masterGain = 1.0f;
    s->listenerX = s->listenerY = s->listenerZ = 0.0f;
    for (int32_t i = 0; i < X360_MAX_LISTENERS; i++) s->listenerGains[i] = 1.0f;
    arrput(s->base.audioGroups, dataWin);

    IXAudio2 *xaudio = nullptr;
    HRESULT hr = XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || xaudio == nullptr) {
        logError("X360Audio: XAudio2Create failed (0x%08X); running silent\n", (unsigned) hr);
        return;
    }
    s->xaudio = xaudio;

    IXAudio2MasteringVoice *master = nullptr;
    hr = IXAudio2_CreateMasteringVoice(xaudio, &master, XAUDIO2_DEFAULT_CHANNELS,
                                       XAUDIO2_DEFAULT_SAMPLERATE, 0, 0, nullptr);
    if (FAILED(hr) || master == nullptr) {
        logError("X360Audio: CreateMasteringVoice failed (0x%08X); running silent\n", (unsigned) hr);
        IXAudio2_Release(xaudio);
        s->xaudio = nullptr;
        return;
    }
    s->mastering = master;
    s->initialized = true;
    logInfo("X360Audio: XAudio2 initialized (mastering voice ready)\n");
}

static void x360Destroy(AudioSystem *audio) {
    X360AudioSystem *s = (X360AudioSystem *) audio;

    for (int32_t i = 0; i < X360_MAX_SOUND_INSTANCES; i++) {
        if (s->instances[i].active) x360ReleaseInstance(&s->instances[i]);
    }
    for (int32_t i = 0; i < X360_MAX_AUDIO_STREAMS; i++) {
        if (s->streams[i].active) {
            x360PcmRelease(s->streams[i].pcm);
            free(s->streams[i].fileData);
            free(s->streams[i].filePath);
            s->streams[i].active = false;
        }
    }
    for (int32_t i = 0; i < (int32_t) arrlen(s->audoCache); i++) {
        x360PcmRelease(s->audoCache[i].pcm);
    }
    arrfree(s->audoCache);

    if (s->xaudio != nullptr) {
        if (s->mastering != nullptr) IXAudio2Voice_DestroyVoice((IXAudio2Voice *) s->mastering);
        IXAudio2_Release((IXAudio2 *) s->xaudio);
        s->xaudio = nullptr;
        s->mastering = nullptr;
    }

    // Audio groups above index 0 are owned by this backend.
    if (arrlen(s->base.audioGroups) > 1) {
        for (int32_t i = 1; i < (int32_t) arrlen(s->base.audioGroups); i++) {
            if (s->base.audioGroups[i] != nullptr) DataWin_free(s->base.audioGroups[i]);
        }
    }
    arrfree(s->base.audioGroups);

    free(s->base.groupGains);
    free(s);
}

static void x360Update(AudioSystem *audio, float deltaTime) {
    X360AudioSystem *s = (X360AudioSystem *) audio;
    bool groupChanged = AudioSystem_updateGroupGains(audio, deltaTime);

    for (int32_t i = 0; i < X360_MAX_SOUND_INSTANCES; i++) {
        X360SoundInstance *inst = &s->instances[i];
        if (!inst->active) continue;

        bool soundFading = inst->fadeTimeRemaining > 0.0f;
        if (soundFading) {
            inst->fadeTimeRemaining -= deltaTime;
            if (inst->fadeTimeRemaining <= 0.0f) {
                inst->fadeTimeRemaining = 0.0f;
                inst->currentGain = inst->targetGain;
            } else {
                float t = 1.0f - (inst->fadeTimeRemaining / inst->fadeTotalTime);
                inst->currentGain = inst->startGain + (inst->targetGain - inst->startGain) * t;
            }
        }

        if (inst->spatial) {
            x360InstanceSpatialFromListener(s, inst);
            x360InstanceApplyPan(s, inst);
            x360InstanceApplyVolume(s, inst);
        } else if (soundFading || groupChanged) {
            x360InstanceApplyVolume(s, inst);
        }

        if (inst->stream != nullptr) {
            x360StreamPump(s, inst);
        }

        if (inst->voice == nullptr) {
            x360ReleaseInstance(inst);
            continue;
        }

        // A non-looping voice whose buffer queue drained has finished. A
        // streaming instance is only done once its decoder has also reached
        // end of stream; until then an empty queue just means the pump has not
        // submitted the next chunk yet.
        XAUDIO2_VOICE_STATE st;
        IXAudio2SourceVoice_GetState((IXAudio2SourceVoice *) inst->voice, &st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        bool streamFinished = inst->stream == nullptr ||
            InterlockedCompareExchange(&((X360StreamPlay *) inst->stream)->eof, 0, 0) != 0;
        if (st.BuffersQueued == 0 && !inst->loop && !inst->paused && streamFinished) {
            x360ReleaseInstance(inst);
        }
    }
}

// ===[ Vtable: playback ]===

static int32_t x360PlayStreamSound(X360AudioSystem *s, int32_t soundIndex,
                                   int32_t priority, bool loop) {
    int32_t streamSlot = soundIndex - X360_AUDIO_STREAM_INDEX_BASE;
    if (streamSlot < 0 || streamSlot >= X360_MAX_AUDIO_STREAMS) return -1;
    X360AudioStream *stream = &s->streams[streamSlot];
    if (!stream->active) return -1;

    X360SoundInstance *slot = x360FindFreeSlot(s);
    if (slot == nullptr) {
        logWarn("X360Audio: no free sound slots for stream %d\n", soundIndex);
        return -1;
    }
    if (slot->active) x360ReleaseInstance(slot);

    X360StreamPlay *play = x360StreamPlayCreate(stream, loop);
    if (play == nullptr) return -1;

    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->soundIndex = soundIndex;
    slot->priority = priority;
    slot->loop = loop;
    slot->currentGain = stream->initialGain;
    slot->targetGain = stream->initialGain;
    slot->startGain = stream->initialGain;
    slot->sondVolume = stream->initialGain;
    slot->pitch = stream->initialPitch > 0.0f ? stream->initialPitch : 1.0f;
    slot->spatialGain = 1.0f;
    slot->stream = play;
    slot->instanceId = X360_SOUND_INSTANCE_ID_BASE + s->nextInstanceCounter++;

    if (!x360StreamBegin(s, slot, 0)) {
        x360ReleaseInstance(slot);
        return -1;
    }
    logInfo("X360Audio: stream %d playing (chunked, %d x %u frames)\n",
            soundIndex, X360_STREAM_SLOTS, (unsigned) X360_STREAM_SLOT_FRAMES);
    return slot->instanceId;
}

static int32_t x360PlaySound(AudioSystem *audio, int32_t soundIndex, int32_t priority, bool loop) {
    X360AudioSystem *s = (X360AudioSystem *) audio;
    if (!s->initialized) return -1;

    // Ogg streams are decoded incrementally. WAV streams and ordinary sounds
    // keep the whole-file path below.
    if (soundIndex >= X360_AUDIO_STREAM_INDEX_BASE) {
        int32_t streamSlot = soundIndex - X360_AUDIO_STREAM_INDEX_BASE;
        if (streamSlot >= 0 && streamSlot < X360_MAX_AUDIO_STREAMS &&
            s->streams[streamSlot].active &&
            x360StreamLoadFile(s, &s->streams[streamSlot]) &&
            s->streams[streamSlot].isOgg) {
            return x360PlayStreamSound(s, soundIndex, priority, loop);
        }
    }

    float volume = 1.0f, pitch = 1.0f;
    X360PcmBlock *pcm = x360PcmForSoundIndex(s, soundIndex, &volume, &pitch);
    if (pcm == nullptr) return -1;

    X360SoundInstance *slot = x360FindFreeSlot(s);
    if (slot == nullptr) {
        logWarn("X360Audio: no free sound slots for sound %d\n", soundIndex);
        x360PcmRelease(pcm);
        return -1;
    }
    if (slot->active) x360ReleaseInstance(slot);

    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->soundIndex = soundIndex;
    slot->priority = priority;
    slot->loop = loop;
    slot->currentGain = volume;
    slot->targetGain = volume;
    slot->startGain = volume;
    slot->sondVolume = volume;
    slot->pitch = pitch;
    slot->spatialGain = 1.0f;
    slot->pcm = pcm; // transfer the retained reference
    slot->instanceId = X360_SOUND_INSTANCE_ID_BASE + s->nextInstanceCounter++;

    if (!x360InstanceStartVoice(s, slot, 0)) {
        x360ReleaseInstance(slot);
        return -1;
    }
    return slot->instanceId;
}

static void x360StopInstance(X360SoundInstance *inst) {
    x360ReleaseInstance(inst);
}

static void x360StopMatching(X360AudioSystem *s, int32_t soundOrInstance) {
    if (x360ValidInstanceId(soundOrInstance)) {
        X360SoundInstance *inst = x360FindInstanceById(s, soundOrInstance);
        if (inst != nullptr) x360StopInstance(inst);
        return;
    }
    for (int32_t i = 0; i < X360_MAX_SOUND_INSTANCES; i++) {
        X360SoundInstance *inst = &s->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance) x360StopInstance(inst);
    }
}

static void x360StopSound(AudioSystem *audio, int32_t soundOrInstance) {
    x360StopMatching((X360AudioSystem *) audio, soundOrInstance);
}

static void x360StopAll(AudioSystem *audio) {
    X360AudioSystem *s = (X360AudioSystem *) audio;
    for (int32_t i = 0; i < X360_MAX_SOUND_INSTANCES; i++) {
        if (s->instances[i].active) x360StopInstance(&s->instances[i]);
    }
}

static bool x360InstancePlaying(X360SoundInstance *inst) {
    if (!inst->active || inst->paused || inst->voice == nullptr) return false;
    XAUDIO2_VOICE_STATE st;
    IXAudio2SourceVoice_GetState((IXAudio2SourceVoice *) inst->voice, &st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (inst->stream != nullptr) {
        // A streaming voice can momentarily have an empty queue while the
        // decoder is refilling it; that is not the end of the sound.
        if (st.BuffersQueued == 0 &&
            InterlockedCompareExchange(&((X360StreamPlay *) inst->stream)->eof, 0, 0) != 0) {
            return false;
        }
        return true;
    }
    return st.BuffersQueued > 0;
}

static bool x360IsPlaying(AudioSystem *audio, int32_t soundOrInstance) {
    X360AudioSystem *s = (X360AudioSystem *) audio;
    if (x360ValidInstanceId(soundOrInstance)) {
        X360SoundInstance *inst = x360FindInstanceById(s, soundOrInstance);
        return inst != nullptr && x360InstancePlaying(inst);
    }
    for (int32_t i = 0; i < X360_MAX_SOUND_INSTANCES; i++) {
        X360SoundInstance *inst = &s->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance && x360InstancePlaying(inst)) return true;
    }
    return false;
}

static void x360PauseInstance(X360SoundInstance *inst) {
    if (!inst->active || inst->paused || inst->voice == nullptr) return;
    IXAudio2SourceVoice_Stop((IXAudio2SourceVoice *) inst->voice, 0, XAUDIO2_COMMIT_NOW);
    inst->paused = true;
}

static void x360ResumeInstance(X360SoundInstance *inst) {
    if (!inst->active || !inst->paused || inst->voice == nullptr) return;
    IXAudio2SourceVoice_Start((IXAudio2SourceVoice *) inst->voice, 0, XAUDIO2_COMMIT_NOW);
    inst->paused = false;
}

static void x360PauseSound(AudioSystem *audio, int32_t soundOrInstance) {
    X360AudioSystem *s = (X360AudioSystem *) audio;
    if (x360ValidInstanceId(soundOrInstance)) {
        X360SoundInstance *inst = x360FindInstanceById(s, soundOrInstance);
        if (inst != nullptr) x360PauseInstance(inst);
        return;
    }
    for (int32_t i = 0; i < X360_MAX_SOUND_INSTANCES; i++) {
        X360SoundInstance *inst = &s->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance) x360PauseInstance(inst);
    }
}

static void x360ResumeSound(AudioSystem *audio, int32_t soundOrInstance) {
    X360AudioSystem *s = (X360AudioSystem *) audio;
    if (x360ValidInstanceId(soundOrInstance)) {
        X360SoundInstance *inst = x360FindInstanceById(s, soundOrInstance);
        if (inst != nullptr) x360ResumeInstance(inst);
        return;
    }
    for (int32_t i = 0; i < X360_MAX_SOUND_INSTANCES; i++) {
        X360SoundInstance *inst = &s->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance) x360ResumeInstance(inst);
    }
}

static void x360PauseAll(AudioSystem *audio) {
    X360AudioSystem *s = (X360AudioSystem *) audio;
    for (int32_t i = 0; i < X360_MAX_SOUND_INSTANCES; i++) {
        if (s->instances[i].active) x360PauseInstance(&s->instances[i]);
    }
}

static void x360ResumeAll(AudioSystem *audio) {
    X360AudioSystem *s = (X360AudioSystem *) audio;
    for (int32_t i = 0; i < X360_MAX_SOUND_INSTANCES; i++) {
        if (s->instances[i].active) x360ResumeInstance(&s->instances[i]);
    }
}

// ===[ Vtable: gain / pitch / position ]===

static void x360SetGainOnInstance(X360SoundInstance *inst, float gain, uint32_t timeMs) {
    if (timeMs == 0) {
        inst->currentGain = gain;
        inst->targetGain = gain;
        inst->fadeTimeRemaining = 0.0f;
    } else {
        inst->startGain = inst->currentGain;
        inst->targetGain = gain;
        inst->fadeTotalTime = (float) timeMs / 1000.0f;
        inst->fadeTimeRemaining = inst->fadeTotalTime;
    }
}

static void x360SetSoundGain(AudioSystem *audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    X360AudioSystem *s = (X360AudioSystem *) audio;

    if (soundOrInstance >= X360_AUDIO_STREAM_INDEX_BASE) {
        int32_t slot = soundOrInstance - X360_AUDIO_STREAM_INDEX_BASE;
        if (slot >= 0 && slot < X360_MAX_AUDIO_STREAMS) s->streams[slot].initialGain = gain;
    }

    if (x360ValidInstanceId(soundOrInstance)) {
        X360SoundInstance *inst = x360FindInstanceById(s, soundOrInstance);
        if (inst != nullptr) {
            x360SetGainOnInstance(inst, gain, timeMs);
            if (timeMs == 0) x360InstanceApplyVolume(s, inst);
        }
        return;
    }
    for (int32_t i = 0; i < X360_MAX_SOUND_INSTANCES; i++) {
        X360SoundInstance *inst = &s->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance) {
            x360SetGainOnInstance(inst, gain, timeMs);
            if (timeMs == 0) x360InstanceApplyVolume(s, inst);
        }
    }
}

static float x360GetSoundGain(AudioSystem *audio, int32_t soundOrInstance) {
    X360AudioSystem *s = (X360AudioSystem *) audio;
    if (soundOrInstance >= X360_AUDIO_STREAM_INDEX_BASE) {
        int32_t slot = soundOrInstance - X360_AUDIO_STREAM_INDEX_BASE;
        if (slot >= 0 && slot < X360_MAX_AUDIO_STREAMS && s->streams[slot].active) return s->streams[slot].initialGain;
    }
    if (x360ValidInstanceId(soundOrInstance)) {
        X360SoundInstance *inst = x360FindInstanceById(s, soundOrInstance);
        if (inst != nullptr) return inst->currentGain;
        return 0.0f;
    }
    for (int32_t i = 0; i < X360_MAX_SOUND_INSTANCES; i++) {
        X360SoundInstance *inst = &s->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance) return inst->currentGain;
    }
    return 0.0f;
}

static void x360SetSoundPitch(AudioSystem *audio, int32_t soundOrInstance, float pitch) {
    X360AudioSystem *s = (X360AudioSystem *) audio;
    if (pitch <= 0.0f) return;
    if (pitch > X360_MAX_FREQ_RATIO) pitch = X360_MAX_FREQ_RATIO;

    if (soundOrInstance >= X360_AUDIO_STREAM_INDEX_BASE) {
        int32_t slot = soundOrInstance - X360_AUDIO_STREAM_INDEX_BASE;
        if (slot >= 0 && slot < X360_MAX_AUDIO_STREAMS) s->streams[slot].initialPitch = pitch;
    }

    if (x360ValidInstanceId(soundOrInstance)) {
        X360SoundInstance *inst = x360FindInstanceById(s, soundOrInstance);
        if (inst != nullptr && inst->voice != nullptr) {
            inst->pitch = pitch;
            IXAudio2SourceVoice_SetFrequencyRatio((IXAudio2SourceVoice *) inst->voice, pitch, XAUDIO2_COMMIT_NOW);
        }
        return;
    }
    for (int32_t i = 0; i < X360_MAX_SOUND_INSTANCES; i++) {
        X360SoundInstance *inst = &s->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance && inst->voice != nullptr) {
            inst->pitch = pitch;
            IXAudio2SourceVoice_SetFrequencyRatio((IXAudio2SourceVoice *) inst->voice, pitch, XAUDIO2_COMMIT_NOW);
        }
    }
}

static float x360GetSoundPitch(AudioSystem *audio, int32_t soundOrInstance) {
    X360AudioSystem *s = (X360AudioSystem *) audio;
    if (x360ValidInstanceId(soundOrInstance)) {
        X360SoundInstance *inst = x360FindInstanceById(s, soundOrInstance);
        if (inst != nullptr) return inst->pitch;
        return 1.0f;
    }
    for (int32_t i = 0; i < X360_MAX_SOUND_INSTANCES; i++) {
        X360SoundInstance *inst = &s->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance) return inst->pitch;
    }
    return 1.0f;
}

static float x360InstancePositionSeconds(X360SoundInstance *inst) {
    if (inst->stream != nullptr) {
        X360StreamPlay *p = (X360StreamPlay *) inst->stream;
        if (inst->voice == nullptr || p->sampleRate == 0) return 0.0f;
        XAUDIO2_VOICE_STATE st;
        IXAudio2SourceVoice_GetState((IXAudio2SourceVoice *) inst->voice, &st, 0);
        uint64_t played = st.SamplesPlayed >= p->samplesAtStart
            ? st.SamplesPlayed - p->samplesAtStart : 0;
        uint64_t pos = (uint64_t) p->startFrame + played;
        uint32_t total = p->totalFrames;
        if (total > 0) {
            if (inst->loop) {
                pos %= total;
            } else if (pos > total) {
                pos = total;
            }
        }
        return (float) pos / (float) p->sampleRate;
    }

    if (inst->voice == nullptr || inst->pcm == nullptr || inst->pcm->sampleRate == 0) return 0.0f;
    XAUDIO2_VOICE_STATE st;
    IXAudio2SourceVoice_GetState((IXAudio2SourceVoice *) inst->voice, &st, 0);
    uint32_t played = (uint32_t) st.SamplesPlayed - inst->samplesPlayedAtStart;
    uint32_t pos = inst->playOffset + played;
    uint32_t total = inst->pcm->totalSamples;
    if (total > 0 && (inst->loop || pos > total)) pos %= total;
    return (float) pos / (float) inst->pcm->sampleRate;
}

static float x360GetTrackPosition(AudioSystem *audio, int32_t soundOrInstance) {
    X360AudioSystem *s = (X360AudioSystem *) audio;
    if (x360ValidInstanceId(soundOrInstance)) {
        X360SoundInstance *inst = x360FindInstanceById(s, soundOrInstance);
        if (inst != nullptr) return x360InstancePositionSeconds(inst);
    } else {
        for (int32_t i = 0; i < X360_MAX_SOUND_INSTANCES; i++) {
            X360SoundInstance *inst = &s->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) return x360InstancePositionSeconds(inst);
        }
    }
    return 0.0f;
}

static void x360SetTrackPosition(AudioSystem *audio, int32_t soundOrInstance, float positionSeconds) {
    X360AudioSystem *s = (X360AudioSystem *) audio;
    if (positionSeconds < 0.0f) positionSeconds = 0.0f;

    X360SoundInstance *target = nullptr;
    if (x360ValidInstanceId(soundOrInstance)) {
        target = x360FindInstanceById(s, soundOrInstance);
    } else {
        for (int32_t i = 0; i < X360_MAX_SOUND_INSTANCES; i++) {
            X360SoundInstance *inst = &s->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) { target = inst; break; }
        }
    }
    if (target == nullptr) return;

    if (target->stream != nullptr) {
        X360StreamPlay *p = (X360StreamPlay *) target->stream;
        if (p->sampleRate == 0) return;
        double frames = (double) positionSeconds * (double) p->sampleRate;
        if (p->totalFrames > 0 && frames >= (double) p->totalFrames) {
            frames = target->loop ? fmod(frames, (double) p->totalFrames) : 0.0;
        }
        uint32_t offset = (uint32_t) frames;

        if (target->voice != nullptr) {
            IXAudio2SourceVoice_Stop((IXAudio2SourceVoice *) target->voice, 0, XAUDIO2_COMMIT_NOW);
            IXAudio2SourceVoice_FlushSourceBuffers((IXAudio2SourceVoice *) target->voice);
            IXAudio2SourceVoice_DestroyVoice((IXAudio2SourceVoice *) target->voice);
            target->voice = nullptr;
        }
        InterlockedExchange(&p->stop, 1);
        if (p->wake != nullptr) SetEvent(p->wake);
        if (p->thread != nullptr) {
            WaitForSingleObject(p->thread, INFINITE);
            CloseHandle(p->thread);
            p->thread = nullptr;
        }
        if (!x360StreamBegin(s, target, offset)) {
            logWarn("X360Audio: stream seek restart failed\n");
            x360ReleaseInstance(target);
        }
        return;
    }

    if (target->pcm == nullptr || target->pcm->sampleRate == 0) return;

    uint32_t offset = (uint32_t) (positionSeconds * (float) target->pcm->sampleRate);
    if (offset >= target->pcm->totalSamples) offset = 0;

    // Rebuild the voice at the new offset. XAudio2 has no PCM seek, so this is
    // stop + flush + resubmit from a shifted data pointer.
    if (target->voice != nullptr) {
        IXAudio2SourceVoice_Stop((IXAudio2SourceVoice *) target->voice, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers((IXAudio2SourceVoice *) target->voice);
        IXAudio2SourceVoice_DestroyVoice((IXAudio2SourceVoice *) target->voice);
        target->voice = nullptr;
    }
    x360InstanceStartVoice(s, target, offset);
}

// ===[ Vtable: spatial / listener ]===

static void x360SetSoundSpatial(AudioSystem *audio, int32_t instanceId, float x, float y, float z,
                                float vx, float vy, float vz, float ref, float max, float factor) {
    (void) vx; (void) vy; (void) vz; // no Doppler in this first pass
    X360AudioSystem *s = (X360AudioSystem *) audio;
    X360SoundInstance *inst = x360FindInstanceById(s, instanceId);
    if (inst == nullptr) return;
    inst->spatial = true;
    inst->spatialX = x;
    inst->spatialY = y;
    inst->spatialZ = z;
    inst->falloffRef = ref;
    inst->falloffMax = max;
    inst->falloffFactor = factor;
    x360InstanceSpatialFromListener(s, inst);
    x360InstanceApplyVolume(s, inst);
    x360InstanceApplyPan(s, inst);
}

static void x360SetListenerPosition(AudioSystem *audio, float x, float y, float z) {
    X360AudioSystem *s = (X360AudioSystem *) audio;
    s->listenerX = x;
    s->listenerY = y;
    s->listenerZ = z;
}

// ===[ Vtable: length / master / groups ]===

static float x360PcmLengthSeconds(X360PcmBlock *pcm) {
    if (pcm == nullptr || pcm->sampleRate == 0) return 0.0f;
    return (float) pcm->totalSamples / (float) pcm->sampleRate;
}

static float x360GetSoundLength(AudioSystem *audio, int32_t soundOrInstance) {
    X360AudioSystem *s = (X360AudioSystem *) audio;

    if (soundOrInstance >= X360_AUDIO_STREAM_INDEX_BASE) {
        int32_t slot = soundOrInstance - X360_AUDIO_STREAM_INDEX_BASE;
        if (slot < 0 || slot >= X360_MAX_AUDIO_STREAMS || !s->streams[slot].active) return 0.0f;
        X360AudioStream *stream = &s->streams[slot];
        if (!stream->probed) x360StreamLoadFile(s, stream);
        if (stream->lengthSeconds > 0.0f) return stream->lengthSeconds;
        if (stream->sampleRate > 0 && stream->totalFrames > 0) {
            stream->lengthSeconds = (float) stream->totalFrames / (float) stream->sampleRate;
            return stream->lengthSeconds;
        }
        // Container we could not probe: fall back to a full decode.
        if (stream->pcm == nullptr) stream->pcm = x360DecodeFile(s, stream->filePath);
        if (stream->lengthSeconds <= 0.0f) stream->lengthSeconds = x360PcmLengthSeconds(stream->pcm);
        return stream->lengthSeconds > 0.0f ? stream->lengthSeconds : 0.0f;
    }

    X360SoundInstance *match = nullptr;
    if (x360ValidInstanceId(soundOrInstance)) {
        match = x360FindInstanceById(s, soundOrInstance);
    } else {
        for (int32_t i = 0; i < X360_MAX_SOUND_INSTANCES; i++) {
            X360SoundInstance *inst = &s->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance) { match = inst; break; }
        }
    }
    if (match != nullptr) {
        if (match->stream != nullptr) {
            X360StreamPlay *p = (X360StreamPlay *) match->stream;
            if (p->sampleRate > 0 && p->totalFrames > 0) {
                return (float) p->totalFrames / (float) p->sampleRate;
            }
            return 0.0f;
        }
        return x360PcmLengthSeconds(match->pcm);
    }

    // No active instance: answer for the asset itself.
    DataWin *dw = s->base.audioGroups[0];
    if (dw == nullptr || soundOrInstance < 0 || (uint32_t) soundOrInstance >= dw->sond.count) return 0.0f;
    Sound *sound = &dw->sond.sounds[soundOrInstance];

    bool isRegular = (sound->flags & AUDIO_ENTRY_FLAG_REGULAR) == AUDIO_ENTRY_FLAG_REGULAR;
    bool isEmbedded = (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) != 0;
    bool isCompressed = (sound->flags & AUDIO_ENTRY_FLAG_IS_COMPRESSED) != 0;
    bool inAudo = !isRegular || isEmbedded || isCompressed;

    if (inAudo) {
        int32_t group = sound->audioGroup;
        if (group < 0 || (uint32_t) group >= arrlen(s->base.audioGroups) || s->base.audioGroups[group] == nullptr) return 0.0f;
        DataWin *audioGroup = s->base.audioGroups[group];
        if (sound->audioFile < 0 || (uint32_t) sound->audioFile >= audioGroup->audo.count) return 0.0f;
        return x360PcmLengthSeconds(x360AudoPcm(s, &audioGroup->audo.entries[sound->audioFile]));
    }
    X360PcmBlock *pcm = x360DecodeFile(s, sound->file);
    float seconds = x360PcmLengthSeconds(pcm);
    x360PcmRelease(pcm);
    return seconds;
}

static void x360SetMasterGain(AudioSystem *audio, float gain) {
    X360AudioSystem *s = (X360AudioSystem *) audio;
    s->masterGain = gain;
    if (s->mastering != nullptr) {
        IXAudio2Voice_SetVolume((IXAudio2Voice *) s->mastering, gain, XAUDIO2_COMMIT_NOW);
    }
}

static void x360SetMasterGainForListener(AudioSystem *audio, float gain, int32_t listenerId) {
    X360AudioSystem *s = (X360AudioSystem *) audio;
    if (listenerId < 0 || listenerId >= X360_MAX_LISTENERS) return;
    s->listenerGains[listenerId] = gain;
    // Only listener 0 maps to the single hardware output.
    if (listenerId == 0) x360SetMasterGain(audio, gain);
}

static void x360SetChannelCount(MAYBE_UNUSED AudioSystem *audio, MAYBE_UNUSED int32_t count) {
    // The mastering voice owns the output channel layout.
}

static void x360SetGroupGain(AudioSystem *audio, int32_t groupIndex, float gain, uint32_t timeMs) {
    X360AudioSystem *s = (X360AudioSystem *) audio;
    AudioSystem_setGroupGain(audio, groupIndex, gain, timeMs);
    for (int32_t i = 0; i < X360_MAX_SOUND_INSTANCES; i++) {
        X360SoundInstance *inst = &s->instances[i];
        if (inst->active && AudioSystem_soundGroup(audio, inst->soundIndex) == groupIndex) {
            x360InstanceApplyVolume(s, inst);
        }
    }
}

static void x360GroupLoad(AudioSystem *audio, int32_t groupIndex) {
    X360AudioSystem *s = (X360AudioSystem *) audio;
    if (groupIndex <= 0 || (uint32_t) groupIndex >= audio->dw->agrp.count) {
        logWarn("X360Audio: audio group %d is not in the AGRP chunk\n", groupIndex);
        return;
    }
    if ((uint32_t) groupIndex < arrlen(audio->audioGroups) && audio->audioGroups[groupIndex] != nullptr) {
        return; // already loaded
    }

    AudioGroup *audioGroupEntry = &audio->dw->agrp.audioGroups[groupIndex];
    char filename[64];
    if (audioGroupEntry->path != nullptr) {
        snprintf(filename, sizeof(filename), "%s", audioGroupEntry->path);
    } else {
        snprintf(filename, sizeof(filename), "audiogroup%d.dat", groupIndex);
    }

    if (!s->fileSystem->vtable->fileExists(s->fileSystem, filename)) {
        logWarn("X360Audio: audio group %d file '%s' not found\n", groupIndex, filename);
        return;
    }

    char *resolved = s->fileSystem->vtable->resolvePath(s->fileSystem, filename);
    if (resolved == nullptr) return;

    DataWinParserOptions options = {0};
    options.parseAudo = true;
    options.lazyLoadAudio = false;
    // The AUDO pointer table holds absolute file offsets; whole-file buffering
    // is the load type the rest of the port uses for the same reason.
    options.loadType = DATAWINLOADTYPE_LOAD_IN_MEMORY_AHEAD_OF_TIME;
    DataWin *group = DataWin_parse(resolved, options);
    free(resolved);
    if (group == nullptr) {
        logWarn("X360Audio: failed to parse audio group %d\n", groupIndex);
        return;
    }

    // Append in order so audioGroups[i] matches group index i.
    while ((uint32_t) arrlen(audio->audioGroups) < (uint32_t) groupIndex) {
        arrput(audio->audioGroups, nullptr);
    }
    if ((uint32_t) groupIndex == arrlen(audio->audioGroups)) {
        arrput(audio->audioGroups, group);
    } else {
        audio->audioGroups[groupIndex] = group;
    }
    logInfo("X360Audio: loaded audio group %d ('%s')\n", groupIndex, filename);
}

static bool x360GroupIsLoaded(AudioSystem *audio, int32_t groupIndex) {
    if (groupIndex < 0) return false;
    if ((uint32_t) groupIndex >= arrlen(audio->audioGroups)) return false;
    return audio->audioGroups[groupIndex] != nullptr;
}

// ===[ Vtable: streams ]===

static int32_t x360CreateStream(AudioSystem *audio, const char *filename) {
    X360AudioSystem *s = (X360AudioSystem *) audio;
    int32_t slot = -1;
    for (int32_t i = 0; i < X360_MAX_AUDIO_STREAMS; i++) {
        if (!s->streams[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        logWarn("X360Audio: no free stream slots for '%s'\n", filename);
        return -1;
    }
    char *resolved = s->fileSystem->vtable->resolvePath(s->fileSystem, filename);
    if (resolved == nullptr) {
        logWarn("X360Audio: could not resolve stream '%s'\n", filename);
        return -1;
    }
    X360AudioStream *stream = &s->streams[slot];
    memset(stream, 0, sizeof(*stream));
    stream->active = true;
    stream->filePath = resolved;
    stream->initialGain = 1.0f;
    stream->initialPitch = 1.0f;
    stream->lengthSeconds = -1.0f;
    int32_t index = X360_AUDIO_STREAM_INDEX_BASE + slot;
    logInfo("X360Audio: created stream %d for '%s'\n", index, filename);
    return index;
}

static bool x360DestroyStream(AudioSystem *audio, int32_t streamIndex) {
    X360AudioSystem *s = (X360AudioSystem *) audio;
    int32_t slot = streamIndex - X360_AUDIO_STREAM_INDEX_BASE;
    if (slot < 0 || slot >= X360_MAX_AUDIO_STREAMS || !s->streams[slot].active) return false;

    for (int32_t i = 0; i < X360_MAX_SOUND_INSTANCES; i++) {
        X360SoundInstance *inst = &s->instances[i];
        if (inst->active && inst->soundIndex == streamIndex) x360StopInstance(inst);
    }
    x360PcmRelease(s->streams[slot].pcm);
    free(s->streams[slot].fileData);
    free(s->streams[slot].filePath);
    s->streams[slot].active = false;
    s->streams[slot].pcm = nullptr;
    s->streams[slot].fileData = nullptr;
    s->streams[slot].fileSize = 0;
    s->streams[slot].probed = false;
    s->streams[slot].filePath = nullptr;
    return true;
}

// ===[ Vtable ]===

static AudioSystemVtable x360AudioSystemVtable;

X360AudioSystem *X360AudioSystem_create(void) {
    X360AudioSystem *s = (X360AudioSystem *) safeCalloc(1, sizeof(X360AudioSystem));
    x360AudioSystemVtable.init = x360Init;
    x360AudioSystemVtable.destroy = x360Destroy;
    x360AudioSystemVtable.update = x360Update;
    x360AudioSystemVtable.playSound = x360PlaySound;
    x360AudioSystemVtable.setSoundSpatial = x360SetSoundSpatial;
    x360AudioSystemVtable.setListenerPosition = x360SetListenerPosition;
    x360AudioSystemVtable.stopSound = x360StopSound;
    x360AudioSystemVtable.stopAll = x360StopAll;
    x360AudioSystemVtable.isPlaying = x360IsPlaying;
    x360AudioSystemVtable.pauseSound = x360PauseSound;
    x360AudioSystemVtable.resumeSound = x360ResumeSound;
    x360AudioSystemVtable.pauseAll = x360PauseAll;
    x360AudioSystemVtable.resumeAll = x360ResumeAll;
    x360AudioSystemVtable.suspend = x360PauseAll;
    x360AudioSystemVtable.resume = x360ResumeAll;
    x360AudioSystemVtable.setSoundGain = x360SetSoundGain;
    x360AudioSystemVtable.getSoundGain = x360GetSoundGain;
    x360AudioSystemVtable.setSoundPitch = x360SetSoundPitch;
    x360AudioSystemVtable.getSoundPitch = x360GetSoundPitch;
    x360AudioSystemVtable.getTrackPosition = x360GetTrackPosition;
    x360AudioSystemVtable.setTrackPosition = x360SetTrackPosition;
    x360AudioSystemVtable.getSoundLength = x360GetSoundLength;
    x360AudioSystemVtable.setMasterGain = x360SetMasterGain;
    x360AudioSystemVtable.setMasterGainForListener = x360SetMasterGainForListener;
    x360AudioSystemVtable.setChannelCount = x360SetChannelCount;
    x360AudioSystemVtable.setGroupGain = x360SetGroupGain;
    x360AudioSystemVtable.groupLoad = x360GroupLoad;
    x360AudioSystemVtable.groupIsLoaded = x360GroupIsLoaded;
    x360AudioSystemVtable.createStream = x360CreateStream;
    x360AudioSystemVtable.destroyStream = x360DestroyStream;
    s->base.vtable = &x360AudioSystemVtable;
    return s;
}

// ===[ Self-test ]===

#ifdef BS_X360_AUDIO_SELFTEST

// Find the first group-0 SOND entry matching the predicate.
static int32_t x360FindSound(const X360AudioSystem *s, bool wantEmbedded) {
    DataWin *dw = s->base.audioGroups[0];
    if (dw == nullptr) return -1;
    for (uint32_t i = 0; i < dw->sond.count; i++) {
        Sound *sound = &dw->sond.sounds[i];
        if (!sound->present) continue;
        if (sound->audioGroup != 0) continue;
        bool embedded = (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) != 0 ||
                        (sound->flags & AUDIO_ENTRY_FLAG_IS_COMPRESSED) != 0 ||
                        (sound->flags & AUDIO_ENTRY_FLAG_REGULAR) != AUDIO_ENTRY_FLAG_REGULAR;
        if (embedded == wantEmbedded) return (int32_t) i;
    }
    return -1;
}

bool X360AudioSystem_runSelftest(X360AudioSystem *s) {
    AudioSystem *audio = &s->base;
    bool ok = true;

    if (!s->initialized) {
        logInfo("AudioSelfTest: FAIL (XAudio2 not initialized)\n");
        return false;
    }

    int32_t embedded = x360FindSound(s, true);
    logInfo("AudioSelfTest: embedded group-0 sound = %d ('%s')\n", embedded,
            embedded >= 0 ? audio->dw->sond.sounds[embedded].name : "<none>");
    if (embedded < 0) return false;

    float length = audio->vtable->getSoundLength(audio, embedded);
    logInfo("AudioSelfTest: getSoundLength(%d) = %.3f s\n", embedded, (double) length);
    if (length <= 0.0f) ok = false;

    int32_t instance = audio->vtable->playSound(audio, embedded, 10, false);
    logInfo("AudioSelfTest: playSound -> instance %d (playing=%d)\n", instance,
            instance >= 0 ? (int) audio->vtable->isPlaying(audio, instance) : 0);
    if (instance < 0) return false;

    // Let the XAudio2 engine run so the voice actually produces samples.
    Sleep(120);

    float pos = audio->vtable->getTrackPosition(audio, instance);
    logInfo("AudioSelfTest: track position after 120 ms = %.4f s\n", (double) pos);
    if (pos <= 0.0f) ok = false;

    audio->vtable->setSoundGain(audio, instance, 0.5f, 0);
    audio->vtable->setSoundPitch(audio, instance, 1.5f);
    logInfo("AudioSelfTest: gain=%.2f pitch=%.2f\n",
            (double) audio->vtable->getSoundGain(audio, instance),
            (double) audio->vtable->getSoundPitch(audio, instance));

    audio->vtable->pauseSound(audio, instance);
    logInfo("AudioSelfTest: after pause isPlaying=%d\n", (int) audio->vtable->isPlaying(audio, instance));
    if (audio->vtable->isPlaying(audio, instance)) ok = false;

    audio->vtable->resumeSound(audio, instance);
    Sleep(30);
    logInfo("AudioSelfTest: after resume isPlaying=%d\n", (int) audio->vtable->isPlaying(audio, instance));

    audio->vtable->setTrackPosition(audio, instance, 0.0f);
    audio->vtable->stopSound(audio, instance);
    logInfo("AudioSelfTest: after stop isPlaying=%d\n", (int) audio->vtable->isPlaying(audio, instance));
    if (audio->vtable->isPlaying(audio, instance)) ok = false;

    // Loop a sound and confirm it survives past its natural length.
    int32_t looped = audio->vtable->playSound(audio, embedded, 10, true);
    if (looped >= 0) {
        Sleep(60);
        bool stillPlaying = audio->vtable->isPlaying(audio, looped);
        logInfo("AudioSelfTest: looping instance %d playing=%d\n", looped, (int) stillPlaying);
        if (!stillPlaying) ok = false;
        audio->vtable->stopSound(audio, looped);
    }

    // External Ogg file sound: exercises the vendored stb_vorbis decoder (the
    // embedded sounds above are RIFF WAVE and do not use it).
    int32_t external = x360FindSound(s, false);
    logInfo("AudioSelfTest: external group-0 sound = %d ('%s')\n", external,
            external >= 0 ? audio->dw->sond.sounds[external].name : "<none>");
    if (external >= 0) {
        float extLength = audio->vtable->getSoundLength(audio, external);
        int32_t extInstance = audio->vtable->playSound(audio, external, 10, false);
        Sleep(120);
        float extPosition = extInstance >= 0 ? audio->vtable->getTrackPosition(audio, extInstance) : 0.0f;
        logInfo("AudioSelfTest: external length=%.3f s instance=%d position=%.4f s\n",
                (double) extLength, extInstance, (double) extPosition);
        if (extInstance < 0 || extLength <= 0.0f || extPosition <= 0.0f) ok = false;
        if (extInstance >= 0) audio->vtable->stopSound(audio, extInstance);

        // Stream: createStream + play the stream index + destroyStream.
        const char *file = audio->dw->sond.sounds[external].file;
        int32_t stream = audio->vtable->createStream(audio, file);
        int32_t streamInstance = stream >= 0 ? audio->vtable->playSound(audio, stream, 10, false) : -1;
        Sleep(120);
        float streamPosition = streamInstance >= 0 ? audio->vtable->getTrackPosition(audio, streamInstance) : 0.0f;
        logInfo("AudioSelfTest: stream=%d instance=%d position=%.4f s\n",
                stream, streamInstance, (double) streamPosition);
        if (stream < 0 || streamInstance < 0 || streamPosition <= 0.0f) ok = false;
        if (stream >= 0) audio->vtable->destroyStream(audio, stream);
    }

    logInfo("AudioSelfTest: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

#else

bool X360AudioSystem_runSelftest(MAYBE_UNUSED X360AudioSystem *s) {
    return true;
}

#endif
