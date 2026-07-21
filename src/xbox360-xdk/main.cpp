#include "platform_xdk.h"

#include "d3d9_renderer.h"
#include "noop_audio_system.h"
#include "runner.h"
#include "vm.h"
#include "xaudio2_audio.h"
#include "xdk_file_system.h"
#include "xdk_gamepad.h"
#include "xdk_phase1_probe.h"

#include <d3d9.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern "C" ULONG __cdecl DbgPrint(const char* format, ...);

extern "C" void Butterscotch_xdkLog(const char* format, ...) {
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    DbgPrint("Butterscotch360-Next: %s\n", message);
}

extern "C" void Butterscotch_xdkAbort(const char* file, int line) {
    DbgPrint("Butterscotch360-Next: fatal abort at %s:%d\n", file, line);
    for (;;) {
        Sleep(1000);
    }
}

static DataWin* loadDataWin(const char* path) {
    DataWinParserOptions options;
    memset(&options, 0, sizeof(options));
    options.parseGen8 = true;
    options.parseOptn = true;
    options.parseLang = true;
    options.parseExtn = true;
    options.parseSond = true;
    options.parseAgrp = true;
    options.parseSprt = true;
    options.parseBgnd = true;
    options.parsePath = true;
    options.parseScpt = true;
    options.parseGlob = true;
    options.parseShdr = true;
    options.parseFont = true;
    options.parseTmln = true;
    options.parseObjt = true;
    options.parseRoom = true;
    options.parseTpag = true;
    options.parseCode = true;
    options.parseVari = true;
    options.parseFunc = true;
    options.parseStrg = true;
    options.parseTxtr = true;
    options.parseAudo = true;
    options.skipLoadingPreciseMasksForNonPreciseSprites = true;
    options.lazyLoadRooms = true;
    options.lazyLoadTextures = true;
    return DataWin_parse(path, options);
}

static IDirect3DDevice9* createD3dDevice(IDirect3D9** outD3d) {
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return NULL;

    D3DPRESENT_PARAMETERS params;
    ZeroMemory(&params, sizeof(params));
    params.BackBufferWidth = XDK_FRAMEBUFFER_WIDTH;
    params.BackBufferHeight = XDK_FRAMEBUFFER_HEIGHT;
    params.BackBufferFormat = D3DFMT_A8R8G8B8;
    params.FrontBufferFormat = D3DFMT_LE_X8R8G8B8;
    params.MultiSampleType = D3DMULTISAMPLE_NONE;
    params.BackBufferCount = 1;
    params.EnableAutoDepthStencil = TRUE;
    params.AutoDepthStencilFormat = D3DFMT_D24S8;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    IDirect3DDevice9* device = NULL;
    HRESULT result = d3d->CreateDevice(
        0,
        D3DDEVTYPE_HAL,
        NULL,
        D3DCREATE_HARDWARE_VERTEXPROCESSING,
        &params,
        &device
    );
    if (FAILED(result) || !device) {
        d3d->Release();
        return NULL;
    }

    *outD3d = d3d;
    return device;
}

