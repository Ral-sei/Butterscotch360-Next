// Xbox 360 D3D9 RendererVtable implementation (port-owned).
//
// This is the real renderer behind docs/xbox360-renderer-design.md: it maps the
// public RendererVtable (src/renderer.h) onto Xenon Direct3D 9 + XGraphics.
//
// Enforced Xenon constraints (see docs/xbox360-renderer-design.md section 2):
//   * No fixed-function pipeline: every draw goes through the precompiled
//     vertex/pixel shaders in src/xbox360/shaders/.
//   * Textures are tiled; upload/readback go through XGTileTextureLevel /
//     XGUntileTextureLevel, never row*pitch+col.
//   * A NULL texture must never be sampled: untextured draws bind a 1x1 white
//     texture.
//   * Render targets cannot be locked and cannot be textured directly. Every
//     GML surface owns an EDRAM render target plus a separate, lockable
//     resolve texture that Resolve() fills; sampling a surface samples that
//     texture.
//   * EDRAM content is not guaranteed to survive Present, so surface render
//     targets are restored from their resolve texture when re-bound in a later
//     frame.
//   * Present takes no arguments and synchronizes internally. The port's main
//     loop calls X360Renderer_present() after the GUI pass.
//   * Runtime HLSL compilation does not exist on Xenon. Game shaders
//     (shdr/*.hlsl9_*) are precompiled offline (scripts/xbox360/precompile_shaders.py)
//     into a sidecar ("data.x360shaders") that x360Init loads through
//     src/xbox360/xbox360_shadersets.c. A shader that has no sidecar entry -
//     a title with no precompile step, a GMS 2.3+ title whose hlsl9 slot is
//     declarations-only, or a sidecar that failed to build - is reported as not
//     compiled and gpuSetShader falls back to the built-in shader (loudly).
//
// Port-owned. No public source is modified.

#include "xbox360_renderer.h"

#include "renderer.h"
#include "runner.h"
#include "data_win.h"
#include "text_utils.h"
#include "image_decoder.h"
#include "file_system.h"
#include "matrix_math.h"
#include "utils.h"
#include "log.h"
#include "debug_font/debug_font.h"
#include "xbox360_d3d_device.h"
#include "xbox360_shadersets.h"
#include "xbox360_textures.h"

#include <xtl.h>
#include <d3d9.h>
#include <xgraphics.h>
#include <string.h>
#include "string_compat.h"
#include "math_compat.h"
#include "stdio_compat.h"

#include "renderer_vs.h"
#include "renderer_ps.h"

#define X360_MAX_QUADS    4096
#define X360_MAX_VERTICES (X360_MAX_QUADS * 4)
#define X360_MAX_INDICES  (X360_MAX_QUADS * 6)

// Worker threads used for first-boot texture generation. 1 = sequential.
#ifndef BS_X360_TEXTURE_WORKERS
#define BS_X360_TEXTURE_WORKERS 2
#endif

// The sidecar writer is needed by both the partial dump build and the
// first-boot autogeneration build.
#if defined(BS_X360_DUMP_TEXTURES) || defined(BS_X360_TEXTURE_AUTOGEN)
#define X360_TEX_DUMP_ENABLED 1
#endif

// Texture-handle flag distinguishing a surface texture (surface_get_texture)
// from a sprite handle (tpagIndex + 1). Matches the desktop GL encoding.
#define X360_SURFACE_TEXTURE_FLAG 0x40000000u

typedef struct {
    float position[4];
    float color[4];
    float texcoord[2];
} X360Vertex;

typedef enum {
    X360_BATCH_QUADS = 0,
    X360_BATCH_TRIANGLES,
} X360BatchType;

typedef struct {
    int32_t width;
    int32_t height;
    bool allocated;
    D3DSurface *rt;        // EDRAM render target (not lockable, not sampleable)
    D3DTexture *tex;       // lockable/tiled resolve target; the sampleable copy
    bool texValid;         // tex holds valid pixels (never lost across Present)
    bool rtStale;          // EDRAM content may have been lost; restore from tex on bind
    uint64_t lastUsed;     // LRU clock for the cached EDRAM RT
    bool tiling;           // rt is a tile-sized predicated-tiling RT
    bool tilingActive;     // BeginTiling issued, EndTiling not yet
    int32_t tileW;
    int32_t tileH;
    uint32_t tileCount;
    D3DRECT tileRects[15];
} X360Surface;

typedef struct {
    Renderer base;         // must stay first

    Xbox360D3DDevice gfx;
    D3DSurface *backBuffer;
    D3DVertexDeclaration *declaration;
    D3DVertexShader *vertexShader;
    D3DPixelShader *pixelShader;
    D3DTexture *whiteTexture;

    int32_t backbufferW;
    int32_t backbufferH;
    int32_t gameW;
    int32_t gameH;
    int32_t windowW;
    int32_t windowH;

    int32_t targetW;       // current render target dimensions
    int32_t targetH;

    // Conservative world-space AABB of the visible area for the projection
    // currently uploaded. Derived from the WVP and used to skip quads/tiles
    // that cannot contribute pixels. Disabled when the WVP is not a 2D affine
    // transform (3D/perspective or singular), where an AABB test is not valid.
    bool cullValid;
    float cullMinX;
    float cullMinY;
    float cullMaxX;
    float cullMaxY;

    // Per-frame draw statistics, only collected with BS_X360_RENDER_STATS.
    uint32_t statFrame;
    uint32_t statDrawCalls;
    uint32_t statQuads;
    uint32_t statCulledQuads;
    uint32_t statTiledCells;

    // Texture pages
    D3DTexture **pageTextures;
    int32_t *pageWidths;
    int32_t *pageHeights;
    bool *pageLoaded;
    X360TextureFormat *pageFormats;
    uint32_t textureCount;
    uint32_t originalTexturePageCount;
    uint32_t originalTpagCount;
    uint32_t originalSpriteCount;

    // Optional precompressed texture sidecar (may be NULL).
    X360TextureSet *textureSet;
    X360TextureDump *textureDump;

    // GML surfaces
    X360Surface *surfaces;
    uint32_t surfaceCount;
    int32_t currentSurface;   // -1 == the back buffer
    uint64_t rtClock;         // LRU clock for cached EDRAM render targets
    uint32_t rtCacheCount;
    uint32_t rtEvictions;

    // Batched geometry
    X360Vertex *vertices;
    uint16_t *indices;
    int32_t batchVertexCount;
    int32_t batchIndexCount;
    X360BatchType batchType;
    D3DTexture *batchTexture;

    // primitive_* state
    int32_t primitiveType;
    D3DTexture *primitiveTexture;
    int32_t primitiveVertexCount;

    // Render state shadow
    bool blendEnable;
    bool separateAlphaBlend;
    int32_t blendMode;
    int32_t sFactor;
    int32_t dFactor;
    int32_t sFactorAlpha;
    int32_t dFactorAlpha;
    bool alphaTestEnable;
    float alphaTestRef;
    bool colorWriteR;
    bool colorWriteG;
    bool colorWriteB;
    bool colorWriteA;
    bool fogEnable;
    uint32_t fogColor;
    bool scissorEnabled;

    // drawTextUI embedded font
    Font debugUIFont;
    FontGlyph debugUIGlyphs[DEBUGFONT_GLYPH_COUNT];
    TexturePageItem debugUITpag;
    bool debugUIInited;
    D3DTexture *debugUITexture;

    // Game shader precompile sidecar (may be NULL: no precompiled shaders).
    X360ShaderSet *shaderSet;
    X360UserShader *activeShader;   // NULL == the built-in renderer shader

    bool *shaderWarned;

    // Set once the data-dependent half of init has run. The port initializes
    // the device before data.win is parsed (for the loading screen), so the
    // vtable init that Runner_create performs must not redo the device setup.
    bool dataInitialized;
} X360Renderer;

// CPU-side result of decoding/compressing one TXTR page. The caller owns data:
// DXT blocks for DXT1/DXT5, or 0xAARRGGBB words for the RGBA8 fallback.
typedef struct {
    X360TextureFormat format;
    int32_t width;
    int32_t height;
    uint32_t rowPitch;
    uint8_t *data;
    uint32_t dataSize;
} X360PagePayload;

// The port's DataWin loader and XGCompressSurface are not documented as
// reentrant. Decoding different pages is independent, but lazy-load file access
// and the compressor are serialized by these locks. The D3D device is never
// touched here: only the device-owning thread may call it.
static CRITICAL_SECTION s_textureDecodeLock;
static CRITICAL_SECTION s_textureCompressLock;
static bool s_textureLocksInitialized;

static void x360EnsureTextureLocks(void) {
    if (!s_textureLocksInitialized) {
        InitializeCriticalSection(&s_textureDecodeLock);
        InitializeCriticalSection(&s_textureCompressLock);
        s_textureLocksInitialized = true;
    }
}

// Defined with the GPU state section.
static D3DBLENDOP x360BlendOp(int32_t mode);
static D3DBLEND x360BlendFactor(int32_t factor);

// Defined with batching / surfaces respectively.
static void x360Flush(X360Renderer *r);
static void x360UnbindTextures(X360Renderer *r);
static void x360ResolveCurrentSurface(X360Renderer *r);

// ===[ Small helpers ]===

static inline X360Renderer *x360(Renderer *renderer) {
    return (X360Renderer *) renderer;
}

// Mirrors the desktop GL letterbox math (src/gl_common/gl_common.c) so the
// application surface is placed identically inside the window.
static void x360ComputeLetterbox(int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH,
                                 int32_t *outStartX, int32_t *outStartY, int32_t *outEndX,
                                 int32_t *outEndY) {
    int32_t effW, effH;
    if (gameW <= 0 || gameH <= 0 || windowW <= 0 || windowH <= 0) {
        *outStartX = 0; *outStartY = 0; *outEndX = windowW; *outEndY = windowH;
        return;
    }
    if ((gameW * windowH) / gameH < windowW) {
        effW = (gameW * windowH) / gameH;
        effH = windowH;
    } else {
        effW = windowW;
        effH = (gameH * windowW) / gameW;
    }
    int32_t startX = (windowW - effW) / 2;
    int32_t startY = (windowH - effH) / 2;
    *outStartX = startX;
    *outStartY = startY;
    *outEndX = startX + effW;
    *outEndY = startY + effH;
}

static inline D3DDevice *x360Device(X360Renderer *r) {
    return r->gfx.device;
}

static inline DWORD x360Color(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return ((DWORD) a << 24) | ((DWORD) r << 16) | ((DWORD) g << 8) | (DWORD) b;
}

static void x360SetViewport(X360Renderer *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > r->targetW) x = r->targetW;
    if (y > r->targetH) y = r->targetH;
    if (x + w > r->targetW) w = r->targetW - x;
    if (y + h > r->targetH) h = r->targetH - y;

    if (r->currentSurface >= 0) {
        X360Surface *s = &r->surfaces[r->currentSurface];
        if (s->tiling && s->tilingActive) {
            // D3D controls the per-tile viewport inside a tiling bracket.
            r->base.CPortX = x;
            r->base.CPortY = y;
            r->base.CPortW = w;
            r->base.CPortH = h;
            return;
        }
    }

    D3DVIEWPORT9 vp;
    memset(&vp, 0, sizeof(vp));
    vp.X = (DWORD) x;
    vp.Y = (DWORD) y;
    vp.Width = (DWORD) w;
    vp.Height = (DWORD) h;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    IDirect3DDevice9_SetViewport(x360Device(r), &vp);

    // SetViewport crops to render target 0; keep the shadow consistent with it.
    r->base.CPortX = x;
    r->base.CPortY = y;
    r->base.CPortW = w;
    r->base.CPortH = h;
}

static void x360SetScissor(X360Renderer *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > r->targetW) x = r->targetW;
    if (y > r->targetH) y = r->targetH;
    if (x + w > r->targetW) w = r->targetW - x;
    if (y + h > r->targetH) h = r->targetH - y;

    if (r->currentSurface >= 0) {
        X360Surface *s = &r->surfaces[r->currentSurface];
        if (s->tiling && s->tilingActive) {
            // Scissor changes inside a tiling bracket would clip every tile;
            // keep the shadow only. Tiled surfaces are expected to be drawn
            // with scissor disabled.
            r->scissorEnabled = true;
            return;
        }
    }

    RECT rect;
    rect.left = (LONG) x;
    rect.top = (LONG) y;
    rect.right = (LONG) (x + w);
    rect.bottom = (LONG) (y + h);
    IDirect3DDevice9_SetScissorRect(x360Device(r), &rect);
    IDirect3DDevice9_SetRenderState(x360Device(r), D3DRS_SCISSORTESTENABLE, TRUE);
    r->scissorEnabled = true;
}

static void x360DisableScissor(X360Renderer *r) {
    IDirect3DDevice9_SetRenderState(x360Device(r), D3DRS_SCISSORTESTENABLE, FALSE);
    r->scissorEnabled = false;
}

// Recomputes the visible world-space AABB from the projection that is about to
// be uploaded. The desktop GL backend uses the same inverse-WVP test to clip
// tiled sprites; doing it for every quad also removes the room-sized tile load
// that the public runner submits (it draws every non-empty tile in the room,
// not just the visible ones).
//
// Only 2D affine projections are handled: a perspective or singular WVP cannot
// be reduced to an axis-aligned world-space rectangle, so culling is disabled
// and every quad is kept.
static void x360UpdateCullBounds(X360Renderer *r, const Matrix4f *wvp) {
    Matrix4f inv;
    if (wvp == NULL || !Matrix4f_isAffine2D(wvp) || !Matrix4f_inverse(&inv, wvp)) {
        r->cullValid = false;
        return;
    }

    static const float ndcCorners[4][2] = {
        { -1.0f, -1.0f }, { 1.0f, -1.0f }, { -1.0f, 1.0f }, { 1.0f, 1.0f },
    };
    float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
    for (int i = 0; 4 > i; i++) {
        float x, y;
        Matrix4f_transformPoint(&inv, ndcCorners[i][0], ndcCorners[i][1], &x, &y);
        if (i == 0 || x < minX) minX = x;
        if (i == 0 || x > maxX) maxX = x;
        if (i == 0 || y < minY) minY = y;
        if (i == 0 || y > maxY) maxY = y;
    }

    // One texel of slack for pixel-centre rounding and the half-pixel offset;
    // culling is an optimization, never a clip.
    r->cullMinX = minX - 2.0f;
    r->cullMinY = minY - 2.0f;
    r->cullMaxX = maxX + 2.0f;
    r->cullMaxY = maxY + 2.0f;
    r->cullValid = true;
}

static void x360SetProjection(X360Renderer *r, const Matrix4f *wvp) {
    IDirect3DDevice9_SetVertexShaderConstantF(x360Device(r), 0, wvp->m, 4);
    x360UpdateCullBounds(r, wvp);
}

// Row/column-major swap of a column-major Matrix4f.
static void x360TransposeMatrix(float out[16], const Matrix4f *in) {
    for (int c = 0; 4 > c; c++) {
        for (int row = 0; 4 > row; row++) {
            out[c * 4 + row] = in->m[row * 4 + c];
        }
    }
}

// Uploads renderer->gmlMatrices to a game shader's `gm_Matrices` register block.
//
// GameMaker's generated HLSL9 reads the block as `mul(transpose(gm_Matrices[i]),
// v)`, i.e. it transposes the stored matrix before the vector multiply, so the
// runtime uploads the transpose when the source asked for one (flag recorded by
// the offline precompile tool). Shaders that read the block without a transpose
// get the matrices verbatim. See docs/xbox360-renderer-design.md step 4.3.
static void x360UploadGameMatrices(X360Renderer *r, X360UserShader *shader, uint32_t regIndex,
                                   uint32_t regCount) {
    float buffer[MATRICES_MAX * 16];
    if ((shader->flags & X360_SHADER_FLAG_MATRICES_TRANSPOSED) != 0) {
        for (int i = 0; MATRICES_MAX > i; i++) {
            x360TransposeMatrix(buffer + i * 16, &r->base.gmlMatrices[i]);
        }
    } else {
        memcpy(buffer, r->base.gmlMatrices, sizeof(buffer));
    }
    // fxc reports the register span the shader actually allocated for
    // gm_Matrices. Upload only that span (clamped to our array) so a shader
    // that declares/uses fewer matrices cannot clobber the constants that
    // follow the block.
    uint32_t registers = regCount;
    if (registers == 0 || registers > MATRICES_MAX * 4) registers = MATRICES_MAX * 4;
    IDirect3DDevice9_SetVertexShaderConstantF(x360Device(r), regIndex, buffer, registers);
}

// Finds a built-in uniform by name and returns its index, or -1.
static int32_t x360FindBuiltin(X360UserShader *shader, const char *name) {
    int32_t idx = X360UserShader_findUniform(shader, name);
    if (idx < 0) return -1;
    if (shader->uniforms[idx].regClass != X360_UNIFORM_REG_CONSTANT) return -1;
    return idx;
}

static void x360UploadCurrentProjection(X360Renderer *r) {
    X360UserShader *shader = r->activeShader;
    if (shader != NULL) {
        int32_t idx = x360FindBuiltin(shader, "gm_Matrices");
        if (idx >= 0) {
            x360UploadGameMatrices(r, shader, shader->uniforms[idx].regIndex,
                                   shader->uniforms[idx].regCount);
            // The built-in path updates the cull bounds inside x360SetProjection;
            // the user-shader path uploads the matrix block itself, so update
            // them here from the same WVP.
            x360UpdateCullBounds(r, &r->base.gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION]);
            return;
        }
    }
    x360SetProjection(r, &r->base.gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION]);
}

static void x360UploadPixelShaderConstants(X360Renderer *r) {
    float fog[4] = {
        (float) BGR_R(r->fogColor) / 255.0f,
        (float) BGR_G(r->fogColor) / 255.0f,
        (float) BGR_B(r->fogColor) / 255.0f,
        r->fogEnable ? 1.0f : 0.0f,
    };

    X360UserShader *shader = r->activeShader;
    if (shader == NULL) {
        IDirect3DDevice9_SetPixelShaderConstantF(x360Device(r), 0, fog, 1);
        return;
    }

    // GameMaker's HLSL9 pixel shader prelude takes the fog colour as a float4
    // and the fog enable as a separate bool (the GL prelude uses a different
    // shape, so these do not map one-to-one onto the built-in uFogColor). The
    // alpha-test uniforms the prelude also declares are stripped from the
    // source (Xenon fxc rejects `discard`), so they never reach the register
    // table; alpha test is a render state instead (x360ApplyAlphaTest).
    D3DDevice *dev = x360Device(r);

    int32_t idx = x360FindBuiltin(shader, "gm_FogColour");
    if (idx >= 0) {
        float colour[4] = { fog[0], fog[1], fog[2], 1.0f };
        IDirect3DDevice9_SetPixelShaderConstantF(dev, shader->uniforms[idx].regIndex, colour, 1);
    }
    idx = x360FindBuiltin(shader, "gm_PS_FogEnabled");
    if (idx >= 0) {
        float enabled = r->fogEnable ? 1.0f : 0.0f;
        IDirect3DDevice9_SetPixelShaderConstantF(dev, shader->uniforms[idx].regIndex, &enabled, 1);
    }
}

// True when a sidecar shader can actually be used: both halves must have been
// compiled. A half-only shader would otherwise bind a vertex shader whose
// varyings do not match the built-in pixel shader (or vice versa).
static bool x360ShaderUsable(X360UserShader *shader) {
    return shader != NULL
        && (shader->flags & X360_SHADER_FLAG_HAS_VS) != 0
        && (shader->flags & X360_SHADER_FLAG_HAS_PS) != 0
        && shader->vs != NULL && shader->ps != NULL;
}

// Binds the device's VS/PS to the active shader (game or built-in) and uploads
// its per-frame uniforms.
static void x360BindActiveShaders(X360Renderer *r) {
    D3DDevice *dev = x360Device(r);
    X360UserShader *shader = x360ShaderUsable(r->activeShader) ? r->activeShader : NULL;
    if (shader != NULL) {
        IDirect3DDevice9_SetVertexShader(dev, shader->vs);
        IDirect3DDevice9_SetPixelShader(dev, shader->ps);
    } else {
        IDirect3DDevice9_SetVertexShader(dev, r->vertexShader);
        IDirect3DDevice9_SetPixelShader(dev, r->pixelShader);
    }
    x360UploadCurrentProjection(r);
    x360UploadPixelShaderConstants(r);
}

// The Xenon microcode compiler does not implement 'discard', so alpha testing
// is a D3D render state on the shader output. GM discards when c.a <= ref, so
// the comparison is GREATER.
static void x360ApplyAlphaTest(X360Renderer *r) {
    D3DDevice *dev = x360Device(r);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, r->alphaTestEnable ? TRUE : FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHAFUNC, D3DCMP_GREATER);
    float ref = r->alphaTestRef;
    if (ref < 0.0f) ref = 0.0f;
    if (ref > 1.0f) ref = 1.0f;
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHAREF, (DWORD) (ref * 255.0f));
}

static void x360ApplySamplerFilter(X360Renderer *r) {
    D3DTEXTUREFILTERTYPE filter = r->base.texFilter ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    D3DDevice *dev = x360Device(r);
    // Sampler state is per stage. Apply the renderer-wide filter to every stage
    // so user shaders that sample s1..s7 (palettes, masks) behave like the GL
    // backend too, not just the stage-0 path used by built-in draws.
    for (int stage = 0; stage < MAX_TEXTURE_STAGES; stage++) {
        IDirect3DDevice9_SetSamplerState(dev, stage, D3DSAMP_MINFILTER, filter);
        IDirect3DDevice9_SetSamplerState(dev, stage, D3DSAMP_MAGFILTER, filter);
        IDirect3DDevice9_SetSamplerState(dev, stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice9_SetSamplerState(dev, stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(dev, stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
}

static void x360ApplyBlendState(X360Renderer *r) {
    D3DDevice *dev = x360Device(r);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, r->blendEnable ? TRUE : FALSE);
    // gpu_set_blendmode_ext can set separate alpha factors. Keep that mode when
    // this function is used to restore state after an internal draw; otherwise
    // every application-surface composite silently reset it to the colour
    // factors (and the game's additive/fade alpha broke on the next frame).
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SEPARATEALPHABLENDENABLE,
                                    r->separateAlphaBlend ? TRUE : FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_BLENDOP, x360BlendOp(r->blendMode));
    IDirect3DDevice9_SetRenderState(dev, D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, x360BlendFactor(r->sFactor));
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, x360BlendFactor(r->dFactor));
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLENDALPHA, x360BlendFactor(r->sFactorAlpha));
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLENDALPHA, x360BlendFactor(r->dFactorAlpha));
}

static void x360ApplyColorWrite(X360Renderer *r) {
    DWORD mask = 0;
    if (r->colorWriteR) mask |= D3DCOLORWRITEENABLE_RED;
    if (r->colorWriteG) mask |= D3DCOLORWRITEENABLE_GREEN;
    if (r->colorWriteB) mask |= D3DCOLORWRITEENABLE_BLUE;
    if (r->colorWriteA) mask |= D3DCOLORWRITEENABLE_ALPHA;
    IDirect3DDevice9_SetRenderState(x360Device(r), D3DRS_COLORWRITEENABLE, mask);
}

// ===[ Tiled textures ]===

// The linear source/destination of XGTile/XGUntileTextureLevel for a 32bpp
// texture is one 32-bit word per texel holding 0xAARRGGBB (verified on hardware
// by the D3D9 probe: a word 0x800000FF tiled as A8R8G8B8 samples as 50% blue).
// ImageDecoder output is RGBA bytes, so convert in place.
static void x360RgbaBytesToArgbWords(uint8_t *pixels, size_t texelCount) {
    for (size_t i = 0; i < texelCount; i++) {
        uint8_t *p = pixels + i * 4;
        uint8_t rr = p[0], gg = p[1], bb = p[2], aa = p[3];
        p[0] = aa; p[1] = rr; p[2] = gg; p[3] = bb;
    }
}

