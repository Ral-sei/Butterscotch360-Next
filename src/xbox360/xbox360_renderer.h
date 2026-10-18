#ifndef BS_XBOX360_RENDERER_H
#define BS_XBOX360_RENDERER_H

#include "renderer.h"

// Back buffer / presentation size. On Xbox 360 HD modes the frame buffer must
// be at least 1280x720 or the XDK raises a certification warning, so that is
// the default. The port's main loop uses the same macros.
#ifndef BS_X360_RENDER_WIDTH
#define BS_X360_RENDER_WIDTH 1280
#endif
#ifndef BS_X360_RENDER_HEIGHT
#define BS_X360_RENDER_HEIGHT 720
#endif

// Port-owned D3D9 renderer. It implements the public RendererVtable with
// precompiled Xenon shaders (see src/xbox360/shaders/) and the EDRAM resolve
// rules described in docs/xbox360-renderer-design.md.
//
// Created like any other renderer: Renderer* r = X360Renderer_create();
// then Renderer_init(renderer, dataWin) (its init vtable entry creates the
// D3D9 device). The port's main loop must call X360Renderer_present() once per
// frame *after* the Draw GUI pass; the vtable has no Present entry.
Renderer *X360Renderer_create(void);

// Device-only initialization, split out of the vtable init so the port can draw
// the boot/loading screen before data.win has been parsed. It is idempotent:
// the later Renderer_init(renderer, dataWin) call (from Runner_create) keeps the
// same device and only finishes the data-dependent setup (texture page tables,
// user-shader sidecar, sampler filter).
void X360Renderer_initDevice(Renderer *renderer);

// Begin/end a port-owned overlay pass drawn directly onto the back buffer.
//
// X360Renderer_beginOverlay() resolves/binds the back buffer, sets a full-screen
// 0,0..backBufferW,H orthographic projection, forces the built-in shaders and
// enables alpha blending, so overlay text/rectangles are unaffected by whatever
// game shader or render state is active. X360Renderer_endOverlay() flushes and
// restores the game shader/blend/alpha-test/colour-write/sampler state.
//
// Contract: the overlay pass must be the last drawing before
// X360Renderer_present(); endOverlay leaves the back buffer bound.
void X360Renderer_beginOverlay(Renderer *renderer);
void X360Renderer_endOverlay(Renderer *renderer);

// Port-specific counters for the debug overlay.
typedef struct {
    uint32_t texturePagesLoaded;
    uint32_t texturePagesTotal;
    uint32_t surfacesAllocated;
    uint32_t surfacesTotal;
    uint32_t renderTargetsCached;
    uint32_t renderTargetEvictions;
} X360RendererStats;

void X360Renderer_getStats(Renderer *renderer, X360RendererStats *out);

// Presents the back buffer (IDirect3DDevice9_Present). Safe to call when the
// device failed to initialize.
void X360Renderer_present(Renderer *renderer);

// Hardware verification aid for the offline user-shader precompile path. Only
// compiled with -DBS_X360_SHADER_SELFTEST; it needs the test sidecar deployed as
// data.x360shaders next to data.win. Logs PASS/FAIL over KDNet.
#ifdef BS_X360_SHADER_SELFTEST
bool X360Renderer_runShaderSelftest(Renderer *renderer);
#endif

// Progress callback for first-boot texture sidecar generation. Called with
// current=0 before the first page and after each page. Return true to skip the
// remaining pages; the sidecar is then left incomplete for a later boot.
typedef bool (*X360TextureProgressFn)(void *user, uint32_t current, uint32_t total);

// Generates a complete data.x360tex when the loaded sidecar is missing or
// incomplete. Pages are decoded/compressed one at a time and released again, so
// the full page set is never resident at once. Returns true when generation ran
// (including when the callback skipped it).
bool X360Renderer_generateTextureSidecar(Renderer *renderer,
                                         X360TextureProgressFn progress,
                                         void *user);

#endif /* BS_XBOX360_RENDERER_H */
