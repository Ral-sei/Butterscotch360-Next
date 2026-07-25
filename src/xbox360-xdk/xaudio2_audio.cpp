/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Derived from ceilingtilefan/Butterscotch-360 commit
 * 7f8f1ea6044dbc55560dfbd2ca9a2f45e472c02e, with later work from
 * flaf1x/Butterscotch360-Refresh and Ral-sei.
 */

#include <xtl.h>
#include <xaudio2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "stb_ds.h"

// stb_vorbis on big-endian Xbox 360:
// Disable the fast float-to-int path which has endian assumptions.
// Use the slow but portable path instead.
#define STB_VORBIS_NO_FAST_SCALED_FLOAT
#include "stb_vorbis.c"

// Core headers ?compiled as C++ alongside the .c files (via /TP flag)
#include "utils.h"
#include "xaudio2_audio.h"

extern "C" unsigned long __cdecl DbgPrint(const char* format, ...);

// ===[ Sound Instance ]===

struct XdkSoundInstance {
    bool active;
    bool paused;
    bool loop;
    int32_t soundIndex;
    int32_t instanceId;
    int32_t priority;

    IXAudio2SourceVoice* pVoice;
    uint8_t* pcmData;       // decoded PCM (owned)
    uint32_t pcmSize;       // in bytes
    uint32_t sampleRate;
    uint16_t channels;
    bool pcmOwned;          // true if this instance owns pcmData (should free)
    uint64_t voiceSampleBase;
    uint32_t startFrame;

    float currentGain;
    float targetGain;
    float startGain;
    float fadeTotalTime;
    float fadeTimeRemaining;
    float pitch;
    float sondVolume;
    float sondPitch;
};

struct XdkInstanceArray {
    XdkSoundInstance instances[XDK_MAX_SOUND_INSTANCES];
};

static inline XdkInstanceArray* Instances(XdkAudioSystem* xa) {
    return (XdkInstanceArray*)xa->instanceData;
}

static void destroyInstance(XdkSoundInstance* inst, XdkAudioSystem* xa);

static XdkSoundInstance* findFreeSlot(XdkAudioSystem* xa) {
    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (!arr->instances[i].active) return &arr->instances[i];
    }
    return NULL;
}

static XdkSoundInstance* findEvictSlot(XdkAudioSystem* xa) {
    XdkInstanceArray* arr = Instances(xa);
    XdkSoundInstance* best = NULL;
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (!inst->active) return inst;
        if (!best || inst->priority < best->priority) {
            best = inst;
        }
    }
    if (best) {
        destroyInstance(best, xa);
    }
    return best;
}

static XdkSoundInstance* findById(XdkAudioSystem* xa, int32_t id) {
    int32_t idx = id - XDK_SOUND_INSTANCE_ID_BASE;
    if (idx < 0 || idx >= XDK_MAX_SOUND_INSTANCES) return NULL;
    XdkSoundInstance* inst = &Instances(xa)->instances[idx];
    if (!inst->active || inst->instanceId != id) return NULL;
    return inst;
}

static uint32_t pcmFrameCount(uint32_t pcmSize, uint16_t channels) {
    uint32_t bytesPerFrame = (uint32_t)channels * 2u;
    return bytesPerFrame > 0 ? pcmSize / bytesPerFrame : 0;
}

static bool submitPcmFromFrame(IXAudio2SourceVoice* voice, const uint8_t* pcmData,
                               uint32_t pcmSize, uint16_t channels, uint32_t startFrame,
                               bool loop, uint64_t* outSampleBase) {
    if (!voice || !pcmData || !outSampleBase) return false;
    uint32_t totalFrames = pcmFrameCount(pcmSize, channels);
    if (totalFrames == 0) return false;
    if (startFrame >= totalFrames) startFrame = 0;

    voice->Stop(0);
    voice->FlushSourceBuffers();

    XAUDIO2_VOICE_STATE state;
    voice->GetState(&state, 0);
    *outSampleBase = (uint64_t)state.SamplesPlayed;

    XAUDIO2_BUFFER first;
    memset(&first, 0, sizeof(first));
    first.AudioBytes = pcmSize;
    first.pAudioData = pcmData;
    first.PlayBegin = startFrame;

    HRESULT hr;
    if (loop && startFrame > 0) {
        // Play the seek-to-end tail once, then continue with a full-buffer loop.
        hr = voice->SubmitSourceBuffer(&first);
        if (FAILED(hr)) return false;

        XAUDIO2_BUFFER looping;
        memset(&looping, 0, sizeof(looping));
        looping.AudioBytes = pcmSize;
        looping.pAudioData = pcmData;
        looping.LoopCount = XAUDIO2_LOOP_INFINITE;
        hr = voice->SubmitSourceBuffer(&looping);
        if (FAILED(hr)) {
            voice->FlushSourceBuffers();
            return false;
        }
        return true;
    }

    if (loop) {
        first.LoopCount = XAUDIO2_LOOP_INFINITE;
    } else {
        first.Flags = XAUDIO2_END_OF_STREAM;
    }
    return SUCCEEDED(voice->SubmitSourceBuffer(&first));
}

static float voiceTrackPosition(IXAudio2SourceVoice* voice, uint32_t sampleRate,
                                uint32_t totalFrames, uint32_t startFrame,
                                uint64_t sampleBase, bool loop) {
    if (!voice || sampleRate == 0 || totalFrames == 0) return 0.0f;
    XAUDIO2_VOICE_STATE state;
    voice->GetState(&state, 0);
    uint64_t played = (uint64_t)state.SamplesPlayed;
    uint64_t elapsed = played >= sampleBase ? played - sampleBase : 0;
    uint64_t frame = (uint64_t)startFrame + elapsed;
    if (loop) {
        frame %= totalFrames;
    } else if (frame > totalFrames) {
        frame = totalFrames;
    }
    return (float)frame / (float)sampleRate;
}

static void destroyInstance(XdkSoundInstance* inst, XdkAudioSystem* xa) {
    if (inst->pVoice) {
        inst->pVoice->Stop();
        inst->pVoice->FlushSourceBuffers();
        inst->pVoice->DestroyVoice();
        inst->pVoice = NULL;
    }
    if (inst->pcmData) {
        XdkInstanceArray* arr = Instances(xa);
        bool shared = false;
        for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
            XdkSoundInstance* other = &arr->instances[i];
            if (other != inst && other->active && other->pcmData == inst->pcmData) {
                shared = true;
                break;
            }
        }
        if (!shared) {
            free(inst->pcmData);
        }
        inst->pcmData = NULL;
    }
    inst->active = false;
}

