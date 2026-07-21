#pragma once

#include "audio_system.h"

#define XDK_MAX_SOUND_INSTANCES 32
#define XDK_SOUND_INSTANCE_ID_BASE 100000
#define XDK_MAX_AUDIO_STREAMS 8
#define XDK_AUDIO_STREAM_ID_BASE 300000

typedef struct {
    bool active;
    bool loop;
    void* pVoice;           // IXAudio2SourceVoice*
    uint8_t* pcmData;       // fully decoded PCM
    uint32_t pcmSize;
    uint32_t sampleRate;
    uint16_t channels;
    float volume;
    float pitch;
} XdkStreamEntry;

typedef struct {
    AudioSystem base;

    void* pXAudio2;         // IXAudio2*
    void* pMasterVoice;     // IXAudio2MasteringVoice*
    float masterGain;
    bool initialized;

    FileSystem* fileSystem; // for loading external audio files

    // Sound instance tracking (managed in C++ implementation)
    void* instanceData;     // opaque pointer to C++ instance array
    int nextInstanceCounter;

    // Audio streams
    XdkStreamEntry streams[XDK_MAX_AUDIO_STREAMS];
} XdkAudioSystem;

XdkAudioSystem* XdkAudioSystem_create(void);
