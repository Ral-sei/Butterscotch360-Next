// Xbox 360 port - loading/status screen and debug stats overlay.
//
// This is the port-owned counterpart of src/ps2/ps2_overlay.c. It is NOT part
// of the public RendererVtable: it draws through the renderer's built-in shader
// and embedded debug font (drawTextUI / drawRectangle / clearScreen), then
// presents with X360Renderer_present().
//
// The loading screen covers DataWin_parse, so the port creates the D3D device
// early (X360Renderer_initDevice) and installs
// X360Overlay_statusScreenCallback as DataWinParserOptions.progressCallback.
//
// Port-owned. No public source is touched.

#include "xbox360_overlay.h"

#include "xbox360_renderer.h"
#include "debug_font.h"
#include "profiler.h"
#include "log.h"
#include "stb_ds.h"
#include "stdio_compat.h"
#include "string_compat.h"

#include <stdlib.h>
#include <string.h>

#include <xtl.h>

#ifndef BUTTERSCOTCH_COMMIT_HASH
#define BUTTERSCOTCH_COMMIT_HASH "unknown"
#endif
#ifndef BUTTERSCOTCH_COMMIT_DATE
#define BUTTERSCOTCH_COMMIT_DATE "unknown"
#endif

#define X360_MAX_CHUNK_STATS 24
#define X360_OVERLAY_PROFILER_WINDOW_FRAMES 60

typedef struct {
    char label[16];
    uint32_t count;
} X360ChunkStat;

typedef struct {
    X360ChunkStat stats[X360_MAX_CHUNK_STATS];
    int statCount;
} X360LoadingScreenState;

typedef struct {
    bool initialized;
    Renderer *renderer;
    X360OverlayState state;
    X360LoadingScreenState loadingState;
    int profilerFramesInWindow;
#ifdef ENABLE_VM_GML_PROFILER
    char profilerOverlayText[4096];
#endif
} X360Overlay;

static X360Overlay gOverlay = { 0 };

// ===[ Colours ]===
//
// The renderer's colour arguments are 0xBBGGRR (see BGR_R/G/B in utils.h), not
// 0xRRGGBB. Keep all overlay colours in one place so this is not forgotten.
static uint32_t x360OverlayColor(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t) b << 16) | ((uint32_t) g << 8) | (uint32_t) r;
}

#define X360_COLOR_WHITE        x360OverlayColor(0xFF, 0xFF, 0xFF)
#define X360_COLOR_BUTTERSCOTCH x360OverlayColor(0xE8, 0xA5, 0x52)
#define X360_COLOR_GRAY         x360OverlayColor(0xAA, 0xAA, 0xAA)
#define X360_COLOR_DARK_GRAY    x360OverlayColor(0x70, 0x70, 0x70)
#define X360_COLOR_BAR_BG       x360OverlayColor(0x40, 0x40, 0x40)
#define X360_COLOR_BAR_FILL     x360OverlayColor(0xFF, 0xCC, 0x00)
#define X360_COLOR_RED          x360OverlayColor(0xFF, 0x40, 0x40)
#define X360_COLOR_BLACK        x360OverlayColor(0x00, 0x00, 0x00)

// ===[ Text helpers ]===

// drawTextUI uses the renderer's drawHalign/drawValign; save and restore them
// so the overlay never changes how the game draws text.
static void overlayPrint(Renderer *renderer, float x, float y, float scale,
                         uint32_t color, float alpha, int32_t halign, const char *text) {
    if (renderer == NULL || renderer->vtable == NULL || renderer->vtable->drawTextUI == NULL) return;
    if (text == NULL) return;

    int32_t oldHalign = renderer->drawHalign;
    int32_t oldValign = renderer->drawValign;
    renderer->drawHalign = halign;
    renderer->drawValign = 0;
    renderer->vtable->drawTextUI(renderer, text, x, y, scale, scale, 0.0f,
                                 (int32_t) color, (int32_t) color,
                                 (int32_t) color, (int32_t) color,
                                 alpha, -1.0f);
    renderer->drawHalign = oldHalign;
    renderer->drawValign = oldValign;
}