static void x360ArgbWordsToRgbaBytes(uint8_t *pixels, size_t texelCount) {
    for (size_t i = 0; i < texelCount; i++) {
        uint8_t *p = pixels + i * 4;
        uint8_t aa = p[0], rr = p[1], gg = p[2], bb = p[3];
        p[0] = rr; p[1] = gg; p[2] = bb; p[3] = aa;
    }
}

static DWORD x360TileFlags(D3DBaseTexture *texture) {
    DWORD flags = 0;
    if (!XGIsPackedTexture(texture)) flags |= XGTILE_NONPACKED;
    if (XGIsBorderTexture(texture)) flags |= XGTILE_BORDER;
    return flags;
}

// Internal full-surface draws (RT restore, application-surface composition,
// surfaceCopy, pixel-upload redraw) must not inherit the game's alpha test,
// blend, fog or colour-write state, or the surface pixels would be discarded or
// tinted by whatever the game last set.
static void x360BeginInternalDraw(X360Renderer *r) {
    x360Flush(r);
    D3DDevice *dev = x360Device(r);
    // Internal draws use the built-in shader (its declarations and its c0
    // fog/projection registers); a game shader that happens to be active is
    // restored by x360EndInternalDraw.
    IDirect3DDevice9_SetVertexShader(dev, r->vertexShader);
    IDirect3DDevice9_SetPixelShader(dev, r->pixelShader);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE,
                                    D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN
                                    | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    float noFog[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, noFog, 1);
}

static void x360EndInternalDraw(X360Renderer *r) {
    x360Flush(r);
    x360ApplyBlendState(r);
    x360ApplyAlphaTest(r);
    x360ApplyColorWrite(r);
    // Restore the game shader (or the built-in one) and its uniforms.
    x360BindActiveShaders(r);
}

static D3DFORMAT x360TextureD3DFormat(X360TextureFormat format) {
    switch (format) {
        case X360_TEXFMT_DXT1: return D3DFMT_DXT1;
        case X360_TEXFMT_DXT5: return D3DFMT_DXT5;
        default: return D3DFMT_A8R8G8B8;
    }
}

// Row pitch in bytes between rows of blocks (DXT) or rows of texels (RGBA).
static uint32_t x360TextureRowPitch(X360TextureFormat format, uint32_t width) {
    switch (format) {
        case X360_TEXFMT_DXT1: return (width / 4u) * 8u;
        case X360_TEXFMT_DXT5: return (width / 4u) * 16u;
        default: return width * 4u;
    }
}

static uint32_t x360TextureDataSize(X360TextureFormat format, uint32_t width, uint32_t height) {
    uint32_t pitch = x360TextureRowPitch(format, width);
    return pitch * (format == X360_TEXFMT_RGBA8 ? height : (height / 4u));
}

static const char *x360TextureFormatName(X360TextureFormat format) {
    switch (format) {
        case X360_TEXFMT_DXT1: return "DXT1";
        case X360_TEXFMT_DXT5: return "DXT5";
        default: return "RGBA8";
    }
}

static bool x360CreateTextureFormat(X360Renderer *r, int32_t width, int32_t height,
                                    D3DFORMAT format, D3DTexture **out) {
    D3DTexture *texture = NULL;
    if (width <= 0 || height <= 0) return false;
    if (FAILED(IDirect3DDevice9_CreateTexture(x360Device(r), (UINT) width, (UINT) height, 1,
                                              0, format, D3DPOOL_DEFAULT,
                                              &texture, NULL)) || !texture) {
        return false;
    }
    *out = texture;
    return true;
}

static bool x360CreateTexture(X360Renderer *r, int32_t width, int32_t height, D3DTexture **out) {
    return x360CreateTextureFormat(r, width, height, D3DFMT_A8R8G8B8, out);
}

// Uploads a linear level (RGBA texels or DXT blocks) into a tiled texture.
static bool x360UploadLevelFormat(D3DTexture *texture, int32_t width, int32_t height,
                                  D3DFORMAT format, const void *linear, uint32_t rowPitch) {
    D3DLOCKED_RECT locked;
    if (FAILED(IDirect3DTexture9_LockRect(texture, 0, &locked, NULL, 0))) return false;
    XGTileTextureLevel((UINT) width, (UINT) height, 0,
                       XGGetGpuFormat(format), x360TileFlags((D3DBaseTexture *) texture),
                       locked.pBits, NULL, linear, rowPitch, NULL);
    IDirect3DTexture9_UnlockRect(texture, 0);
    return true;
}

static bool x360UploadLevel(D3DTexture *texture, int32_t width, int32_t height,
                            const void *linear, uint32_t rowPitch) {
    return x360UploadLevelFormat(texture, width, height, D3DFMT_A8R8G8B8, linear, rowPitch);
}

// ===[ Texture pages ]===

static bool x360PageHasAlpha(const uint8_t *rgba, size_t texelCount) {
    for (size_t i = 0; i < texelCount; i++) {
        if (rgba[i * 4 + 3] != 255) return true;
    }
    return false;
}

// GameMaker texture pages often store fully transparent texels with a chroma
// colour (this title uses pure green). DXT colour blocks include those texels,
// so a blue sprite next to a green background gets a green fringe after
// compression. Replace the RGB of fully transparent texels with the average
// visible colour of their 4x4 block; empty blocks next to a non-empty block
// inherit the neighbouring average so bilinear sampling cannot pull green in.
// Alpha is never modified. Returns the number of texels whose RGB changed.
static uint32_t x360BleedTransparentBlockColors(uint8_t *rgba, int32_t width,
                                                int32_t height) {
    if (rgba == NULL || width < 4 || height < 4
        || (width % 4) != 0 || (height % 4) != 0) {
        return 0;
    }

    int32_t bw = width / 4;
    int32_t bh = height / 4;
    uint8_t *blockColor = (uint8_t *) safeMalloc((size_t) bw * (size_t) bh * 3u);
    uint8_t *blockHasColor = (uint8_t *) safeCalloc((size_t) bw * (size_t) bh, 1u);
    uint32_t changed = 0;

    for (int32_t by = 0; by < bh; by++) {
        for (int32_t bx = 0; bx < bw; bx++) {
            uint32_t sr = 0, sg = 0, sb = 0;
            int32_t count = 0;
            for (int32_t py = 0; py < 4; py++) {
                for (int32_t px = 0; px < 4; px++) {
                    size_t o = ((size_t) (by * 4 + py) * (size_t) width
                                + (size_t) (bx * 4 + px)) * 4u;
                    if (rgba[o + 3] != 0) {
                        sr += rgba[o];
                        sg += rgba[o + 1];
                        sb += rgba[o + 2];
                        count++;
                    }
                }
            }
            if (count > 0) {
                size_t bi = (size_t) by * (size_t) bw + (size_t) bx;
                blockColor[bi * 3 + 0] = (uint8_t) (sr / (uint32_t) count);
                blockColor[bi * 3 + 1] = (uint8_t) (sg / (uint32_t) count);
                blockColor[bi * 3 + 2] = (uint8_t) (sb / (uint32_t) count);
                blockHasColor[bi] = 1;
            }
        }
    }

    // Mixed blocks: transparent texels take the block's visible average.
    for (int32_t by = 0; by < bh; by++) {
        for (int32_t bx = 0; bx < bw; bx++) {
            size_t bi = (size_t) by * (size_t) bw + (size_t) bx;
            if (!blockHasColor[bi]) continue;
            uint8_t cr = blockColor[bi * 3 + 0];
            uint8_t cg = blockColor[bi * 3 + 1];
            uint8_t cb = blockColor[bi * 3 + 2];
            for (int32_t py = 0; py < 4; py++) {
                for (int32_t px = 0; px < 4; px++) {
                    size_t o = ((size_t) (by * 4 + py) * (size_t) width
                                + (size_t) (bx * 4 + px)) * 4u;
                    if (rgba[o + 3] == 0) {
                        if (rgba[o] != cr || rgba[o + 1] != cg || rgba[o + 2] != cb) {
                            changed++;
                        }
                        rgba[o] = cr;
                        rgba[o + 1] = cg;
                        rgba[o + 2] = cb;
                    }
                }
            }
        }
    }

    // Empty blocks adjacent to a non-empty block: use the neighbour average so
    // the 4x4 block does not contribute the original chroma colour.
    for (int32_t by = 0; by < bh; by++) {
        for (int32_t bx = 0; bx < bw; bx++) {
            size_t bi = (size_t) by * (size_t) bw + (size_t) bx;
            if (blockHasColor[bi]) continue;
            uint32_t sr = 0, sg = 0, sb = 0;
            int32_t count = 0;
            for (int32_t dy = -1; dy <= 1; dy++) {
                for (int32_t dx = -1; dx <= 1; dx++) {
                    int32_t nx = bx + dx;
                    int32_t ny = by + dy;
                    if (nx < 0 || nx >= bw || ny < 0 || ny >= bh) continue;
                    size_t ni = (size_t) ny * (size_t) bw + (size_t) nx;
                    if (!blockHasColor[ni]) continue;
                    sr += blockColor[ni * 3 + 0];
                    sg += blockColor[ni * 3 + 1];
                    sb += blockColor[ni * 3 + 2];
                    count++;
                }
            }
            if (count == 0) continue;
            uint8_t cr = (uint8_t) (sr / (uint32_t) count);
            uint8_t cg = (uint8_t) (sg / (uint32_t) count);
            uint8_t cb = (uint8_t) (sb / (uint32_t) count);
            for (int32_t py = 0; py < 4; py++) {
                for (int32_t px = 0; px < 4; px++) {
                    size_t o = ((size_t) (by * 4 + py) * (size_t) width
                                + (size_t) (bx * 4 + px)) * 4u;
                    if (rgba[o + 3] == 0) {
                        if (rgba[o] != cr || rgba[o + 1] != cg || rgba[o + 2] != cb) {
                            changed++;
                        }
                        rgba[o] = cr;
                        rgba[o + 1] = cg;
                        rgba[o + 2] = cb;
                    }
                }
            }
        }
    }

    free(blockHasColor);
    free(blockColor);
    return changed;
}

// Decodes one page and (when possible) compresses it to DXT. No D3D calls: this
// is safe to run on worker threads. The caller frees payload->data.
static bool x360BuildPagePayload(X360Renderer *r, int32_t pageId, X360PagePayload *out) {
    memset(out, 0, sizeof(*out));
    DataWin *dw = r->base.dataWin;
    if (dw == NULL || pageId < 0 || (uint32_t) pageId >= dw->txtr.count) return false;

    x360EnsureTextureLocks();
    Texture *txtr = &dw->txtr.textures[pageId];

    // Lazy-load file access is shared; serialize it. With the port's
    // LOAD_IN_MEMORY_AHEAD_OF_TIME this is normally a no-op.
    EnterCriticalSection(&s_textureDecodeLock);
    DataWin_loadTxtrIfNeeded(dw, (uint32_t) pageId);
    LeaveCriticalSection(&s_textureDecodeLock);

    int w = 0, h = 0;
    bool gm2022_5 = DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0);
    uint8_t *external = NULL;
    int32_t externalSize = 0;
    if (txtr->externalPath != NULL && r->base.runner != NULL) {
        // The port file system is not documented as reentrant either.
        EnterCriticalSection(&s_textureDecodeLock);
        r->base.runner->fileSystem->vtable->readFileBinary(r->base.runner->fileSystem,
                                                          txtr->externalPath, &external, &externalSize);
        LeaveCriticalSection(&s_textureDecodeLock);
    }

    uint8_t *pixels = ImageDecoder_decodeToRgba(
        external ? external : txtr->blobData,
        external ? (size_t) externalSize : (size_t) txtr->blobSize,
        gm2022_5, &w, &h);
    free(external);
    if (pixels == NULL) {
        logWarn("X360Render: failed to decode TXTR page %d\n", pageId);
        return false;
    }

    // Only this job owns this page; freeing the source blob here is safe for
    // distinct pages. A page that later falls back is re-read by the game only
    // when the sidecar lacks it (and the blob is still present on failure).
    if (!txtr->mapped) {
        free(txtr->blobData);
        txtr->blobData = NULL;
    } else if (txtr->blobData && txtr->blobSize) {
        dropMappedRange(txtr->blobData, 0, txtr->blobSize);
    }

    size_t texels = (size_t) w * (size_t) h;
    bool hasAlpha = x360PageHasAlpha(pixels, texels);

    // Remove chroma-coloured transparent texels before either upload path. This
    // is what keeps DXT blocks from bleeding green into blue sprite edges.
    if (hasAlpha) {
        uint32_t bled = x360BleedTransparentBlockColors(pixels, w, h);
        if (bled > 0) {
            logInfo("X360Render: TXTR page %d: bled %u transparent texels for clean edges\n",
                    pageId, bled);
        }
    }

    // XGCompressSurface consumes the same 0xAARRGGBB words the RGBA upload path
    // uses, so convert once and keep the result for either path.
    x360RgbaBytesToArgbWords(pixels, texels);

    out->format = X360_TEXFMT_RGBA8;
    out->width = w;
    out->height = h;
    out->rowPitch = (uint32_t) w * 4u;
    out->data = pixels;
    out->dataSize = (uint32_t) texels * 4u;

    // DXT blocks cover 4x4 texels. GameMaker pages are normally power-of-two
    // and therefore block aligned; anything else keeps the RGBA path.
    if (w >= 4 && h >= 4 && (w % 4) == 0 && (h % 4) == 0) {
        X360TextureFormat dxt = hasAlpha ? X360_TEXFMT_DXT5 : X360_TEXFMT_DXT1;
        D3DFORMAT linearFormat = (dxt == X360_TEXFMT_DXT5) ? D3DFMT_LIN_DXT5 : D3DFMT_LIN_DXT1;
        uint32_t pitch = x360TextureRowPitch(dxt, (uint32_t) w);
        uint32_t size = pitch * ((uint32_t) h / 4u);
        uint8_t *dst = (uint8_t *) safeMalloc(size);

        // XGCompressSurface has no documented thread-safety guarantee; keep it
        // serialized. Decoding still runs in parallel across workers.
        EnterCriticalSection(&s_textureCompressLock);
        HRESULT hr = XGCompressSurface(dst, pitch, (UINT) w, (UINT) h, linearFormat, NULL,
                                       pixels, (UINT) w * 4u, D3DFMT_LIN_A8R8G8B8, NULL,
                                       0, 0.5f);
        LeaveCriticalSection(&s_textureCompressLock);

        if (SUCCEEDED(hr)) {
            out->format = dxt;
            out->rowPitch = pitch;
            out->data = dst;
            out->dataSize = size;
            free(pixels);
        } else {
            free(dst);
            logWarn("X360Render: XGCompressSurface failed for TXTR page %d (hr=0x%08X); using RGBA\n",
                    pageId, (unsigned) hr);
        }
    }

    return true;
}

// Uploads a page from data.x360tex. Returns NULL when the sidecar has no entry
// for this page or the entry cannot be uploaded; the caller then decodes and
// compresses it at runtime.
static D3DTexture *x360LoadPageFromSidecar(X360Renderer *r, int32_t pageId,
                                           int32_t *outW, int32_t *outH) {
    const X360TextureEntry *entry = X360TextureSet_find(r->textureSet, pageId);
    if (entry == NULL || entry->width == 0 || entry->height == 0
        || entry->width > 0x7FFFFFFFu || entry->height > 0x7FFFFFFFu
        || entry->format > (uint8_t) X360_TEXFMT_DXT5) {
        return NULL;
    }

    uint8_t *data = X360TextureSet_readData(r->textureSet, entry);
    if (data == NULL) return NULL;

    X360TextureFormat format = (X360TextureFormat) entry->format;
    D3DFORMAT d3dFormat = x360TextureD3DFormat(format);
    uint32_t rowPitch = x360TextureRowPitch(format, entry->width);
    D3DTexture *texture = NULL;
    bool uploaded = x360CreateTextureFormat(r, (int32_t) entry->width, (int32_t) entry->height,
                                            d3dFormat, &texture)
                 && x360UploadLevelFormat(texture, (int32_t) entry->width,
                                          (int32_t) entry->height, d3dFormat, data, rowPitch);
    free(data);

    if (!uploaded) {
        if (texture != NULL) IDirect3DTexture9_Release(texture);
        logWarn("X360Render: sidecar page %d failed to upload; using runtime path\n", pageId);
        return NULL;
    }

    r->pageWidths[pageId] = (int32_t) entry->width;
    r->pageHeights[pageId] = (int32_t) entry->height;
    r->pageFormats[pageId] = format;
    if (outW) *outW = (int32_t) entry->width;
    if (outH) *outH = (int32_t) entry->height;
    logInfo("X360Render: loaded TXTR page %d from sidecar (%ux%u, %s)\n",
            pageId, entry->width, entry->height, x360TextureFormatName(format));
    return texture;
}

static D3DTexture *x360EnsurePage(X360Renderer *r, int32_t pageId, int32_t *outW, int32_t *outH) {
    if (0 > pageId || (uint32_t) pageId >= r->textureCount) return NULL;
    if (r->pageLoaded[pageId]) {
        if (r->pageWidths[pageId] == 0) return NULL;
        if (outW) *outW = r->pageWidths[pageId];
        if (outH) *outH = r->pageHeights[pageId];
        return r->pageTextures[pageId];
    }
    r->pageLoaded[pageId] = true;

    // Prefer a precompressed page from data.x360tex. Missing pages fall through
    // to the runtime decode/compress path.
    D3DTexture *sidecarTexture = x360LoadPageFromSidecar(r, pageId, outW, outH);
    if (sidecarTexture != NULL) {
        r->pageTextures[pageId] = sidecarTexture;
        return sidecarTexture;
    }

    X360PagePayload payload;
    if (!x360BuildPagePayload(r, pageId, &payload)) return NULL;

    D3DFORMAT d3dFormat = x360TextureD3DFormat(payload.format);
    D3DTexture *texture = NULL;
    bool uploaded = x360CreateTextureFormat(r, payload.width, payload.height, d3dFormat, &texture)
                 && x360UploadLevelFormat(texture, payload.width, payload.height, d3dFormat,
                                          payload.data, payload.rowPitch);
    if (!uploaded) {
        if (texture != NULL) IDirect3DTexture9_Release(texture);
        free(payload.data);
        logWarn("X360Render: failed to upload TXTR page %d\n", pageId);
        return NULL;
    }

#ifdef X360_TEX_DUMP_ENABLED
    if (r->textureDump != NULL && pageId < (int32_t) r->originalTexturePageCount) {
        X360TextureDump_add(r->textureDump, pageId, payload.format,
                            (uint32_t) payload.width, (uint32_t) payload.height,
                            payload.data, payload.dataSize);
    }
#endif

    free(payload.data);

    r->pageTextures[pageId] = texture;
    r->pageWidths[pageId] = payload.width;
    r->pageHeights[pageId] = payload.height;
    r->pageFormats[pageId] = payload.format;
    logInfo("X360Render: loaded TXTR page %d (%dx%d, %s, %u KB)\n",
            pageId, payload.width, payload.height, x360TextureFormatName(payload.format),
            payload.dataSize / 1024u);

    if (outW) *outW = payload.width;
    if (outH) *outH = payload.height;
    return texture;
}

static bool x360ResolveTPAG(X360Renderer *r, int32_t tpagIndex, TexturePageItem **outTpag,
                            D3DTexture **outTex, int32_t *outW, int32_t *outH) {
    DataWin *dw = r->base.dataWin;
    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return false;
    TexturePageItem *tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId) return false;
    D3DTexture *tex = x360EnsurePage(r, pageId, outW, outH);
    if (tex == NULL) return false;
    if (outTpag) *outTpag = tpag;
    if (outTex) *outTex = tex;
    return true;
}

// ===[ Surfaces ]===

static bool x360SurfaceValid(X360Renderer *r, int32_t surfaceId) {
    return 0 <= surfaceId && (uint32_t) surfaceId < r->surfaceCount && r->surfaces[surfaceId].allocated;
}

static bool x360IsApplicationSurface(X360Renderer *r, X360Surface *s) {
    if (r->base.runner == NULL) return false;
    int32_t appId = r->base.runner->applicationSurfaceId;
    return appId >= 0 && (uint32_t) appId < r->surfaceCount && &r->surfaces[appId] == s;
}

// Releases only the EDRAM render target. The resolve texture stays valid and
// the surface is restored from it on the next bind. The caller must have
// unbound the render target (or be releasing a non-current surface).
static void x360ReleaseSurfaceRT(X360Renderer *r, X360Surface *s) {
    if (s->rt != NULL) {
        IDirect3DSurface9_Release(s->rt);
        s->rt = NULL;
        s->rtStale = true;
        if (r->rtCacheCount > 0) r->rtCacheCount--;
    }
}

// Picks the least-recently-used cached RT that can be evicted. The application
// surface RT is excluded unless includeApp is set (last resort).
static X360Surface *x360FindLruRT(X360Renderer *r, X360Surface *skip, bool includeApp) {
    X360Surface *victim = NULL;
    for (uint32_t i = 0; i < r->surfaceCount; i++) {
        X360Surface *other = &r->surfaces[i];
        if (other == skip || other->rt == NULL) continue;
        if (r->currentSurface == (int32_t) i) continue;
        if (!includeApp && x360IsApplicationSurface(r, other)) continue;
        if (victim == NULL || other->lastUsed < victim->lastUsed) victim = other;
    }
    return victim;
}

// Creates an EDRAM RT of the requested size, evicting cached RTs in LRU order
// under pressure. The application surface RT is evicted only as a last resort.
static bool x360CreateRTWithEviction(X360Renderer *r, X360Surface *s,
                                     int32_t width, int32_t height, D3DSurface **out) {
    D3DSurface *rt = NULL;
    HRESULT hr = IDirect3DDevice9_CreateRenderTarget(x360Device(r), (UINT) width,
                                                     (UINT) height, D3DFMT_A8R8G8B8,
                                                     D3DMULTISAMPLE_NONE, 0, FALSE,
                                                     &rt, NULL);
    if (FAILED(hr) || !rt) {
        // Pass 0: evict non-application RTs in LRU order.
        // Pass 1: also evict the application RT (its resolve texture holds the
        // last frame; it is recreated and restored on the next app bind).
        for (int pass = 0; pass < 2 && (FAILED(hr) || !rt); pass++) {
            bool includeApp = (pass == 1);
            for (;;) {
                X360Surface *victim = x360FindLruRT(r, s, includeApp);
                if (victim == NULL) break;
                x360ReleaseSurfaceRT(r, victim);
                r->rtEvictions++;
                rt = NULL;
                hr = IDirect3DDevice9_CreateRenderTarget(x360Device(r), (UINT) width,
                                                         (UINT) height, D3DFMT_A8R8G8B8,
                                                         D3DMULTISAMPLE_NONE, 0, FALSE,
                                                         &rt, NULL);
                if (SUCCEEDED(hr) && rt) break;
            }
        }
    }
    if (FAILED(hr) || !rt) return false;
    *out = rt;
    return true;
}

