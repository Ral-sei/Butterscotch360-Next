// Butterscotch Xbox 360 port - runtime entry point.
//
// Boots the runtime with the port's D3D9 renderer, reads the game's data.win,
// creates the VM/Runner and runs the frame loop, then shuts down. The audio
// backend is XAudio2 and the file system is the backslash-native port backend.
//
// The port owns main(), so the frame order mirrors the other console ports
// (src/ps2/main.c, src/ps3/main.c) and src/loop.c:
//
//   Runner_drawPre -> Runner_beginFrame -> Runner_drawViews -> endFrameInit
//   -> Runner_drawPost -> endFrameEnd (composites to the back buffer)
//   -> Runner_drawGUI -> overlay -> X360Renderer_present
//
// The D3D device is created before data.win is parsed so the port's loading
// screen (src/xbox360/xbox360_overlay.c) can cover the parse. The debug stats
// overlay is toggled with LB+RB on controller 0.
//
// Port-owned. No public source is touched.

#include "data_win.h"
#include "vm.h"
#include "runner.h"
#include "renderer.h"
#include "xbox360_renderer.h"
#include "xbox360_overlay.h"
#include "xbox360_audio.h"
#include "xbox360_file_system.h"
#include "xbox360_input.h"
#include "log.h"
#include "gettime.h"
#include "utils.h"
#include "stdio_compat.h"
#include "stb_ds.h"

#include <xtl.h>

#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD) -1)
#endif

// Provisional writable root for the save area. NULL means "the directory the
// data.win came from", which is writable on a devkit but not on retail. A real
// save policy (XContent content packages, user selection, quota) is step 3.2's
// open item and is deliberately not faked here.
#ifndef BS_X360_SAVE_ROOT
#define BS_X360_SAVE_ROOT NULL
#endif

// Frames to run before shutting down, so a one-shot deployment exits instead of
// running forever. Override with -DBS_X360_TEST_FRAMES=<n>; 0 means "run until
// the game asks to exit".
#ifndef BS_X360_TEST_FRAMES
#define BS_X360_TEST_FRAMES 120
#endif

static bool bs_x360_fileExists(const char *path) {
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES;
}

static const char *bs_x360_findDataWin(void) {    // A title deployed with magicboot gets its directory as the working
    // directory; a disc title has the game partition. Try both.
    static const char *candidates[] = {
        "game:\\data.win",
        "data.win",
        "D:\\data.win",
    };
    for (int i = 0; i < (int) (sizeof(candidates) / sizeof(candidates[0])); i++) {
        if (bs_x360_fileExists(candidates[i])) {
            return candidates[i];
        }
    }
    return NULL;
}

// Optional hardware smoke test for the renderer's surface/EDRAM path, which a
// game may never exercise. Enabled with -DBS_X360_RENDERER_SELFTEST; it creates
// a surface, clears it, reads it back (Resolve + XGUntileTextureLevel), uploads
// pixels (XGTileTextureLevel + redraw), makes a sprite from it, and frees it,
// logging each step so KDNet shows exactly which capability failed.
#ifdef BS_X360_RENDERER_SELFTEST
static void bs_x360_logPixel(const char *label, const uint8_t *p) {
    logInfo("RendererSelfTest: %s = (%u,%u,%u,%u)\n", label, p[0], p[1], p[2], p[3]);
}