// Forward declarations for functions used before their definition
static void xdkGroupLoad(AudioSystem* audio, int32_t groupIndex);

static uint16_t readLe16(const uint8_t* value) {
    return (uint16_t)(value[0] | ((uint16_t)value[1] << 8));
}

static uint32_t readLe32(const uint8_t* value) {
    return (uint32_t)value[0]
        | ((uint32_t)value[1] << 8)
        | ((uint32_t)value[2] << 16)
        | ((uint32_t)value[3] << 24);
}

static float measureAudioData(const uint8_t* data, int dataSize) {
    if (!data || dataSize < 4) return 0.0f;

    if (dataSize >= 12
        && memcmp(data, "RIFF", 4) == 0
        && memcmp(data + 8, "WAVE", 4) == 0) {
        uint16_t channels = 0;
        uint16_t bitsPerSample = 0;
        uint32_t sampleRate = 0;
        uint32_t pcmBytes = 0;
        int offset = 12;
        while (offset + 8 <= dataSize) {
            uint32_t chunkSize = readLe32(data + offset + 4);
            int payload = offset + 8;
            if (chunkSize > (uint32_t)(dataSize - payload)) break;
            if (memcmp(data + offset, "fmt ", 4) == 0 && chunkSize >= 16) {
                channels = readLe16(data + payload + 2);
                sampleRate = readLe32(data + payload + 4);
                bitsPerSample = readLe16(data + payload + 14);
            } else if (memcmp(data + offset, "data", 4) == 0) {
                pcmBytes = chunkSize;
            }
            offset = payload + (int)chunkSize + ((chunkSize & 1u) ? 1 : 0);
        }
        uint32_t bytesPerFrame = (uint32_t)channels * ((uint32_t)bitsPerSample / 8u);
        if (sampleRate > 0 && bytesPerFrame > 0 && pcmBytes > 0) {
            return (float)pcmBytes / (float)(sampleRate * bytesPerFrame);
        }
        return 0.0f;
    }

    int error = 0;
    stb_vorbis* vorbis = stb_vorbis_open_memory(data, dataSize, &error, NULL);
    if (!vorbis) return 0.0f;
    float seconds = stb_vorbis_stream_length_in_seconds(vorbis);
    stb_vorbis_close(vorbis);
    return seconds;
}

// ===[ Vtable Implementations ]===

static void xdkAudioInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    xa->base.dw = dataWin;
    arrput(xa->base.audioGroups, dataWin);
    xa->fileSystem = fileSystem;
    xa->masterGain = 1.0f;

    HRESULT hr = XAudio2Create((IXAudio2**)&xa->pXAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        OutputDebugStringA("XAudio2Create failed\n");
        return;
    }

    IXAudio2* pXA = (IXAudio2*)xa->pXAudio2;
    hr = pXA->CreateMasteringVoice((IXAudio2MasteringVoice**)&xa->pMasterVoice,
        XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, NULL);
    if (FAILED(hr)) {
        return;
    }

    xa->initialized = true;
    (void)fileSystem;
}

static void xdkAudioDestroy(AudioSystem* audio) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);

    // Source voices must be destroyed while their owning XAudio2 engine is
    // still alive. In particular, external audio streams can remain active
    // when game_end exits the main loop.
    if (arr) {
        for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
            if (arr->instances[i].active) destroyInstance(&arr->instances[i], xa);
        }
        free(arr);
        xa->instanceData = NULL;
    }

    for (int i = 0; i < XDK_MAX_AUDIO_STREAMS; i++) {
        XdkStreamEntry* stream = &xa->streams[i];
        if (stream->pVoice) {
            IXAudio2SourceVoice* voice = (IXAudio2SourceVoice*)stream->pVoice;
            voice->Stop(0);
            voice->FlushSourceBuffers();
            voice->DestroyVoice();
            stream->pVoice = NULL;
        }
        free(stream->pcmData);
        stream->pcmData = NULL;
        stream->active = false;
    }

    // Free audio groups (skip group 0 which is the main dataWin freed elsewhere)
    if (arrlen(xa->base.audioGroups) > 1) {
        for (int gi = 1; gi < (int)arrlen(xa->base.audioGroups); gi++) {
            DataWin_free(xa->base.audioGroups[gi]);
        }
    }
    arrfree(xa->base.audioGroups);

    if (xa->pMasterVoice) {
        ((IXAudio2MasteringVoice*)xa->pMasterVoice)->DestroyVoice();
        xa->pMasterVoice = NULL;
    }
    if (xa->pXAudio2) {
        ((IXAudio2*)xa->pXAudio2)->Release();
        xa->pXAudio2 = NULL;
    }

    free(xa);
}

static void xdkAudioUpdate(AudioSystem* audio, float deltaTime) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (!xa->initialized) return;

    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (!inst->active) continue;

        // Gain fading
        if (inst->fadeTimeRemaining > 0.0f) {
            inst->fadeTimeRemaining -= deltaTime;
            if (inst->fadeTimeRemaining <= 0.0f) {
                inst->currentGain = inst->targetGain;
                inst->fadeTimeRemaining = 0.0f;
            } else {
                float t = 1.0f - (inst->fadeTimeRemaining / inst->fadeTotalTime);
                inst->currentGain = inst->startGain + (inst->targetGain - inst->startGain) * t;
            }
            if (inst->pVoice) {
                inst->pVoice->SetVolume(inst->currentGain * inst->sondVolume * xa->masterGain);
            }
        }

        // Clean up instances with no voice (failed to create/decode)
        if (!inst->pVoice) {
            inst->active = false;
            continue;
        }

        // Check if playback finished
        XAUDIO2_VOICE_STATE state;
        inst->pVoice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued == 0 && !inst->loop) {
            destroyInstance(inst, xa);
        }
    }
}