// 8-direction soft black halo, like the PS2 DebugFontRenderer outline.
static void overlayPrintOutlined(Renderer *renderer, float x, float y, float scale,
                                 uint32_t color, float alpha, int32_t halign,
                                 const char *text) {
    static const float offsets[8][2] = {
        { -1.0f, -1.0f }, { 0.0f, -1.0f }, { 1.0f, -1.0f },
        { -1.0f,  0.0f },                  { 1.0f,  0.0f },
        { -1.0f,  1.0f }, { 0.0f,  1.0f }, { 1.0f,  1.0f },
    };
    const float radius = 1.5f;
    for (int i = 0; i < 8; i++) {
        overlayPrint(renderer, x + offsets[i][0] * radius, y + offsets[i][1] * radius,
                     scale, X360_COLOR_BLACK, alpha * 0.15f, halign, text);
    }
    overlayPrint(renderer, x, y, scale, color, alpha, halign, text);
}

// ===[ Memory ]===

// PS2 uses sbrk/mallinfo; Xenon exposes GlobalMemoryStatus. The XDK documents
// that it may overstate free memory on XDK-GB and is not a precise dynamic
// tracker, so the overlay labels it "approx".
static void overlayMemory(uint32_t *usedMB, uint32_t *totalMB, uint32_t *freeMB) {
    MEMORYSTATUS stat;
    memset(&stat, 0, sizeof(stat));
    GlobalMemoryStatus(&stat);

    const double mb = 1024.0 * 1024.0;
    if (totalMB != NULL) *totalMB = (uint32_t) ((double) stat.dwTotalPhys / mb);
    if (freeMB != NULL) *freeMB = (uint32_t) ((double) stat.dwAvailPhys / mb);
    if (usedMB != NULL) {
        SIZE_T used = stat.dwTotalPhys > stat.dwAvailPhys
                      ? stat.dwTotalPhys - stat.dwAvailPhys : 0;
        *usedMB = (uint32_t) ((double) used / mb);
    }
}

// ===[ Loading screen ]===

static void drawCredits(Renderer *renderer) {
    char buildText[128];
    snprintf(buildText, sizeof(buildText), "Build %s [%s]",
             BUTTERSCOTCH_COMMIT_HASH, BUTTERSCOTCH_COMMIT_DATE);

    overlayPrint(renderer, 20.0f, 650.0f, 0.65f, X360_COLOR_DARK_GRAY, 1.0f, 0,
                 "Butterscotch360-Next");
    overlayPrint(renderer, 20.0f, 675.0f, 0.65f, X360_COLOR_DARK_GRAY, 1.0f, 0, buildText);
}

static void beginStatusScreen(Renderer *renderer, const char *gameName) {
    X360Renderer_beginOverlay(renderer);
    renderer->vtable->clearScreen(renderer, 0x000000, 1.0f);

    overlayPrint(renderer, 640.0f, 280.0f, 1.6f, X360_COLOR_BUTTERSCOTCH, 1.0f, 1,
                 "Butterscotch360-Next");
    if (gameName != NULL && gameName[0] != '\0') {
        overlayPrint(renderer, 640.0f, 340.0f, 1.0f, X360_COLOR_GRAY, 1.0f, 1, gameName);
    }
}

static void endStatusScreen(Renderer *renderer) {
    drawCredits(renderer);
    X360Renderer_endOverlay(renderer);
    X360Renderer_present(renderer);
}

static void drawChunkStats(Renderer *renderer) {
    if (renderer == NULL || gOverlay.loadingState.statCount <= 0) return;

    char line[48];
    float y = 20.0f;
    for (int i = 0; i < gOverlay.loadingState.statCount; i++) {
        snprintf(line, sizeof(line), "%u %s",
                 gOverlay.loadingState.stats[i].count,
                 gOverlay.loadingState.stats[i].label);
        overlayPrint(renderer, 20.0f, y, 0.5f, X360_COLOR_GRAY, 1.0f, 0, line);
        y += 22.0f;
    }
}