// Builds a predicated-tiling RT for a surface whose full-size RT does not fit in
// EDRAM. The RT is sized to the largest tile; EndTiling resolves the tiles into
// the full-size resolve texture. Tiles are 32x32-aligned except at the surface
// edges, and the API limit is 15 tiles.
static bool x360SetupTiling(X360Renderer *r, X360Surface *s) {
    if (s->width <= 0 || s->height <= 0) return false;

    const int32_t maxTile = 1024;
    int32_t tileW = s->width < maxTile ? ((s->width + 31) & ~31) : maxTile;
    int32_t tileH = s->height < maxTile ? ((s->height + 31) & ~31) : maxTile;
    if (tileW > s->width) tileW = s->width;
    if (tileH > s->height) tileH = s->height;
    if (tileW <= 0 || tileH <= 0) return false;

    uint32_t cols = (uint32_t) ((s->width + tileW - 1) / tileW);
    uint32_t rows = (uint32_t) ((s->height + tileH - 1) / tileH);
    if (cols == 0 || rows == 0 || cols * rows > 15) {
        logWarn("X360Render: %dx%d needs %ux%u predicated tiles (max 15); cannot tile\n",
                s->width, s->height, cols, rows);
        return false;
    }

    D3DSurface *rt = NULL;
    if (!x360CreateRTWithEviction(r, s, tileW, tileH, &rt)) {
        logWarn("X360Render: tiling RT %dx%d creation failed (EDRAM budget?)\n", tileW, tileH);
        return false;
    }

    s->tiling = true;
    s->tilingActive = false;
    s->tileW = tileW;
    s->tileH = tileH;
    s->tileCount = 0;
    for (uint32_t row = 0; row < rows; row++) {
        for (uint32_t col = 0; col < cols; col++) {
            int32_t x1 = (int32_t) (col * (uint32_t) tileW);
            int32_t y1 = (int32_t) (row * (uint32_t) tileH);
            int32_t x2 = x1 + tileW;
            int32_t y2 = y1 + tileH;
            if (x2 > s->width) x2 = s->width;
            if (y2 > s->height) y2 = s->height;
            D3DRECT *rect = &s->tileRects[s->tileCount++];
            rect->x1 = x1;
            rect->y1 = y1;
            rect->x2 = x2;
            rect->y2 = y2;
        }
    }

    s->rt = rt;
    s->rtStale = true;
    s->lastUsed = ++r->rtClock;
    r->rtCacheCount++;
    logInfo("X360Render: surface %dx%d uses predicated tiling (%dx%d RT, %u tiles)\n",
            s->width, s->height, tileW, tileH, s->tileCount);
    return true;
}

// Creates the EDRAM render target for a surface on demand and caches it. A
// full-size RT is tried first; if it cannot fit in EDRAM even after LRU
// eviction, a predicated-tiling RT is used instead.
static bool x360CreateSurfaceRT(X360Renderer *r, X360Surface *s) {
    D3DSurface *rt = NULL;
    if (x360CreateRTWithEviction(r, s, s->width, s->height, &rt)) {
        s->rt = rt;
        s->rtStale = true;
        s->tiling = false;
        s->tilingActive = false;
        s->tileCount = 0;
        s->lastUsed = ++r->rtClock;
        r->rtCacheCount++;
        return true;
    }

    if (x360SetupTiling(r, s)) return true;

    logError("X360Render: CreateRenderTarget(%dx%d) failed after eviction/tiling (EDRAM budget?)\n",
             s->width, s->height);
    return false;
}

static bool x360CreateSurfaceResources(X360Renderer *r, X360Surface *s, int32_t width, int32_t height) {
    // Only the main-memory resolve texture is allocated here. The EDRAM render
    // target is created on first bind and released when another surface takes
    // over, so many live GML surfaces no longer exhaust the 10 MB EDRAM.
    D3DTexture *tex = NULL;
    if (!x360CreateTexture(r, width, height, &tex)) {
        logError("X360Render: surface texture (%dx%d) creation failed\n", width, height);
        return false;
    }

    // Define initial content: transparent black.
    size_t texels = (size_t) width * (size_t) height;
    uint8_t *zero = (uint8_t *) safeCalloc(texels, 4);
    if (zero != NULL) {
        x360UploadLevel(tex, width, height, zero, (uint32_t) width * 4);
        free(zero);
    }

    s->rt = NULL;
    s->tex = tex;
    s->width = width;
    s->height = height;
    s->allocated = true;
    s->texValid = true;
    s->rtStale = true;
    return true;
}

static void x360ReleaseSurfaceResources(X360Renderer *r, X360Surface *s) {
    bool wasCurrent = (r->currentSurface >= 0 && &r->surfaces[r->currentSurface] == s);
    if (wasCurrent) {
        // End any active predicated-tiling bracket and resolve first.
        x360ResolveCurrentSurface(r);
    }
    x360Flush(r);
    if (wasCurrent) {
        // Never leave the device pointing at a surface we are about to release.
        IDirect3DDevice9_SetRenderTarget(x360Device(r), 0, r->backBuffer);
        r->currentSurface = -1;
        r->targetW = r->backbufferW;
        r->targetH = r->backbufferH;
    }
    if (s->tex != NULL) {
        x360UnbindTextures(r);
        IDirect3DTexture9_Release(s->tex);
        s->tex = NULL;
    }
    x360ReleaseSurfaceRT(r, s);
    s->allocated = false;
    s->texValid = false;
    s->rtStale = true;
}

static uint32_t x360AllocSurfaceSlot(X360Renderer *r) {
    uint32_t index = r->surfaceCount++;
    r->surfaces = (X360Surface *) safeRealloc(r->surfaces, r->surfaceCount * sizeof(X360Surface));
    memset(&r->surfaces[index], 0, sizeof(X360Surface));
    return index;
}

static int32_t x360CreateSurface(Renderer *renderer, int32_t width, int32_t height);

static int32_t x360EnsureApplicationSurface(Renderer *renderer, int32_t width, int32_t height) {
    X360Renderer *r = x360(renderer);
    int32_t id = renderer->runner->applicationSurfaceId;
    if (!x360SurfaceValid(r, id)) {
        id = x360CreateSurface(renderer, width, height);
        renderer->runner->applicationSurfaceId = id;
        return id;
    }
    if (r->surfaces[id].width != width || r->surfaces[id].height != height) {
        renderer->vtable->surfaceResize(renderer, id, width, height);
    }
    return id;
}

// ===[ Batched geometry ]===

static void x360Flush(X360Renderer *r);

static void x360SetBatchTexture(X360Renderer *r, D3DTexture *texture, X360BatchType type) {
    if (r->batchVertexCount > 0 && (r->batchType != type || r->batchTexture != texture)) {
        x360Flush(r);
    }
    r->batchType = type;
    r->batchTexture = texture;
}

static void x360EmitVertex(X360Renderer *r, float x, float y, float z,
                           float cr, float cg, float cb, float ca, float u, float v) {
    X360Vertex *vertex = &r->vertices[r->batchVertexCount++];
    vertex->position[0] = x;
    vertex->position[1] = y;
    vertex->position[2] = z;
    vertex->position[3] = 1.0f;
    vertex->color[0] = cr;
    vertex->color[1] = cg;
    vertex->color[2] = cb;
    vertex->color[3] = ca;
    vertex->texcoord[0] = u;
    vertex->texcoord[1] = v;
}

static void x360EmitIndex(X360Renderer *r, uint16_t index) {
    r->indices[r->batchIndexCount++] = index;
}

static void x360EmitQuad(X360Renderer *r, D3DTexture *texture,
                         float x0, float y0, float x1, float y1,
                         float x2, float y2, float x3, float y3,
                         float u0, float v0, float u1, float v1,
                         uint8_t r0, uint8_t g0, uint8_t b0,
                         uint8_t r1, uint8_t g1, uint8_t b1,
                         uint8_t r2, uint8_t g2, uint8_t b2,
                         uint8_t r3, uint8_t g3, uint8_t b3,
                         float alpha) {
    // CPU-side visible-area cull. The runner submits every non-empty room tile
    // (a 5000x2320 room with 40px tiles is ~12k quads) plus every instance, so
    // without this test the vertex batch carries the whole room every frame.
    if (r->cullValid) {
        float minX = x0, maxX = x0, minY = y0, maxY = y0;
        if (x1 < minX) minX = x1; else if (x1 > maxX) maxX = x1;
        if (x2 < minX) minX = x2; else if (x2 > maxX) maxX = x2;
        if (x3 < minX) minX = x3; else if (x3 > maxX) maxX = x3;
        if (y1 < minY) minY = y1; else if (y1 > maxY) maxY = y1;
        if (y2 < minY) minY = y2; else if (y2 > maxY) maxY = y2;
        if (y3 < minY) minY = y3; else if (y3 > maxY) maxY = y3;
        if (maxX < r->cullMinX || minX > r->cullMaxX
            || maxY < r->cullMinY || minY > r->cullMaxY) {
#ifdef BS_X360_RENDER_STATS
            r->statCulledQuads++;
#endif
            return;
        }
    }

    if (r->batchVertexCount + 4 > X360_MAX_VERTICES || r->batchIndexCount + 6 > X360_MAX_INDICES) {
        x360Flush(r);
    }
    x360SetBatchTexture(r, texture, X360_BATCH_QUADS);

#ifdef BS_X360_RENDER_STATS
    r->statQuads++;
#endif

    // The colour channels arrive as bytes. Multiplying by the reciprocal is
    // much cheaper than twelve float divisions per quad on Xenon, and the
    // room-tile path emits thousands of quads per frame.
    const float inv255 = 1.0f / 255.0f;
    uint16_t base = (uint16_t) r->batchVertexCount;
    float ca = alpha;
    x360EmitVertex(r, x0, y0, 0.0f, (float) r0 * inv255, (float) g0 * inv255, (float) b0 * inv255, ca, u0, v0);
    x360EmitVertex(r, x1, y1, 0.0f, (float) r1 * inv255, (float) g1 * inv255, (float) b1 * inv255, ca, u1, v0);
    x360EmitVertex(r, x2, y2, 0.0f, (float) r2 * inv255, (float) g2 * inv255, (float) b2 * inv255, ca, u1, v1);
    x360EmitVertex(r, x3, y3, 0.0f, (float) r3 * inv255, (float) g3 * inv255, (float) b3 * inv255, ca, u0, v1);
    x360EmitIndex(r, base + 0);
    x360EmitIndex(r, base + 1);
    x360EmitIndex(r, base + 2);
    x360EmitIndex(r, base + 2);
    x360EmitIndex(r, base + 3);
    x360EmitIndex(r, base + 0);
}

static void x360EmitQuadFlat(X360Renderer *r, D3DTexture *texture,
                             float x0, float y0, float x1, float y1,
                             float x2, float y2, float x3, float y3,
                             float u0, float v0, float u1, float v1,
                             uint32_t color, float alpha) {
    uint8_t rr = (uint8_t) BGR_R(color), gg = (uint8_t) BGR_G(color), bb = (uint8_t) BGR_B(color);
    x360EmitQuad(r, texture, x0, y0, x1, y1, x2, y2, x3, y3, u0, v0, u1, v1,
                 rr, gg, bb, rr, gg, bb, rr, gg, bb, rr, gg, bb, alpha);
}

// Starts a predicated-tiling bracket for the current surface if one is pending.
// Called immediately before the first draw/clear, when the caller has already
// set the viewport and projection for the full surface.
static void x360BeginTilingIfNeeded(X360Renderer *r) {
    if (r->currentSurface < 0) return;
    X360Surface *s = &r->surfaces[r->currentSurface];
    if (!s->tiling || s->tilingActive || s->tileCount == 0) return;

    // D3D controls the per-tile viewport/clip rectangle; a stale scissor would
    // clip every tile to the last game scissor.
    IDirect3DDevice9_SetRenderState(x360Device(r), D3DRS_SCISSORTESTENABLE, FALSE);
    IDirect3DDevice9_BeginTiling(x360Device(r), 0, s->tileCount, s->tileRects, NULL, 1.0f, 0);
    s->tilingActive = true;
}

static void x360Flush(X360Renderer *r) {
    if (r->batchVertexCount == 0 || r->gfx.device == NULL) {
        r->batchVertexCount = 0;
        r->batchIndexCount = 0;
        return;
    }
    x360BeginTilingIfNeeded(r);
#ifdef BS_X360_RENDER_STATS
    r->statDrawCalls++;
#endif
    D3DTexture *texture = r->batchTexture != NULL ? r->batchTexture : r->whiteTexture;
    IDirect3DDevice9_SetTexture(x360Device(r), 0, (D3DBaseTexture *) texture);
    IDirect3DDevice9_DrawIndexedPrimitiveUP(x360Device(r), D3DPT_TRIANGLELIST, 0,
                                           (UINT) r->batchVertexCount,
                                           (UINT) (r->batchIndexCount / 3),
                                           r->indices, D3DFMT_INDEX16,
                                           r->vertices, sizeof(X360Vertex));
    r->batchVertexCount = 0;
    r->batchIndexCount = 0;
}

// ===[ Target binding ]===

static void x360ResolveCurrentSurface(X360Renderer *r) {
    if (r->currentSurface < 0) return;
    X360Surface *s = &r->surfaces[r->currentSurface];
    if (!s->allocated || s->tex == NULL || s->rt == NULL) return;

    // Capture any pending geometry for this target before copying it out.
    x360Flush(r);

    if (s->tiling) {
        if (s->tilingActive) {
            x360UnbindTextures(r);
            HRESULT hr = IDirect3DDevice9_EndTiling(x360Device(r), D3DRESOLVE_RENDERTARGET0,
                                                    NULL, (D3DBaseTexture *) s->tex,
                                                    NULL, 1.0f, 0, NULL);
            s->tilingActive = false;
            if (SUCCEEDED(hr)) {
                s->texValid = true;
            } else {
                logWarn("X360Render: EndTiling(%dx%d) failed (hr=0x%08X)\n",
                        s->width, s->height, (unsigned) hr);
                s->texValid = false;
            }
            IDirect3DDevice9_SetRenderState(x360Device(r), D3DRS_SCISSORTESTENABLE,
                                            r->scissorEnabled ? TRUE : FALSE);
        }
        return;
    }

    // Resolve copies the *current* render target; the destination texture must
    // not be bound to the device.
    IDirect3DDevice9_SetTexture(x360Device(r), 0, NULL);
    r->batchTexture = NULL;
    IDirect3DDevice9_Resolve(x360Device(r), D3DRESOLVE_RENDERTARGET0, NULL,
                             (D3DBaseTexture *) s->tex, NULL, 0, 0, NULL, 1.0f, 0, NULL);
    s->texValid = true;
}

// Draws s->tex over the surface's render target (used to restore EDRAM content
// that Present may have discarded). Leaves the projection/state as the caller
// expects by restoring the batch projection afterwards.
static void x360RestoreSurface(X360Renderer *r, X360Surface *s) {
    // The restore is a full-surface blit. Set the viewport/scissor explicitly:
    // D3D resets the viewport on SetRenderTarget, but the scissor state is
    // sticky, so a surface bound while a game view is active could otherwise
    // restore only the part inside the old view rect.
    x360SetViewport(r, 0, 0, s->width, s->height);
    x360DisableScissor(r);

    Matrix4f ortho;
    Matrix4f_viewProjection(&ortho, 0.0f, 0.0f, (float) s->width, (float) s->height, 0.0f);
    x360SetProjection(r, &ortho);

    x360BeginInternalDraw(r);
    x360EmitQuadFlat(r, s->tex, 0.0f, 0.0f, (float) s->width, 0.0f,
                     (float) s->width, (float) s->height, 0.0f, (float) s->height,
                     0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFF, 1.0f);
    x360EndInternalDraw(r);

    x360UploadCurrentProjection(r);
}

static bool x360BindSurface(X360Renderer *r, int32_t surfaceId) {
    if (!x360SurfaceValid(r, surfaceId)) return false;
    X360Surface *s = &r->surfaces[surfaceId];
    if (r->currentSurface == surfaceId && s->rt != NULL) return true;

    x360Flush(r);
    x360ResolveCurrentSurface(r);

    // Unbind the previous target so its EDRAM render target can be released.
    // The application surface RT is kept: it is rebound every frame, and its
    // resolve texture still lets us recreate it if EDRAM is tight.
    if (r->currentSurface >= 0) {
        IDirect3DDevice9_SetRenderTarget(x360Device(r), 0, r->backBuffer);
        r->currentSurface = -1;
        r->targetW = r->backbufferW;
        r->targetH = r->backbufferH;
        // Keep the old RT cached; x360CreateSurfaceRT evicts LRU RTs only if
        // the new allocation does not fit in EDRAM.
    }

    if (s->rt == NULL) {
        if (!x360CreateSurfaceRT(r, s)) {
            // The previous target is already unbound; leave the back buffer as
            // the active target and report failure to the caller.
            return false;
        }
    }

    IDirect3DDevice9_SetRenderTarget(x360Device(r), 0, s->rt);
    r->targetW = s->width;
    r->targetH = s->height;
    r->currentSurface = surfaceId;
    s->lastUsed = ++r->rtClock;

    if (s->rtStale) {
        if (s->tiling) {
            // Predicated tiling starts with BeginTiling (which clears each
            // tile) on the first draw/clear. Prior resolve content is not
            // restored into the tile-sized RT.
        } else if (s->texValid) {
            x360RestoreSurface(r, s);
        } else {
            IDirect3DDevice9_Clear(x360Device(r), 0, NULL, D3DCLEAR_TARGET,
                                   x360Color(0, 0, 0, 0), 1.0f, 0);
        }
        s->rtStale = false;
    }
    return true;
}

static void x360BindBackbuffer(X360Renderer *r) {
    if (r->currentSurface == -1 && r->targetW == r->backbufferW) return;
    x360Flush(r);
    x360ResolveCurrentSurface(r);

    if (r->currentSurface >= 0) {
        IDirect3DDevice9_SetRenderTarget(x360Device(r), 0, r->backBuffer);
        r->currentSurface = -1;
        r->targetW = r->backbufferW;
        r->targetH = r->backbufferH;
        // Keep the surface RT cached for its next bind.
    } else {
        IDirect3DDevice9_SetRenderTarget(x360Device(r), 0, r->backBuffer);
        r->targetW = r->backbufferW;
        r->targetH = r->backbufferH;
    }
}

// ===[ Frame lifecycle ]===

static void x360BeginFrame(Renderer *renderer, int32_t gameW, int32_t gameH,
                           int32_t windowW, int32_t windowH) {
    X360Renderer *r = x360(renderer);

#ifdef BS_X360_RENDER_STATS
    // Averages over the last 300 frames: enough to show the tile load and how
    // many quads the visible-area cull removes without flooding KDNet.
    r->statFrame++;
    if (r->statFrame >= 300) {
        float inv = 1.0f / (float) r->statFrame;
        logInfo("X360Render: stats draws=%.1f quads=%.1f culled=%.1f tiled=%.1f per frame\n",
                (double) ((float) r->statDrawCalls * inv),
                (double) ((float) r->statQuads * inv),
                (double) ((float) r->statCulledQuads * inv),
                (double) ((float) r->statTiledCells * inv));
        r->statFrame = 0;
        r->statDrawCalls = 0;
        r->statQuads = 0;
        r->statCulledQuads = 0;
        r->statTiledCells = 0;
    }
#endif

    r->gameW = gameW;
    r->gameH = gameH;
    r->windowW = windowW;
    r->windowH = windowH;

    int32_t appId = renderer->runner->applicationSurfaceId;
    if (x360BindSurface(r, appId)) {
        x360SetViewport(r, 0, 0, gameW, gameH);
        x360SetScissor(r, 0, 0, gameW, gameH);
    } else {
        x360BindBackbuffer(r);
        x360SetViewport(r, 0, 0, windowW, windowH);
        x360DisableScissor(r);
    }
}

static void x360EndFrameInit(Renderer *renderer) {
    (void) renderer;
    // Application-surface pass ends here; draws are immediate.
}

// Composites the application surface onto the back buffer, letterboxed into the
// window. Matches the desktop GL endFrameEnd, which is the step that makes the
// render pass visible before Post Draw / Draw GUI.
static void x360EndFrameEnd(Renderer *renderer) {
    X360Renderer *r = x360(renderer);
    Runner *runner = renderer->runner;

    if (!(runner->usingAppSurface && runner->appSurfaceAutoDraw)) return;

    int32_t appId = runner->applicationSurfaceId;
    if (!x360SurfaceValid(r, appId)) return;

    x360Flush(r);
    if (r->currentSurface == appId) {
        x360ResolveCurrentSurface(r);
    }
    X360Surface *app = &r->surfaces[appId];
    if (app->tex == NULL) return;

    x360BindBackbuffer(r);

    // EDRAM content does not survive Present, so the back buffer may hold
    // arbitrary pixels here. Clear it black first (the desktop GL path does the
    // same) so nothing stale shows in the letterbox bars.
    IDirect3DDevice9_Clear(x360Device(r), 0, NULL, D3DCLEAR_TARGET, x360Color(255, 0, 0, 0), 1.0f, 0);

    int32_t sx, sy, ex, ey;
    x360ComputeLetterbox(r->gameW, r->gameH, r->windowW, r->windowH, &sx, &sy, &ex, &ey);
    x360SetViewport(r, 0, 0, r->windowW, r->windowH);
    x360DisableScissor(r);

    Matrix4f ortho;
    Matrix4f_viewProjection(&ortho, 0.0f, 0.0f, (float) r->windowW, (float) r->windowH, 0.0f);
    x360SetProjection(r, &ortho);

    x360BeginInternalDraw(r);
    x360EmitQuadFlat(r, app->tex, (float) sx, (float) sy, (float) ex, (float) sy,
                     (float) ex, (float) ey, (float) sx, (float) ey,
                     0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFF, 1.0f);
    x360EndInternalDraw(r);
    x360UploadCurrentProjection(r);
}

static void x360FlushRenderer(Renderer *renderer) {
    x360Flush(x360(renderer));
}

static void x360ClearScreen(Renderer *renderer, uint32_t color, float alpha) {
    X360Renderer *r = x360(renderer);
    x360Flush(r);
    x360BeginTilingIfNeeded(r);
    uint8_t a = (uint8_t) (alpha <= 0.0f ? 0 : (alpha >= 1.0f ? 255 : (int) (alpha * 255.0f + 0.5f)));
    IDirect3DDevice9_Clear(x360Device(r), 0, NULL, D3DCLEAR_TARGET,
                           x360Color(a, (uint8_t) BGR_R(color), (uint8_t) BGR_G(color),
                                     (uint8_t) BGR_B(color)), 1.0f, 0);
}

// ===[ Views and projections ]===

static void x360ApplyProjection(Renderer *renderer, const Matrix4f *viewMatrix,
                                const Matrix4f *projectionMatrix) {
    X360Renderer *r = x360(renderer);
    x360Flush(r);
    Renderer_applyProjection(renderer, viewMatrix, projectionMatrix);
    x360UploadCurrentProjection(r);
}

static void x360BeginView(Renderer *renderer, int32_t viewX, int32_t viewY, int32_t viewW,
                          int32_t viewH, int32_t portX, int32_t portY, int32_t portW,
                          int32_t portH, float viewAngle) {
    X360Renderer *r = x360(renderer);
    (void) viewX; (void) viewY; (void) viewW; (void) viewH; (void) viewAngle;
    x360SetViewport(r, portX, portY, portW, portH);
    x360SetScissor(r, portX, portY, portW, portH);

    int32_t viewCurrent = 0;
    if (renderer->runner->viewsEnabled) {
        viewCurrent = renderer->runner->viewCurrent;
    }
    RuntimeView *view = &renderer->runner->views[viewCurrent];
    renderer->cameraCurrent = view->cameraId;
    GMLCamera *camera = Runner_getCameraById(renderer->runner, renderer->cameraCurrent);
    if (camera != NULL) {
        x360ApplyProjection(renderer, &camera->viewMatrix, &camera->projectionMatrix);
    }
}

static void x360EndView(Renderer *renderer) {
    x360Flush(x360(renderer));
    x360DisableScissor(x360(renderer));
}