static int32_t xdkPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (!xa->initialized) { DbgPrint("BS: audio not initialized\n"); return -1; }

    // Handle stream indices
    if (soundIndex >= XDK_AUDIO_STREAM_ID_BASE) {
        int32_t streamSlot = soundIndex - XDK_AUDIO_STREAM_ID_BASE;
        if (streamSlot < 0 || streamSlot >= XDK_MAX_AUDIO_STREAMS) return -1;
        XdkStreamEntry* stream = &xa->streams[streamSlot];
        if (!stream->active || !stream->pcmData || !stream->pVoice) return -1;

        stream->loop = loop;
        stream->paused = false;
        stream->volume = 1.0f;

        IXAudio2SourceVoice* pVoice = (IXAudio2SourceVoice*)stream->pVoice;
        stream->startFrame = 0;
        if (!submitPcmFromFrame(pVoice, stream->pcmData, stream->pcmSize,
                                stream->channels, 0, loop, &stream->voiceSampleBase)) {
            return -1;
        }
        pVoice->SetVolume(stream->volume * xa->masterGain);
        pVoice->SetFrequencyRatio(stream->pitch);
        pVoice->Start(0);

        return soundIndex;
    }

    DataWin* dw = xa->base.dw;
    if (soundIndex < 0 || (uint32_t)soundIndex >= dw->sond.count) return -1;

    XdkSoundInstance* inst = findEvictSlot(xa);
    if (!inst) return -1;

    memset(inst, 0, sizeof(XdkSoundInstance));
    inst->active = true;
    inst->soundIndex = soundIndex;
    inst->priority = priority;
    inst->loop = loop;
    inst->pitch = 1.0f;
    inst->sondPitch = 1.0f;
    inst->currentGain = 1.0f;
    inst->targetGain = 1.0f;
    inst->sondVolume = 1.0f;

    int32_t slotIndex = (int32_t)(inst - Instances(xa)->instances);
    inst->instanceId = XDK_SOUND_INSTANCE_ID_BASE + slotIndex;
    xa->nextInstanceCounter++;

    Sound* sound = &dw->sond.sounds[soundIndex];
    if (sound->volume >= 0) {
        inst->sondVolume = sound->volume;
    }
    if (sound->pitch > 0.0f) {
        inst->sondPitch = sound->pitch;
    }

    // Decode audio ?either embedded (AUDO) or external (.ogg file)
    bool isRegular = (sound->flags & AUDIO_ENTRY_FLAG_REGULAR) == AUDIO_ENTRY_FLAG_REGULAR;
    bool isEmbedded = (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) != 0;
    bool isCompressed = (sound->flags & AUDIO_ENTRY_FLAG_IS_COMPRESSED) != 0;
    bool inAudo = !isRegular || isEmbedded || isCompressed;

    // Auto-load audio group if not loaded yet (GMS2 on-demand loading)
    if (inAudo && sound->audioGroup > 0) {
        xdkGroupLoad(audio, sound->audioGroup);
    }

    uint8_t* oggData = NULL;
    int oggSize = 0;
    bool freeOggData = false;

    if (inAudo) {
        DataWin* groupDw = (sound->audioGroup >= 0 && sound->audioGroup < (int)arrlen(xa->base.audioGroups))
            ? xa->base.audioGroups[sound->audioGroup]
            : xa->base.audioGroups[0];
        if (!groupDw) {
            DbgPrint("BS: null audio group %d for sound '%s'\n", sound->audioGroup, sound->name ? sound->name : "?");
            return inst->instanceId;
        }
        if (sound->audioFile < 0 || (uint32_t)sound->audioFile >= groupDw->audo.count) {
            DbgPrint("BS: bad audioFile idx %d (group %d audo count=%d)\n", sound->audioFile, sound->audioGroup, (int)groupDw->audo.count);
            return inst->instanceId;
        }
        AudioEntry* entry = &groupDw->audo.entries[sound->audioFile];
        if (entry->dataSize == 0) {
            DbgPrint("BS: empty audio entry\n");
            return inst->instanceId;
        }
        if (entry->data) {
            oggData = entry->data;
            oggSize = (int)entry->dataSize;
        } else {
            FILE* f = (FILE*)groupDw->lazyLoadFile;
            if (!f) { DbgPrint("BS: no data.win file handle for audio\n"); return inst->instanceId; }
            oggData = (uint8_t*)malloc(entry->dataSize);
            if (!oggData) return inst->instanceId;
            long previousPosition = ftell(f);
            if (fseek(f, entry->dataOffset, SEEK_SET) != 0
                || fread(oggData, 1, entry->dataSize, f) != entry->dataSize) {
                if (previousPosition >= 0) fseek(f, previousPosition, SEEK_SET);
                free(oggData);
                DbgPrint("BS: failed to read lazy audio entry %d\n", sound->audioFile);
                return inst->instanceId;
            }
            if (previousPosition >= 0) fseek(f, previousPosition, SEEK_SET);
            oggSize = (int)entry->dataSize;
            freeOggData = true;
        }
    } else {
        // External: load .ogg file via FileSystem
        const char* file = sound->file;
        if (!file || !file[0] || !xa->fileSystem) {
            DbgPrint("BS: external sound: no file path or no filesystem\n");
            return inst->instanceId;
        }
        bool hasExt = (strchr(file, '.') != NULL);
        char filename[512];
        if (hasExt) snprintf(filename, sizeof(filename), "%s", file);
        else snprintf(filename, sizeof(filename), "%s.ogg", file);

        char* fullPath = xa->fileSystem->vtable->resolvePath(xa->fileSystem, filename);
        if (!fullPath) { DbgPrint("BS: could not resolve path for %s\n", filename); return inst->instanceId; }
        FILE* f = fopen(fullPath, "rb");
        free(fullPath);
        if (!f) { DbgPrint("BS: fopen failed for external audio\n"); return inst->instanceId; }
        fseek(f, 0, SEEK_END);
        oggSize = (int)ftell(f);
        fseek(f, 0, SEEK_SET);
        if (oggSize <= 0) { fclose(f); return inst->instanceId; }
        oggData = (uint8_t*)malloc(oggSize);
        fread(oggData, 1, oggSize, f);
        fclose(f);
        freeOggData = true;
    }

    // Detect format from header bytes
    int channels = 0, sampleRate = 0, sampleCount = 0;
    short* pcm = NULL;
    bool pcmOwned = true;

    // Check if another active instance already has this sound's PCM decoded
    {
        XdkInstanceArray* sharedArr = Instances(xa);
        for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
            XdkSoundInstance* other = &sharedArr->instances[i];
            if (other->active && other != inst && other->soundIndex == soundIndex && other->pcmData) {
                channels = other->channels;
                sampleRate = other->sampleRate;
                pcm = (short*)other->pcmData;
                sampleCount = other->pcmSize / (other->channels * sizeof(short));
                pcmOwned = false;
                break;
            }
        }
    }

    if (!pcm) {
    // Check if it's WAV (RIFF header) or OGG (OggS header)
    if (oggSize >= 44 && oggData[0] == 'R' && oggData[1] == 'I' && oggData[2] == 'F' && oggData[3] == 'F') {
        // WAV format ?parse RIFF/WAVE header
        channels = (int)(oggData[22] | (oggData[23] << 8));
        sampleRate = (int)(oggData[24] | (oggData[25] << 8) | (oggData[26] << 16) | (oggData[27] << 24));
        int bitsPerSample = (int)(oggData[34] | (oggData[35] << 8));

        // Find "data" subchunk
        int dataOffset = 12;
        int dataSize = 0;
        while (dataOffset + 8 <= oggSize) {
            if (oggData[dataOffset] == 'd' && oggData[dataOffset+1] == 'a' &&
                oggData[dataOffset+2] == 't' && oggData[dataOffset+3] == 'a') {
                dataSize = (int)(oggData[dataOffset+4] | (oggData[dataOffset+5] << 8) |
                                 (oggData[dataOffset+6] << 16) | (oggData[dataOffset+7] << 24));
                dataOffset += 8;
                break;
            }
            int chunkSize = (int)(oggData[dataOffset+4] | (oggData[dataOffset+5] << 8) |
                                  (oggData[dataOffset+6] << 16) | (oggData[dataOffset+7] << 24));
            dataOffset += 8 + chunkSize;
        }

        if (dataSize > 0 && bitsPerSample == 16 && channels > 0) {
            sampleCount = dataSize / (channels * 2);
            pcm = (short*)malloc(dataSize);
            if (!pcm) return -1;
            memcpy(pcm, oggData + dataOffset, dataSize);
            // WAV stores little-endian PCM; Xbox 360 XAudio2 expects big-endian
            for (int s = 0; s < sampleCount * channels; s++) {
                uint16_t v = (uint16_t)pcm[s];
                pcm[s] = (short)((v >> 8) | (v << 8));
            }
        } else {
            DbgPrint("BS: WAV parse failed: bits=%d ch=%d dataSize=%d\n", bitsPerSample, channels, dataSize);
            if (freeOggData) free(oggData);
            return inst->instanceId;
        }
    } else {
        // Try OGG/Vorbis
        int stbErr = 0;
        stb_vorbis* vorbis = stb_vorbis_open_memory(oggData, oggSize, &stbErr, NULL);
        if (!vorbis) {
            DbgPrint("BS: vorbis open failed: err=%d size=%d\n", stbErr, oggSize);
            if (freeOggData) free(oggData);
            return inst->instanceId;
        }
        stb_vorbis_info info = stb_vorbis_get_info(vorbis);
        channels = info.channels;
        sampleRate = info.sample_rate;
        int totalSamples = stb_vorbis_stream_length_in_samples(vorbis);
        pcm = (short*)malloc(totalSamples * channels * sizeof(short));
        if (!pcm) {
            stb_vorbis_close(vorbis);
            if (freeOggData) free(oggData);
            return inst->instanceId;
        }
        sampleCount = stb_vorbis_get_samples_short_interleaved(vorbis, channels, pcm, totalSamples * channels);
        stb_vorbis_close(vorbis);
    }

    if (sampleCount <= 0 || !pcm) {
        if (freeOggData) { free(oggData); oggData = NULL; freeOggData = false; }
        free(pcm);
        return inst->instanceId;
    }
    }

    if (freeOggData) { free(oggData); oggData = NULL; freeOggData = false; }

    inst->sampleRate = (uint32_t)sampleRate;
    inst->channels = (uint16_t)channels;
    inst->pcmSize = (uint32_t)(sampleCount * channels * sizeof(short));
    inst->pcmData = (uint8_t*)pcm;
    inst->pcmOwned = pcmOwned;

    // Xbox 360 XAudio2 expects big-endian PCM samples.
    // WAV samples were already byte-swapped during parsing above.
    // OGG samples from stb_vorbis: the int16 conversion uses native endian,
    // which is already big-endian on Xbox 360. No additional swap needed.

    // Create XAudio2 source voice using WAVEFORMATEXTENSIBLE with channel mask
    WAVEFORMATEXTENSIBLE wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels = (WORD)channels;
    wfx.Format.nSamplesPerSec = (DWORD)sampleRate;
    wfx.Format.wBitsPerSample = 16;
    wfx.Format.nBlockAlign = (WORD)(channels * 2);
    wfx.Format.nAvgBytesPerSec = (DWORD)(sampleRate * channels * 2);
    wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 16;
    wfx.dwChannelMask = (channels == 1) ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

    IXAudio2* pXA = (IXAudio2*)xa->pXAudio2;
    HRESULT hr = pXA->CreateSourceVoice(&inst->pVoice, (WAVEFORMATEX*)&wfx);
    if (FAILED(hr) || !inst->pVoice) {
        DbgPrint("BS: CreateSourceVoice failed: hr=0x%08X\n", (unsigned)hr);
        if (inst->pcmOwned) {
            free(inst->pcmData);
        }
        inst->pcmData = NULL;
        return inst->instanceId;
    }

    inst->startFrame = 0;
    if (!submitPcmFromFrame(inst->pVoice, inst->pcmData, inst->pcmSize,
                            inst->channels, 0, loop, &inst->voiceSampleBase)) {
        DbgPrint("BS: SubmitSourceBuffer failed\n");
    }
    inst->pVoice->SetVolume(inst->currentGain * inst->sondVolume * xa->masterGain);
    inst->pVoice->SetFrequencyRatio(inst->pitch * inst->sondPitch);
    hr = inst->pVoice->Start(0);
    if (FAILED(hr)) {
        DbgPrint("BS: Start failed: hr=0x%08X\n", (unsigned)hr);
    }

    {
        static int loggedStarts = 0;
        if (loggedStarts < 32) {
            DbgPrint(
                "Butterscotch360-Next: XAudio2 start index=%d name=%s instance=%d rate=%d channels=%d seconds=%.3f loop=%d\n",
                soundIndex,
                sound->name ? sound->name : "?",
                inst->instanceId,
                sampleRate,
                channels,
                (float)sampleCount / (float)sampleRate,
                loop ? 1 : 0
            );
            loggedStarts++;
        }
    }

    return inst->instanceId;
}