static void recordChunkStats(DataWin *dataWin) {
    if (dataWin == NULL) return;

    typedef struct {
        uint32_t *countPtr;
        const char *label;
    } CountSource;

    const CountSource sources[] = {
        { &dataWin->sond.count, "sounds" },
        { &dataWin->sprt.count, "sprites" },
        { &dataWin->bgnd.count, "backgrounds" },
        { &dataWin->font.count, "fonts" },
        { &dataWin->objt.count, "objects" },
        { &dataWin->room.count, "rooms" },
        { &dataWin->code.count, "code entries" },
        { &dataWin->txtr.count, "textures" },
    };

    for (int i = 0; i < (int) (sizeof(sources) / sizeof(sources[0])); i++) {
        if (*sources[i].countPtr == 0) continue;

        bool found = false;
        for (int j = 0; j < gOverlay.loadingState.statCount; j++) {
            if (strcmp(gOverlay.loadingState.stats[j].label, sources[i].label) == 0) {
                found = true;
                break;
            }
        }
        if (found) continue;
        if (gOverlay.loadingState.statCount >= X360_MAX_CHUNK_STATS) continue;

        X360ChunkStat *stat = &gOverlay.loadingState.stats[gOverlay.loadingState.statCount++];
        snprintf(stat->label, sizeof(stat->label), "%s", sources[i].label);
        stat->count = *sources[i].countPtr;
    }
}

void X360Overlay_init(Renderer *renderer) {
    if (gOverlay.initialized) return;

    memset(&gOverlay, 0, sizeof(gOverlay));
    gOverlay.renderer = renderer;
    gOverlay.state = X360_STATS_DISABLED;
#ifdef BS_X360_DEBUG_OVERLAY
    gOverlay.state = X360_STATS_ENABLED;
#endif
    gOverlay.initialized = true;

    logInfo("X360Overlay: initialized (debug overlay %s)\n",
            gOverlay.state == X360_STATS_DISABLED ? "off" : "on");
}

void X360Overlay_deinit(void) {
    if (!gOverlay.initialized) return;
    memset(&gOverlay, 0, sizeof(gOverlay));
}

static const char *overlayStateName(X360OverlayState state) {
    switch (state) {
        case X360_STATS_ENABLED: return "stats";
        case X360_STATS_ENABLED_WITH_PROFILER: return "stats+profiler";
        case X360_STATS_DISABLED: return "off";
        default: return "?";
    }
}

X360OverlayState X360Overlay_getState(void) {
    if (!gOverlay.initialized) return X360_STATS_DISABLED;
    return gOverlay.state;
}

void X360Overlay_setState(X360OverlayState state, Runner *runner) {
    if (!gOverlay.initialized) return;
    gOverlay.state = state;

#ifdef ENABLE_VM_GML_PROFILER
    if (runner != NULL) {
        Profiler_setEnabled(&runner->vmContext->profiler,
                            state == X360_STATS_ENABLED_WITH_PROFILER);
    }
    gOverlay.profilerFramesInWindow = 0;
    gOverlay.profilerOverlayText[0] = '\0';
#else
    (void) runner;
#endif
}

void X360Overlay_toggle(Runner *runner) {
    if (!gOverlay.initialized) return;
    X360OverlayState next = (X360OverlayState) ((gOverlay.state + 1) % X360_STATS_MAX);
    X360Overlay_setState(next, runner);
    logInfo("X360Overlay: %s\n", overlayStateName(next));
}

void *X360Overlay_getCallbackData(void) {
    return &gOverlay;
}