static void x360SetMatrix(Renderer *renderer, int32_t matrixType, Matrix4f matrix) {
    if (memcmp(&renderer->gmlMatrices[matrixType], &matrix, sizeof(Matrix4f)) == 0) return;

    renderer->gmlMatrices[matrixType] = matrix;
    Matrix4f world = renderer->gmlMatrices[MATRIX_WORLD];
    Matrix4f view = renderer->gmlMatrices[MATRIX_VIEW];
    Matrix4f projection = renderer->gmlMatrices[MATRIX_PROJECTION];

    Matrix4f worldView;
    Matrix4f_multiply(&worldView, &view, &world);
    Matrix4f worldViewProjection;
    Matrix4f_multiply(&worldViewProjection, &projection, &worldView);

    renderer->gmlMatrices[MATRIX_WORLD_VIEW] = worldView;
    renderer->gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION] = worldViewProjection;
    x360Flush(x360(renderer));
    x360UploadCurrentProjection(x360(renderer));
}

// ===[ GUI ]===

// GUI projection for D3D: GM GUI coordinates are (0,0) top-left. Unlike the GL
// path we never flip the clip Y (D3D render targets are already top-down), and
// user surfaces need no extra flip.
static void x360BuildGuiProjection(X360Renderer *r, int32_t guiW, int32_t guiH) {
    Matrix4f projection;
    Matrix4f_viewProjection(&projection, 0.0f, 0.0f, (float) guiW, (float) guiH, 0.0f);
    Matrix4f view;
    Matrix4f_identity(&view);
    Renderer_applyProjection(&r->base, &view, &projection);
    x360UploadCurrentProjection(r);
}

static void x360BeginGUI(Renderer *renderer, int32_t guiW, int32_t guiH,
                         int32_t portX, int32_t portY, int32_t portW, int32_t portH,
                         int32_t targetSurfaceId) {
    X360Renderer *r = x360(renderer);
    x360Flush(r);

    if (targetSurfaceId == RENDER_TARGET_HOST_FRAMEBUFFER) {
        x360BindBackbuffer(r);
        int32_t sx, sy, ex, ey;
        x360ComputeLetterbox(guiW, guiH, portW, portH, &sx, &sy, &ex, &ey);
        x360SetViewport(r, sx, sy, ex - sx, ey - sy);
        x360SetScissor(r, sx, sy, ex - sx, ey - sy);
    } else {
        if (!x360BindSurface(r, targetSurfaceId)) return;
        x360SetViewport(r, portX, portY, portW, portH);
        x360SetScissor(r, portX, portY, portW, portH);
    }

    renderer->cameraCurrent = GUI_CAMERA;
    GMLCamera *camera = &renderer->runner->guiCamera;
    camera->allocated = true;
    camera->viewX = 0.0;
    camera->viewY = 0.0;
    camera->viewWidth = guiW;
    camera->viewHeight = guiH;
    camera->borderX = 0;
    camera->borderY = 0;
    camera->speedX = 0;
    camera->speedY = 0;
    camera->objectId = -1;
    camera->viewAngle = 0;

    x360BuildGuiProjection(r, guiW, guiH);
}

static void x360SetGuiProjection(Renderer *renderer, int32_t guiW, int32_t guiH,
                                 int32_t portW, int32_t portH, bool renderingToUserSurface) {
    X360Renderer *r = x360(renderer);
    (void) portW; (void) portH; (void) renderingToUserSurface;
    x360Flush(r);
    renderer->cameraCurrent = GUI_CAMERA;
    GMLCamera *camera = &renderer->runner->guiCamera;
    camera->allocated = true;
    camera->viewX = 0.0;
    camera->viewY = 0.0;
    camera->viewWidth = guiW;
    camera->viewHeight = guiH;
    camera->borderX = 0;
    camera->borderY = 0;
    camera->speedX = 0;
    camera->speedY = 0;
    camera->objectId = -1;
    camera->viewAngle = 0;
    x360BuildGuiProjection(r, guiW, guiH);
}

static void x360EndGUI(Renderer *renderer) {
    X360Renderer *r = x360(renderer);
    x360Flush(r);
    x360DisableScissor(r);
}

// ===[ Sprites ]===

static void x360DrawSprite(Renderer *renderer, int32_t tpagIndex, float x, float y,
                           float originX, float originY, float xscale, float yscale,
                           float angleDeg, uint32_t color, float alpha) {
    X360Renderer *r = x360(renderer);
    TexturePageItem *tpag;
    D3DTexture *tex;
    int32_t texW, texH;
    if (!x360ResolveTPAG(r, tpagIndex, &tpag, &tex, &texW, &texH)) return;

    float u0 = (float) tpag->sourceX / (float) texW;
    float v0 = (float) tpag->sourceY / (float) texH;
    float u1 = (float) (tpag->sourceX + tpag->sourceWidth) / (float) texW;
    float v1 = (float) (tpag->sourceY + tpag->sourceHeight) / (float) texH;

    float localX0 = (float) tpag->targetX - originX;
    float localY0 = (float) tpag->targetY - originY;
    float localX1 = localX0 + (float) tpag->targetWidth;
    float localY1 = localY0 + (float) tpag->targetHeight;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

    float cx0, cy0, cx1, cy1, cx2, cy2, cx3, cy3;
    Matrix4f_transformPoint(&transform, localX0, localY0, &cx0, &cy0);
    Matrix4f_transformPoint(&transform, localX1, localY0, &cx1, &cy1);
    Matrix4f_transformPoint(&transform, localX1, localY1, &cx2, &cy2);
    Matrix4f_transformPoint(&transform, localX0, localY1, &cx3, &cy3);

    x360EmitQuadFlat(r, tex, cx0, cy0, cx1, cy1, cx2, cy2, cx3, cy3,
                     u0, v0, u1, v1, color, alpha);
}

static void x360DrawSpritePartColor(Renderer *renderer, int32_t tpagIndex, float srcOffX,
                                    float srcOffY, float srcW, float srcH, float x, float y,
                                    float xscale, float yscale, float angleDeg, float pivotX,
                                    float pivotY, uint32_t color1, uint32_t color2,
                                    uint32_t color3, uint32_t color4, float alpha) {
    X360Renderer *r = x360(renderer);
    DataWin *dw = renderer->dataWin;
    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;

    TexturePageItem *tpag = &dw->tpag.items[tpagIndex];
    D3DTexture *tex;
    int32_t texW, texH;
    if (!x360ResolveTPAG(r, tpagIndex, NULL, &tex, &texW, &texH)) return;

    float u0 = (float) (tpag->sourceX + srcOffX) / (float) texW;
    float v0 = (float) (tpag->sourceY + srcOffY) / (float) texH;
    float u1 = (float) (tpag->sourceX + srcOffX + srcW) / (float) texW;
    float v1 = (float) (tpag->sourceY + srcOffY + srcH) / (float) texH;

    uint8_t r1 = (uint8_t) BGR_R(color1), g1 = (uint8_t) BGR_G(color1), b1 = (uint8_t) BGR_B(color1);
    uint8_t r2 = (uint8_t) BGR_R(color2), g2 = (uint8_t) BGR_G(color2), b2 = (uint8_t) BGR_B(color2);
    uint8_t r3 = (uint8_t) BGR_R(color3), g3 = (uint8_t) BGR_G(color3), b3 = (uint8_t) BGR_B(color3);
    uint8_t r4 = (uint8_t) BGR_R(color4), g4 = (uint8_t) BGR_G(color4), b4 = (uint8_t) BGR_B(color4);

    float cx0, cy0, cx1, cy1, cx2, cy2, cx3, cy3;
    if (angleDeg == 0.0f) {
        cx0 = x;                            cy0 = y;
        cx1 = x + (float) srcW * xscale;    cy1 = y;
        cx2 = x + (float) srcW * xscale;    cy2 = y + (float) srcH * yscale;
        cx3 = x;                            cy3 = y + (float) srcH * yscale;
    } else {
        float angleRad = -angleDeg * ((float) M_PI / 180.0f);
        float cosA = cosf(angleRad);
        float sinA = sinf(angleRad);
        float qx0 = x, qy0 = y;
        float qx1 = x + (float) srcW * xscale, qy1 = y;
        float qx2 = x + (float) srcW * xscale, qy2 = y + (float) srcH * yscale;
        float qx3 = x, qy3 = y + (float) srcH * yscale;
        float dx, dy;
        dx = qx0 - pivotX; dy = qy0 - pivotY; cx0 = cosA * dx - sinA * dy + pivotX; cy0 = sinA * dx + cosA * dy + pivotY;
        dx = qx1 - pivotX; dy = qy1 - pivotY; cx1 = cosA * dx - sinA * dy + pivotX; cy1 = sinA * dx + cosA * dy + pivotY;
        dx = qx2 - pivotX; dy = qy2 - pivotY; cx2 = cosA * dx - sinA * dy + pivotX; cy2 = sinA * dx + cosA * dy + pivotY;
        dx = qx3 - pivotX; dy = qy3 - pivotY; cx3 = cosA * dx - sinA * dy + pivotX; cy3 = sinA * dx + cosA * dy + pivotY;
    }

    x360EmitQuad(r, tex, cx0, cy0, cx1, cy1, cx2, cy2, cx3, cy3, u0, v0, u1, v1,
                 r1, g1, b1, r2, g2, b2, r3, g3, b3, r4, g4, b4, alpha);
}

static void x360DrawSpritePart(Renderer *renderer, int32_t tpagIndex, float srcOffX,
                               float srcOffY, float srcW, float srcH, float x, float y,
                               float xscale, float yscale, float angleDeg, float pivotX,
                               float pivotY, uint32_t color, float alpha) {
    x360DrawSpritePartColor(renderer, tpagIndex, srcOffX, srcOffY, srcW, srcH, x, y,
                            xscale, yscale, angleDeg, pivotX, pivotY,
                            color, color, color, color, alpha);
}

static void x360DrawSpritePos(Renderer *renderer, int32_t tpagIndex, float x1, float y1,
                              float x2, float y2, float x3, float y3, float x4, float y4,
                              float alpha) {
    X360Renderer *r = x360(renderer);
    TexturePageItem *tpag;
    D3DTexture *tex;
    int32_t texW, texH;
    if (!x360ResolveTPAG(r, tpagIndex, &tpag, &tex, &texW, &texH)) return;

    float u0 = (float) tpag->sourceX / (float) texW;
    float v0 = (float) tpag->sourceY / (float) texH;
    float u1 = (float) (tpag->sourceX + tpag->sourceWidth) / (float) texW;
    float v1 = (float) (tpag->sourceY + tpag->sourceHeight) / (float) texH;

    x360EmitQuadFlat(r, tex, x1, y1, x2, y2, x3, y3, x4, y4, u0, v0, u1, v1, 0xFFFFFF, alpha);
}

static void x360DrawTiled(X360Renderer *r, D3DTexture *tex, float gridX, float gridY,
                          float tileW, float tileH, bool tileX, bool tileY, float roomW,
                          float roomH, float quadOffsetX0, float quadW, float quadOffsetY0,
                          float quadH, float u0, float v0, float u1, float v1,
                          uint32_t color, float alpha) {
    if (0 >= tileW || 0 >= tileH) return;

    float startX, endX, startY, endY;
    if (tileX) {
        startX = fmodf(gridX, tileW);
        if (startX > 0) startX -= tileW;
        endX = roomW;
    } else {
        startX = gridX;
        endX = startX + tileW;
    }
    if (tileY) {
        startY = fmodf(gridY, tileH);
        if (startY > 0) startY -= tileH;
        endY = roomH;
    } else {
        startY = gridY;
        endY = startY + tileH;
    }

    // Clip the tiled extent to the visible world-space AABB (the same
    // optimization the GL backend applies). Without it a tiled background over
    // a large room emits one quad per tile across the whole room, even though
    // only the screen-sized part can be seen.
    if (r->cullValid) {
        if (tileX) {
            if (r->cullMinX > startX) startX += floorf((r->cullMinX - startX) / tileW) * tileW;
            if (r->cullMaxX < endX) endX = r->cullMaxX;
        }
        if (tileY) {
            if (r->cullMinY > startY) startY += floorf((r->cullMinY - startY) / tileH) * tileH;
            if (r->cullMaxY < endY) endY = r->cullMaxY;
        }
    }
    if (startX >= endX || startY >= endY) return;

    int32_t tilesX = (int32_t) ((endX - startX) / tileW) + 1;
    int32_t tilesY = (int32_t) ((endY - startY) / tileH) + 1;
    if (0 >= tilesX || 0 >= tilesY) return;

#ifdef BS_X360_RENDER_STATS
    r->statTiledCells += (uint32_t) (tilesX * tilesY);
#endif

    for (int32_t iy = 0; iy < tilesY; iy++) {
        float dy = startY + (float) iy * tileH;
        if (dy >= endY) break;
        float vy0 = dy + quadOffsetY0;
        float vy1 = vy0 + quadH;
        for (int32_t ix = 0; ix < tilesX; ix++) {
            float dx = startX + (float) ix * tileW;
            if (dx >= endX) break;
            float vx0 = dx + quadOffsetX0;
            float vx1 = vx0 + quadW;
            x360EmitQuadFlat(r, tex, vx0, vy0, vx1, vy0, vx1, vy1, vx0, vy1,
                             u0, v0, u1, v1, color, alpha);
        }
    }
}

static void x360DrawSpriteTiled(Renderer *renderer, int32_t tpagIndex, float originX,
                                float originY, float x, float y, float xscale, float yscale,
                                bool tileX, bool tileY, float roomW, float roomH,
                                uint32_t color, float alpha) {
    X360Renderer *r = x360(renderer);
    TexturePageItem *tpag;
    D3DTexture *tex;
    int32_t texW, texH;
    if (!x360ResolveTPAG(r, tpagIndex, &tpag, &tex, &texW, &texH)) return;

    float axScale = fabsf(xscale);
    float ayScale = fabsf(yscale);
    float tileW = (float) tpag->boundingWidth * axScale;
    float tileH = (float) tpag->boundingHeight * ayScale;

    float u0 = (float) tpag->sourceX / (float) texW;
    float v0 = (float) tpag->sourceY / (float) texH;
    float u1 = (float) (tpag->sourceX + tpag->sourceWidth) / (float) texW;
    float v1 = (float) (tpag->sourceY + tpag->sourceHeight) / (float) texH;

    float localX0 = (float) tpag->targetX - originX;
    float localY0 = (float) tpag->targetY - originY;
    float quadOffX0 = originX * axScale + xscale * localX0;
    float quadOffY0 = originY * ayScale + yscale * localY0;
    float quadW = xscale * (float) tpag->targetWidth;
    float quadH = yscale * (float) tpag->targetHeight;

    x360DrawTiled(r, tex, x - originX * axScale, y - originY * ayScale, tileW, tileH,
                  tileX, tileY, roomW, roomH, quadOffX0, quadW, quadOffY0, quadH,
                  u0, v0, u1, v1, color, alpha);
}

static void x360DrawTiledPart(Renderer *renderer, int32_t tpagIndex, int32_t srcX,
                              int32_t srcY, int32_t srcW, int32_t srcH, float dstX,
                              float dstY, float dstW, float dstH, uint32_t color,
                              float alpha) {
    X360Renderer *r = x360(renderer);
    TexturePageItem *tpag;
    D3DTexture *tex;
    int32_t texW, texH;
    if (!x360ResolveTPAG(r, tpagIndex, &tpag, &tex, &texW, &texH)) return;
    if (srcW <= 0 || srcH <= 0 || dstW <= 0.0f || dstH <= 0.0f) return;

    // srcX/srcY are already relative to the TPAG's drawable region: the
    // nine-slice helper subtracts tpag->targetX/Y before calling here (same
    // convention as drawSpritePart). The page coordinate is therefore
    // sourceX/Y + srcX/srcY, not targetX/Y + srcX/srcY. Using targetX/Y here
    // sampled the wrong part of the atlas whenever sourceX != targetX.
    float u0 = (float) (tpag->sourceX + srcX) / (float) texW;
    float v0 = (float) (tpag->sourceY + srcY) / (float) texH;
    float u1 = (float) (tpag->sourceX + srcX + srcW) / (float) texW;
    float v1 = (float) (tpag->sourceY + srcY + srcH) / (float) texH;

    x360EmitQuadFlat(r, tex, dstX, dstY, dstX + dstW, dstY, dstX + dstW, dstY + dstH,
                     dstX, dstY + dstH, u0, v0, u1, v1, color, alpha);
}

// ===[ Solid geometry ]===

static void x360DrawRectangle(Renderer *renderer, float x1, float y1, float x2, float y2,
                              uint32_t color, float alpha, bool outline) {
    X360Renderer *r = x360(renderer);
    if (outline) {
        x360EmitQuadFlat(r, r->whiteTexture, x1, y1, x2 + 1, y1, x2 + 1, y1 + 1, x1, y1 + 1, 0.5f, 0.5f, 0.5f, 0.5f, color, alpha);
        x360EmitQuadFlat(r, r->whiteTexture, x1, y2, x2 + 1, y2, x2 + 1, y2 + 1, x1, y2 + 1, 0.5f, 0.5f, 0.5f, 0.5f, color, alpha);
        x360EmitQuadFlat(r, r->whiteTexture, x1, y1 + 1, x1 + 1, y1 + 1, x1 + 1, y2, x1, y2, 0.5f, 0.5f, 0.5f, 0.5f, color, alpha);
        x360EmitQuadFlat(r, r->whiteTexture, x2, y1 + 1, x2 + 1, y1 + 1, x2 + 1, y2, x2, y2, 0.5f, 0.5f, 0.5f, 0.5f, color, alpha);
    } else {
        // GML adds +1 to width/height for filled rectangles.
        x360EmitQuadFlat(r, r->whiteTexture, x1, y1, x2 + 1, y1, x2 + 1, y2 + 1, x1, y2 + 1, 0.5f, 0.5f, 0.5f, 0.5f, color, alpha);
    }
}

static void x360DrawRectangleColor(Renderer *renderer, float x1, float y1, float x2, float y2,
                                   uint32_t color1, uint32_t color2, uint32_t color3,
                                   uint32_t color4, float alpha, bool outline) {
    X360Renderer *r = x360(renderer);
    if (outline) {
        renderer->vtable->drawLineColor(renderer, x1, y1, x2, y1, 1.0f, color1, color2, alpha);
        renderer->vtable->drawLineColor(renderer, x2, y1, x2, y2, 1.0f, color2, color3, alpha);
        renderer->vtable->drawLineColor(renderer, x2, y2, x1, y2, 1.0f, color3, color4, alpha);
        renderer->vtable->drawLineColor(renderer, x1, y2, x1, y1, 1.0f, color4, color1, alpha);
        return;
    }
    uint8_t r1 = (uint8_t) BGR_R(color1), g1 = (uint8_t) BGR_G(color1), b1 = (uint8_t) BGR_B(color1);
    uint8_t r2 = (uint8_t) BGR_R(color2), g2 = (uint8_t) BGR_G(color2), b2 = (uint8_t) BGR_B(color2);
    uint8_t r3 = (uint8_t) BGR_R(color3), g3 = (uint8_t) BGR_G(color3), b3 = (uint8_t) BGR_B(color3);
    uint8_t r4 = (uint8_t) BGR_R(color4), g4 = (uint8_t) BGR_G(color4), b4 = (uint8_t) BGR_B(color4);
    x360EmitQuad(r, r->whiteTexture, x1, y1, x2 + 1, y1, x2 + 1, y2 + 1, x1, y2 + 1,
                 0.5f, 0.5f, 0.5f, 0.5f, r1, g1, b1, r2, g2, b2, r3, g3, b3, r4, g4, b4, alpha);
}

static void x360DrawLineColor(Renderer *renderer, float x1, float y1, float x2, float y2,
                              float width, uint32_t color1, uint32_t color2, float alpha) {
    X360Renderer *r = x360(renderer);
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (0.0001f > len) return;
    float halfW = width * 0.5f;
    float px = (-dy / len) * halfW;
    float py = (dx / len) * halfW;

    uint8_t r1 = (uint8_t) BGR_R(color1), g1 = (uint8_t) BGR_G(color1), b1 = (uint8_t) BGR_B(color1);
    uint8_t r2 = (uint8_t) BGR_R(color2), g2 = (uint8_t) BGR_G(color2), b2 = (uint8_t) BGR_B(color2);
    x360EmitQuad(r, r->whiteTexture, x1 + px, y1 + py, x1 - px, y1 - py, x2 - px, y2 - py,
                 x2 + px, y2 + py, 0.5f, 0.5f, 0.5f, 0.5f,
                 r1, g1, b1, r1, g1, b1, r2, g2, b2, r2, g2, b2, alpha);
}

static void x360DrawLine(Renderer *renderer, float x1, float y1, float x2, float y2,
                         float width, uint32_t color, float alpha) {
    x360DrawLineColor(renderer, x1, y1, x2, y2, width, color, color, alpha);
}

static void x360DrawTriangle(Renderer *renderer, float x1, float y1, float x2, float y2,
                             float x3, float y3, uint32_t color1, uint32_t color2,
                             uint32_t color3, float alpha, bool outline) {
    X360Renderer *r = x360(renderer);
    if (outline) {
        renderer->vtable->drawLineColor(renderer, x1, y1, x2, y2, 1.0f, color1, color2, alpha);
        renderer->vtable->drawLineColor(renderer, x2, y2, x3, y3, 1.0f, color2, color3, alpha);
        renderer->vtable->drawLineColor(renderer, x3, y3, x1, y1, 1.0f, color3, color1, alpha);
        return;
    }

    if (r->batchVertexCount + 3 > X360_MAX_VERTICES || r->batchIndexCount + 3 > X360_MAX_INDICES) {
        x360Flush(r);
    }
    x360SetBatchTexture(r, r->whiteTexture, X360_BATCH_TRIANGLES);
    uint16_t base = (uint16_t) r->batchVertexCount;
    x360EmitVertex(r, x1, y1, 0.0f, BGR_R(color1) / 255.0f, BGR_G(color1) / 255.0f, BGR_B(color1) / 255.0f, alpha, 0.0f, 0.0f);
    x360EmitVertex(r, x2, y2, 0.0f, BGR_R(color2) / 255.0f, BGR_G(color2) / 255.0f, BGR_B(color2) / 255.0f, alpha, 0.0f, 0.0f);
    x360EmitVertex(r, x3, y3, 0.0f, BGR_R(color3) / 255.0f, BGR_G(color3) / 255.0f, BGR_B(color3) / 255.0f, alpha, 0.0f, 0.0f);
    x360EmitIndex(r, base + 0);
    x360EmitIndex(r, base + 1);
    x360EmitIndex(r, base + 2);
}

// ===[ Text ]===

typedef struct {
    Font *font;
    TexturePageItem *fontTpag;
    D3DTexture *tex;
    int32_t texW;
    int32_t texH;
    Sprite *spriteFontSprite;
} X360FontState;

static bool x360ResolveFontState(X360Renderer *r, DataWin *dw, Font *font, X360FontState *state) {
    memset(state, 0, sizeof(*state));
    state->font = font;
    if (!font->isSpriteFont) {
        int32_t fontTpagIndex = font->tpagIndex;
        if (0 > fontTpagIndex || (uint32_t) fontTpagIndex >= dw->tpag.count) return false;
        state->fontTpag = &dw->tpag.items[fontTpagIndex];
        int16_t pageId = state->fontTpag->texturePageId;
        if (0 > pageId) return false;
        state->tex = x360EnsurePage(r, pageId, &state->texW, &state->texH);
        if (state->tex == NULL) return false;
    } else if (font->spriteIndex >= 0 && dw->sprt.count > (uint32_t) font->spriteIndex) {
        state->spriteFontSprite = &dw->sprt.sprites[font->spriteIndex];
    } else {
        return false;
    }
    return true;
}