static void xdkStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;

    // Handle stream index
    if (soundOrInstance >= XDK_AUDIO_STREAM_ID_BASE) {
        int32_t slot = soundOrInstance - XDK_AUDIO_STREAM_ID_BASE;
        if (slot >= 0 && slot < XDK_MAX_AUDIO_STREAMS && xa->streams[slot].active) {
            if (xa->streams[slot].pVoice) {
                IXAudio2SourceVoice* pVoice = (IXAudio2SourceVoice*)xa->streams[slot].pVoice;
                pVoice->Stop();
                pVoice->FlushSourceBuffers();
            }
        }
        return;
    }

    XdkInstanceArray* arr = Instances(xa);

    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        if (inst) destroyInstance(inst, xa);
    } else {
        for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
            if (arr->instances[i].active && arr->instances[i].soundIndex == soundOrInstance)
                destroyInstance(&arr->instances[i], xa);
        }
    }
}

static void xdkStopAll(AudioSystem* audio) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active) destroyInstance(&arr->instances[i], xa);
    }
    // Stop all streams too
    for (int i = 0; i < XDK_MAX_AUDIO_STREAMS; i++) {
        if (xa->streams[i].active && xa->streams[i].pVoice) {
            ((IXAudio2SourceVoice*)xa->streams[i].pVoice)->Stop();
            ((IXAudio2SourceVoice*)xa->streams[i].pVoice)->FlushSourceBuffers();
        }
    }
}

