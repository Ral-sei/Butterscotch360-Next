/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Derived from ceilingtilefan/Butterscotch-360 commit
 * 7f8f1ea6044dbc55560dfbd2ca9a2f45e472c02e, with later work from
 * flaf1x/Butterscotch360-Refresh and Ral-sei.
 */

#include "platform_xdk.h"

#include "d3d9_renderer.h"
#include "noop_audio_system.h"
#include "runner.h"
#include "vm.h"
#include "xaudio2_audio.h"
#include "xdk_file_system.h"
#include "xdk_gamepad.h"
#include "xdk_phase1_probe.h"
#include "xdk_ui.h"

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

static DataWin* loadDataWin(const char* path, XdkUi* ui) {
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
    options.lazyLoadAudio = true;
    options.progressCallback = XdkUi_dataWinProgress;
    options.progressCallbackUserData = ui;
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
    Butterscotch_xdkLog("Phase 5 startup (loading UI + diagnostics)");

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

    XdkUi* ui = XdkUi_create(device);
    if (ui) XdkUi_drawLoading(ui, 0.03f, "Opening data.win");

    Butterscotch_xdkLog("parsing data.win");
    DataWin* dataWin = loadDataWin(dataWinPath, ui);
    if (!dataWin) {
        Butterscotch_xdkLog("fatal: DataWin_parse failed");
        XdkUi_destroy(ui);
        device->Release();
        d3d->Release();
        XdkFileSystem_destroy(xdkFileSystem);
        return;
    }

    if (ui) XdkUi_drawLoading(ui, 0.94f, "Initializing runtime");

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
    if (ui) XdkUi_drawLoading(ui, 0.98f, "Entering first room");
    Runner_initFirstRoom(runner);
    Butterscotch_xdkLog(
        "Phase 5 ready (room=%s, textures=%u, audio=%s, controllers=%d)",
        runner->currentRoom && runner->currentRoom->name ? runner->currentRoom->name : "unknown",
        dataWin->txtr.count,
        xdkAudio->initialized ? "ready" : "failed",
        RunnerGamepad_getDeviceCount(runner->gamepads)
    );

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

        if (runner->shouldExit) break;

        if (framesRun > 0) {
            if (!runner->appSurfaceEnabled) {
                runner->applicationWidth = XDK_FRAMEBUFFER_WIDTH;
                runner->applicationHeight = XDK_FRAMEBUFFER_HEIGHT;
                runner->usingAppSurface = false;
            } else {
                if (runner->applicationWidth <= 0 || runner->applicationHeight <= 0) {
                    runner->applicationWidth = (int32_t)dataWin->gen8.defaultWindowWidth;
                    runner->applicationHeight = (int32_t)dataWin->gen8.defaultWindowHeight;
                }
                runner->usingAppSurface = true;
            }
            int32_t gameWidth = runner->applicationWidth > 0
                ? runner->applicationWidth
                : 640;
            int32_t gameHeight = runner->applicationHeight > 0
                ? runner->applicationHeight
                : 480;
            int32_t nativeWidth = (int32_t)dataWin->gen8.defaultWindowWidth;
            int32_t nativeHeight = (int32_t)dataWin->gen8.defaultWindowHeight;
            if (nativeWidth <= 0) nativeWidth = 640;
            if (nativeHeight <= 0) nativeHeight = 480;

            // A border/widescreen mod may enlarge the logical window without
            // updating every view port. Preserve the native pixel scale and
            // expose the added area through the existing widescreen camera path.
            runner->widescreenExtraWidth = 0;
            runner->widescreenExtraHeight = 0;
            int64_t requestedAspect = (int64_t)gameWidth * (int64_t)nativeHeight;
            int64_t nativeAspect = (int64_t)gameHeight * (int64_t)nativeWidth;
            if (requestedAspect > nativeAspect) {
                int32_t scaledNativeWidth = (int32_t)(
                    ((int64_t)gameHeight * nativeWidth + nativeHeight / 2) / nativeHeight
                );
                if (gameWidth > scaledNativeWidth) {
                    runner->widescreenExtraWidth = gameWidth - scaledNativeWidth;
                }
            } else if (requestedAspect < nativeAspect) {
                int32_t scaledNativeHeight = (int32_t)(
                    ((int64_t)gameWidth * nativeHeight + nativeWidth / 2) / nativeWidth
                );
                if (gameHeight > scaledNativeHeight) {
                    runner->widescreenExtraHeight = gameHeight - scaledNativeHeight;
                }
            }

            static int32_t lastGameWidth = 0;
            static int32_t lastGameHeight = 0;
            if (gameWidth != lastGameWidth || gameHeight != lastGameHeight) {
                Butterscotch_xdkLog(
                    "render size: application=%dx%d framebuffer=%dx%d widescreenExtra=%dx%d",
                    gameWidth, gameHeight,
                    XDK_FRAMEBUFFER_WIDTH, XDK_FRAMEBUFFER_HEIGHT,
                    runner->widescreenExtraWidth, runner->widescreenExtraHeight
                );
                lastGameWidth = gameWidth;
                lastGameHeight = gameHeight;
            }

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
            XdkUi_updateDiagnostics(ui, deltaTime, framesRun);
            XdkUi_drawDiagnostics(ui, runner, renderer);
            D3D9Renderer_present(renderer);
        } else {
            XdkUi_updateDiagnostics(ui, deltaTime, framesRun);
            Sleep(1);
        }

        if (!runner->shouldExit) {
            audio->vtable->update(audio, (float)deltaTime);
        }
    }

    Butterscotch_xdkLog("game exit requested; shutting down runtime");
    audio->vtable->destroy(audio);
    runner->audioSystem = NULL;
    XdkUi_destroy(ui);
    renderer->vtable->destroy(renderer);
    Runner_free(runner);
    VM_free(vm);
    DataWin_free(dataWin);
    XdkFileSystem_destroy(xdkFileSystem);
    device->Release();
    d3d->Release();
}