static bool x360ResolveGlyph(X360Renderer *r, DataWin *dw, X360FontState *state, FontGlyph *glyph,
                             float cursorX, float cursorY, D3DTexture **outTex, float *outU0,
                             float *outV0, float *outU1, float *outV1, float *outLocalX0,
                             float *outLocalY0) {
    Font *font = state->font;
    if (font->isSpriteFont && state->spriteFontSprite != NULL) {
        Sprite *sprite = state->spriteFontSprite;
        int32_t glyphIndex = (int32_t) (glyph - font->glyphs);
        if (0 > glyphIndex || glyphIndex >= (int32_t) sprite->textureCount) return false;
        int32_t tpagIdx = sprite->tpagIndices[glyphIndex];
        if (0 > tpagIdx || (uint32_t) tpagIdx >= dw->tpag.count) return false;
        TexturePageItem *glyphTpag = &dw->tpag.items[tpagIdx];
        int16_t pid = glyphTpag->texturePageId;
        if (0 > pid) return false;
        int32_t tw = 0, th = 0;
        D3DTexture *tex = x360EnsurePage(r, pid, &tw, &th);
        if (tex == NULL) return false;
        *outTex = tex;
        *outU0 = (float) glyphTpag->sourceX / (float) tw;
        *outV0 = (float) glyphTpag->sourceY / (float) th;
        *outU1 = (float) (glyphTpag->sourceX + glyphTpag->sourceWidth) / (float) tw;
        *outV1 = (float) (glyphTpag->sourceY + glyphTpag->sourceHeight) / (float) th;
        *outLocalX0 = cursorX + (float) glyph->offset;
        *outLocalY0 = cursorY + (float) (int32_t) glyphTpag->targetY - (float) font->spriteOriginYAdjust;
    } else {
        *outTex = state->tex;
        *outU0 = (float) (state->fontTpag->sourceX + glyph->sourceX) / (float) state->texW;
        *outV0 = (float) (state->fontTpag->sourceY + glyph->sourceY) / (float) state->texH;
        *outU1 = (float) (state->fontTpag->sourceX + glyph->sourceX + glyph->sourceWidth) / (float) state->texW;
        *outV1 = (float) (state->fontTpag->sourceY + glyph->sourceY + glyph->sourceHeight) / (float) state->texH;
        *outLocalX0 = cursorX + glyph->offset;
        *outLocalY0 = cursorY;
        if (font == &r->debugUIFont
            && DEBUGFONT_FIRST_CP <= glyph->character && glyph->character <= DEBUGFONT_LAST_CP) {
            *outLocalY0 += (float) debugFontGlyphs[glyph->character - DEBUGFONT_FIRST_CP].yoffset;
        }
    }
    return true;
}

static void x360DrawTextInternal(Renderer *renderer, const char *text, float x, float y,
                                 float xscale, float yscale, float angleDeg, float lineSeparation,
                                 uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4, float alpha,
                                 Font *font, X360FontState *fs) {
    X360Renderer *r = x360(renderer);
    DataWin *dw = renderer->dataWin;

    if (!font) {
        int32_t fontIndex = renderer->drawFont;
        if (0 > fontIndex || dw->font.count <= (uint32_t) fontIndex) return;
        font = &dw->font.fonts[fontIndex];
    }

    X360FontState fontState;
    if (!fs) {
        if (!x360ResolveFontState(r, dw, font, &fontState)) return;
    } else {
        fontState = *fs;
    }

    int32_t textLen = (int32_t) strlen(text);
    if (textLen == 0) return;

    int32_t lineCount = TextUtils_countLines(text, textLen);
    float lineStride = (0.0f > lineSeparation) ? TextUtils_lineStride(font)
                                               : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));

    float totalHeight = (float) lineCount * lineStride;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale * font->scaleX, yscale * font->scaleY, angleRad);

    float cursorY = valignOffset - (float) font->ascenderOffset;
    int32_t lineStart = 0;

    int32_t lc1 = (int32_t) c1, lc2 = (int32_t) c2, lc3 = (int32_t) c3, lc4 = (int32_t) c4;
    bool lerpColors = lc1 != lc2 || lc2 != lc3 || lc3 != lc4;

    for (int32_t lineIdx = 0; lineIdx < lineCount; lineIdx++) {
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(text[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;
        float gradientX = 0.0f;

        int32_t pos = 0;
        uint16_t ch = 0;
        bool hasCh = false;
        if (lineLen > pos) {
            ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
            hasCh = true;
        }

        while (hasCh) {
            FontGlyph *glyph = TextUtils_findGlyph(font, ch);

            uint16_t nextCh = 0;
            bool hasNext = lineLen > pos;
            if (hasNext) nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);

            if (glyph != NULL) {
                float advance = (float) glyph->shift;
                float leftFrac = (lineWidth > 0.0f) ? (gradientX / lineWidth) : 0.0f;
                float rightFrac = (lineWidth > 0.0f) ? ((gradientX + advance) / lineWidth) : 1.0f;
                if (lerpColors) {
                    lc1 = Color_lerp(c1, c2, leftFrac);
                    lc2 = Color_lerp(c1, c2, rightFrac);
                    lc3 = Color_lerp(c4, c3, rightFrac);
                    lc4 = Color_lerp(c4, c3, leftFrac);
                }

                bool drewSuccessfully = false;
                if (ch == ' ') {
                    drewSuccessfully = true;
                } else if (glyph->sourceWidth != 0 && glyph->sourceHeight != 0) {
                    float u0, v0, u1, v1, localX0, localY0;
                    D3DTexture *glyphTex;
                    if (x360ResolveGlyph(r, dw, &fontState, glyph, cursorX, cursorY, &glyphTex,
                                         &u0, &v0, &u1, &v1, &localX0, &localY0)) {
                        float localX1 = localX0 + (float) glyph->sourceWidth;
                        float localY1 = localY0 + (float) glyph->sourceHeight;
                        float px0, py0, px1, py1, px2, py2, px3, py3;
                        Matrix4f_transformPoint(&transform, localX0, localY0, &px0, &py0);
                        Matrix4f_transformPoint(&transform, localX1, localY0, &px1, &py1);
                        Matrix4f_transformPoint(&transform, localX1, localY1, &px2, &py2);
                        Matrix4f_transformPoint(&transform, localX0, localY1, &px3, &py3);
                        x360EmitQuad(r, glyphTex, px0, py0, px1, py1, px2, py2, px3, py3,
                                     u0, v0, u1, v1,
                                     (uint8_t) BGR_R(lc1), (uint8_t) BGR_G(lc1), (uint8_t) BGR_B(lc1),
                                     (uint8_t) BGR_R(lc2), (uint8_t) BGR_G(lc2), (uint8_t) BGR_B(lc2),
                                     (uint8_t) BGR_R(lc3), (uint8_t) BGR_G(lc3), (uint8_t) BGR_B(lc3),
                                     (uint8_t) BGR_R(lc4), (uint8_t) BGR_G(lc4), (uint8_t) BGR_B(lc4),
                                     alpha);
                        drewSuccessfully = true;
                    }
                }

                cursorX += glyph->shift;
                gradientX += glyph->shift;
                if (drewSuccessfully && hasNext) {
                    float kern = TextUtils_getKerningOffset(glyph, nextCh);
                    cursorX += kern;
                    gradientX += kern;
                }
            }

            ch = nextCh;
            hasCh = hasNext;
        }

        cursorY += lineStride;
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(text, lineEnd, textLen);
        } else {
            lineStart = lineEnd;
        }
    }
}

static void x360DrawText(Renderer *renderer, const char *text, float x, float y, float xscale,
                         float yscale, float angleDeg, float lineSeparation) {
    x360DrawTextInternal(renderer, text, x, y, xscale, yscale, angleDeg, lineSeparation,
                         renderer->drawColor, renderer->drawColor, renderer->drawColor,
                         renderer->drawColor, renderer->drawAlpha, NULL, NULL);
}

static void x360DrawTextColor(Renderer *renderer, const char *text, float x, float y,
                              float xscale, float yscale, float angleDeg, int32_t c1, int32_t c2,
                              int32_t c3, int32_t c4, float alpha, float lineSeparation) {
    x360DrawTextInternal(renderer, text, x, y, xscale, yscale, angleDeg, lineSeparation,
                         (uint32_t) c1, (uint32_t) c2, (uint32_t) c3, (uint32_t) c4,
                         alpha, NULL, NULL);
}

static bool x360EnsureDebugUITexture(X360Renderer *r) {
    if (r->debugUITexture != NULL) return true;

    size_t texels = (size_t) DEBUGFONT_ATLAS_W * (size_t) DEBUGFONT_ATLAS_H;
    uint8_t *rgba = (uint8_t *) safeMalloc(texels * 4);
    if (rgba == NULL) return false;
    for (size_t i = 0; i < texels; i++) {
        rgba[i * 4 + 0] = 0xFF;
        rgba[i * 4 + 1] = 0xFF;
        rgba[i * 4 + 2] = 0xFF;
        rgba[i * 4 + 3] = debugFontPixels[i];
    }
    x360RgbaBytesToArgbWords(rgba, texels);

    D3DTexture *texture = NULL;
    if (!x360CreateTexture(r, DEBUGFONT_ATLAS_W, DEBUGFONT_ATLAS_H, &texture) ||
        !x360UploadLevel(texture, DEBUGFONT_ATLAS_W, DEBUGFONT_ATLAS_H, rgba,
                         DEBUGFONT_ATLAS_W * 4)) {
        if (texture) IDirect3DTexture9_Release(texture);
        free(rgba);
        return false;
    }
    free(rgba);
    r->debugUITexture = texture;
    return true;
}

static void x360InitDebugUIFont(X360Renderer *r) {
    if (r->debugUIInited) return;
    r->debugUIInited = true;

    r->debugUIFont.name = "DebugUI";
    r->debugUIFont.displayName = "DebugUI";
    r->debugUIFont.scaleX = 1.0f;
    r->debugUIFont.scaleY = 1.0f;
    r->debugUIFont.ascenderOffset = 0;
    r->debugUIFont.maxGlyphHeight = DEBUGFONT_LINE_HEIGHT;
    r->debugUIFont.emSize = (float) DEBUGFONT_LINE_HEIGHT;
    r->debugUIFont.isSpriteFont = false;
    r->debugUIFont.tpagIndex = -1;

    for (int i = 0; i < DEBUGFONT_GLYPH_COUNT; i++) {
        const DebugFontGlyphEntry *e = &debugFontGlyphs[i];
        FontGlyph *g = &r->debugUIGlyphs[i];
        g->character = (uint16_t) (DEBUGFONT_FIRST_CP + i);
        g->sourceX = e->x;
        g->sourceY = e->y;
        g->sourceWidth = e->w;
        g->sourceHeight = e->h;
        g->shift = e->xadvance;
        g->offset = e->xoffset;
        g->kerningCount = 0;
        g->kerning = NULL;
    }
    r->debugUIFont.glyphs = r->debugUIGlyphs;
    r->debugUIFont.glyphCount = DEBUGFONT_GLYPH_COUNT;
    Font_buildGlyphLUT(&r->debugUIFont);

    memset(&r->debugUITpag, 0, sizeof(r->debugUITpag));
    r->debugUITpag.sourceWidth = DEBUGFONT_ATLAS_W;
    r->debugUITpag.sourceHeight = DEBUGFONT_ATLAS_H;
    r->debugUITpag.texturePageId = -1;
}

static void x360DrawTextUI(Renderer *renderer, const char *text, float x, float y, float xscale,
                           float yscale, float angleDeg, int32_t c1, int32_t c2, int32_t c3,
                           int32_t c4, float alpha, float lineSeparation) {
    if (text == NULL) return;
    X360Renderer *r = x360(renderer);
    x360InitDebugUIFont(r);
    if (!x360EnsureDebugUITexture(r)) return;

    X360FontState fs;
    fs.font = &r->debugUIFont;
    fs.fontTpag = &r->debugUITpag;
    fs.tex = r->debugUITexture;
    fs.texW = DEBUGFONT_ATLAS_W;
    fs.texH = DEBUGFONT_ATLAS_H;
    fs.spriteFontSprite = NULL;

    x360DrawTextInternal(renderer, text, x, y, xscale, yscale, angleDeg, lineSeparation,
                         (uint32_t) c1, (uint32_t) c2, (uint32_t) c3, (uint32_t) c4,
                         alpha, fs.font, &fs);
}

// ===[ Primitives ]===

static void x360PrimitiveBegin(Renderer *renderer, int32_t primitiveType) {
    X360Renderer *r = x360(renderer);
    x360Flush(r);
    r->primitiveType = primitiveType;
    r->primitiveTexture = r->whiteTexture;
    r->primitiveVertexCount = 0;
}

static bool x360ResolveTextureHandleFull(X360Renderer *r, uint32_t texHandle,
                                         TexturePageItem **outTpag, D3DTexture **outTex,
                                         int32_t *outW, int32_t *outH) {
    if (texHandle == 0) return false;
    if (texHandle & X360_SURFACE_TEXTURE_FLAG) {
        uint32_t sid = texHandle & ~X360_SURFACE_TEXTURE_FLAG;
        if (!x360SurfaceValid(r, (int32_t) sid)) return false;
        if (r->currentSurface == (int32_t) sid) x360ResolveCurrentSurface(r);
        X360Surface *s = &r->surfaces[sid];
        if (outTpag) *outTpag = NULL;
        if (outTex) *outTex = s->tex;
        if (outW) *outW = s->width;
        if (outH) *outH = s->height;
        return true;
    }
    return x360ResolveTPAG(r, (int32_t) texHandle - 1, outTpag, outTex, outW, outH);
}

static void x360PrimitiveBeginTexture(Renderer *renderer, int32_t primitiveType, int32_t texture) {
    X360Renderer *r = x360(renderer);
    x360Flush(r);
    r->primitiveType = primitiveType;
    r->primitiveVertexCount = 0;
    TexturePageItem *tpag;
    D3DTexture *tex;
    int32_t w, h;
    if (texture > 0 && x360ResolveTextureHandleFull(r, (uint32_t) texture, &tpag, &tex, &w, &h)) {
        r->primitiveTexture = tex;
    } else {
        r->primitiveTexture = r->whiteTexture;
    }
}

static D3DPRIMITIVETYPE x360PrimitiveToD3D(int32_t primitiveType, int32_t *outVertexStride) {
    (void) outVertexStride;
    switch (primitiveType) {
        case PRIMITIVE_POINTS: return D3DPT_POINTLIST;
        case PRIMITIVE_LINES: return D3DPT_LINELIST;
        case PRIMITIVE_LINE_STRIP: return D3DPT_LINESTRIP;
        case PRIMITIVE_TRIANGLES: return D3DPT_TRIANGLELIST;
        case PRIMITIVE_TRIANGLE_STRIP: return D3DPT_TRIANGLESTRIP;
        case PRIMITIVE_TRIANGLE_FAN: return D3DPT_TRIANGLEFAN;
        default: return D3DPT_TRIANGLELIST;
    }
}

static int32_t x360PrimitiveVertexCountForDraw(int32_t primitiveType, int32_t vertices) {
    switch (primitiveType) {
        case PRIMITIVE_POINTS: return vertices;
        case PRIMITIVE_LINES: return vertices / 2;
        case PRIMITIVE_LINE_STRIP: return vertices >= 2 ? vertices - 1 : 0;
        case PRIMITIVE_TRIANGLES: return vertices / 3;
        case PRIMITIVE_TRIANGLE_STRIP:
        case PRIMITIVE_TRIANGLE_FAN: return vertices >= 3 ? vertices - 2 : 0;
        default: return 0;
    }
}

static void x360PrimitiveEnd(Renderer *renderer) {
    X360Renderer *r = x360(renderer);
    if (r->primitiveVertexCount <= 0) return;
    int32_t count = x360PrimitiveVertexCountForDraw(r->primitiveType, r->primitiveVertexCount);
    if (count > 0) {
        D3DTexture *texture = r->primitiveTexture != NULL ? r->primitiveTexture : r->whiteTexture;
        IDirect3DDevice9_SetTexture(x360Device(r), 0, (D3DBaseTexture *) texture);
        IDirect3DDevice9_DrawPrimitiveUP(x360Device(r), x360PrimitiveToD3D(r->primitiveType, NULL),
                                        (UINT) count, r->vertices, sizeof(X360Vertex));
        r->batchTexture = NULL; // DrawPrimitiveUP bypassed the batch texture state
    }
    r->primitiveVertexCount = 0;
}

static void x360DrawVertex(Renderer *renderer, float x, float y, float z, uint32_t color,
                           float alpha, float u, float v) {
    X360Renderer *r = x360(renderer);
    if (r->primitiveVertexCount >= X360_MAX_VERTICES) return;
    X360Vertex *vertex = &r->vertices[r->primitiveVertexCount++];
    vertex->position[0] = x;
    vertex->position[1] = y;
    vertex->position[2] = z;
    vertex->position[3] = 1.0f;
    vertex->color[0] = (float) BGR_R(color) / 255.0f;
    vertex->color[1] = (float) BGR_G(color) / 255.0f;
    vertex->color[2] = (float) BGR_B(color) / 255.0f;
    vertex->color[3] = alpha;
    vertex->texcoord[0] = u;
    vertex->texcoord[1] = v;
}

static void x360DrawVertexBuffer(Renderer *renderer, VertexBuffer *buffer, int32_t primitive,
                                 int32_t texture, int32_t offset, int32_t count) {
    X360Renderer *r = x360(renderer);
    if (!buffer || !buffer->format || buffer->data == NULL) return;
    if (buffer->format->stride == 0) return;

    x360Flush(r);
    int32_t vertexCount = (int32_t) (buffer->size / buffer->format->stride);
    if (vertexCount <= 0) return;
    if (offset < 0) offset = 0;
    if (offset > vertexCount) offset = vertexCount;
    int32_t number = (count == -1) ? (vertexCount - offset) : count;
    if (number < 0) number = 0;
    if (offset + number > vertexCount) number = vertexCount - offset;
    if (number <= 0) return;
    if (number > X360_MAX_VERTICES) number = X360_MAX_VERTICES;

    // Locate the attributes we understand. Unsupported element types are
    // treated as opaque; this mirrors the desktop GL path's attribute binding.
    VertexElement *position = NULL;
    VertexElement *colorElement = NULL;
    VertexElement *texcoord = NULL;
    for (int32_t i = 0; i < buffer->format->numElements; i++) {
        VertexElement *e = &buffer->format->elements[i];
        if (e->usage == VERTEX_USAGE_POSITION && position == NULL) position = e;
        else if (e->usage == VERTEX_USAGE_COLOR && colorElement == NULL) colorElement = e;
        else if (e->usage == VERTEX_USAGE_TEXCOORD && texcoord == NULL) texcoord = e;
    }
    if (position == NULL) return;

    D3DTexture *tex = r->whiteTexture;
    if (texcoord == NULL) {
        tex = r->whiteTexture;
    } else if (texture != -1) {
        TexturePageItem *tpag;
        D3DTexture *resolved;
        int32_t w, h;
        if (x360ResolveTextureHandleFull(r, (uint32_t) texture, &tpag, &resolved, &w, &h)) {
            tex = resolved;
        }
    }

    const uint8_t *base = buffer->data + (size_t) offset * buffer->format->stride;
    uint32_t stride = buffer->format->stride;
    for (int32_t i = 0; i < number; i++) {
        const uint8_t *src = base + (size_t) i * stride;
        float px = 0, py = 0, pz = 0;
        const float *pf = (const float *) (src + position->offset);
        int comps = position->type == VERTEX_TYPE_FLOAT1 ? 1
                  : position->type == VERTEX_TYPE_FLOAT2 ? 2
                  : position->type == VERTEX_TYPE_FLOAT3 ? 3 : 4;
        px = pf[0];
        if (comps > 1) py = pf[1];
        if (comps > 2) pz = pf[2];

        float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
        if (colorElement != NULL) {
            if (colorElement->type == VERTEX_TYPE_FLOAT4) {
                const float *cf = (const float *) (src + colorElement->offset);
                cr = cf[0]; cg = cf[1]; cb = cf[2]; ca = cf[3];
            } else {
                const uint8_t *cc = src + colorElement->offset;
                cr = cc[0] / 255.0f; cg = cc[1] / 255.0f; cb = cc[2] / 255.0f; ca = cc[3] / 255.0f;
            }
        }

        float u = 0.5f, v = 0.5f;
        if (texcoord != NULL) {
            const float *tf = (const float *) (src + texcoord->offset);
            u = tf[0];
            v = tf[1];
        }

        X360Vertex *vertex = &r->vertices[i];
        vertex->position[0] = px;
        vertex->position[1] = py;
        vertex->position[2] = pz;
        vertex->position[3] = 1.0f;
        vertex->color[0] = cr;
        vertex->color[1] = cg;
        vertex->color[2] = cb;
        vertex->color[3] = ca;
        vertex->texcoord[0] = u;
        vertex->texcoord[1] = v;
    }

    IDirect3DDevice9_SetTexture(x360Device(r), 0, (D3DBaseTexture *) tex);
    r->batchTexture = NULL;
    int32_t primCount = x360PrimitiveVertexCountForDraw(primitive, number);
    if (primCount > 0) {
        IDirect3DDevice9_DrawPrimitiveUP(x360Device(r), x360PrimitiveToD3D(primitive, NULL),
                                        (UINT) primCount, r->vertices, sizeof(X360Vertex));
    }
}

// ===[ Surfaces API ]===

static int32_t x360CreateSurface(Renderer *renderer, int32_t width, int32_t height) {
    X360Renderer *r = x360(renderer);
    x360Flush(r);
    uint32_t index = x360AllocSurfaceSlot(r);
    if (!x360CreateSurfaceResources(r, &r->surfaces[index], width, height)) {
        // Slot stays allocated but invalid; surfaceExists() reports false.
        return (int32_t) index;
    }
    logInfo("X360Render: created surface %u (%dx%d)\n", index, width, height);
    return (int32_t) index;
}

static bool x360SurfaceExists(Renderer *renderer, int32_t surfaceId) {
    return x360SurfaceValid(x360(renderer), surfaceId);
}

static void x360SurfaceFree(Renderer *renderer, int32_t surfaceID) {
    X360Renderer *r = x360(renderer);
    if (!x360SurfaceValid(r, surfaceID)) return;
    if (surfaceID == renderer->runner->applicationSurfaceId) return;
    x360Flush(r);
    x360ReleaseSurfaceResources(r, &r->surfaces[surfaceID]);
    logInfo("X360Render: freed surface %d\n", surfaceID);
}

static void x360SurfaceResize(Renderer *renderer, int32_t surfaceID, int32_t width, int32_t height) {
    X360Renderer *r = x360(renderer);
    if (!x360SurfaceValid(r, surfaceID)) return;
    X360Surface *s = &r->surfaces[surfaceID];
    if (s->width == width && s->height == height) return;
    x360Flush(r);
    x360ReleaseSurfaceResources(r, s);
    x360CreateSurfaceResources(r, s, width, height);
    logInfo("X360Render: resized surface %d (%dx%d)\n", surfaceID, width, height);
}