static bool xdkIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;

    // Handle stream index
    if (soundOrInstance >= XDK_AUDIO_STREAM_ID_BASE) {
        int32_t slot = soundOrInstance - XDK_AUDIO_STREAM_ID_BASE;
        if (slot >= 0 && slot < XDK_MAX_AUDIO_STREAMS && xa->streams[slot].active && xa->streams[slot].pVoice) {
            XAUDIO2_VOICE_STATE state;
            ((IXAudio2SourceVoice*)xa->streams[slot].pVoice)->GetState(&state, 0);
            return state.BuffersQueued > 0;
        }
        return false;
    }

    XdkInstanceArray* arr = Instances(xa);

    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        return inst && !inst->paused;
    }
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active && arr->instances[i].soundIndex == soundOrInstance && !arr->instances[i].paused)
            return true;
    }
    return false;
}

static void xdkPauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;

    // Handle stream index
    if (soundOrInstance >= XDK_AUDIO_STREAM_ID_BASE) {
        int32_t slot = soundOrInstance - XDK_AUDIO_STREAM_ID_BASE;
        if (slot >= 0 && slot < XDK_MAX_AUDIO_STREAMS && xa->streams[slot].active && xa->streams[slot].pVoice) {
            xa->streams[slot].paused = true;
            ((IXAudio2SourceVoice*)xa->streams[slot].pVoice)->Stop();
        }
        return;
    }

    XdkInstanceArray* arr = Instances(xa);

    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        if (inst) { inst->paused = true; if (inst->pVoice) inst->pVoice->Stop(); }
    } else {
        for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
            if (arr->instances[i].active && arr->instances[i].soundIndex == soundOrInstance) {
                arr->instances[i].paused = true;
                if (arr->instances[i].pVoice) arr->instances[i].pVoice->Stop();
            }
        }
    }
}

static void xdkResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;

    // Handle stream index
    if (soundOrInstance >= XDK_AUDIO_STREAM_ID_BASE) {
        int32_t slot = soundOrInstance - XDK_AUDIO_STREAM_ID_BASE;
        if (slot >= 0 && slot < XDK_MAX_AUDIO_STREAMS && xa->streams[slot].active && xa->streams[slot].pVoice) {
            xa->streams[slot].paused = false;
            ((IXAudio2SourceVoice*)xa->streams[slot].pVoice)->Start(0);
        }
        return;
    }

    XdkInstanceArray* arr = Instances(xa);

    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        if (inst) { inst->paused = false; if (inst->pVoice) inst->pVoice->Start(0); }
    } else {
        for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
            if (arr->instances[i].active && arr->instances[i].soundIndex == soundOrInstance) {
                arr->instances[i].paused = false;
                if (arr->instances[i].pVoice) arr->instances[i].pVoice->Start(0);
            }
        }
    }
}

static void xdkPauseAll(AudioSystem* audio) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active) {
            arr->instances[i].paused = true;
            if (arr->instances[i].pVoice) arr->instances[i].pVoice->Stop();
        }
    }
    for (int i = 0; i < XDK_MAX_AUDIO_STREAMS; i++) {
        if (xa->streams[i].active && xa->streams[i].pVoice) {
            xa->streams[i].paused = true;
            ((IXAudio2SourceVoice*)xa->streams[i].pVoice)->Stop();
        }
    }
}

static void xdkResumeAll(AudioSystem* audio) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active) {
            arr->instances[i].paused = false;
            if (arr->instances[i].pVoice) arr->instances[i].pVoice->Start(0);
        }
    }
    for (int i = 0; i < XDK_MAX_AUDIO_STREAMS; i++) {
        if (xa->streams[i].active && xa->streams[i].pVoice) {
            xa->streams[i].paused = false;
            ((IXAudio2SourceVoice*)xa->streams[i].pVoice)->Start(0);
        }
    }
}

static void xdkSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;

    // Handle stream index
    if (soundOrInstance >= XDK_AUDIO_STREAM_ID_BASE) {
        int32_t slot = soundOrInstance - XDK_AUDIO_STREAM_ID_BASE;
        if (slot >= 0 && slot < XDK_MAX_AUDIO_STREAMS && xa->streams[slot].active) {
            xa->streams[slot].volume = gain;
            if (xa->streams[slot].pVoice) {
                ((IXAudio2SourceVoice*)xa->streams[slot].pVoice)->SetVolume(gain * xa->masterGain);
            }
        }
        return;
    }

    XdkSoundInstance* inst = NULL;

    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        inst = findById(xa, soundOrInstance);
    } else {
        XdkInstanceArray* arr = Instances(xa);
        for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
            if (arr->instances[i].active && arr->instances[i].soundIndex == soundOrInstance) {
                inst = &arr->instances[i]; break;
            }
        }
    }
    if (!inst) return;

    if (timeMs == 0) {
        inst->currentGain = gain;
        inst->targetGain = gain;
        inst->fadeTimeRemaining = 0.0f;
        if (inst->pVoice) inst->pVoice->SetVolume(gain * inst->sondVolume * xa->masterGain);
    } else {
        inst->startGain = inst->currentGain;
        inst->targetGain = gain;
        inst->fadeTotalTime = (float)timeMs / 1000.0f;
        inst->fadeTimeRemaining = inst->fadeTotalTime;
    }
}

static float xdkGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;

    // Handle stream index
    if (soundOrInstance >= XDK_AUDIO_STREAM_ID_BASE) {
        int32_t slot = soundOrInstance - XDK_AUDIO_STREAM_ID_BASE;
        if (slot >= 0 && slot < XDK_MAX_AUDIO_STREAMS && xa->streams[slot].active) {
            return xa->streams[slot].volume;
        }
        return 0.0f;
    }

    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        return inst ? inst->currentGain : 0.0f;
    }
    return 0.0f;
}