void X360Overlay_statusScreenCallback(const char *chunkName, int chunkIndex,
                                      int totalChunks, DataWin *dataWin, void *userData) {
    (void) userData;
    if (!gOverlay.initialized || gOverlay.renderer == NULL) return;

    Renderer *renderer = gOverlay.renderer;
    const char *gameName = (dataWin != NULL && dataWin->gen8.displayName != NULL)
                           ? dataWin->gen8.displayName : NULL;

    beginStatusScreen(renderer, gameName);

    // Progress bar (drawRectangle adds +1 to the right/bottom edges for GML
    // semantics, so the end coordinates are decremented by one).
    const float barX = 240.0f;
    const float barY = 470.0f;
    const float barW = 800.0f;
    const float barH = 36.0f;

    renderer->vtable->drawRectangleColor(renderer, barX, barY,
                                         barX + barW - 1.0f, barY + barH - 1.0f,
                                         X360_COLOR_BAR_BG, X360_COLOR_BAR_BG,
                                         X360_COLOR_BAR_BG, X360_COLOR_BAR_BG,
                                         1.0f, false);

    float progress = totalChunks > 0
                     ? (float) (chunkIndex + 1) / (float) totalChunks : 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    float fillW = barW * progress;
    if (fillW >= 1.0f) {
        renderer->vtable->drawRectangleColor(renderer, barX, barY,
                                             barX + fillW - 1.0f, barY + barH - 1.0f,
                                             X360_COLOR_BAR_FILL, X360_COLOR_BAR_FILL,
                                             X360_COLOR_BAR_FILL, X360_COLOR_BAR_FILL,
                                             1.0f, false);
    }

    char percentText[16];
    snprintf(percentText, sizeof(percentText), "%d%%", (int) (progress * 100.0f));
    overlayPrint(renderer, 640.0f, barY + 5.0f, 0.7f, X360_COLOR_WHITE, 1.0f, 1, percentText);

    char statusText[64];
    snprintf(statusText, sizeof(statusText), "Loading %.4s... (%d/%d)",
             chunkName != NULL ? chunkName : "????", chunkIndex + 1, totalChunks);
    overlayPrint(renderer, 640.0f, 520.0f, 0.9f, X360_COLOR_WHITE, 1.0f, 1, statusText);

    uint32_t usedMB = 0, totalMB = 0;
    overlayMemory(&usedMB, &totalMB, NULL);
    char memText[64];
    snprintf(memText, sizeof(memText), "Memory: %u/%u MB", usedMB, totalMB);
    overlayPrint(renderer, 640.0f, 552.0f, 0.7f, X360_COLOR_GRAY, 1.0f, 1, memText);

    recordChunkStats(dataWin);
    drawChunkStats(renderer);

    endStatusScreen(renderer);
}

void X360Overlay_drawStatusScreen(const char *gameName, const char *statusText,
                                  bool includeChunkStats) {
    if (!gOverlay.initialized || gOverlay.renderer == NULL) return;

    Renderer *renderer = gOverlay.renderer;
    beginStatusScreen(renderer, gameName);
    if (statusText != NULL) {
        overlayPrint(renderer, 640.0f, 460.0f, 1.0f, X360_COLOR_GRAY, 1.0f, 1, statusText);
    }
    if (includeChunkStats) drawChunkStats(renderer);
    endStatusScreen(renderer);
}

void X360Overlay_drawTextureProgress(const char *gameName, uint32_t current,
                                     uint32_t total, bool skipped) {
    if (!gOverlay.initialized || gOverlay.renderer == NULL) return;

    Renderer *renderer = gOverlay.renderer;
    beginStatusScreen(renderer, gameName);

    const float barX = 240.0f;
    const float barY = 470.0f;
    const float barW = 800.0f;
    const float barH = 36.0f;

    renderer->vtable->drawRectangleColor(renderer, barX, barY,
                                         barX + barW - 1.0f, barY + barH - 1.0f,
                                         X360_COLOR_BAR_BG, X360_COLOR_BAR_BG,
                                         X360_COLOR_BAR_BG, X360_COLOR_BAR_BG,
                                         1.0f, false);

    float progress = total > 0 ? (float) current / (float) total : 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    float fillW = barW * progress;
    if (fillW >= 1.0f) {
        renderer->vtable->drawRectangleColor(renderer, barX, barY,
                                             barX + fillW - 1.0f, barY + barH - 1.0f,
                                             X360_COLOR_BAR_FILL, X360_COLOR_BAR_FILL,
                                             X360_COLOR_BAR_FILL, X360_COLOR_BAR_FILL,
                                             1.0f, false);
    }

    char percentText[16];
    snprintf(percentText, sizeof(percentText), "%d%%", (int) (progress * 100.0f));
    overlayPrint(renderer, 640.0f, barY + 5.0f, 0.7f, X360_COLOR_WHITE, 1.0f, 1, percentText);

    char statusText[96];
    if (skipped) {
        snprintf(statusText, sizeof(statusText), "Skipping texture compression...");
    } else {
        snprintf(statusText, sizeof(statusText), "Compressing textures... (%u/%u)",
                 current, total);
    }
    overlayPrint(renderer, 640.0f, 520.0f, 0.9f, X360_COLOR_WHITE, 1.0f, 1, statusText);
    overlayPrint(renderer, 640.0f, 584.0f, 0.7f, X360_COLOR_GRAY, 1.0f, 1,
                 "Hold START to skip");

    uint32_t usedMB = 0, totalMB = 0;
    overlayMemory(&usedMB, &totalMB, NULL);
    char memText[64];
    snprintf(memText, sizeof(memText), "Memory: %u/%u MB", usedMB, totalMB);
    overlayPrint(renderer, 640.0f, 552.0f, 0.7f, X360_COLOR_GRAY, 1.0f, 1, memText);

    drawChunkStats(renderer);
    endStatusScreen(renderer);
}