static bool x360SetRenderTarget(Renderer *renderer, int32_t surfaceId, bool implicitApplicationSurface) {
    X360Renderer *r = x360(renderer);
    if (!x360SurfaceValid(r, surfaceId)) return false;

    x360Flush(r);
    if (!x360BindSurface(r, surfaceId)) return false;

    int32_t viewCurrent = 0;
    if (renderer->runner->viewsEnabled) viewCurrent = renderer->runner->viewCurrent;
    RuntimeView *view = &renderer->runner->views[viewCurrent];
    renderer->cameraCurrent = view->cameraId;
    GMLCamera *camera = Runner_getCameraById(renderer->runner, renderer->cameraCurrent);

    if (surfaceId == renderer->runner->applicationSurfaceId && implicitApplicationSurface) {
        x360SetViewport(r, 0, 0, r->gameW, r->gameH);
        x360SetScissor(r, 0, 0, r->gameW, r->gameH);
        if (camera != NULL) x360ApplyProjection(renderer, &camera->viewMatrix, &camera->projectionMatrix);
        return true;
    }

    X360Surface *s = &r->surfaces[surfaceId];
    if (camera != NULL && surfaceId == view->surfaceId) {
        x360SetViewport(r, 0, 0, s->width, s->height);
        x360DisableScissor(r);
        x360ApplyProjection(renderer, &camera->viewMatrix, &camera->projectionMatrix);
        return true;
    }

    renderer->cameraCurrent = SURFACE_CAMERA;
    GMLCamera *surfaceCamera = &renderer->runner->surfaceCamera;
    surfaceCamera->allocated = true;
    surfaceCamera->viewX = 0.0;
    surfaceCamera->viewY = 0.0;
    surfaceCamera->viewWidth = (float) s->width;
    surfaceCamera->viewHeight = (float) s->height;
    surfaceCamera->borderX = 0;
    surfaceCamera->borderY = 0;
    surfaceCamera->speedX = 0;
    surfaceCamera->speedY = 0;
    surfaceCamera->objectId = -1;
    surfaceCamera->viewAngle = 0;
    Runner_updateCameraViewSimple(surfaceCamera);

    x360SetViewport(r, 0, 0, s->width, s->height);
    x360DisableScissor(r);
    x360ApplyProjection(renderer, &surfaceCamera->viewMatrix, &surfaceCamera->projectionMatrix);
    return true;
}

static float x360GetSurfaceWidth(Renderer *renderer, int32_t surfaceId) {
    X360Renderer *r = x360(renderer);
    if (!x360SurfaceValid(r, surfaceId)) return 0.0f;
    return (float) r->surfaces[surfaceId].width;
}

static float x360GetSurfaceHeight(Renderer *renderer, int32_t surfaceId) {
    X360Renderer *r = x360(renderer);
    if (!x360SurfaceValid(r, surfaceId)) return 0.0f;
    return (float) r->surfaces[surfaceId].height;
}

// Makes a surface's resolve texture current for sampling. Assumes the caller
// already flushed.
static D3DTexture *x360SurfaceSampleTexture(X360Renderer *r, int32_t surfaceId, int32_t *outW, int32_t *outH) {
    if (!x360SurfaceValid(r, surfaceId)) return NULL;
    X360Surface *s = &r->surfaces[surfaceId];
    if (r->currentSurface == surfaceId) {
        x360ResolveCurrentSurface(r);
    }
    if (outW) *outW = s->width;
    if (outH) *outH = s->height;
    return s->tex;
}

static void x360DrawSurface(Renderer *renderer, int32_t surfaceID, int32_t srcLeft, int32_t srcTop,
                            int32_t srcWidth, int32_t srcHeight, float x, float y, float xscale,
                            float yscale, float angleDeg, uint32_t color, float alpha) {
    X360Renderer *r = x360(renderer);
    x360Flush(r);
    int32_t texW, texH;
    D3DTexture *tex = x360SurfaceSampleTexture(r, surfaceID, &texW, &texH);
    if (tex == NULL || texW <= 0 || texH <= 0) return;

    if (0 > srcWidth) { srcLeft = 0; srcTop = 0; srcWidth = texW; srcHeight = texH; }

    float u0 = (float) srcLeft / (float) texW;
    float v0 = (float) srcTop / (float) texH;
    float u1 = (float) (srcLeft + srcWidth) / (float) texW;
    float v1 = (float) (srcTop + srcHeight) / (float) texH;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);
    float cx0, cy0, cx1, cy1, cx2, cy2, cx3, cy3;
    Matrix4f_transformPoint(&transform, 0.0f, 0.0f, &cx0, &cy0);
    Matrix4f_transformPoint(&transform, (float) srcWidth, 0.0f, &cx1, &cy1);
    Matrix4f_transformPoint(&transform, (float) srcWidth, (float) srcHeight, &cx2, &cy2);
    Matrix4f_transformPoint(&transform, 0.0f, (float) srcHeight, &cx3, &cy3);
    x360EmitQuadFlat(r, tex, cx0, cy0, cx1, cy1, cx2, cy2, cx3, cy3, u0, v0, u1, v1, color, alpha);
}

static void x360DrawSurfaceColor(Renderer *renderer, int32_t surfaceID, int32_t srcLeft,
                                 int32_t srcTop, int32_t srcWidth, int32_t srcHeight, float x,
                                 float y, float xscale, float yscale, float angleDeg,
                                 uint32_t color1, uint32_t color2, uint32_t color3,
                                 uint32_t color4, float alpha) {
    X360Renderer *r = x360(renderer);
    x360Flush(r);
    int32_t texW, texH;
    D3DTexture *tex = x360SurfaceSampleTexture(r, surfaceID, &texW, &texH);
    if (tex == NULL || texW <= 0 || texH <= 0) return;
    if (0 > srcWidth) { srcLeft = 0; srcTop = 0; srcWidth = texW; srcHeight = texH; }

    float u0 = (float) srcLeft / (float) texW;
    float v0 = (float) srcTop / (float) texH;
    float u1 = (float) (srcLeft + srcWidth) / (float) texW;
    float v1 = (float) (srcTop + srcHeight) / (float) texH;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);
    float cx0, cy0, cx1, cy1, cx2, cy2, cx3, cy3;
    Matrix4f_transformPoint(&transform, 0.0f, 0.0f, &cx0, &cy0);
    Matrix4f_transformPoint(&transform, (float) srcWidth, 0.0f, &cx1, &cy1);
    Matrix4f_transformPoint(&transform, (float) srcWidth, (float) srcHeight, &cx2, &cy2);
    Matrix4f_transformPoint(&transform, 0.0f, (float) srcHeight, &cx3, &cy3);
    x360EmitQuad(r, tex, cx0, cy0, cx1, cy1, cx2, cy2, cx3, cy3, u0, v0, u1, v1,
                 (uint8_t) BGR_R(color1), (uint8_t) BGR_G(color1), (uint8_t) BGR_B(color1),
                 (uint8_t) BGR_R(color2), (uint8_t) BGR_G(color2), (uint8_t) BGR_B(color2),
                 (uint8_t) BGR_R(color3), (uint8_t) BGR_G(color3), (uint8_t) BGR_B(color3),
                 (uint8_t) BGR_R(color4), (uint8_t) BGR_G(color4), (uint8_t) BGR_B(color4), alpha);
}

static void x360DrawSurfaceTiled(Renderer *renderer, int32_t surfaceID, float x, float y,
                                 float xscale, float yscale, float roomW, float roomH,
                                 uint32_t color, float alpha) {
    X360Renderer *r = x360(renderer);
    x360Flush(r);
    int32_t texW, texH;
    D3DTexture *tex = x360SurfaceSampleTexture(r, surfaceID, &texW, &texH);
    if (tex == NULL || texW <= 0 || texH <= 0) return;
    float tileW = (float) texW * fabsf(xscale);
    float tileH = (float) texH * fabsf(yscale);
    x360DrawTiled(r, tex, x, y, tileW, tileH, true, true, roomW, roomH,
                  0.0f, xscale * (float) texW, 0.0f, yscale * (float) texH,
                  0.0f, 0.0f, 1.0f, 1.0f, color, alpha);
}

static void x360UnbindTextures(X360Renderer *r) {
    if (r->gfx.device == NULL) return;
    IDirect3DDevice9_SetTexture(x360Device(r), 0, NULL);
    r->batchTexture = NULL;
}

static bool x360SurfaceGetPixels(Renderer *renderer, int32_t surfaceId, uint8_t *outRGBA) {
    X360Renderer *r = x360(renderer);
    if (outRGBA == NULL || !x360SurfaceValid(r, surfaceId)) return false;
    x360Flush(r);
    D3DTexture *tex = x360SurfaceSampleTexture(r, surfaceId, NULL, NULL);
    if (tex == NULL) return false;
    X360Surface *s = &r->surfaces[surfaceId];
    size_t texels = (size_t) s->width * (size_t) s->height;

    x360UnbindTextures(r);
    D3DLOCKED_RECT locked;
    if (FAILED(IDirect3DTexture9_LockRect(tex, 0, &locked, NULL, 0))) return false;
    XGUntileTextureLevel((UINT) s->width, (UINT) s->height, 0, XGGetGpuFormat(D3DFMT_A8R8G8B8),
                         x360TileFlags((D3DBaseTexture *) tex), outRGBA,
                         (UINT) s->width * 4, NULL, locked.pBits, NULL);
    IDirect3DTexture9_UnlockRect(tex, 0);
    x360ArgbWordsToRgbaBytes(outRGBA, texels);
    return true;
}

// Uploads straight RGBA8 bytes into a surface's resolve texture and refreshes
// the render target so the change is visible. Xenon render targets cannot be
// locked, so the pixels go into the resolve texture and are redrawn onto the RT.
static bool x360SurfaceUploadRGBA(X360Renderer *r, int32_t surfaceId, int32_t w, int32_t h,
                                  const uint8_t *rgba) {
    if (rgba == NULL || w <= 0 || h <= 0) return false;
    if (!x360SurfaceValid(r, surfaceId)) return false;
    X360Surface *s = &r->surfaces[surfaceId];
    if (s->tiling) {
        logWarn("X360Render: surfaceSetPixels/uploadPixels is unsupported on a predicated-tiled surface\n");
        return false;
    }
    if (w > s->width || h > s->height) return false;

    x360Flush(r);
    x360UnbindTextures(r);
    uint8_t *scratch = (uint8_t *) safeMalloc((size_t) w * (size_t) h * 4);
    if (scratch == NULL) return false;
    memcpy(scratch, rgba, (size_t) w * (size_t) h * 4);
    x360RgbaBytesToArgbWords(scratch, (size_t) w * (size_t) h);

    D3DLOCKED_RECT locked;
    if (FAILED(IDirect3DTexture9_LockRect(s->tex, 0, &locked, NULL, 0))) {
        free(scratch);
        return false;
    }
    POINT point;
    point.x = 0;
    point.y = 0;
    RECT rect;
    rect.left = 0;
    rect.top = 0;
    rect.right = (LONG) w;
    rect.bottom = (LONG) h;
    XGTileTextureLevel((UINT) s->width, (UINT) s->height, 0, XGGetGpuFormat(D3DFMT_A8R8G8B8),
                       x360TileFlags((D3DBaseTexture *) s->tex), locked.pBits, &point,
                       scratch, (UINT) w * 4, &rect);
    IDirect3DTexture9_UnlockRect(s->tex, 0);
    free(scratch);

    s->texValid = true;
    if (r->currentSurface == surfaceId) {
        // Redraw the updated region over the render target so subsequent draws
        // see it without re-binding.
        Matrix4f ortho;
        Matrix4f_viewProjection(&ortho, 0.0f, 0.0f, (float) s->width, (float) s->height, 0.0f);
        x360SetProjection(r, &ortho);
        x360BeginInternalDraw(r);
        x360EmitQuadFlat(r, s->tex, 0.0f, 0.0f, (float) w, 0.0f, (float) w, (float) h,
                         0.0f, (float) h, 0.0f, 0.0f, (float) w / (float) s->width,
                         (float) h / (float) s->height, 0xFFFFFF, 1.0f);
        x360EndInternalDraw(r);
        x360UploadCurrentProjection(r);
    } else {
        s->rtStale = true;
    }
    return true;
}

static bool x360SurfaceSetPixels(Renderer *renderer, int32_t surfaceID, const uint8_t *rgba) {
    X360Renderer *r = x360(renderer);
    if (!x360SurfaceValid(r, surfaceID)) return false;
    X360Surface *s = &r->surfaces[surfaceID];
    return x360SurfaceUploadRGBA(r, surfaceID, s->width, s->height, rgba);
}

static void x360SurfaceUploadPixels(Renderer *renderer, int32_t surfaceID, int32_t w, int32_t h,
                                    const uint8_t *rgba) {
    X360Renderer *r = x360(renderer);
    x360SurfaceUploadRGBA(r, surfaceID, w, h, rgba);
}

static void x360SurfaceCopy(Renderer *renderer, int32_t destSurfaceID, int32_t destX,
                            int32_t destY, int32_t srcSurfaceID, int32_t srcX, int32_t srcY,
                            int32_t srcW, int32_t srcH, bool part) {
    X360Renderer *r = x360(renderer);
    if (!x360SurfaceValid(r, destSurfaceID) || !x360SurfaceValid(r, srcSurfaceID)) return;

    x360Flush(r);
    int32_t texW, texH;
    D3DTexture *srcTex = x360SurfaceSampleTexture(r, srcSurfaceID, &texW, &texH);
    if (srcTex == NULL) return;

    int32_t sX = part ? srcX : 0;
    int32_t sY = part ? srcY : 0;
    int32_t sW = part ? srcW : texW;
    int32_t sH = part ? srcH : texH;

    int32_t prevSurface = r->currentSurface;
    Matrix4f prevWVP = r->base.gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION];
    int32_t prevPortX = r->base.CPortX, prevPortY = r->base.CPortY;
    int32_t prevPortW = r->base.CPortW, prevPortH = r->base.CPortH;
    bool prevScissor = r->scissorEnabled;

    if (!x360BindSurface(r, destSurfaceID)) return;
    X360Surface *dest = &r->surfaces[destSurfaceID];

    Matrix4f ortho;
    Matrix4f_viewProjection(&ortho, 0.0f, 0.0f, (float) dest->width, (float) dest->height, 0.0f);
    x360SetProjection(r, &ortho);
    x360SetViewport(r, 0, 0, dest->width, dest->height);
    x360DisableScissor(r);

    x360BeginInternalDraw(r);
    x360EmitQuadFlat(r, srcTex,
                     (float) destX, (float) destY,
                     (float) (destX + sW), (float) destY,
                     (float) (destX + sW), (float) (destY + sH),
                     (float) destX, (float) (destY + sH),
                     (float) sX / (float) texW, (float) sY / (float) texH,
                     (float) (sX + sW) / (float) texW, (float) (sY + sH) / (float) texH,
                     0xFFFFFF, 1.0f);
    x360EndInternalDraw(r);

    r->base.gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION] = prevWVP;
    if (prevSurface >= 0 && x360SurfaceValid(r, prevSurface)) {
        x360BindSurface(r, prevSurface);
    } else {
        x360BindBackbuffer(r);
    }
    x360SetViewport(r, prevPortX, prevPortY, prevPortW, prevPortH);
    if (prevScissor) x360SetScissor(r, prevPortX, prevPortY, prevPortW, prevPortH);
    x360UploadCurrentProjection(r);
}

// ===[ Dynamic sprites from surfaces ]===

static uint32_t x360AllocTexturePageSlot(X360Renderer *r) {
    for (uint32_t i = r->originalTexturePageCount; i < r->textureCount; i++) {
        if (r->pageTextures[i] == NULL) {
            r->pageFormats[i] = X360_TEXFMT_RGBA8;
            return i;
        }
    }
    uint32_t newPageId = r->textureCount++;
    r->pageTextures = (D3DTexture **) safeRealloc(r->pageTextures, r->textureCount * sizeof(D3DTexture *));
    r->pageWidths = (int32_t *) safeRealloc(r->pageWidths, r->textureCount * sizeof(int32_t));
    r->pageHeights = (int32_t *) safeRealloc(r->pageHeights, r->textureCount * sizeof(int32_t));
    r->pageLoaded = (bool *) safeRealloc(r->pageLoaded, r->textureCount * sizeof(bool));
    r->pageFormats = (X360TextureFormat *) safeRealloc(r->pageFormats,
                                                       r->textureCount * sizeof(X360TextureFormat));
    r->pageTextures[newPageId] = NULL;
    r->pageWidths[newPageId] = 0;
    r->pageHeights[newPageId] = 0;
    r->pageLoaded[newPageId] = false;
    r->pageFormats[newPageId] = X360_TEXFMT_RGBA8;
    return newPageId;
}

static uint32_t x360AllocTpagSlot(DataWin *dw, uint32_t originalTpagCount) {
    for (uint32_t i = originalTpagCount; i < dw->tpag.count; i++) {
        if (dw->tpag.items[i].texturePageId == -1) return i;
    }
    uint32_t newIndex = dw->tpag.count++;
    dw->tpag.items = (TexturePageItem *) safeRealloc(dw->tpag.items, dw->tpag.count * sizeof(TexturePageItem));
    memset(&dw->tpag.items[newIndex], 0, sizeof(TexturePageItem));
    dw->tpag.items[newIndex].texturePageId = -1;
    return newIndex;
}

static int32_t x360CreateSpriteFromSurface(Renderer *renderer, int32_t surfaceID, int32_t x,
                                           int32_t y, int32_t w, int32_t h, bool removeback,
                                           bool smooth, int32_t xorig, int32_t yorig) {
    (void) removeback;
    (void) smooth;
    X360Renderer *r = x360(renderer);
    if (w <= 0 || h <= 0) return -1;
    if (!x360SurfaceValid(r, surfaceID)) return -1;
    X360Surface *s = &r->surfaces[surfaceID];
    if (x < 0 || y < 0 || x + w > s->width || y + h > s->height) return -1;

    uint8_t *pixels = (uint8_t *) safeMalloc((size_t) w * (size_t) h * 4);
    if (pixels == NULL) return -1;

    // Read the source region out of the full-surface readback, top-down.
    uint8_t *full = (uint8_t *) safeMalloc((size_t) s->width * (size_t) s->height * 4);
    if (full == NULL || !x360SurfaceGetPixels(renderer, surfaceID, full)) {
        free(full);
        free(pixels);
        return -1;
    }
    for (int32_t row = 0; row < h; row++) {
        memcpy(pixels + (size_t) row * w * 4,
               full + (((size_t) (y + row) * s->width) + (size_t) x) * 4,
               (size_t) w * 4);
    }
    free(full);

    D3DTexture *tex = NULL;
    if (!x360CreateTexture(r, w, h, &tex)) {
        free(pixels);
        return -1;
    }
    uint8_t *converted = (uint8_t *) safeMalloc((size_t) w * (size_t) h * 4);
    if (converted == NULL) {
        IDirect3DTexture9_Release(tex);
        free(pixels);
        return -1;
    }
    memcpy(converted, pixels, (size_t) w * (size_t) h * 4);
    x360RgbaBytesToArgbWords(converted, (size_t) w * (size_t) h);
    bool uploaded = x360UploadLevel(tex, w, h, converted, (uint32_t) w * 4);
    free(converted);
    free(pixels);
    if (!uploaded) {
        IDirect3DTexture9_Release(tex);
        return -1;
    }

    uint32_t pageId = x360AllocTexturePageSlot(r);
    r->pageTextures[pageId] = tex;
    r->pageWidths[pageId] = w;
    r->pageHeights[pageId] = h;
    r->pageLoaded[pageId] = true;
    r->pageFormats[pageId] = X360_TEXFMT_RGBA8;

    DataWin *dw = renderer->dataWin;
    uint32_t tpagIndex = x360AllocTpagSlot(dw, r->originalTpagCount);
    TexturePageItem *tpag = &dw->tpag.items[tpagIndex];
    tpag->sourceX = 0;
    tpag->sourceY = 0;
    tpag->sourceWidth = (uint16_t) w;
    tpag->sourceHeight = (uint16_t) h;
    tpag->targetX = 0;
    tpag->targetY = 0;
    tpag->targetWidth = (uint16_t) w;
    tpag->targetHeight = (uint16_t) h;
    tpag->boundingWidth = (uint16_t) w;
    tpag->boundingHeight = (uint16_t) h;
    tpag->texturePageId = (int16_t) pageId;

    uint32_t spriteIndex = DataWin_allocSpriteSlot(dw, r->originalSpriteCount);
    Sprite *sprite = &dw->sprt.sprites[spriteIndex];
    sprite->width = (uint32_t) w;
    sprite->height = (uint32_t) h;
    sprite->originX = xorig;
    sprite->originY = yorig;
    sprite->textureCount = 1;
    sprite->tpagIndices = (int32_t *) safeMalloc(sizeof(int32_t));
    sprite->tpagIndices[0] = (int32_t) tpagIndex;
    sprite->maskCount = 0;
    sprite->masks = NULL;

    logInfo("X360Render: created sprite %u (%dx%d) from surface %d\n", spriteIndex, w, h, surfaceID);
    return (int32_t) spriteIndex;
}

static void x360DeleteSprite(Renderer *renderer, int32_t spriteIndex) {
    X360Renderer *r = x360(renderer);
    DataWin *dw = renderer->dataWin;
    if (0 > spriteIndex || dw->sprt.count <= (uint32_t) spriteIndex) return;
    if (r->originalSpriteCount > (uint32_t) spriteIndex) {
        logWarn("X360Render: cannot delete data.win sprite %d\n", spriteIndex);
        return;
    }
    Sprite *sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return;

    x360UnbindTextures(r);
    for (uint32_t i = 0; i < sprite->textureCount; i++) {
        int32_t tpagIdx = sprite->tpagIndices[i];
        if (tpagIdx >= 0 && (uint32_t) tpagIdx >= r->originalTpagCount) {
            TexturePageItem *tpag = &dw->tpag.items[tpagIdx];
            int16_t pageId = tpag->texturePageId;
            if (pageId >= 0 && (uint32_t) pageId < r->textureCount && r->pageTextures[pageId] != NULL) {
                IDirect3DTexture9_Release(r->pageTextures[pageId]);
                r->pageTextures[pageId] = NULL;
                r->pageLoaded[pageId] = false;
            }
            tpag->texturePageId = -1;
        }
    }

    free(sprite->tpagIndices);
    const char *keepName = sprite->name;
    memset(sprite, 0, sizeof(Sprite));
    sprite->name = keepName;
    logInfo("X360Render: deleted sprite %d\n", spriteIndex);
}

// ===[ GPU state ]===

static D3DBLENDOP x360BlendOp(int32_t mode) {
    switch (mode) {
        case bm_reverse_subtract: return D3DBLENDOP_REVSUBTRACT;
        case bm_min: return D3DBLENDOP_MIN;
        default: return D3DBLENDOP_ADD;
    }
}

static D3DBLEND x360BlendFactor(int32_t factor) {
    switch (factor) {
        case bm_zero: return D3DBLEND_ZERO;
        case bm_src_color: return D3DBLEND_SRCCOLOR;
        case bm_inv_src_color: return D3DBLEND_INVSRCCOLOR;
        case bm_src_alpha: return D3DBLEND_SRCALPHA;
        case bm_inv_src_alpha: return D3DBLEND_INVSRCALPHA;
        case bm_dest_alpha: return D3DBLEND_DESTALPHA;
        case bm_inv_dest_alpha: return D3DBLEND_INVDESTALPHA;
        case bm_dest_color: return D3DBLEND_DESTCOLOR;
        case bm_inv_dest_color: return D3DBLEND_INVDESTCOLOR;
        case bm_src_alpha_sat: return D3DBLEND_SRCALPHASAT;
        case bm_one:
        default: return D3DBLEND_ONE;
    }
}

static int32_t x360BlendModeSFactor(int32_t mode) {
    switch (mode) {
        case bm_subtract: return bm_zero;
        case bm_min: return bm_one;
        default: return bm_src_alpha;
    }
}

static int32_t x360BlendModeDFactor(int32_t mode) {
    switch (mode) {
        case bm_add:
        case bm_min:
        case bm_reverse_subtract: return bm_one;
        case bm_subtract:
        case bm_max: return bm_inv_src_color;
        default: return bm_inv_src_alpha;
    }
}

static BlendFactors x360GpuGetBlendFactors(Renderer *renderer) {
    X360Renderer *r = x360(renderer);
    BlendFactors factors;
    factors.src = r->sFactor;
    factors.dst = r->dFactor;
    factors.srcAlpha = r->sFactorAlpha;
    factors.dstAlpha = r->dFactorAlpha;
    return factors;
}