static void xdkSetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (soundOrInstance >= XDK_AUDIO_STREAM_ID_BASE) {
        int32_t slot = soundOrInstance - XDK_AUDIO_STREAM_ID_BASE;
        if (slot >= 0 && slot < XDK_MAX_AUDIO_STREAMS && xa->streams[slot].active) {
            xa->streams[slot].pitch = pitch;
            if (xa->streams[slot].pVoice) {
                ((IXAudio2SourceVoice*)xa->streams[slot].pVoice)->SetFrequencyRatio(pitch);
            }
        }
        return;
    }
    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        if (inst) {
            inst->pitch = pitch;
            if (inst->pVoice) inst->pVoice->SetFrequencyRatio(pitch * inst->sondPitch);
        }
        return;
    }
    XdkInstanceArray* instances = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &instances->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance) {
            inst->pitch = pitch;
            if (inst->pVoice) inst->pVoice->SetFrequencyRatio(pitch * inst->sondPitch);
        }
    }
}

static float xdkGetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (soundOrInstance >= XDK_AUDIO_STREAM_ID_BASE) {
        int32_t slot = soundOrInstance - XDK_AUDIO_STREAM_ID_BASE;
        if (slot >= 0 && slot < XDK_MAX_AUDIO_STREAMS && xa->streams[slot].active) return xa->streams[slot].pitch;
        return 1.0f;
    }
    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        return inst ? inst->pitch : 1.0f;
    }
    XdkInstanceArray* instances = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (instances->instances[i].active && instances->instances[i].soundIndex == soundOrInstance) {
            return instances->instances[i].pitch;
        }
    }
    return 1.0f;
}

static float xdkGetTrackPosition(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (soundOrInstance >= XDK_AUDIO_STREAM_ID_BASE) {
        int32_t slot = soundOrInstance - XDK_AUDIO_STREAM_ID_BASE;
        if (slot >= 0 && slot < XDK_MAX_AUDIO_STREAMS) {
            XdkStreamEntry* stream = &xa->streams[slot];
            if (stream->active && stream->pVoice) {
                return voiceTrackPosition(
                    (IXAudio2SourceVoice*)stream->pVoice, stream->sampleRate,
                    pcmFrameCount(stream->pcmSize, stream->channels), stream->startFrame,
                    stream->voiceSampleBase, stream->loop
                );
            }
        }
        return 0.0f;
    }
    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        if (inst && inst->pVoice) return voiceTrackPosition(
            inst->pVoice, inst->sampleRate, pcmFrameCount(inst->pcmSize, inst->channels),
            inst->startFrame, inst->voiceSampleBase, inst->loop
        );
        return 0.0f;
    }
    XdkInstanceArray* instances = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &instances->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance && inst->pVoice) {
            return voiceTrackPosition(
                inst->pVoice, inst->sampleRate, pcmFrameCount(inst->pcmSize, inst->channels),
                inst->startFrame, inst->voiceSampleBase, inst->loop
            );
        }
    }
    return 0.0f;
}

static void xdkSetTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (positionSeconds < 0.0f) positionSeconds = 0.0f;

    if (soundOrInstance >= XDK_AUDIO_STREAM_ID_BASE) {
        int32_t slot = soundOrInstance - XDK_AUDIO_STREAM_ID_BASE;
        if (slot < 0 || slot >= XDK_MAX_AUDIO_STREAMS) return;
        XdkStreamEntry* stream = &xa->streams[slot];
        if (!stream->active || !stream->pVoice || stream->sampleRate == 0) return;
        XAUDIO2_VOICE_STATE state;
        ((IXAudio2SourceVoice*)stream->pVoice)->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued == 0) return;
        uint32_t totalFrames = pcmFrameCount(stream->pcmSize, stream->channels);
        float duration = (float)totalFrames / (float)stream->sampleRate;
        uint32_t frame = positionSeconds >= duration
            ? 0
            : (uint32_t)(positionSeconds * (float)stream->sampleRate);
        if (submitPcmFromFrame((IXAudio2SourceVoice*)stream->pVoice,
                               stream->pcmData, stream->pcmSize, stream->channels,
                               frame, stream->loop, &stream->voiceSampleBase)) {
            stream->startFrame = frame;
            if (!stream->paused) ((IXAudio2SourceVoice*)stream->pVoice)->Start(0);
        }
        return;
    }

    XdkInstanceArray* instances = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &instances->instances[i];
        if (!inst->active || !inst->pVoice || inst->sampleRate == 0) continue;
        bool matches = soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE
            ? inst->instanceId == soundOrInstance
            : inst->soundIndex == soundOrInstance;
        if (!matches) continue;

        uint32_t totalFrames = pcmFrameCount(inst->pcmSize, inst->channels);
        float duration = (float)totalFrames / (float)inst->sampleRate;
        uint32_t frame = positionSeconds >= duration
            ? 0
            : (uint32_t)(positionSeconds * (float)inst->sampleRate);
        if (submitPcmFromFrame(inst->pVoice, inst->pcmData, inst->pcmSize,
                               inst->channels, frame, inst->loop,
                               &inst->voiceSampleBase)) {
            inst->startFrame = frame;
            if (!inst->paused) inst->pVoice->Start(0);
        }
    }
}

static void xdkSetMasterGain(AudioSystem* audio, float gain) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (gain < 0.0f) gain = 0.0f;
    xa->masterGain = gain;
    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (inst->active && inst->pVoice) {
            inst->pVoice->SetVolume(inst->currentGain * inst->sondVolume * gain);
        }
    }
    for (int i = 0; i < XDK_MAX_AUDIO_STREAMS; i++) {
        if (xa->streams[i].active && xa->streams[i].pVoice) {
            ((IXAudio2SourceVoice*)xa->streams[i].pVoice)->SetVolume(xa->streams[i].volume * gain);
        }
    }
}

static void xdkSetChannelCount(AudioSystem* audio, int32_t count) {
    (void)audio; (void)count;
}

