#ifndef _BS_X360_OVERLAY_H_
#define _BS_X360_OVERLAY_H_

#include <stdbool.h>

#include "renderer.h"
#include "data_win.h"
#include "runner.h"

// Port-owned loading/status screen and debug stats overlay, modelled on the
// PS2 port's PS2Overlay.
//
// It is not part of the public RendererVtable: it draws through the renderer's
// built-in shader and embedded debug font (drawTextUI / drawRectangle /
// clearScreen), and presents with X360Renderer_present().
//
// The loading screen covers the data.win parse. The port therefore calls
// X360Renderer_initDevice() before DataWin_parse and installs
// X360Overlay_statusScreenCallback as the parser progress callback.
//
// The debug overlay is toggled at runtime with LB+RB (consumed by
// X360Input_consumeOverlayToggle; it is not injected into the game's key state).

typedef enum {
    X360_STATS_ENABLED = 0,
    X360_STATS_ENABLED_WITH_PROFILER = 1,
    X360_STATS_DISABLED = 2,
    X360_STATS_MAX
} X360OverlayState;

// Call once after X360Renderer_initDevice() and before data.win is parsed.
void X360Overlay_init(Renderer *renderer);
void X360Overlay_deinit(void);

X360OverlayState X360Overlay_getState(void);
void X360Overlay_setState(X360OverlayState state, Runner *runner);
void X360Overlay_toggle(Runner *runner);

// DataWinParserOptions.progressCallbackUserData.
void *X360Overlay_getCallbackData(void);
void X360Overlay_statusScreenCallback(const char *chunkName, int chunkIndex,
                                      int totalChunks, DataWin *dataWin, void *userData);

// Full-screen status screen (title, optional game name, status text, optional
// chunk statistics, credits). Presents when done.
void X360Overlay_drawStatusScreen(const char *gameName, const char *statusText,
                                  bool includeChunkStats);

// Texture sidecar generation progress. Draws the loading screen with a
// "Compressing textures" bar and a START-to-skip hint, then presents.
// current/total are page counts; skipped shows the skip message.
void X360Overlay_drawTextureProgress(const char *gameName, uint32_t current,
                                     uint32_t total, bool skipped);

// Error variant: same layout with a red message. The caller decides whether to
// hold or exit.
void X360Overlay_drawErrorScreen(const char *gameName, const char *message);

// Draws the stats overlay over the already-rendered back buffer. Call after
// Runner_drawGUI() and immediately before X360Renderer_present(); it does not
// present by itself.
void X360Overlay_drawDebugOverlay(Runner *runner, float tickMs, float stepMs,
                                  float drawMs, float audioMs);

#endif /* _BS_X360_OVERLAY_H_ */