void X360Overlay_drawErrorScreen(const char *gameName, const char *message) {
    if (!gOverlay.initialized || gOverlay.renderer == NULL) return;

    Renderer *renderer = gOverlay.renderer;
    beginStatusScreen(renderer, gameName);
    if (message != NULL) {
        overlayPrint(renderer, 640.0f, 460.0f, 1.0f, X360_COLOR_RED, 1.0f, 1, message);
    }
    drawChunkStats(renderer);
    endStatusScreen(renderer);
}

// ===[ Debug overlay ]===

void X360Overlay_drawDebugOverlay(Runner *runner, float tickMs, float stepMs,
                                  float drawMs, float audioMs) {
    if (!gOverlay.initialized || gOverlay.renderer == NULL) return;
    if (gOverlay.state == X360_STATS_DISABLED || runner == NULL) return;

    Renderer *renderer = gOverlay.renderer;
    const char *roomName = (runner->currentRoom != NULL && runner->currentRoom->name != NULL)
                           ? runner->currentRoom->name : "?";
    uint32_t roomSpeed = runner->currentRoom != NULL ? runner->currentRoom->speed : 0;

    X360RendererStats stats;
    X360Renderer_getStats(renderer, &stats);

    uint32_t freeMB = 0;
    overlayMemory(NULL, NULL, &freeMB);

    char debugText[768];
    snprintf(debugText, sizeof(debugText),
             "Room: %s\n"
             "Tick: %.2fms\n"
             "Step: %.2fms\n"
             "Draw: %.2fms\n"
             "Audio: %.2fms\n"
             "Free: %u MB (approx)\n"
             "Room Speed: %u\n"
             "Textures: %u/%u\n"
             "Surfaces: %u/%u  RTs: %u (evict %u)\n"
             "Instances: %d\n"
             "Structs: %d",
             roomName, (double) tickMs, (double) stepMs, (double) drawMs,
             (double) audioMs, freeMB, roomSpeed,
             stats.texturePagesLoaded, stats.texturePagesTotal,
             stats.surfacesAllocated, stats.surfacesTotal,
             stats.renderTargetsCached, stats.renderTargetEvictions,
             (int) arrlen(runner->instances), (int) arrlen(runner->structInstances));

    X360Renderer_beginOverlay(renderer);
    overlayPrintOutlined(renderer, 20.0f, 20.0f, 0.7f, X360_COLOR_WHITE, 1.0f, 0, debugText);

    if (gOverlay.state == X360_STATS_ENABLED_WITH_PROFILER) {
        const float lineHeight = (float) DEBUGFONT_LINE_HEIGHT * 0.7f;
        float profilerY = 20.0f + lineHeight * 11.0f + 10.0f;

#ifdef ENABLE_VM_GML_PROFILER
        gOverlay.profilerFramesInWindow++;
        if (gOverlay.profilerFramesInWindow >= X360_OVERLAY_PROFILER_WINDOW_FRAMES) {
            char *report = Profiler_createReport(runner->vmContext->profiler, 25,
                                                 gOverlay.profilerFramesInWindow);
            if (report != NULL) {
                snprintf(gOverlay.profilerOverlayText, sizeof(gOverlay.profilerOverlayText),
                         "%s", report);
                free(report);
            }
            Profiler_reset(runner->vmContext->profiler);
            gOverlay.profilerFramesInWindow = 0;
        }
        const char *profilerDisplay = gOverlay.profilerOverlayText[0] != '\0'
                                      ? gOverlay.profilerOverlayText
                                      : "GML Profiler (collecting...)";
        overlayPrintOutlined(renderer, 20.0f, profilerY, 0.5f, X360_COLOR_WHITE, 1.0f, 0,
                             profilerDisplay);
#else
        overlayPrintOutlined(renderer, 20.0f, profilerY, 0.5f, X360_COLOR_WHITE, 1.0f, 0,
                             "Butterscotch GML Profiler is disabled on this build :(");
#endif
    }

    X360Renderer_endOverlay(renderer);
}