static void xdkGroupLoad(AudioSystem* audio, int32_t groupIndex) {
    if (groupIndex <= 0) return;
    if (groupIndex < (int)arrlen(audio->audioGroups) && audio->audioGroups[groupIndex]) return;

    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    char fallbackName[64];
    const char* groupPath = NULL;
    if ((uint32_t)groupIndex < audio->dw->agrp.count) {
        groupPath = audio->dw->agrp.audioGroups[groupIndex].path;
    }
    if (!groupPath || !groupPath[0]) {
        snprintf(fallbackName, sizeof(fallbackName), "audiogroup%d.dat", groupIndex);
        groupPath = fallbackName;
    }

    while ((int)arrlen(audio->audioGroups) <= groupIndex) {
        arrput(audio->audioGroups, (DataWin*)NULL);
    }

    if (!xa->fileSystem->vtable->fileExists(xa->fileSystem, groupPath)) {
        DbgPrint("BS: groupLoad: %s does not exist\n", groupPath);
        audio->audioGroups[groupIndex] = (DataWin*)safeCalloc(1, sizeof(DataWin));
        return;
    }

    char* resolved = xa->fileSystem->vtable->resolvePath(xa->fileSystem, groupPath);
    if (!resolved) {
        DbgPrint("BS: groupLoad: could not resolve %s\n", groupPath);
        audio->audioGroups[groupIndex] = (DataWin*)safeCalloc(1, sizeof(DataWin));
        return;
    }

    DbgPrint("BS: groupLoad: loading %s\n", resolved);

    DataWinParserOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.parseAudo = 1;
    opts.lazyLoadAudio = 1;
    DataWin* audioGroup = DataWin_parse(resolved, opts);
    free(resolved);
    if (audioGroup) {
        audio->audioGroups[groupIndex] = audioGroup;
        DbgPrint("BS: groupLoad: group %d loaded OK (audo count=%d)\n", groupIndex, (int)audioGroup->audo.count);
    } else {
        DbgPrint("BS: groupLoad: DataWin_parse failed for group %d\n", groupIndex);
        audio->audioGroups[groupIndex] = (DataWin*)safeCalloc(1, sizeof(DataWin));
    }
}

static bool xdkGroupIsLoaded(AudioSystem* audio, int32_t groupIndex) {
    return groupIndex >= 0
        && groupIndex < (int32_t)arrlen(audio->audioGroups)
        && audio->audioGroups[groupIndex] != NULL;
}

static float xdkGetSoundLength(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    DataWin* dw = xa->base.dw;

    if (soundOrInstance >= XDK_AUDIO_STREAM_ID_BASE) {
        int32_t streamSlot = soundOrInstance - XDK_AUDIO_STREAM_ID_BASE;
        if (streamSlot < 0 || streamSlot >= XDK_MAX_AUDIO_STREAMS) return 0.0f;
        XdkStreamEntry* stream = &xa->streams[streamSlot];
        if (!stream->active || stream->sampleRate == 0 || stream->channels == 0) return 0.0f;
        return (float)stream->pcmSize
            / (float)(stream->sampleRate * stream->channels * sizeof(short));
    }

    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        XdkSoundInstance* instance = findById(xa, soundOrInstance);
        if (!instance || instance->sampleRate == 0 || instance->channels == 0) return 0.0f;
        return (float)instance->pcmSize
            / (float)(instance->sampleRate * instance->channels * sizeof(short));
    }
    if (soundOrInstance < 0 || soundOrInstance >= (int)dw->sond.count)
        return 0.0f;
    Sound* snd = &dw->sond.sounds[soundOrInstance];
    bool isRegular = (snd->flags & AUDIO_ENTRY_FLAG_REGULAR) == AUDIO_ENTRY_FLAG_REGULAR;
    bool isEmbedded = (snd->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) != 0;
    bool isCompressed = (snd->flags & AUDIO_ENTRY_FLAG_IS_COMPRESSED) != 0;
    bool inAudo = !isRegular || isEmbedded || isCompressed;

    if (inAudo) {
        if (snd->audioGroup > 0) xdkGroupLoad(audio, snd->audioGroup);
        DataWin* groupDw = (snd->audioGroup >= 0 && snd->audioGroup < (int)arrlen(xa->base.audioGroups))
            ? xa->base.audioGroups[snd->audioGroup]
            : xa->base.audioGroups[0];
        if (!groupDw || snd->audioFile < 0 || (uint32_t)snd->audioFile >= groupDw->audo.count) return 0.0f;
        AudioEntry* entry = &groupDw->audo.entries[snd->audioFile];
        if (entry->data) return measureAudioData(entry->data, (int)entry->dataSize);
        if (!groupDw->lazyLoadFile || entry->dataSize == 0) return 0.0f;

        FILE* file = groupDw->lazyLoadFile;
        long previousPosition = ftell(file);
        if (fseek(file, entry->dataOffset, SEEK_SET) != 0) return 0.0f;
        uint8_t* encoded = (uint8_t*)malloc(entry->dataSize);
        if (!encoded) return 0.0f;
        size_t bytesRead = fread(encoded, 1, entry->dataSize, file);
        if (previousPosition >= 0) fseek(file, previousPosition, SEEK_SET);
        float seconds = bytesRead == entry->dataSize
            ? measureAudioData(encoded, (int)entry->dataSize)
            : 0.0f;
        free(encoded);
        return seconds;
    }

    if (!snd->file || !snd->file[0] || !xa->fileSystem) return 0.0f;
    char filename[512];
    if (strchr(snd->file, '.')) snprintf(filename, sizeof(filename), "%s", snd->file);
    else snprintf(filename, sizeof(filename), "%s.ogg", snd->file);
    char* resolved = xa->fileSystem->vtable->resolvePath(xa->fileSystem, filename);
    if (!resolved) return 0.0f;
    FILE* file = fopen(resolved, "rb");
    free(resolved);
    if (!file) return 0.0f;
    fseek(file, 0, SEEK_END);
    long encodedSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (encodedSize <= 0) { fclose(file); return 0.0f; }
    uint8_t* encoded = (uint8_t*)malloc((size_t)encodedSize);
    if (!encoded) { fclose(file); return 0.0f; }
    size_t bytesRead = fread(encoded, 1, (size_t)encodedSize, file);
    fclose(file);
    float seconds = bytesRead == (size_t)encodedSize
        ? measureAudioData(encoded, (int)encodedSize)
        : 0.0f;
    free(encoded);
    return seconds;
}