static int32_t x360GpuGetBlendMode(Renderer *renderer) {
    return x360(renderer)->blendMode;
}

static void x360GpuSetBlendMode(Renderer *renderer, int32_t mode) {
    X360Renderer *r = x360(renderer);
    if (mode == r->blendMode) return;
    x360Flush(r);
    r->blendMode = mode;
    r->separateAlphaBlend = false;
    r->sFactor = x360BlendModeSFactor(mode);
    r->dFactor = x360BlendModeDFactor(mode);
    r->sFactorAlpha = r->sFactor;
    r->dFactorAlpha = r->dFactor;
    x360ApplyBlendState(r);
}

static void x360GpuSetBlendModeExt(Renderer *renderer, int32_t sfactor, int32_t dfactor,
                                   int32_t sfactor_alpha, int32_t dfactor_alpha) {
    X360Renderer *r = x360(renderer);
    if (sfactor == r->sFactor && dfactor == r->dFactor
        && sfactor_alpha == r->sFactorAlpha && dfactor_alpha == r->dFactorAlpha) return;
    x360Flush(r);
    r->blendMode = bm_complex;
    r->separateAlphaBlend = true;
    r->sFactor = sfactor;
    r->dFactor = dfactor;
    r->sFactorAlpha = sfactor_alpha;
    r->dFactorAlpha = dfactor_alpha;
    x360ApplyBlendState(r);
}

static void x360GpuSetBlendEnable(Renderer *renderer, bool enable) {
    X360Renderer *r = x360(renderer);
    if (r->blendEnable == enable) return;
    x360Flush(r);
    r->blendEnable = enable;
    IDirect3DDevice9_SetRenderState(x360Device(r), D3DRS_ALPHABLENDENABLE, enable ? TRUE : FALSE);
}

static bool x360GpuGetBlendEnable(Renderer *renderer) {
    return x360(renderer)->blendEnable;
}

static void x360GpuSetTexFilter(Renderer *renderer, bool enable) {
    X360Renderer *r = x360(renderer);
    if (renderer->texFilter == enable) return;
    x360Flush(r);
    renderer->texFilter = enable;
    x360ApplySamplerFilter(r);
}

static void x360GpuSetAlphaTestEnable(Renderer *renderer, bool enable) {
    X360Renderer *r = x360(renderer);
    if (r->alphaTestEnable == enable) return;
    x360Flush(r);
    r->alphaTestEnable = enable;
    x360ApplyAlphaTest(r);
}

static bool x360GpuGetAlphaTestEnable(Renderer *renderer) {
    return x360(renderer)->alphaTestEnable;
}

static void x360GpuSetAlphaTestRef(Renderer *renderer, uint8_t ref) {
    X360Renderer *r = x360(renderer);
    float refF = ref / 255.0f;
    if (r->alphaTestRef == refF) return;
    x360Flush(r);
    r->alphaTestRef = refF;
    x360ApplyAlphaTest(r);
}

static void x360GpuSetColorWriteEnable(Renderer *renderer, bool red, bool green, bool blue, bool alpha) {
    X360Renderer *r = x360(renderer);
    if (r->colorWriteR == red && r->colorWriteG == green && r->colorWriteB == blue && r->colorWriteA == alpha) return;
    x360Flush(r);
    r->colorWriteR = red;
    r->colorWriteG = green;
    r->colorWriteB = blue;
    r->colorWriteA = alpha;
    x360ApplyColorWrite(r);
}

static void x360GpuGetColorWriteEnable(Renderer *renderer, bool *red, bool *green, bool *blue, bool *alpha) {
    X360Renderer *r = x360(renderer);
    *red = r->colorWriteR;
    *green = r->colorWriteG;
    *blue = r->colorWriteB;
    *alpha = r->colorWriteA;
}

static void x360GpuSetFog(Renderer *renderer, bool enable, uint32_t color) {
    X360Renderer *r = x360(renderer);
    if (r->fogEnable == enable && r->fogColor == color) return;
    x360Flush(r);
    r->fogEnable = enable;
    r->fogColor = color;
    x360UploadPixelShaderConstants(r);
}

// ===[ User shaders ]===
//
// Xenon has no runtime HLSL compiler, so a game's HLSL9 shaders are precompiled
// offline into a sidecar (data.x360shaders); x360Init loads it and this section
// binds the resulting D3D9 shaders. A shader with no sidecar entry falls back to
// the built-in shader, loudly, because an unbound shader mis-renders silently.
// The uniform table comes from the fxc listing the precompile tool parses, so
// shader_get_uniform / shader_set_uniform_* need no d3dx9 reflection.

static void x360WarnShaderOnce(X360Renderer *r, DataWin *dw, int32_t shaderIndex) {
    if (dw == NULL || 0 > shaderIndex || (uint32_t) shaderIndex >= dw->shdr.count) return;
    if (r->shaderWarned == NULL) {
        r->shaderWarned = (bool *) safeCalloc(dw->shdr.count, sizeof(bool));
    }
    if (r->shaderWarned != NULL && !r->shaderWarned[shaderIndex]) {
        r->shaderWarned[shaderIndex] = true;
        logWarn("X360Render: shader \"%s\" is not precompiled for Xenon; using the built-in shader\n",
                dw->shdr.shaders[shaderIndex].name != NULL ? dw->shdr.shaders[shaderIndex].name : "?");
    }
}

static void x360GpuSetShader(Renderer *renderer, int32_t shaderIndex) {
    X360Renderer *r = x360(renderer);
    x360Flush(r);

    X360UserShader *shader = X360ShaderSet_byIndex(r->shaderSet, shaderIndex);
    if (!x360ShaderUsable(shader)) {
        x360WarnShaderOnce(r, renderer->dataWin, shaderIndex);
        r->activeShader = NULL;
        renderer->currentShader = -1;
        x360BindActiveShaders(r);
        return;
    }

    r->activeShader = shader;
    renderer->currentShader = shaderIndex;
    x360BindActiveShaders(r);
}

static void x360GpuResetShader(Renderer *renderer) {
    X360Renderer *r = x360(renderer);
    x360Flush(r);
    r->activeShader = NULL;
    renderer->currentShader = -1;
    x360BindActiveShaders(r);
}

// Resolves the shader a uniform query refers to: an explicit index, else the
// active one. Returns NULL if there is no usable shader.
static X360UserShader *x360ShaderForQuery(X360Renderer *r, int32_t shaderIndex) {
    int32_t target = shaderIndex != -1 ? shaderIndex : r->base.currentShader;
    X360UserShader *shader = X360ShaderSet_byIndex(r->shaderSet, target);
    return x360ShaderUsable(shader) ? shader : NULL;
}

// A uniform handle is the index of the uniform within the shader's table; the
// set functions resolve it against the active shader, exactly as the GL backend
// uses a GLSL uniform location valid for the active program.
static int32_t x360ShaderGetUniform(Renderer *renderer, int32_t shaderIndex, char *uniform) {
    X360UserShader *shader = x360ShaderForQuery(x360(renderer), shaderIndex);
    if (shader == NULL || uniform == NULL) return -1;
    return X360UserShader_findUniform(shader, uniform);
}

static int32_t x360ShaderGetSamplerIndex(Renderer *renderer, int32_t shaderIndex, char *uniform) {
    X360UserShader *shader = x360ShaderForQuery(x360(renderer), shaderIndex);
    if (shader == NULL || uniform == NULL) return -1;
    int32_t idx = X360UserShader_findUniform(shader, uniform);
    if (idx < 0) return -1;
    if (shader->uniforms[idx].regClass != X360_UNIFORM_REG_SAMPLER) return -1;
    return (int32_t) shader->uniforms[idx].regIndex;
}

static X360ShaderUniform *x360ResolveUniform(X360Renderer *r, int32_t handle) {
    X360UserShader *shader = r->activeShader;
    if (!x360ShaderUsable(shader)) return NULL;
    if (0 > handle || (uint32_t) handle >= shader->uniformCount) return NULL;
    return &shader->uniforms[handle];
}

static void x360SetShaderConstantF(X360Renderer *r, X360ShaderUniform *u, const float *values,
                                   uint32_t registerCount) {
    if (u->stage == X360_SHADER_STAGE_VERTEX) {
        IDirect3DDevice9_SetVertexShaderConstantF(x360Device(r), u->regIndex, values, registerCount);
    } else {
        IDirect3DDevice9_SetPixelShaderConstantF(x360Device(r), u->regIndex, values, registerCount);
    }
}

static void x360SetShaderConstantI(X360Renderer *r, X360ShaderUniform *u, const int32_t *values,
                                   uint32_t registerCount) {
    if (u->stage == X360_SHADER_STAGE_VERTEX) {
        IDirect3DDevice9_SetVertexShaderConstantI(x360Device(r), u->regIndex, values, registerCount);
    } else {
        IDirect3DDevice9_SetPixelShaderConstantI(x360Device(r), u->regIndex, values, registerCount);
    }
}

static void x360ShaderSetUniformF(Renderer *renderer, int32_t handle, int32_t count,
                                  float v1, float v2, float v3, float v4) {
    X360Renderer *r = x360(renderer);
    X360ShaderUniform *u = x360ResolveUniform(r, handle);
    if (u == NULL || u->regClass != X360_UNIFORM_REG_CONSTANT) return;
    (void) count; // the four arguments fill one constant register regardless
    float values[4] = { v1, v2, v3, v4 };
    x360SetShaderConstantF(r, u, values, 1);
}

static void x360ShaderSetUniformFArray(Renderer *renderer, int32_t handle, float *values, uint32_t count) {
    X360Renderer *r = x360(renderer);
    X360ShaderUniform *u = x360ResolveUniform(r, handle);
    if (u == NULL || u->regClass != X360_UNIFORM_REG_CONSTANT || values == NULL || count == 0) return;

    // A constant register holds four floats; pad a partial tail with zeroes so
    // the register upload never reads past the caller's array.
    uint32_t registers = (count + 3) / 4;
    float *padded = values;
    if ((count & 3u) != 0) {
        padded = (float *) safeCalloc((size_t) registers * 4, sizeof(float));
        memcpy(padded, values, (size_t) count * sizeof(float));
    }
    x360SetShaderConstantF(r, u, padded, registers);
    if (padded != values) free(padded);
}

static void x360ShaderSetUniformI(Renderer *renderer, int32_t handle, int32_t count,
                                  int32_t v1, int32_t v2, int32_t v3, int32_t v4) {
    X360Renderer *r = x360(renderer);
    X360ShaderUniform *u = x360ResolveUniform(r, handle);
    if (u == NULL || u->regClass != X360_UNIFORM_REG_CONSTANT) return;
    (void) count;
    int32_t values[4] = { v1, v2, v3, v4 };
    x360SetShaderConstantI(r, u, values, 1);
}

static bool x360ShaderIsCompiled(Renderer *renderer, int32_t shaderID) {
    X360Renderer *r = x360(renderer);
    DataWin *dw = renderer->dataWin;
    if (0 > shaderID || (uint32_t) shaderID >= dw->shdr.count) return false;
    return x360ShaderUsable(X360ShaderSet_byIndex(r->shaderSet, shaderID));
}

static bool x360ShadersSupported(void) {
    // The platform has a shader pipeline (everything is shaders on Xenon); the
    // per-shader shader_is_compiled answer is what reports unavailability.
    return true;
}

static uint32_t x360SpriteGetTexture(Renderer *renderer, int32_t tpagIndex) {
    X360Renderer *r = x360(renderer);
    if (!x360ResolveTPAG(r, tpagIndex, NULL, NULL, NULL, NULL)) return 0;
    return (uint32_t) (tpagIndex + 1);
}

static uint32_t x360SurfaceGetTexture(Renderer *renderer, int32_t surfaceID) {
    X360Renderer *r = x360(renderer);
    if (!x360SurfaceValid(r, surfaceID)) return 0;
    if (r->surfaces[surfaceID].tex == NULL) return 0;
    return X360_SURFACE_TEXTURE_FLAG | (uint32_t) surfaceID;
}

static float x360TextureGetTexelWidth(Renderer *renderer, uint32_t texHandle) {
    X360Renderer *r = x360(renderer);
    TexturePageItem *tpag;
    D3DTexture *tex;
    int32_t w = 0, h = 0;
    if (!x360ResolveTextureHandleFull(r, texHandle, &tpag, &tex, &w, &h) || w <= 0) return 1.0f;
    return 1.0f / (float) w;
}

static float x360TextureGetTexelHeight(Renderer *renderer, uint32_t texHandle) {
    X360Renderer *r = x360(renderer);
    TexturePageItem *tpag;
    D3DTexture *tex;
    int32_t w = 0, h = 0;
    if (!x360ResolveTextureHandleFull(r, texHandle, &tpag, &tex, &w, &h) || h <= 0) return 1.0f;
    return 1.0f / (float) h;
}

static bool x360TextureGetUVs(Renderer *renderer, uint32_t texHandle, float *outUVs) {
    X360Renderer *r = x360(renderer);
    TexturePageItem *tpag;
    D3DTexture *tex;
    int32_t w = 0, h = 0;
    if (!x360ResolveTextureHandleFull(r, texHandle, &tpag, &tex, &w, &h) || w <= 0 || h <= 0) return false;
    if (tpag == NULL) {
        outUVs[0] = 0.0f; outUVs[1] = 0.0f; outUVs[2] = 1.0f; outUVs[3] = 1.0f;
        return true;
    }
    float divW = 1.0f / (float) w;
    float divH = 1.0f / (float) h;
    outUVs[0] = (float) tpag->sourceX * divW;
    outUVs[1] = (float) tpag->sourceY * divH;
    outUVs[2] = outUVs[0] + (float) tpag->sourceWidth * divW;
    outUVs[3] = outUVs[1] + (float) tpag->sourceHeight * divH;
    return true;
}

static void x360TextureSetStage(Renderer *renderer, int32_t slot, uint32_t texHandle) {
    X360Renderer *r = x360(renderer);
    if (slot < 0 || slot >= MAX_TEXTURE_STAGES) return;

    D3DTexture *tex = NULL;
    if (texHandle != 0) {
        TexturePageItem *tpag;
        int32_t w, h;
        if (!x360ResolveTextureHandleFull(r, texHandle, &tpag, &tex, &w, &h)) return;
    }

    if (slot == 0) {
        // The pending batch owns the deferred stage-0 texture; flush it before
        // clearing batchTexture so its draws keep the texture they were queued
        // with.
        x360Flush(r);
        // Xenon asserts if a NULL texture is sampled. texture_set_stage(0, -1)
        // means "no texture", so bind the 1x1 white texture instead of NULL;
        // the batch flush does the same for untextured draws.
        IDirect3DDevice9_SetTexture(x360Device(r), 0,
                                    (D3DBaseTexture *) (tex != NULL ? tex : r->whiteTexture));
        r->batchTexture = NULL;
        return;
    }

    // Extra stages feed game shaders (e.g. a palette on s1). Never bind NULL:
    // Xenon asserts when a NULL texture is sampled, so an unbind binds the 1x1
    // white texture instead.
    IDirect3DDevice9_SetTexture(x360Device(r), slot,
                                (D3DBaseTexture *) (tex != NULL ? tex : r->whiteTexture));
}

// ===[ Init / destroy ]===

static void x360Destroy(Renderer *renderer) {
    X360Renderer *r = x360(renderer);
    if (r->gfx.device == NULL) {
        X360TextureSet_destroy(r->textureSet);
        X360TextureDump_close(r->textureDump, false);
        free(r);
        return;
    }
    x360Flush(r);
    // Render targets cannot be freed while set on the device; return to the
    // back buffer first.
    x360BindBackbuffer(r);

    // Log the resident texture footprint before releasing the pages. This is
    // the number the compression work is meant to reduce.
    {
        uint32_t resident = 0;
        uint64_t bytes = 0;
        for (uint32_t i = 0; i < r->textureCount; i++) {
            if (r->pageTextures != NULL && r->pageTextures[i] != NULL) {
                resident++;
                bytes += x360TextureDataSize(r->pageFormats[i],
                                             (uint32_t) r->pageWidths[i],
                                             (uint32_t) r->pageHeights[i]);
            }
        }
        logInfo("X360Render: %u texture page(s) resident, %.1f MB\n",
                resident, (double) bytes / (1024.0 * 1024.0));
    }

    for (uint32_t i = 0; i < r->textureCount; i++) {
        if (r->pageTextures != NULL && r->pageTextures[i] != NULL) {
            IDirect3DTexture9_Release(r->pageTextures[i]);
        }
    }
    free(r->pageTextures);
    free(r->pageWidths);
    free(r->pageHeights);
    free(r->pageLoaded);
    free(r->pageFormats);
    free(r->shaderWarned);

    X360TextureSet_destroy(r->textureSet);
    X360TextureDump_close(r->textureDump, false);

    for (uint32_t i = 0; i < r->surfaceCount; i++) {
        X360Surface *s = &r->surfaces[i];
        if (s->rt != NULL) IDirect3DSurface9_Release(s->rt);
        if (s->tex != NULL) IDirect3DTexture9_Release(s->tex);
    }
    free(r->surfaces);

    if (r->debugUITexture != NULL) IDirect3DTexture9_Release(r->debugUITexture);
    if (r->whiteTexture != NULL) IDirect3DTexture9_Release(r->whiteTexture);
    X360ShaderSet_destroy(r->shaderSet);
    if (r->pixelShader != NULL) IDirect3DPixelShader9_Release(r->pixelShader);
    if (r->vertexShader != NULL) IDirect3DVertexShader9_Release(r->vertexShader);
    if (r->declaration != NULL) IDirect3DVertexDeclaration9_Release(r->declaration);

    free(r->vertices);
    free(r->indices);

    if (r->backBuffer != NULL) IDirect3DSurface9_Release(r->backBuffer);

    Xbox360D3DDevice_destroy(&r->gfx);
    free(r);
}

// Device-only half of init. Idempotent: the port calls this before data.win is
// parsed (so the loading screen can draw), then Runner_create's vtable init
// calls it again and only the data-dependent half runs.
static void x360InitDevice(X360Renderer *r) {
    if (r->gfx.device != NULL) return;

    r->blendEnable = true;
    r->separateAlphaBlend = false;
    r->blendMode = bm_normal;
    r->sFactor = bm_src_alpha;
    r->dFactor = bm_inv_src_alpha;
    r->sFactorAlpha = bm_src_alpha;
    r->dFactorAlpha = bm_inv_src_alpha;
    r->alphaTestEnable = false;
    r->alphaTestRef = 0.0f;
    r->colorWriteR = true;
    r->colorWriteG = true;
    r->colorWriteB = true;
    r->colorWriteA = true;
    r->fogEnable = false;
    r->fogColor = 0;
    r->currentSurface = -1;

    if (!Xbox360D3DDevice_createEx(&r->gfx, BS_X360_RENDER_WIDTH, BS_X360_RENDER_HEIGHT, false)) {
        // No device means no renderer at all; abort instead of handing back a
        // half-initialized renderer (matches the desktop GL renderer's behavior).
        logError("X360Render: D3D9 device creation failed\n");
        abort();
    }
    D3DDevice *dev = x360Device(r);
    r->backbufferW = BS_X360_RENDER_WIDTH;
    r->backbufferH = BS_X360_RENDER_HEIGHT;
    r->targetW = r->backbufferW;
    r->targetH = r->backbufferH;

    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, 0, &r->backBuffer)) || r->backBuffer == NULL) {
        logError("X360Render: GetBackBuffer failed\n");
        abort();
    }

    static const D3DVERTEXELEMENT9 elements[] = {
        {0, 0,  D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 32, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(dev, elements, &r->declaration)) ||
        FAILED(IDirect3DDevice9_CreateVertexShader(dev, bs_renderer_vs, &r->vertexShader)) ||
        FAILED(IDirect3DDevice9_CreatePixelShader(dev, bs_renderer_ps, &r->pixelShader))) {
        logError("X360Render: shader/declaration creation failed\n");
        abort();
    }
    IDirect3DDevice9_SetVertexDeclaration(dev, r->declaration);
    IDirect3DDevice9_SetVertexShader(dev, r->vertexShader);
    IDirect3DDevice9_SetPixelShader(dev, r->pixelShader);

    // 1x1 white texture for untextured draws (never sample NULL).
    {
        uint8_t white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
        x360RgbaBytesToArgbWords(white, 1);
        if (!x360CreateTexture(r, 1, 1, &r->whiteTexture)) {
            logError("X360Render: white texture creation failed\n");
            abort();
        }
        x360UploadLevel(r->whiteTexture, 1, 1, white, 4);
    }

    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);

    // GameMaker's projection math is written for the OpenGL/D3D10 pixel-center
    // convention (pixel centres at half-integer coordinates). Xenon D3D9
    // defaults to the D3D1-9 convention (centres at integer coordinates), which
    // makes a quad that spans a texel's full extent sample exactly on the texel
    // boundary; point sampling then picks the neighbouring texel and the whole
    // scene looks half a pixel off / shows seams. This render state applies the
    // effective (-0.5, -0.5) post-viewport offset and restores the GL
    // convention. See D3DRENDERSTATETYPE (D3DRS_HALFPIXELOFFSET) in the XDK.
    IDirect3DDevice9_SetRenderState(dev, D3DRS_HALFPIXELOFFSET, TRUE);

    x360ApplyBlendState(r);
    x360ApplyColorWrite(r);
    x360ApplySamplerFilter(r);
    x360ApplyAlphaTest(r);
    x360UploadPixelShaderConstants(r);

    // Identity matrices so an overlay drawn before the runner exists can still
    // restore a defined projection. beginView/applyProjection replace them.
    Matrix4f identity;
    Matrix4f_identity(&identity);
    for (int i = 0; MATRICES_MAX > i; i++) {
        r->base.gmlMatrices[i] = identity;
    }
    x360UploadCurrentProjection(r);

    r->vertices = (X360Vertex *) safeMalloc(X360_MAX_VERTICES * sizeof(X360Vertex));
    r->indices = (uint16_t *) safeMalloc(X360_MAX_INDICES * sizeof(uint16_t));
    r->batchVertexCount = 0;
    r->batchIndexCount = 0;
    r->batchTexture = NULL;
    r->primitiveVertexCount = 0;

    logInfo("X360Render: device initialized (%dx%d back buffer)\n",
            r->backbufferW, r->backbufferH);
}

// ===[ First-boot texture generation ]===
//
// Decoding and compression are CPU work and are safe to run on worker threads.
// The D3D device is not touched here: only the device-owning thread may call it.
// The sidecar writer and the progress overlay stay on the main thread.

static uint32_t x360GeneratePagesSequential(X360Renderer *r, X360TextureProgressFn progress,
                                            void *user, uint32_t total, bool *skipped) {
    uint32_t done = 0;
    *skipped = progress != NULL ? progress(user, 0, total) : false;

    for (uint32_t i = 0; i < total && !*skipped; i++) {
        X360PagePayload payload;
        if (x360BuildPagePayload(r, (int32_t) i, &payload)) {
            X360TextureDump_add(r->textureDump, (int32_t) i, payload.format,
                                (uint32_t) payload.width, (uint32_t) payload.height,
                                payload.data, payload.dataSize);
            free(payload.data);
        }
        done = i + 1u;
        if (progress != NULL) *skipped = progress(user, done, total);
    }
    return done;
}

#if BS_X360_TEXTURE_WORKERS > 1
typedef struct {
    X360Renderer *renderer;
    uint32_t total;
    volatile LONG nextJob;
    volatile LONG stop;
    X360PagePayload *payloads;
    uint8_t *states; // 0 pending, 1 done, 2 failed, 3 consumed
    CRITICAL_SECTION lock;
    HANDLE workEvent;
} X360TextureGenContext;