VOID __cdecl main() {
    const char* dataWinPath = "game:\\data.win";
    Butterscotch_xdkLog("Phase 2/4 startup (D3D9 + XAudio2)");

    XdkFileSystem* xdkFileSystem = XdkFileSystem_create(dataWinPath);
    if (!xdkFileSystem) {
        Butterscotch_xdkLog("fatal: failed to initialize filesystem");
        return;
    }

    bool probePassed = XdkPhase1_runFileSystemProbe((FileSystem*)xdkFileSystem);
    if (!probePassed) {
        Butterscotch_xdkLog("fatal: Phase 1 filesystem regression");
        XdkFileSystem_destroy(xdkFileSystem);
        return;
    }

    IDirect3D9* d3d = NULL;
    IDirect3DDevice9* device = createD3dDevice(&d3d);
    if (!device) {
        Butterscotch_xdkLog("fatal: D3D9 device creation failed");
        XdkFileSystem_destroy(xdkFileSystem);
        return;
    }

    Butterscotch_xdkLog("parsing data.win");
    DataWin* dataWin = loadDataWin(dataWinPath);
    if (!dataWin) {
        Butterscotch_xdkLog("fatal: DataWin_parse failed");
        device->Release();
        d3d->Release();
        XdkFileSystem_destroy(xdkFileSystem);
        return;
    }

    Butterscotch_xdkLog(
        "loaded %s (WAD %u, GameMaker %u.%u.%u.%u)",
        dataWin->gen8.displayName ? dataWin->gen8.displayName : "unknown game",
        dataWin->gen8.wadVersion,
        dataWin->detectedFormat.major,
        dataWin->detectedFormat.minor,
        dataWin->detectedFormat.release,
        dataWin->detectedFormat.build
    );

    VMContext* vm = VM_create(dataWin);
    Renderer* renderer = D3D9Renderer_create(device);
    XdkAudioSystem* xdkAudio = XdkAudioSystem_create();
    AudioSystem* audio = (AudioSystem*)xdkAudio;
    Runner* runner = Runner_create(
        dataWin,
        vm,
        renderer,
        (FileSystem*)xdkFileSystem,
        audio
    );
    runner->osType = OS_XBOX360;
    XdkPlatform_attachRunnerCallbacks(runner);

    char* gameArgs[] = { (char*)"game:\\Butterscotch360-Next.xex" };
    Runner_setGameArgs(runner, gameArgs, 1);

    XdkInputState input;
    XdkInput_initialize(&input);
    XdkInput_beginFrame(&input, runner->gamepads, runner->keyboard);

    Butterscotch_xdkLog("initializing first room");
    Runner_initFirstRoom(runner);
    Butterscotch_xdkLog(
        "Phase 2/4 ready (room=%s, textures=%u, audio=%s, controllers=%d)",
        runner->currentRoom && runner->currentRoom->name ? runner->currentRoom->name : "unknown",
        dataWin->txtr.count,
        xdkAudio->initialized ? "ready" : "failed",
        RunnerGamepad_getDeviceCount(runner->gamepads)
    );

    int32_t gameWidth = (int32_t)dataWin->gen8.defaultWindowWidth;
    int32_t gameHeight = (int32_t)dataWin->gen8.defaultWindowHeight;
    if (gameWidth <= 0) gameWidth = 640;
    if (gameHeight <= 0) gameHeight = 480;

    LARGE_INTEGER frequency;
    LARGE_INTEGER previousTime;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&previousTime);
    double accumulator = 0.0;

    while (!runner->shouldExit) {
        LARGE_INTEGER currentTime;
        QueryPerformanceCounter(&currentTime);
        double deltaTime = (double)(currentTime.QuadPart - previousTime.QuadPart)
            / (double)frequency.QuadPart;
        previousTime = currentTime;
        if (deltaTime < 0.0) deltaTime = 0.0;
        if (deltaTime > 0.25) deltaTime = 0.25;
        accumulator += deltaTime;

        uint32_t roomSpeed = runner->currentRoom && runner->currentRoom->speed
            ? runner->currentRoom->speed
            : 60;
        double targetFrameTime = 1.0 / (double)roomSpeed;
        double maxAccumulator = targetFrameTime * 4.0;
        if (accumulator > maxAccumulator) accumulator = maxAccumulator;

        int32_t framesRun = 0;
        while (accumulator >= targetFrameTime && framesRun < 4 && !runner->shouldExit) {
            XdkInput_beginFrame(&input, runner->gamepads, runner->keyboard);
            Runner_step(runner);
            accumulator -= targetFrameTime;
            framesRun++;
            roomSpeed = runner->currentRoom && runner->currentRoom->speed
                ? runner->currentRoom->speed
                : 60;
            targetFrameTime = 1.0 / (double)roomSpeed;
        }

        if (framesRun > 0) {
            Runner_drawPre(runner, XDK_FRAMEBUFFER_WIDTH, XDK_FRAMEBUFFER_HEIGHT);
            Runner_beginFrame(
                runner,
                gameWidth,
                gameHeight,
                XDK_FRAMEBUFFER_WIDTH,
                XDK_FRAMEBUFFER_HEIGHT,
                XDK_FRAMEBUFFER_WIDTH,
                XDK_FRAMEBUFFER_HEIGHT
            );
            Runner_drawViews(runner, gameWidth, gameHeight, false);
            renderer->vtable->endFrameInit(renderer);
            Runner_drawPost(runner, XDK_FRAMEBUFFER_WIDTH, XDK_FRAMEBUFFER_HEIGHT);
            renderer->vtable->endFrameEnd(renderer);
            Runner_drawGUI(
                runner,
                XDK_FRAMEBUFFER_WIDTH,
                XDK_FRAMEBUFFER_HEIGHT,
                gameWidth,
                gameHeight
            );
            D3D9Renderer_present(renderer);
        } else {
            Sleep(1);
        }

        audio->vtable->update(audio, (float)deltaTime);
    }

    audio->vtable->destroy(audio);
    runner->audioSystem = NULL;
    renderer->vtable->destroy(renderer);
    Runner_free(runner);
    VM_free(vm);
    DataWin_free(dataWin);
    XdkFileSystem_destroy(xdkFileSystem);
    device->Release();
    d3d->Release();
}