static bool bs_x360_rendererSelftest(Renderer *renderer) {
    RendererVtable *v = renderer->vtable;
    bool ok = true;

    int32_t sid = v->createSurface(renderer, 64, 64);
    logInfo("RendererSelfTest: createSurface -> %d (exists=%d)\n", sid,
            sid >= 0 ? (int) v->surfaceExists(renderer, sid) : 0);
    if (sid < 0 || !v->surfaceExists(renderer, sid)) return false;

    if (!v->setRenderTarget(renderer, sid, false)) {
        logError("RendererSelfTest: setRenderTarget failed\n");
        ok = false;
    }

    v->clearScreen(renderer, 0x0000FF, 1.0f); // BGR red

    uint8_t *pixels = (uint8_t *) safeMalloc(64 * 64 * 4);
    if (!v->surfaceGetPixels(renderer, sid, pixels)) {
        logError("RendererSelfTest: surfaceGetPixels (after clear) failed\n");
        ok = false;
    } else {
        bs_x360_logPixel("after clear   (expect 255,0,0,255)", pixels);
        if (pixels[0] != 255 || pixels[1] != 0 || pixels[2] != 0) ok = false;
    }

    for (int i = 0; i < 64 * 64; i++) {
        pixels[i * 4 + 0] = 0; pixels[i * 4 + 1] = 255; pixels[i * 4 + 2] = 0; pixels[i * 4 + 3] = 255;
    }
    if (!v->surfaceSetPixels(renderer, sid, pixels)) {
        logError("RendererSelfTest: surfaceSetPixels failed\n");
        ok = false;
    }
    if (!v->surfaceGetPixels(renderer, sid, pixels)) {
        logError("RendererSelfTest: surfaceGetPixels (after setPixels) failed\n");
        ok = false;
    } else {
        bs_x360_logPixel("after setPixels (expect 0,255,0,255)", pixels);
        if (pixels[0] != 0 || pixels[1] != 255 || pixels[2] != 0) ok = false;
    }

    int32_t sprite = v->createSpriteFromSurface(renderer, sid, 0, 0, 32, 32, false, false, 0, 0);
    logInfo("RendererSelfTest: createSpriteFromSurface -> %d\n", sprite);
    if (sprite >= 0) v->deleteSprite(renderer, sprite);

    v->surfaceFree(renderer, sid);
    free(pixels);

    logInfo("RendererSelfTest: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

// Regression test for the room_dw_fcastle_orange_gauntlet EDRAM failure:
// several live 2000x120 / 2080x120 GML surfaces used to exhaust EDRAM because
// each kept a persistent render target. Surface RTs are now transient, so all
// of these must be creatable and bindable while the others stay alive.
static bool bs_x360_edramStressTest(Renderer *renderer) {
    RendererVtable *v = renderer->vtable;
    enum { COUNT = 8 };
    int32_t ids[COUNT];
    int32_t created = 0;
    int32_t lastW = 0, lastH = 0;
    bool ok = true;

    for (int i = 0; i < COUNT; i++) {
        int32_t w = (i & 1) ? 2080 : 2000;
        int32_t h = 120;
        int32_t sid = v->createSurface(renderer, w, h);
        if (sid < 0 || !v->surfaceExists(renderer, sid)) {
            logError("EdramStress: createSurface %d (%dx%d) failed\n", i, w, h);
            ok = false;
            break;
        }
        ids[created++] = sid;
        lastW = w;
        lastH = h;
    }

    for (int i = 0; i < created; i++) {
        if (!v->setRenderTarget(renderer, ids[i], false)) {
            logError("EdramStress: setRenderTarget %d failed\n", ids[i]);
            ok = false;
            break;
        }
        v->clearScreen(renderer, 0x0000FF, 1.0f); // BGR red
    }

    if (ok && created > 0) {
        uint8_t *pixels = (uint8_t *) safeMalloc((size_t) lastW * (size_t) lastH * 4);
        if (!v->surfaceGetPixels(renderer, ids[created - 1], pixels)
            || pixels[0] != 255 || pixels[1] != 0 || pixels[2] != 0) {
            logError("EdramStress: readback failed (%u,%u,%u,%u)\n",
                     pixels[0], pixels[1], pixels[2], pixels[3]);
            ok = false;
        }
        free(pixels);
    }

    for (int i = 0; i < created; i++) {
        v->surfaceFree(renderer, ids[i]);
    }

    logInfo("EdramStress: %s (%d surfaces)\n", ok ? "PASS" : "FAIL", created);
    return ok;
}

// Exercises the predicated-tiling fallback with a surface that cannot fit in
// EDRAM as a single RT. Clears red, draws a green 1024x1024 quadrant, and reads
// back both halves to prove the tile viewport/projection and EndTiling resolve.
static bool bs_x360_tilingSelftest(Renderer *renderer) {
    RendererVtable *v = renderer->vtable;
    const int32_t size = 2048;
    int32_t sid = v->createSurface(renderer, size, size);
    if (sid < 0 || !v->surfaceExists(renderer, sid)) {
        logError("TilingSelfTest: createSurface failed\n");
        return false;
    }
    if (!v->setRenderTarget(renderer, sid, false)) {
        logError("TilingSelfTest: setRenderTarget failed\n");
        v->surfaceFree(renderer, sid);
        return false;
    }

    v->clearScreen(renderer, 0x0000FF, 1.0f); // BGR red
    v->drawRectangleColor(renderer, 0.0f, 0.0f, 1024.0f, 1024.0f,
                          0x00FF00, 0x00FF00, 0x00FF00, 0x00FF00, 1.0f, false);
    v->flush(renderer);

    uint8_t *pixels = (uint8_t *) safeMalloc((size_t) size * (size_t) size * 4);
    bool ok = v->surfaceGetPixels(renderer, sid, pixels);
    if (ok) {
        const uint8_t *green = pixels + (((size_t) 512 * size) + 512) * 4;
        const uint8_t *red = pixels + (((size_t) 512 * size) + 1536) * 4;
        logInfo("TilingSelfTest: (512,512) expect green = (%u,%u,%u,%u)\n",
                green[0], green[1], green[2], green[3]);
        logInfo("TilingSelfTest: (1536,512) expect red   = (%u,%u,%u,%u)\n",
                red[0], red[1], red[2], red[3]);
        if (green[0] != 0 || green[1] != 255 || green[2] != 0) ok = false;
        if (red[0] != 255 || red[1] != 0 || red[2] != 0) ok = false;
    }
    free(pixels);

    v->surfaceFree(renderer, sid);
    logInfo("TilingSelfTest: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
#endif

// The runner's game speed is expressed in steps per second (the room speed,
// or game_set_speed). Present synchronises to the 60 Hz vblank, so without an
// explicit wait the title would step twice as fast as a 30 fps game expects.
// Sleep() only has millisecond granularity; sleep until 1 ms before the
// deadline and yield for the remainder so the average period stays on target.
static void bs_x360_sleepUntil(uint64_t deadline) {
    for (;;) {
        uint64_t now = nowNanos();
        if (now >= deadline) return;
        uint64_t remaining = deadline - now;
        if (remaining > 2000000ull) {
            DWORD ms = (DWORD) ((remaining - 1000000ull) / 1000000ull);
            if (ms > 0) Sleep(ms);
        } else {
            Sleep(0);
        }
    }
}

#ifdef BS_X360_RENDER_STATS
// Diagnostic only (BS_X360_RENDER_STATS): logs what the runtime actually
// decoded for a room, and where the draw phase spends its time. The tile-layer
// dump matters because a room can be huge while the visible part is small; it
// separates "too many quads" from "slow per-quad/runner work".
static uint64_t bs_x360_phaseAccum[7];
static uint32_t bs_x360_phaseFrames;
static Room *bs_x360_lastLoggedRoom;

static void bs_x360_logRoomTileLayers(DataWin *dw, Room *room) {
    if (dw == NULL || room == NULL) return;
    logInfo("X360Stats: room '%s' %ux%u layers=%u detected=%u.%u.%u.%u\n",
            room->name != NULL ? room->name : "?", room->width, room->height,
            room->layerCount, dw->detectedFormat.major, dw->detectedFormat.minor,
            dw->detectedFormat.release, dw->detectedFormat.build);
    for (uint32_t i = 0; i < room->layerCount; i++) {
        RoomLayer *layer = &room->layers[i];
        if (layer->type != RoomLayerType_Tiles || layer->tilesData == NULL) continue;
        RoomLayerTilesData *td = layer->tilesData;
        uint32_t cells = td->tilesX * td->tilesY;
        uint32_t nonEmpty = 0;
        for (uint32_t c = 0; c < cells; c++) {
            if ((td->tileData[c] & 0x0FFFFFFFu) != 0) nonEmpty++;
        }
        logInfo("X360Stats: tile layer '%s' grid=%ux%u bg=%d nonEmpty=%u\n",
                layer->name != NULL ? layer->name : "?", td->tilesX, td->tilesY,
                td->backgroundIndex, nonEmpty);
    }
}

static void bs_x360_logPhaseStats(uint64_t pre, uint64_t begin, uint64_t views,
                                  uint64_t init, uint64_t post, uint64_t end,
                                  uint64_t gui) {
    bs_x360_phaseAccum[0] += pre;
    bs_x360_phaseAccum[1] += begin;
    bs_x360_phaseAccum[2] += views;
    bs_x360_phaseAccum[3] += init;
    bs_x360_phaseAccum[4] += post;
    bs_x360_phaseAccum[5] += end;
    bs_x360_phaseAccum[6] += gui;
    bs_x360_phaseFrames++;
    if (bs_x360_phaseFrames < 300) return;

    double inv = 1.0 / (double) bs_x360_phaseFrames / 1000000.0;
    logInfo("X360Stats: draw phases ms pre=%.2f begin=%.2f views=%.2f init=%.2f post=%.2f end=%.2f gui=%.2f\n",
            (double) bs_x360_phaseAccum[0] * inv, (double) bs_x360_phaseAccum[1] * inv,
            (double) bs_x360_phaseAccum[2] * inv, (double) bs_x360_phaseAccum[3] * inv,
            (double) bs_x360_phaseAccum[4] * inv, (double) bs_x360_phaseAccum[5] * inv,
            (double) bs_x360_phaseAccum[6] * inv);
    memset(bs_x360_phaseAccum, 0, sizeof(bs_x360_phaseAccum));
    bs_x360_phaseFrames = 0;
}
#endif

#ifdef BS_X360_TEXTURE_AUTOGEN
static bool bs_x360_textureProgress(void *user, uint32_t current, uint32_t total) {
    Gen8 *gen8 = (Gen8 *) user;
    const char *gameName = gen8 != NULL ? gen8->displayName : NULL;

    X360Overlay_drawTextureProgress(gameName, current, total, false);
    if (X360Input_consumeStartPress()) {
        X360Overlay_drawTextureProgress(gameName, current, total, true);
        return true;
    }
    return false;
}
#endif

int main(void) {
    OutputDebugStringA("Butterscotch Xbox 360: D3D9 runtime boot\n");

    const char *dataWinPath = bs_x360_findDataWin();
    if (dataWinPath == NULL) {
        logError("data.win not found (tried game:\\data.win, data.win, D:\\data.win)\n");
        return 1;
    }
    logInfo("Loading %s...\n", dataWinPath);

    const int32_t fbWidth = BS_X360_RENDER_WIDTH;
    const int32_t fbHeight = BS_X360_RENDER_HEIGHT;

    // Create the D3D device before data.win is parsed so the loading screen can
    // cover the (potentially multi-second) parse. Runner_create's vtable init
    // later finishes the data-dependent half: texture page tables and the
    // user-shader sidecar.
    Renderer *renderer = X360Renderer_create();
    X360Renderer_initDevice(renderer);
    X360Overlay_init(renderer);
    X360Overlay_drawStatusScreen(NULL, "Loading data.win...", false);

    DataWinParserOptions options = {0};
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
    // The renderer needs the texture payloads; the audio backend needs the
    // embedded AUDO data (the target's embedded sounds are PCM WAVE).
    options.parseTxtr = true;
    options.parseAudo = true;
    options.skipLoadingPreciseMasksForNonPreciseSprites = true;
    options.lazyLoadRooms = false;
    // Per-chunk buffering trips the reader's range check on TXTR (the chunk's
    // blob offsets are absolute file offsets). The probe uses whole-file
    // buffering and it works; do the same here.
    options.loadType = DATAWINLOADTYPE_LOAD_IN_MEMORY_AHEAD_OF_TIME;
    options.progressCallback = X360Overlay_statusScreenCallback;
    options.progressCallbackUserData = X360Overlay_getCallbackData();

    DataWin *dataWin = DataWin_parse(dataWinPath, options);
    if (dataWin == NULL) {
        logError("Failed to parse %s\n", dataWinPath);
        X360Overlay_drawErrorScreen(NULL, "Failed to parse data.win");
        Sleep(5000);
        X360Overlay_deinit();
        renderer->vtable->destroy(renderer);
        return 1;
    }

    Gen8 *gen8 = &dataWin->gen8;
    logInfo("Loaded \"%s\" (WAD %u, GM %u.%u.%u.%u)\n",
            gen8->displayName, gen8->wadVersion,
            dataWin->detectedFormat.major, dataWin->detectedFormat.minor,
            dataWin->detectedFormat.release, dataWin->detectedFormat.build);

    // Match the PS2 port: refuse unsupported WAD versions with a visible error
    // instead of failing later in the VM.
    bool wadVersionSupported = false;
#ifdef ENABLE_WAD14
    if (14 >= dataWin->gen8.wadVersion) wadVersionSupported = true;
#endif
#ifdef ENABLE_WAD16
    if (dataWin->gen8.wadVersion == 15 || dataWin->gen8.wadVersion == 16) wadVersionSupported = true;
#endif
#ifdef ENABLE_WAD17
    if (dataWin->gen8.wadVersion == 17) wadVersionSupported = true;
#endif
    if (!wadVersionSupported) {
        char errorText[96];
        snprintf(errorText, sizeof(errorText), "Unsupported WAD version %u!",
                 dataWin->gen8.wadVersion);
        logError("%s\n", errorText);
        X360Overlay_drawErrorScreen(gen8->displayName, errorText);
        Sleep(5000);
        X360Overlay_deinit();
        renderer->vtable->destroy(renderer);
        DataWin_free(dataWin);
        return 1;
    }

    X360Overlay_drawStatusScreen(gen8->displayName, "Creating VM...", true);
    VMContext *vm = VM_create(dataWin);

    X360Overlay_drawStatusScreen(gen8->displayName, "Initializing file system...", true);
    // The bundle (read-only game data) is the directory data.win came from; the
    // save area defaults to the same directory until a real save policy exists.
    char *bundleRoot = safeStrdup(dataWinPath);
    bsGetDirname(bundleRoot);
    const char *saveRoot = BS_X360_SAVE_ROOT != NULL ? BS_X360_SAVE_ROOT : bundleRoot;
    FileSystem *fileSystem = X360FileSystem_create(bundleRoot, saveRoot);
    logInfo("FileSystem: bundle=%s save=%s\n", bundleRoot, saveRoot);

    X360Overlay_drawStatusScreen(gen8->displayName, "Initializing audio...", true);
    AudioSystem *audioSystem = (AudioSystem *) X360AudioSystem_create();

    X360Overlay_drawStatusScreen(gen8->displayName, "Creating runner...", true);
    Runner *runner = Runner_create(dataWin, vm, renderer, fileSystem, audioSystem, 0);
    runner->osType = OS_XBOX360;

#ifdef BS_X360_TEXTURE_AUTOGEN
    // Default build: generate data.x360tex on first boot (or finish an
    // incomplete one) with an overlay progress bar and START-to-skip. On later
    // boots a complete sidecar makes this return immediately.
    X360Renderer_generateTextureSidecar(renderer, bs_x360_textureProgress, gen8);
#endif

    X360Overlay_drawStatusScreen(gen8->displayName, "Initializing first room...", true);
    Runner_initFirstRoom(runner);
    logInfo("First room: %s\n", runner->currentRoom != NULL ? runner->currentRoom->name : "<none>");

#ifdef BS_X360_RENDERER_SELFTEST
    bs_x360_rendererSelftest(renderer);
    bs_x360_edramStressTest(renderer);
    bs_x360_tilingSelftest(renderer);
#endif
#ifdef BS_X360_SHADER_SELFTEST
    X360Renderer_runShaderSelftest(renderer);
#endif
#ifdef BS_X360_AUDIO_SELFTEST
    X360AudioSystem_runSelftest((X360AudioSystem *) audioSystem);
#endif

    X360Overlay_drawStatusScreen(gen8->displayName, "Reticulating splines...", true);

    int32_t gameW = (int32_t) gen8->defaultWindowWidth;
    int32_t gameH = (int32_t) gen8->defaultWindowHeight;
    if (gameW <= 0 || gameH <= 0) {
        gameW = fbWidth;
        gameH = fbHeight;
    }

    uint64_t start = nowNanos();
    uint64_t lastFrameStart = start;
    uint64_t steadyStart = 0;   // excludes first-room/transition texture loads
    int frames = 0;
    for (; (BS_X360_TEST_FRAMES <= 0 || frames < BS_X360_TEST_FRAMES) && !runner->shouldExit; frames++) {
        // delta_time is the wall-clock interval between game steps, exactly as
        // the desktop loop computes it. The port previously never assigned it,
        // so delta_time read 0 and anything paced by it (physics, fades) ran at
        // the wrong rate.
        uint64_t frameStart = nowNanos();
        runner->deltaTime = (GMLReal) ((double) (frameStart - lastFrameStart) / 1000.0);
        lastFrameStart = frameStart;

#ifdef BS_X360_RENDER_STATS
        if (runner->currentRoom != bs_x360_lastLoggedRoom) {
            bs_x360_lastLoggedRoom = runner->currentRoom;
            bs_x360_logRoomTileLayers(runner->dataWin, runner->currentRoom);
        }
#endif

        RunnerKeyboard_beginFrame(runner->keyboard);
        RunnerGamepad_beginFrame(runner->gamepads);
        X360Input_poll(runner);

        // LB+RB on controller 0 toggles the port debug overlay. The combo is
        // consumed here and never reaches the game's key state.
        if (X360Input_consumeOverlayToggle()) {
            X360Overlay_toggle(runner);
        }

        uint64_t stepStart = nowNanos();
        Runner_step(runner);
        uint64_t stepEnd = nowNanos();

        // Drive audio gain fades and reap finished voices. The public runner
        // does not update the audio system itself (each console port does it in
        // its own frame loop), so the port must call it here.
        float dt = (float) (runner->deltaTime / 1000000.0);
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.1f) dt = 0.1f;
        uint64_t audioStart = nowNanos();
        audioSystem->vtable->update(audioSystem, dt);
        uint64_t audioEnd = nowNanos();

        uint64_t drawStart = nowNanos();
        Runner_drawPre(runner, fbWidth, fbHeight);
        uint64_t drawPreEnd = nowNanos();
        Runner_beginFrame(runner, gameW, gameH, fbWidth, fbHeight, fbWidth, fbHeight);
        uint64_t drawBeginEnd = nowNanos();
        Runner_drawViews(runner, gameW, gameH, false);
        uint64_t drawViewsEnd = nowNanos();
        renderer->vtable->endFrameInit(renderer);
        uint64_t drawInitEnd = nowNanos();
        Runner_drawPost(runner, fbWidth, fbHeight);
        uint64_t drawPostEnd = nowNanos();
        renderer->vtable->endFrameEnd(renderer);
        uint64_t drawEndEnd = nowNanos();
        Runner_drawGUI(runner, fbWidth, fbHeight, gameW, gameH);
        uint64_t drawEnd = nowNanos();

#ifdef BS_X360_RENDER_STATS
        bs_x360_logPhaseStats(drawPreEnd - drawStart, drawBeginEnd - drawPreEnd,
                              drawViewsEnd - drawBeginEnd, drawInitEnd - drawViewsEnd,
                              drawPostEnd - drawInitEnd, drawEndEnd - drawPostEnd,
                              drawEnd - drawEndEnd);
#endif

        uint64_t tickEnd = nowNanos();
        float tickMs = (float) ((double) (tickEnd - frameStart) / 1000000.0);
        float stepMs = (float) ((double) (stepEnd - stepStart) / 1000000.0);
        float drawMs = (float) ((double) (drawEnd - drawStart) / 1000000.0);
        float audioMs = (float) ((double) (audioEnd - audioStart) / 1000000.0);

        // Match the desktop loop and the PS2/PS3 ports: when a room change is
        // pending, the frame that was just drawn is the outgoing room's last
        // frame. Presenting it would flash that frame after the black
        // transition. Keep the previously presented frame (the transition
        // black) on screen and apply the room change before the next draw.
        if (runner->pendingRoom == -1) {
            X360Overlay_drawDebugOverlay(runner, tickMs, stepMs, drawMs, audioMs);
            X360Renderer_present(renderer);
        }
        Runner_handlePendingRoomChange(runner);

        // Lock the game step to the room/game speed. The vblank-synchronised
        // Present alone runs at 60 Hz, which made a 30 fps title step twice as
        // fast. Runner_getEffectiveGameSpeed covers room speed, game_set_speed
        // and the data.win default.
        double gameSpeed = Runner_getEffectiveGameSpeed(runner);
        if (gameSpeed > 0.0) {
            uint64_t targetFrameTime = (uint64_t) (1000000000.0 / gameSpeed);
            bs_x360_sleepUntil(frameStart + targetFrameTime);
        }

        // Start the steady-state sample after the first-room/transition texture
        // loads, which otherwise dominate the average of a short test run.
        if (frames == 120) steadyStart = nowNanos();
    }
    uint64_t elapsed = nowNanos() - start;
    logInfo("Ran %d frame(s) in %.2f ms (%.2f ms/frame)\n",
            frames, (double) elapsed / 1000000.0,
            frames > 0 ? (double) elapsed / 1000000.0 / (double) frames : 0.0);
    if (steadyStart != 0 && frames > 121) {
        int steadyFrames = frames - 121;
        double steadyMs = (double) (nowNanos() - steadyStart) / 1000000.0;
        logInfo("Steady-state: %d frame(s) in %.2f ms (%.2f ms/frame)\n",
                steadyFrames, steadyMs, steadyMs / (double) steadyFrames);
    }

    // Teardown. Repeat-exit safe.
    X360Overlay_deinit();
    Runner_free(runner);
    X360FileSystem_destroy(fileSystem);
    free(bundleRoot);
    audioSystem->vtable->destroy(audioSystem);
    renderer->vtable->destroy(renderer);
    VM_free(vm);
    DataWin_free(dataWin);

    OutputDebugStringA("Butterscotch Xbox 360: runtime shutdown complete\n");
    return 0;
}