static DWORD WINAPI x360TextureWorker(void *arg) {
    X360TextureGenContext *ctx = (X360TextureGenContext *) arg;
    for (;;) {
        if (InterlockedCompareExchange(&ctx->stop, 0, 0) != 0) break;

        LONG job = InterlockedIncrement(&ctx->nextJob) - 1;
        if (job >= (LONG) ctx->total) break;

        X360PagePayload payload;
        bool ok = x360BuildPagePayload(ctx->renderer, (int32_t) job, &payload);

        EnterCriticalSection(&ctx->lock);
        if (ok) {
            ctx->payloads[job] = payload;
            ctx->states[job] = 1;
        } else {
            ctx->states[job] = 2;
        }
        LeaveCriticalSection(&ctx->lock);
        SetEvent(ctx->workEvent);
    }
    return 0;
}

static uint32_t x360GeneratePagesParallel(X360Renderer *r, X360TextureProgressFn progress,
                                          void *user, uint32_t total, bool *skipped) {
    X360TextureGenContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.renderer = r;
    ctx.total = total;
    ctx.payloads = (X360PagePayload *) safeCalloc(total, sizeof(X360PagePayload));
    ctx.states = (uint8_t *) safeCalloc(total, sizeof(uint8_t));
    InitializeCriticalSection(&ctx.lock);
    ctx.workEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    if (ctx.workEvent == NULL) {
        logWarn("X360Tex: CreateEvent failed; generating sequentially\n");
        DeleteCriticalSection(&ctx.lock);
        free(ctx.payloads);
        free(ctx.states);
        return x360GeneratePagesSequential(r, progress, user, total, skipped);
    }

    uint32_t requested = BS_X360_TEXTURE_WORKERS;
    if (requested > 4) requested = 4;
    static const DWORD affinity[4] = { 2, 3, 1, 0 }; // 4/5 are used by XAudio2
    HANDLE handles[4];
    uint32_t created = 0;
    for (uint32_t i = 0; i < requested; i++) {
        DWORD threadId = 0;
        HANDLE handle = CreateThread(NULL, 512 * 1024, x360TextureWorker, &ctx,
                                     CREATE_SUSPENDED, &threadId);
        if (handle == NULL) break;
        XSetThreadProcessor(handle, affinity[i]);
        ResumeThread(handle);
        handles[created++] = handle;
    }

    if (created == 0) {
        logWarn("X360Tex: CreateThread failed; generating sequentially\n");
        XCloseHandle(ctx.workEvent);
        DeleteCriticalSection(&ctx.lock);
        free(ctx.payloads);
        free(ctx.states);
        return x360GeneratePagesSequential(r, progress, user, total, skipped);
    }

    *skipped = progress != NULL ? progress(user, 0, total) : false;
    uint32_t consumed = 0;

    while (consumed < total && !*skipped) {
        WaitForSingleObject(ctx.workEvent, 50);

        bool changed = false;
        EnterCriticalSection(&ctx.lock);
        for (uint32_t i = 0; i < total; i++) {
            if (ctx.states[i] == 1) {
                X360PagePayload *p = &ctx.payloads[i];
                X360TextureDump_add(r->textureDump, (int32_t) i, p->format,
                                    (uint32_t) p->width, (uint32_t) p->height,
                                    p->data, p->dataSize);
                free(p->data);
                p->data = NULL;
                ctx.states[i] = 3;
                consumed++;
                changed = true;
            } else if (ctx.states[i] == 2) {
                ctx.states[i] = 3;
                consumed++;
                changed = true;
            }
        }
        LeaveCriticalSection(&ctx.lock);

        if (changed && progress != NULL) {
            *skipped = progress(user, consumed, total);
            if (*skipped) InterlockedExchange(&ctx.stop, 1);
        }
    }

    if (*skipped) InterlockedExchange(&ctx.stop, 1);
    WaitForMultipleObjects(created, handles, TRUE, INFINITE);

    // Drain pages that finished while the workers were stopping.
    EnterCriticalSection(&ctx.lock);
    for (uint32_t i = 0; i < total; i++) {
        if (ctx.states[i] == 1) {
            X360PagePayload *p = &ctx.payloads[i];
            X360TextureDump_add(r->textureDump, (int32_t) i, p->format,
                                (uint32_t) p->width, (uint32_t) p->height,
                                p->data, p->dataSize);
            free(p->data);
            p->data = NULL;
            ctx.states[i] = 3;
            consumed++;
        } else if (ctx.states[i] == 2) {
            ctx.states[i] = 3;
            consumed++;
        }
    }
    LeaveCriticalSection(&ctx.lock);

    for (uint32_t i = 0; i < created; i++) {
        XCloseHandle(handles[i]);
    }
    XCloseHandle(ctx.workEvent);
    DeleteCriticalSection(&ctx.lock);
    free(ctx.payloads);
    free(ctx.states);
    return consumed;
}
#endif

// Generates a complete sidecar by decoding/compressing every static page and
// writing the result. D3D is not used: pages are uploaded later from the
// sidecar, so a generation pass never keeps the full page set resident.
bool X360Renderer_generateTextureSidecar(Renderer *renderer,
                                         X360TextureProgressFn progress,
                                         void *user) {
    X360Renderer *r = x360(renderer);
    if (r->gfx.device == NULL || r->base.dataWin == NULL || r->base.runner == NULL
        || r->base.runner->fileSystem == NULL) {
        return false;
    }
    if (r->textureSet != NULL && X360TextureSet_isComplete(r->textureSet)) {
        return false;
    }

    FileSystem *fs = r->base.runner->fileSystem;
    uint32_t gameId = r->base.dataWin->gen8.gameID;
    uint64_t dataWinSize = (uint64_t) r->base.dataWin->fileSize;

    x360EnsureTextureLocks();

    if (r->textureSet != NULL) {
        X360TextureSet_destroy(r->textureSet);
        r->textureSet = NULL;
    }

    r->textureDump = X360TextureDump_create(fs, "data.x360tex", r->textureCount,
                                            gameId, dataWinSize);
    if (r->textureDump == NULL) {
        logWarn("X360Tex: cannot write data.x360tex (read-only game directory?); using runtime compression\n");
        r->textureSet = X360TextureSet_load(fs, "data.x360tex", gameId, dataWinSize);
        return false;
    }

    uint32_t total = r->originalTexturePageCount;
    bool skipped = false;
    uint32_t done;
    DWORD genStart = GetTickCount();

#if BS_X360_TEXTURE_WORKERS > 1
    done = x360GeneratePagesParallel(r, progress, user, total, &skipped);
#else
    done = x360GeneratePagesSequential(r, progress, user, total, &skipped);
#endif

    // A skipped pass must stay incomplete so the next boot can finish it.
    X360TextureDump_close(r->textureDump, !skipped);
    r->textureDump = NULL;
    r->textureSet = X360TextureSet_load(fs, "data.x360tex", gameId, dataWinSize);

    double genSeconds = (double) (GetTickCount() - genStart) / 1000.0;
    logInfo("X360Tex: sidecar generation %s (%u/%u pages, %d worker(s), %.1f s)\n",
            skipped ? "skipped" : "finished", done, total,
            BS_X360_TEXTURE_WORKERS, genSeconds);
    return true;
}

// Data-dependent half of init: texture page tables, user-shader sidecar, and
// the sampler filter (Runner_create sets renderer->texFilter before calling
// init, which is after the port's early device init).
static void x360InitData(X360Renderer *r, DataWin *dataWin) {
    if (dataWin == NULL || r->dataInitialized) return;
    r->dataInitialized = true;
    r->base.dataWin = dataWin;

    // Load the game shader precompile sidecar, if the title deployed one next
    // to data.win. Absent/empty leaves game shaders on the built-in fallback.
    if (r->base.runner != NULL && r->base.runner->fileSystem != NULL) {
        r->shaderSet = X360ShaderSet_load(r->base.runner->fileSystem, "data.x360shaders",
                                          x360Device(r));
    }

    // Texture page tables
    r->textureCount = dataWin->txtr.count;
    r->originalTexturePageCount = r->textureCount;
    r->originalTpagCount = dataWin->tpag.count;
    r->originalSpriteCount = dataWin->sprt.count;
    r->pageTextures = (D3DTexture **) safeCalloc(r->textureCount, sizeof(D3DTexture *));
    r->pageWidths = (int32_t *) safeCalloc(r->textureCount, sizeof(int32_t));
    r->pageHeights = (int32_t *) safeCalloc(r->textureCount, sizeof(int32_t));
    r->pageLoaded = (bool *) safeCalloc(r->textureCount, sizeof(bool));
    r->pageFormats = (X360TextureFormat *) safeCalloc(r->textureCount, sizeof(X360TextureFormat));

    // Texture sidecar. In the default (autogen) build, main() calls
    // X360Renderer_generateTextureSidecar after Runner_create, when the loading
    // overlay is available for progress and START-to-skip. A partial-dump build
    // writes pages as they load instead.
    if (r->base.runner != NULL && r->base.runner->fileSystem != NULL) {
        FileSystem *fs = r->base.runner->fileSystem;
        uint32_t gameId = dataWin->gen8.gameID;
        uint64_t dataWinSize = (uint64_t) dataWin->fileSize;

#if defined(BS_X360_DUMP_TEXTURES) && !defined(BS_X360_TEXTURE_AUTOGEN)
        r->textureDump = X360TextureDump_create(fs, "data.x360tex", r->textureCount,
                                                gameId, dataWinSize);
#else
        r->textureSet = X360TextureSet_load(fs, "data.x360tex", gameId, dataWinSize);
#endif
    }

    x360ApplySamplerFilter(r);
    x360UploadCurrentProjection(r);

    logInfo("X360Render: initialized (%dx%d back buffer, %u texture pages)\n",
            r->backbufferW, r->backbufferH, r->textureCount);
}

static void x360Init(Renderer *renderer, DataWin *dataWin) {
    X360Renderer *r = x360(renderer);
    x360InitDevice(r);
    x360InitData(r, dataWin);
}

void X360Renderer_initDevice(Renderer *renderer) {
    if (renderer == NULL) return;
    x360InitDevice(x360(renderer));
}

// ===[ Port overlay pass ]===
//
// The loading/status screen and the debug stats overlay are port-owned and are
// not part of the public RendererVtable. They reuse the renderer's built-in
// shader, white texture and embedded debug font (drawTextUI), but force the
// built-in shader and a known state so a game shader cannot transform them.

void X360Renderer_beginOverlay(Renderer *renderer) {
    if (renderer == NULL) return;
    X360Renderer *r = x360(renderer);
    if (r->gfx.device == NULL) return;

    x360Flush(r);
    x360BindBackbuffer(r);
    x360SetViewport(r, 0, 0, r->backbufferW, r->backbufferH);
    x360DisableScissor(r);

    Matrix4f ortho;
    Matrix4f_viewProjection(&ortho, 0.0f, 0.0f, (float) r->backbufferW,
                            (float) r->backbufferH, 0.0f);

    // Built-in shaders, alpha test off, full colour writes, no fog.
    x360BeginInternalDraw(r);
    x360SetProjection(r, &ortho);

    // Overlay text/rectangles need normal source-alpha blending, which
    // x360BeginInternalDraw deliberately disables for its blits.
    D3DDevice *dev = x360Device(r);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    // Linear filtering makes the debug font readable when scaled. The game's
    // filter choice is restored by X360Renderer_endOverlay.
    for (int stage = 0; stage < MAX_TEXTURE_STAGES; stage++) {
        IDirect3DDevice9_SetSamplerState(dev, stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9_SetSamplerState(dev, stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9_SetSamplerState(dev, stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice9_SetSamplerState(dev, stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(dev, stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
}

void X360Renderer_endOverlay(Renderer *renderer) {
    if (renderer == NULL) return;
    X360Renderer *r = x360(renderer);
    if (r->gfx.device == NULL) return;

    x360Flush(r);
    // Restores blend/alpha-test/colour-write and rebinds the active game shader
    // (which re-uploads its projection and fog constants).
    x360EndInternalDraw(r);
    x360ApplySamplerFilter(r);
}

void X360Renderer_getStats(Renderer *renderer, X360RendererStats *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (renderer == NULL) return;

    X360Renderer *r = x360(renderer);
    out->texturePagesTotal = r->textureCount;
    if (r->pageLoaded != NULL && r->pageWidths != NULL) {
        for (uint32_t i = 0; i < r->textureCount; i++) {
            if (r->pageLoaded[i] && r->pageWidths[i] > 0) out->texturePagesLoaded++;
        }
    }
    out->surfacesTotal = r->surfaceCount;
    if (r->surfaces != NULL) {
        for (uint32_t i = 0; i < r->surfaceCount; i++) {
            if (r->surfaces[i].allocated) out->surfacesAllocated++;
        }
    }
    out->renderTargetsCached = r->rtCacheCount;
    out->renderTargetEvictions = r->rtEvictions;
}

// ===[ Shader self-test (hardware verification aid) ]===
//
// Enabled with -DBS_X360_SHADER_SELFTEST. It exercises the whole offline-
// precompile path on the console: sidecar load + D3D shader creation, the
// transposed gm_Matrices upload, a game uniform set through shader_get_uniform /
// shader_set_uniform_f, a draw through the game shader, and a pixel readback.
// It expects the test sidecar (built from src/xbox360/shaders/selftest) to be
// deployed next to data.win as data.x360shaders.
#ifdef BS_X360_SHADER_SELFTEST
static void x360LogShaderPixel(const char *label, const uint8_t *p) {
    logInfo("ShaderSelfTest: %s = (%u,%u,%u,%u)\n", label, p[0], p[1], p[2], p[3]);
}

bool X360Renderer_runShaderSelftest(Renderer *renderer) {
    X360Renderer *r = x360(renderer);
    if (r->shaderSet == NULL || X360ShaderSet_count(r->shaderSet) == 0) {
        logInfo("ShaderSelfTest: FAIL (no sidecar shaders loaded)\n");
        return false;
    }

    int32_t shaderIndex = X360ShaderSet_indexAt(r->shaderSet, 0);
    logInfo("ShaderSelfTest: sidecar shaders=%u, using index %d\n",
            X360ShaderSet_count(r->shaderSet), shaderIndex);

    const int32_t size = 64;
    int32_t sid = renderer->vtable->createSurface(renderer, size, size);
    if (sid < 0 || !renderer->vtable->surfaceExists(renderer, sid)) {
        logInfo("ShaderSelfTest: FAIL (createSurface)\n");
        return false;
    }
    renderer->vtable->setRenderTarget(renderer, sid, false);
    renderer->vtable->clearScreen(renderer, 0x0000FF, 1.0f); // GML red background

    renderer->vtable->gpuSetShader(renderer, shaderIndex);

    // A green result can only appear if the game shader's transposed
    // gm_Matrices maps the quad over the readback pixel *and* the user uniform
    // was uploaded; otherwise the readback stays red.
    int32_t handle = x360ShaderGetUniform(renderer, shaderIndex, "outColor");
    logInfo("ShaderSelfTest: shader_get_uniform(\"outColor\") = %d\n", handle);
    if (handle < 0) {
        logInfo("ShaderSelfTest: FAIL (outColor uniform not found)\n");
    }
    x360ShaderSetUniformF(renderer, handle, 4, 0.0f, 1.0f, 0.0f, 1.0f);

    int32_t sampler = x360ShaderGetSamplerIndex(renderer, shaderIndex, "gm_BaseTexture");
    logInfo("ShaderSelfTest: shader_get_sampler_index(\"gm_BaseTexture\") = %d\n", sampler);

    // Orthographic world -> clip for this surface, uploaded as the game shader's
    // gm_Matrices, then draw a textured (1x1 white) full-surface quad.
    Matrix4f savedWvp = r->base.gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION];
    Matrix4f ortho;
    Matrix4f_viewProjection(&ortho, 0.0f, 0.0f, (float) size, (float) size, 0.0f);
    r->base.gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION] = ortho;
    x360UploadCurrentProjection(r);

    x360SetViewport(r, 0, 0, size, size);
    x360SetScissor(r, 0, 0, size, size);
    x360EmitQuadFlat(r, NULL, 0.0f, 0.0f, (float) size, 0.0f,
                     (float) size, (float) size, 0.0f, (float) size,
                     0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFF, 1.0f);
    x360Flush(r);

    uint8_t *pixels = (uint8_t *) safeMalloc((size_t) size * (size_t) size * 4);
    bool ok = true;
    if (!renderer->vtable->surfaceGetPixels(renderer, sid, pixels)) {
        logInfo("ShaderSelfTest: FAIL (surfaceGetPixels)\n");
        ok = false;
    } else {
        const uint8_t *center = pixels + (((size / 2) * size) + (size / 2)) * 4;
        x360LogShaderPixel("center (expect 0,255,0,255)", center);
        if (center[0] != 0 || center[1] != 255 || center[2] != 0) ok = false;
    }
    free(pixels);

    renderer->vtable->gpuResetShader(renderer);
    r->base.gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION] = savedWvp;
    x360UploadCurrentProjection(r);
    renderer->vtable->surfaceFree(renderer, sid);

    logInfo("ShaderSelfTest: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
#endif // BS_X360_SHADER_SELFTEST

// ===[ Present ]===

void X360Renderer_present(Renderer *renderer) {
    if (renderer == NULL) return;
    X360Renderer *r = x360(renderer);
    if (r->gfx.device == NULL) return;

    x360Flush(r);
    x360ResolveCurrentSurface(r);
    x360BindBackbuffer(r);

    // Any surface drawn into this frame is preserved in its resolve texture;
    // EDRAM itself must not be assumed to survive Present.
    for (uint32_t i = 0; i < r->surfaceCount; i++) {
        if (r->surfaces[i].allocated) r->surfaces[i].rtStale = true;
    }

    Xbox360D3DDevice_present(&r->gfx);
}

// ===[ Vtable + factory ]===

static RendererVtable x360Vtable;

Renderer *X360Renderer_create(void) {
    X360Renderer *r = (X360Renderer *) safeCalloc(1, sizeof(X360Renderer));
    Renderer *base = &r->base;
    base->vtable = &x360Vtable;

    x360Vtable.init = x360Init;
    x360Vtable.destroy = x360Destroy;
    x360Vtable.beginFrame = x360BeginFrame;
    x360Vtable.endFrameInit = x360EndFrameInit;
    x360Vtable.endFrameEnd = x360EndFrameEnd;
    x360Vtable.beginView = x360BeginView;
    x360Vtable.endView = x360EndView;
    x360Vtable.applyProjection = x360ApplyProjection;
    x360Vtable.beginGUI = x360BeginGUI;
    x360Vtable.setGuiProjection = x360SetGuiProjection;
    x360Vtable.endGUI = x360EndGUI;
    x360Vtable.drawSprite = x360DrawSprite;
    x360Vtable.drawSpritePos = x360DrawSpritePos;
    x360Vtable.drawSpritePart = x360DrawSpritePart;
    x360Vtable.drawSpritePartColor = x360DrawSpritePartColor;
    x360Vtable.drawRectangle = x360DrawRectangle;
    x360Vtable.drawRectangleColor = x360DrawRectangleColor;
    x360Vtable.drawLine = x360DrawLine;
    x360Vtable.drawLineColor = x360DrawLineColor;
    x360Vtable.drawTriangle = x360DrawTriangle;
    x360Vtable.drawVertexBuffer = x360DrawVertexBuffer;
    x360Vtable.drawText = x360DrawText;
    x360Vtable.drawTextColor = x360DrawTextColor;
    x360Vtable.drawTextUI = x360DrawTextUI;
    x360Vtable.primitiveBegin = x360PrimitiveBegin;
    x360Vtable.primitiveBeginTexture = x360PrimitiveBeginTexture;
    x360Vtable.primitiveEnd = x360PrimitiveEnd;
    x360Vtable.drawVertex = x360DrawVertex;
    x360Vtable.flush = x360FlushRenderer;
    x360Vtable.clearScreen = x360ClearScreen;
    x360Vtable.createSpriteFromSurface = x360CreateSpriteFromSurface;
    x360Vtable.deleteSprite = x360DeleteSprite;
    x360Vtable.gpuGetBlendFactors = x360GpuGetBlendFactors;
    x360Vtable.gpuGetBlendMode = x360GpuGetBlendMode;
    x360Vtable.gpuSetBlendMode = x360GpuSetBlendMode;
    x360Vtable.gpuSetBlendModeExt = x360GpuSetBlendModeExt;
    x360Vtable.gpuSetBlendEnable = x360GpuSetBlendEnable;
    x360Vtable.gpuSetTexFilter = x360GpuSetTexFilter;
    x360Vtable.gpuSetAlphaTestEnable = x360GpuSetAlphaTestEnable;
    x360Vtable.gpuGetAlphaTestEnable = x360GpuGetAlphaTestEnable;
    x360Vtable.gpuSetAlphaTestRef = x360GpuSetAlphaTestRef;
    x360Vtable.gpuSetColorWriteEnable = x360GpuSetColorWriteEnable;
    x360Vtable.gpuGetColorWriteEnable = x360GpuGetColorWriteEnable;
    x360Vtable.gpuSetFog = x360GpuSetFog;
    x360Vtable.gpuGetBlendEnable = x360GpuGetBlendEnable;
    x360Vtable.drawTile = NULL;
    x360Vtable.drawSpriteTiled = x360DrawSpriteTiled;
    x360Vtable.drawTiledPart = x360DrawTiledPart;
    x360Vtable.createSurface = x360CreateSurface;
    x360Vtable.surfaceExists = x360SurfaceExists;
    x360Vtable.setRenderTarget = x360SetRenderTarget;
    x360Vtable.ensureApplicationSurface = x360EnsureApplicationSurface;
    x360Vtable.surfaceCopy = x360SurfaceCopy;
    x360Vtable.surfaceGetPixels = x360SurfaceGetPixels;
    x360Vtable.surfaceSetPixels = x360SurfaceSetPixels;
    x360Vtable.surfaceUploadPixels = x360SurfaceUploadPixels;
    x360Vtable.getSurfaceWidth = x360GetSurfaceWidth;
    x360Vtable.getSurfaceHeight = x360GetSurfaceHeight;
    x360Vtable.drawSurface = x360DrawSurface;
    x360Vtable.drawSurfaceColor = x360DrawSurfaceColor;
    x360Vtable.drawSurfaceTiled = x360DrawSurfaceTiled;
    x360Vtable.surfaceResize = x360SurfaceResize;
    x360Vtable.surfaceFree = x360SurfaceFree;
    x360Vtable.gpuSetShader = x360GpuSetShader;
    x360Vtable.gpuResetShader = x360GpuResetShader;
    x360Vtable.shaderGetUniform = x360ShaderGetUniform;
    x360Vtable.shaderSetUniformF = x360ShaderSetUniformF;
    x360Vtable.shaderSetUniformFArray = x360ShaderSetUniformFArray;
    x360Vtable.shaderSetUniformI = x360ShaderSetUniformI;
    x360Vtable.spriteGetTexture = x360SpriteGetTexture;
    x360Vtable.surfaceGetTexture = x360SurfaceGetTexture;
    x360Vtable.textureGetTexelWidth = x360TextureGetTexelWidth;
    x360Vtable.textureGetTexelHeight = x360TextureGetTexelHeight;
    x360Vtable.textureGetUVs = x360TextureGetUVs;
    x360Vtable.shaderGetSamplerIndex = x360ShaderGetSamplerIndex;
    x360Vtable.textureSetStage = x360TextureSetStage;
    x360Vtable.shaderIsCompiled = x360ShaderIsCompiled;
    x360Vtable.shadersSupported = x360ShadersSupported;
    x360Vtable.setMatrix = x360SetMatrix;

    base->drawColor = 0xFFFFFF;
    base->drawAlpha = 1.0f;
    base->drawFont = -1;
    base->drawHalign = 0;
    base->drawValign = 0;
    base->circlePrecision = 24;
    base->currentShader = -1;
    base->cameraCurrent = 0;
    base->texFilter = false;

    return base;
}