static int32_t xdkCreateStream(AudioSystem* audio, const char* filename) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (!xa->initialized || !filename) return -1;

    // Find a free stream slot
    int32_t freeSlot = -1;
    for (int i = 0; i < XDK_MAX_AUDIO_STREAMS; i++) {
        if (!xa->streams[i].active) { freeSlot = i; break; }
    }
    if (freeSlot < 0) return -1;

    // Resolve file path
    char* fullPath = xa->fileSystem->vtable->resolvePath(xa->fileSystem, filename);
    if (!fullPath) return -1;

    // Open and decode the OGG file
    FILE* f = fopen(fullPath, "rb");
    if (!f) { free(fullPath); return -1; }
    fseek(f, 0, SEEK_END);
    int oggSize = (int)ftell(f);
    fseek(f, 0, SEEK_SET);
    if (oggSize <= 0) { fclose(f); free(fullPath); return -1; }
    uint8_t* oggData = (uint8_t*)malloc(oggSize);
    if (!oggData) { fclose(f); free(fullPath); return -1; }
    fread(oggData, 1, oggSize, f);
    fclose(f);
    free(fullPath);

    // Decode OGG
    int stbErr = 0;
    stb_vorbis* vorbis = stb_vorbis_open_memory(oggData, oggSize, &stbErr, NULL);
    if (!vorbis) {
        free(oggData);
        return -1;
    }
    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    int channels = info.channels;
    int sampleRate = info.sample_rate;
    int totalSamples = stb_vorbis_stream_length_in_samples(vorbis);
    short* pcm = (short*)malloc(totalSamples * channels * sizeof(short));
    if (!pcm) {
        stb_vorbis_close(vorbis);
        free(oggData);
        return -1;
    }
    int sampleCount = stb_vorbis_get_samples_short_interleaved(vorbis, channels, pcm, totalSamples * channels);
    stb_vorbis_close(vorbis);
    free(oggData);

    if (sampleCount <= 0) { free(pcm); return -1; }

    // Create XAudio2 source voice
    WAVEFORMATEXTENSIBLE wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels = (WORD)channels;
    wfx.Format.nSamplesPerSec = (DWORD)sampleRate;
    wfx.Format.wBitsPerSample = 16;
    wfx.Format.nBlockAlign = (WORD)(channels * 2);
    wfx.Format.nAvgBytesPerSec = (DWORD)(sampleRate * channels * 2);
    wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 16;
    wfx.dwChannelMask = (channels == 1) ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

    IXAudio2SourceVoice* pVoice = NULL;
    IXAudio2* pXA = (IXAudio2*)xa->pXAudio2;
    HRESULT hr = pXA->CreateSourceVoice(&pVoice, (WAVEFORMATEX*)&wfx);
    if (FAILED(hr) || !pVoice) {
        free(pcm);
        return -1;
    }

    // Store in stream entry
    XdkStreamEntry* stream = &xa->streams[freeSlot];
    stream->active = true;
    stream->paused = false;
    stream->loop = false;
    stream->pVoice = pVoice;
    stream->pcmData = (uint8_t*)pcm;
    stream->pcmSize = (uint32_t)(sampleCount * channels * sizeof(short));
    stream->sampleRate = (uint32_t)sampleRate;
    stream->channels = (uint16_t)channels;
    stream->volume = 1.0f;
    stream->pitch = 1.0f;
    stream->voiceSampleBase = 0;
    stream->startFrame = 0;

    return XDK_AUDIO_STREAM_ID_BASE + freeSlot;
}

static bool xdkDestroyStream(AudioSystem* audio, int32_t streamIndex) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    int32_t slot = streamIndex - XDK_AUDIO_STREAM_ID_BASE;
    if (slot < 0 || slot >= XDK_MAX_AUDIO_STREAMS) return false;
    XdkStreamEntry* stream = &xa->streams[slot];
    if (!stream->active) return false;

    if (stream->pVoice) {
        IXAudio2SourceVoice* pVoice = (IXAudio2SourceVoice*)stream->pVoice;
        pVoice->Stop();
        pVoice->FlushSourceBuffers();
        pVoice->DestroyVoice();
        stream->pVoice = NULL;
    }
    if (stream->pcmData) {
        free(stream->pcmData);
        stream->pcmData = NULL;
    }
    stream->active = false;
    return true;
}

// ===[ Vtable ]===

static void xdkSetMasterGainForListener(AudioSystem* audio, float gain, int32_t listenerId) {
    (void)listenerId;
    xdkSetMasterGain(audio, gain);
}

static AudioSystemVtable xdkAudioVtable;

// ===[ Public API ]===

XdkAudioSystem* XdkAudioSystem_create(void) {
    XdkAudioSystem* xa = (XdkAudioSystem*)calloc(1, sizeof(XdkAudioSystem));
    xa->base.vtable = &xdkAudioVtable;
    xdkAudioVtable.init = xdkAudioInit;
    xdkAudioVtable.destroy = xdkAudioDestroy;
    xdkAudioVtable.update = xdkAudioUpdate;
    xdkAudioVtable.playSound = xdkPlaySound;
    xdkAudioVtable.stopSound = xdkStopSound;
    xdkAudioVtable.stopAll = xdkStopAll;
    xdkAudioVtable.isPlaying = xdkIsPlaying;
    xdkAudioVtable.pauseSound = xdkPauseSound;
    xdkAudioVtable.resumeSound = xdkResumeSound;
    xdkAudioVtable.pauseAll = xdkPauseAll;
    xdkAudioVtable.resumeAll = xdkResumeAll;
    xdkAudioVtable.suspend = xdkPauseAll;
    xdkAudioVtable.resume = xdkResumeAll;
    xdkAudioVtable.setSoundGain = xdkSetSoundGain;
    xdkAudioVtable.getSoundGain = xdkGetSoundGain;
    xdkAudioVtable.setSoundPitch = xdkSetSoundPitch;
    xdkAudioVtable.getSoundPitch = xdkGetSoundPitch;
    xdkAudioVtable.getTrackPosition = xdkGetTrackPosition;
    xdkAudioVtable.setTrackPosition = xdkSetTrackPosition;
    xdkAudioVtable.getSoundLength = xdkGetSoundLength;
    xdkAudioVtable.setMasterGain = xdkSetMasterGain;
    xdkAudioVtable.setMasterGainForListener = xdkSetMasterGainForListener;
    xdkAudioVtable.setChannelCount = xdkSetChannelCount;
    xdkAudioVtable.groupLoad = xdkGroupLoad;
    xdkAudioVtable.groupIsLoaded = xdkGroupIsLoaded;
    xdkAudioVtable.createStream = xdkCreateStream;
    xdkAudioVtable.destroyStream = xdkDestroyStream;
    xa->masterGain = 1.0f;
    xa->instanceData = calloc(1, sizeof(XdkInstanceArray));
    return xa;
}
