/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Derived from ceilingtilefan/Butterscotch-360 commit
 * 7f8f1ea6044dbc55560dfbd2ca9a2f45e472c02e, with later work from
 * flaf1x/Butterscotch360-Refresh and Ral-sei.
 */

#include <xtl.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <xgraphics.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <climits>

// Core headers ?compiled as C++ alongside the .c files (via /TP flag)
#include "utils.h"
#include "text_utils.h"
#include "d3d9_renderer.h"
#include "image_decoder.h"
#include "data_win.h"

#include "runner.h"

extern "C" unsigned long __cdecl DbgPrint(const char* format, ...);

void D3D9Renderer_applyGpuState(D3D9Renderer* dr);

static void d3d9DrawSurface(Renderer* renderer, int32_t surfaceID,
                            int32_t srcLeft, int32_t srcTop,
                            int32_t srcWidth, int32_t srcHeight,
                            float x, float y, float xscale, float yscale,
                            float angleDeg, uint32_t color, float alpha);

// ===[ Vertex Format ]===
// Uses FLOAT4 position (pre-transformed screen coords, z=0, w=1)
// and D3DCOLOR for color (packed ARGB, auto-normalized to float4 by GPU).
// 28 bytes per vertex (down from 40), better cache utilization.
struct SpriteVertex {
    float x, y, z, w; // position (screen-space, z=0, w=1) ?16 bytes
    float u, v;        // texcoord ?8 bytes
    DWORD color;       // D3DCOLOR ARGB ?4 bytes
};

// ===[ HLSL Shader Source ]===
// Vertex shader: simple pass-through for pre-transformed screen-space vertices.
// Position is already in screen pixels with z=0, w=1.
// With D3DRS_VIEWPORTENABLE=FALSE, the GPU uses these directly.
static const char* g_vsSource =
    "struct VS_IN  { float4 Pos : POSITION; float2 Tex : TEXCOORD0; float4 Col : COLOR0; };\n"
    "struct VS_OUT { float4 Pos : POSITION; float2 Tex : TEXCOORD0; float4 Col : COLOR0; };\n"
    "VS_OUT main(VS_IN i) {\n"
    "  VS_OUT o;\n"
    "  o.Pos = i.Pos;\n"
    "  o.Tex = i.Tex;\n"
    "  o.Col = i.Col;\n"
    "  return o;\n"
    "}\n";

static const char* g_psSource =
    "sampler2D s0 : register(s0);\n"
    "struct PS_IN { float2 Tex : TEXCOORD0; float4 Col : COLOR0; };\n"
    "float4 main(PS_IN i) : COLOR0 {\n"
    "  return tex2D(s0, i.Tex) * i.Col;\n"
    "}\n";

// ===[ Helpers ]===

static inline void setVertex(SpriteVertex* sv, float px, float py, float tu, float tv,
                              DWORD d3dColor) {
    sv->x = px; sv->y = py; sv->z = 0.0f; sv->w = 1.0f;
    sv->u = tu; sv->v = tv;
    sv->color = d3dColor;
}

static inline IDirect3DDevice9* Dev(D3D9Renderer* r) {
    return (IDirect3DDevice9*)r->pd3dDevice;
}

static inline uint8_t alphaToByte(float alpha) {
    if (alpha <= 0.0f) return 0;
    if (alpha >= 1.0f) return 255;
    return (uint8_t)(alpha * 255.0f + 0.5f);
}

static bool bindRenderTarget(D3D9Renderer* dr, int32_t surfaceID) {
    IDirect3DDevice9* dev = Dev(dr);
    IDirect3DSurface9* target = NULL;
    int32_t targetW = dr->screenW;
    int32_t targetH = dr->screenH;

    if (surfaceID < 0) {
        HRESULT hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &target);
        if (FAILED(hr) || !target) return false;
        hr = dev->SetRenderTarget(0, target);
        target->Release();
        if (FAILED(hr)) return false;
        dr->currentSurfaceTarget = -1;
    } else {
        if (surfaceID >= D3D9_MAX_SURFACES || !dr->surfaceActive[surfaceID]) return false;
        target = (IDirect3DSurface9*)dr->surfaceSurfaces[surfaceID];
        if (!target) return false;

        // A render-target texture cannot remain bound for sampling while it is
        // being rendered into. Every draw path rebinds its texture as needed.
        dev->SetTexture(0, NULL);
        if (FAILED(dev->SetRenderTarget(0, target))) return false;
        targetW = dr->surfaceWidths[surfaceID];
        targetH = dr->surfaceHeights[surfaceID];
        dr->currentSurfaceTarget = surfaceID;
    }

    D3DVIEWPORT9 vp;
    vp.X = 0;
    vp.Y = 0;
    vp.Width = (DWORD)targetW;
    vp.Height = (DWORD)targetH;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    dev->SetViewport(&vp);
    dr->currentTextureIndex = -1;
    return true;
}

// Convert Butterscotch BGR color + alpha to D3DCOLOR (packed ARGB)
static inline DWORD bgrToD3DColor(uint32_t bgr, float alpha) {
    uint8_t r = (uint8_t)(bgr & 0xFF);
    uint8_t g = (uint8_t)((bgr >> 8) & 0xFF);
    uint8_t b = (uint8_t)((bgr >> 16) & 0xFF);
    uint8_t a = alphaToByte(alpha);
    return D3DCOLOR_ARGB(a, r, g, b);
}

// Convert BGR color with separate alpha components for gradient drawing
static inline void bgrToD3DColor4(uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4, float alpha,
                                   DWORD* out1, DWORD* out2, DWORD* out3, DWORD* out4) {
    *out1 = bgrToD3DColor(c1, alpha);
    *out2 = bgrToD3DColor(c2, alpha);
    *out3 = bgrToD3DColor(c3, alpha);
    *out4 = bgrToD3DColor(c4, alpha);
}

// ===[ Batch Flush ]===

static void flushBatch(D3D9Renderer* dr) {
    if (dr->quadCount == 0) return;

    IDirect3DDevice9* dev = Dev(dr);

    // Bind texture
    if (dr->currentTextureIndex >= 0 && (uint32_t)dr->currentTextureIndex < dr->textureCount) {
        dev->SetTexture(0, (IDirect3DBaseTexture9*)dr->textures[dr->currentTextureIndex]);
    } else {
        dev->SetTexture(0, (IDirect3DBaseTexture9*)dr->whiteTexture);
    }

    // Ensure vertex declaration is set ?DrawPrimitiveUP on Xbox 360 D3D9 may
    // invalidate it, so we must re-apply before every DrawPrimitiveUP call.
    dev->SetVertexDeclaration((IDirect3DVertexDeclaration9*)dr->pVertexDecl);

    // Draw using DrawPrimitiveUP ?Xbox 360 D3D9 QUADLIST works with DrawPrimitiveUP
    dev->DrawPrimitiveUP(D3DPT_QUADLIST, dr->quadCount,
                         dr->vertexData, sizeof(SpriteVertex));

    dr->quadCount = 0;
}

static bool readSurfacePixels(D3D9Renderer* dr, int32_t surfaceID, uint8_t* outRGBA) {
    if (!outRGBA || surfaceID < 0 || surfaceID >= D3D9_MAX_SURFACES ||
        !dr->surfaceActive[surfaceID]) {
        return false;
    }

    int32_t width = dr->surfaceWidths[surfaceID];
    int32_t height = dr->surfaceHeights[surfaceID];
    if (width <= 0 || height <= 0) return false;

    flushBatch(dr);
    int32_t previousTarget = dr->currentSurfaceTarget;
    bool targetChanged = previousTarget != surfaceID;
    if (targetChanged && !bindRenderTarget(dr, surfaceID)) return false;

    IDirect3DTexture9* resolveTexture = NULL;
    HRESULT hr = Dev(dr)->CreateTexture(
        width, height, 1, 0, D3DFMT_LIN_A8R8G8B8,
        D3DPOOL_DEFAULT, &resolveTexture, NULL
    );
    if (FAILED(hr) || !resolveTexture) {
        if (targetChanged) bindRenderTarget(dr, previousTarget);
        return false;
    }

    Dev(dr)->Resolve(
        D3DRESOLVE_RENDERTARGET0 | D3DRESOLVE_ALLFRAGMENTS,
        NULL, (IDirect3DBaseTexture9*)resolveTexture,
        NULL, 0, 0, NULL, 0.0f, 0, NULL
    );

    D3DLOCKED_RECT locked;
    hr = resolveTexture->LockRect(0, &locked, NULL, D3DLOCK_READONLY);
    if (FAILED(hr)) {
        resolveTexture->Release();
        if (targetChanged) bindRenderTarget(dr, previousTarget);
        return false;
    }

    size_t rowBytes = (size_t)width * 4u;
    for (int32_t row = 0; row < height; row++) {
        const uint8_t* src = (const uint8_t*)locked.pBits +
                             (size_t)(height - 1 - row) * (size_t)locked.Pitch;
        memcpy(outRGBA + (size_t)row * rowBytes, src, rowBytes);
    }

    resolveTexture->UnlockRect(0);
    resolveTexture->Release();
    if (targetChanged && !bindRenderTarget(dr, previousTarget)) return false;
    return true;
}

static bool evictTexturesFor(D3D9Renderer* dr, size_t requiredBytes) {
    while (dr->textureResidentBytes + requiredBytes > D3D9_TXTR_CACHE_BUDGET) {
        uint32_t oldestPage = UINT_MAX;
        uint32_t oldestFrame = UINT_MAX;

        for (uint32_t i = 0; i < dr->originalTexturePageCount; i++) {
            if (!dr->textures[i] || dr->textureBytes[i] == 0) continue;
            if ((int32_t)i == dr->currentTextureIndex) continue;
            if (dr->textureLastUsedFrame[i] == dr->textureFrame) continue;
            if (dr->textureLastUsedFrame[i] < oldestFrame) {
                oldestFrame = dr->textureLastUsedFrame[i];
                oldestPage = i;
            }
        }

        if (oldestPage == UINT_MAX) return false;

        flushBatch(dr);
        Dev(dr)->SetTexture(0, NULL);
        ((IDirect3DTexture9*)dr->textures[oldestPage])->Release();
        dr->textures[oldestPage] = NULL;
        dr->textureLoaded[oldestPage] = false;
        dr->textureResidentBytes -= dr->textureBytes[oldestPage];
        dr->textureBytes[oldestPage] = 0;
        DbgPrint(
            "BS: texture cache evict page=%u resident=%uMiB\n",
            (unsigned)oldestPage,
            (unsigned)(dr->textureResidentBytes / (1024u * 1024u))
        );
    }
    return true;
}

static void ensureTextureLoaded(D3D9Renderer* dr, int32_t textureIndex) {
    // Lazy-load: decode and upload a single TXTR page on first access.
    // Matches original Butterscotch gl_renderer.c:399-428 (ensureTextureLoaded).
    if (textureIndex < 0 || (uint32_t)textureIndex >= dr->textureCount) return;
    if (dr->textureLoaded[textureIndex]) {
        dr->textureLastUsedFrame[textureIndex] = dr->textureFrame;
        return;
    }
    if (dr->textureFailureFrame[textureIndex] != 0 &&
        dr->textureFrame - dr->textureFailureFrame[textureIndex] < 30) return;
    if (dr->textures[textureIndex] != nullptr) {
        dr->textureLoaded[textureIndex] = true;
        dr->textureLastUsedFrame[textureIndex] = dr->textureFrame;
        return;
    }

    DataWin* dataWin = dr->base.dataWin;
    if (dataWin == nullptr) return;

    DataWin_loadTxtrIfNeeded(dataWin, (uint32_t)textureIndex);
    Texture* txtr = &dataWin->txtr.textures[textureIndex];
    bool gm2022_5 = DataWin_isVersionAtLeast(dataWin, 2022, 5, 0, 0);
    int w, h;
    uint8_t* pixels = ImageDecoder_decodeToRgba(txtr->blobData, (size_t)txtr->blobSize, gm2022_5, &w, &h);
    if (!pixels) {
        DbgPrint("BS: Failed to decode TXTR page %u (blobSize=%u, magic=%02X%02X%02X%02X)\n",
            (unsigned)textureIndex, (unsigned)txtr->blobSize,
            txtr->blobData ? txtr->blobData[0] : 0,
            txtr->blobData ? txtr->blobData[1] : 0,
            txtr->blobData ? txtr->blobData[2] : 0,
            txtr->blobData ? txtr->blobData[3] : 0);
        dr->textureFailureFrame[textureIndex] = dr->textureFrame;
        return;
    }

    dr->textureWidths[textureIndex] = w;
    dr->textureHeights[textureIndex] = h;

    IDirect3DDevice9* dev = Dev(dr);
    IDirect3DTexture9* linearTex = NULL;
    size_t textureBytes = (size_t)w * (size_t)h * 2;
    bool cacheHasSpace = evictTexturesFor(dr, textureBytes);
    HRESULT createHr = dev->CreateTexture(
        w, h, 1, 0, D3DFMT_LIN_A4R4G4B4, D3DPOOL_DEFAULT, &linearTex, NULL
    );
    if (linearTex) {
        D3DLOCKED_RECT lr;
        linearTex->LockRect(0, &lr, NULL, 0);
        for (int y2 = 0; y2 < h; y2++) {
            uint8_t* src = pixels + y2 * w * 4;
            uint16_t* dst = (uint16_t*)((uint8_t*)lr.pBits + y2 * lr.Pitch);
            for (int x2 = 0; x2 < w; x2++) {
                uint8_t r = src[x2 * 4 + 0];
                uint8_t g = src[x2 * 4 + 1];
                uint8_t b = src[x2 * 4 + 2];
                uint8_t a = src[x2 * 4 + 3];
                if (a == 0) { r = 0; g = 0; b = 0; }
                dst[x2] = (uint16_t)(((uint16_t)(a >> 4) << 12) |
                                     ((uint16_t)(r >> 4) << 8) |
                                     ((uint16_t)(g >> 4) << 4) |
                                     (uint16_t)(b >> 4));
            }
        }
        linearTex->UnlockRect(0);
    }

    dr->textures[textureIndex] = linearTex;
    dr->textureLoaded[textureIndex] = linearTex != NULL;
    if (linearTex) {
        dr->textureFailureFrame[textureIndex] = 0;
        dr->textureBytes[textureIndex] = textureBytes;
        dr->textureResidentBytes += textureBytes;
        dr->textureLastUsedFrame[textureIndex] = dr->textureFrame;
        DbgPrint(
            "Butterscotch360-Next: D3D9 texture page=%d size=%dx%d format=A4R4G4B4\n",
            textureIndex,
            w,
            h
        );
        if (!txtr->mapped) {
            free(txtr->blobData);
            txtr->blobData = nullptr;
        }
    } else {
        dr->textureFailureFrame[textureIndex] = dr->textureFrame;
        DbgPrint(
            "BS: CreateTexture failed page=%d size=%dx%d hr=0x%08X cacheSpace=%d resident=%uMiB\n",
            textureIndex,
            w,
            h,
            (unsigned)createHr,
            cacheHasSpace ? 1 : 0,
            (unsigned)(dr->textureResidentBytes / (1024u * 1024u))
        );
    }
    free(pixels);
}

static void ensureTexture(D3D9Renderer* dr, int32_t textureIndex) {
    if (dr->currentTextureIndex != textureIndex) {
        flushBatch(dr);
        dr->currentTextureIndex = textureIndex;
    }
    // Switching first lets the LRU evict the previously bound page safely.
    ensureTextureLoaded(dr, textureIndex);
}

static SpriteVertex* allocQuad(D3D9Renderer* dr) {
    if (dr->quadCount >= D3D9_MAX_QUADS) {
        flushBatch(dr);
    }
    if (!dr->vertexData) return nullptr;
    SpriteVertex* v = (SpriteVertex*)(dr->vertexData + dr->quadCount * D3D9_VERTS_PER_QUAD * sizeof(SpriteVertex));
    dr->quadCount++;

    return v;
}

// Transform a game-space point to screen-space pixels (with pillarbox centering)
static inline void transformPoint(D3D9Renderer* dr, float inX, float inY, float* outX, float* outY) {
    float gx = dr->portOffsetX + (inX - dr->offsetX) * dr->portScaleX;
    float gy = dr->portOffsetY + (inY - dr->offsetY) * dr->portScaleY;
    if (dr->currentSurfaceTarget >= 0) {
        *outX = gx;
        *outY = gy;
    } else {
        *outX = gx * dr->renderScale + dr->renderOffsetX;
        *outY = gy * dr->renderScale + dr->renderOffsetY;
    }
}

// ===[ Vtable Implementations ]===

static void d3d9Init(Renderer* renderer, DataWin* dataWin) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);
    renderer->dataWin = dataWin;

    // Allocate CPU vertex staging buffer
    dr->vertexData = (uint8_t*)malloc(D3D9_MAX_QUADS * D3D9_VERTS_PER_QUAD * sizeof(SpriteVertex));

    // Compile shaders from source
    ID3DXBuffer* pCode = NULL;
    ID3DXBuffer* pErr = NULL;

    HRESULT hr = D3DXCompileShader(g_vsSource, (UINT)strlen(g_vsSource),
                                   NULL, NULL, "main", "vs_2_0", 0, &pCode, &pErr, NULL);
    if (FAILED(hr)) {
        OutputDebugStringA("VS compile failed: ");
        if (pErr) OutputDebugStringA((const char*)pErr->GetBufferPointer());
        if (pErr) pErr->Release();
        return;
    }
    dev->CreateVertexShader((const DWORD*)pCode->GetBufferPointer(),
                            (IDirect3DVertexShader9**)&dr->pVertexShader);
    pCode->Release();

    hr = D3DXCompileShader(g_psSource, (UINT)strlen(g_psSource),
                           NULL, NULL, "main", "ps_2_0", 0, &pCode, &pErr, NULL);
    if (FAILED(hr)) {
        OutputDebugStringA("PS compile failed: ");
        if (pErr) OutputDebugStringA((const char*)pErr->GetBufferPointer());
        if (pErr) pErr->Release();
        return;
    }
    dev->CreatePixelShader((const DWORD*)pCode->GetBufferPointer(),
                           (IDirect3DPixelShader9**)&dr->pPixelShader);
    pCode->Release();

    // Create vertex declaration
    static const D3DVERTEXELEMENT9 decl[] = {
        { 0,  0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
        D3DDECL_END()
    };
    dev->CreateVertexDeclaration(decl, (IDirect3DVertexDeclaration9**)&dr->pVertexDecl);

    // Create 1x1 white texture for primitives
    IDirect3DTexture9* whiteTex = NULL;
    dev->CreateTexture(1, 1, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &whiteTex, NULL);
    if (whiteTex) {
        D3DLOCKED_RECT lr;
        whiteTex->LockRect(0, &lr, NULL, 0);
        *(DWORD*)lr.pBits = 0xFFFFFFFF;
        whiteTex->UnlockRect(0);
    }
    dr->whiteTexture = whiteTex;

    // Allocate TXTR page slots for lazy loading (PNG decode deferred to first use).
    // Matches original Butterscotch gl_renderer.c:197-210 which only reserves
    // texture names at init and defers PNG decoding to ensureTextureLoaded.
    dr->textureCount = dataWin->txtr.count;
    dr->textures = (void**)calloc(dr->textureCount, sizeof(void*));
    dr->textureWidths = (int32_t*)calloc(dr->textureCount, sizeof(int32_t));
    dr->textureHeights = (int32_t*)calloc(dr->textureCount, sizeof(int32_t));
    dr->textureLoaded = (bool*)calloc(dr->textureCount, sizeof(bool));
    dr->textureBytes = (size_t*)calloc(dr->textureCount, sizeof(size_t));
    dr->textureLastUsedFrame = (uint32_t*)calloc(dr->textureCount, sizeof(uint32_t));
    dr->textureFailureFrame = (uint32_t*)calloc(dr->textureCount, sizeof(uint32_t));
    dr->textureResidentBytes = 0;
    dr->textureFrame = 1;

    dr->originalTexturePageCount = dataWin->txtr.count;
    dr->originalTpagCount = dataWin->tpag.count;
    dr->originalSpriteCount = dataWin->sprt.count;

    dr->currentTextureIndex = -1;
    dr->quadCount = 0;
    dr->pVertexBuffer = NULL;
    dr->vbSize = 0;

    for (int32_t i = 0; i < D3D9_MAX_SURFACES; i++) {
        dr->surfaceTextures[i] = NULL;
        dr->surfaceSurfaces[i] = NULL;
        dr->surfaceWidths[i] = 0;
        dr->surfaceHeights[i] = 0;
        dr->surfaceActive[i] = false;
    }
    dr->currentSurfaceTarget = -1;

    dr->blendEnable = true;
    dr->srcBlend = D3DBLEND_SRCALPHA;
    dr->destBlend = D3DBLEND_INVSRCALPHA;
    dr->srcBlendAlpha = D3DBLEND_SRCALPHA;
    dr->destBlendAlpha = D3DBLEND_INVSRCALPHA;
    dr->alphaTestEnable = false;
    dr->alphaTestRef = 0;
    dr->colorWriteR = true;
    dr->colorWriteG = true;
    dr->colorWriteB = true;
    dr->colorWriteA = true;
    dr->fogEnable = false;
    dr->fogColor = 0;
    dr->fogStart = 0.0f;
    dr->fogEnd = 0.0f;
    dr->runner = NULL;
}

static void d3d9Destroy(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    for (uint32_t i = 0; i < dr->textureCount; i++) {
        if (dr->textures[i]) ((IDirect3DTexture9*)dr->textures[i])->Release();
    }
    free(dr->textures);
    free(dr->textureWidths);
    free(dr->textureHeights);
    free(dr->textureLoaded);
    free(dr->textureBytes);
    free(dr->textureLastUsedFrame);
    free(dr->textureFailureFrame);
    free(dr->vertexData);
    if (dr->whiteTexture) ((IDirect3DTexture9*)dr->whiteTexture)->Release();
    if (dr->pVertexBuffer) ((IDirect3DVertexBuffer9*)dr->pVertexBuffer)->Release();
    if (dr->pVertexShader) ((IDirect3DVertexShader9*)dr->pVertexShader)->Release();
    if (dr->pPixelShader) ((IDirect3DPixelShader9*)dr->pPixelShader)->Release();
    if (dr->pVertexDecl) ((IDirect3DVertexDeclaration9*)dr->pVertexDecl)->Release();
    for (int32_t i = 0; i < D3D9_MAX_SURFACES; i++) {
        if (dr->surfaceSurfaces[i]) ((IDirect3DSurface9*)dr->surfaceSurfaces[i])->Release();
        if (dr->surfaceTextures[i]) ((IDirect3DTexture9*)dr->surfaceTextures[i])->Release();
    }
    free(dr);
}

static void d3d9BeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);

    dr->textureFrame++;
    if (dr->textureFrame == 0) dr->textureFrame = 1;

    dr->gameW = gameW;
    dr->gameH = gameH;
    dr->screenW = windowW;
    dr->screenH = windowH;

    // Compute uniform scale to fit game in screen with letterboxing
    float scaleX = (float)windowW / (float)gameW;
    float scaleY = (float)windowH / (float)gameH;
    dr->renderScale = (scaleX < scaleY) ? scaleX : scaleY;
    dr->renderOffsetX = ((float)windowW - (float)gameW * dr->renderScale) * 0.5f;
    dr->renderOffsetY = ((float)windowH - (float)gameH * dr->renderScale) * 0.5f;

    dev->BeginScene();

    // Room/view rendering always targets the real application surface. The
    // surface is composited to the 720p backbuffer in endFrameEnd, after the
    // Post Draw pass and before Draw GUI, matching the current GL lifecycle.
    int32_t appSurfaceID = renderer->runner ? renderer->runner->applicationSurfaceId : -1;
    if (!bindRenderTarget(dr, appSurfaceID)) bindRenderTarget(dr, RENDER_TARGET_HOST_FRAMEBUFFER);
    dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);

    // Set shared render state
    dev->SetVertexShader((IDirect3DVertexShader9*)dr->pVertexShader);
    dev->SetPixelShader((IDirect3DPixelShader9*)dr->pPixelShader);
    dev->SetVertexDeclaration((IDirect3DVertexDeclaration9*)dr->pVertexDecl);


    // Alpha blending
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    // No depth testing for 2D
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    // Disable viewport transform ?we use pre-transformed screen-space vertices
    dev->SetRenderState(D3DRS_VIEWPORTENABLE, FALSE);

    // Point filtering ?pixel-perfect for 2D sprite games like Undertale
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    D3D9Renderer_applyGpuState(dr);
}

static void d3d9EndFrameInit(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    flushBatch(dr);

    if (renderer->runner && renderer->runner->usingAppSurface &&
        !renderer->runner->appSurfaceAutoDraw) {
        bindRenderTarget(dr, RENDER_TARGET_HOST_FRAMEBUFFER);
    }
}

static void d3d9EndFrameEnd(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);

    if (renderer->runner && renderer->runner->usingAppSurface &&
        !renderer->runner->appSurfaceAutoDraw) {
        return;
    }

    int32_t appSurfaceID = renderer->runner ? renderer->runner->applicationSurfaceId : -1;
    if (appSurfaceID < 0 || appSurfaceID >= D3D9_MAX_SURFACES ||
        !dr->surfaceActive[appSurfaceID]) {
        return;
    }

    bindRenderTarget(dr, RENDER_TARGET_HOST_FRAMEBUFFER);
    Dev(dr)->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);

    float savedPortScaleX = dr->portScaleX;
    float savedPortScaleY = dr->portScaleY;
    float savedOffsetX = dr->offsetX;
    float savedOffsetY = dr->offsetY;
    float savedPortOffsetX = dr->portOffsetX;
    float savedPortOffsetY = dr->portOffsetY;
    bool savedBlendEnable = dr->blendEnable;

    dr->portScaleX = 1.0f;
    dr->portScaleY = 1.0f;
    dr->offsetX = 0.0f;
    dr->offsetY = 0.0f;
    dr->portOffsetX = 0.0f;
    dr->portOffsetY = 0.0f;
    Dev(dr)->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d3d9DrawSurface(renderer, appSurfaceID, 0, 0, -1, -1,
                    0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFF, 1.0f);
    Dev(dr)->SetRenderState(D3DRS_ALPHABLENDENABLE, savedBlendEnable ? TRUE : FALSE);

    dr->portScaleX = savedPortScaleX;
    dr->portScaleY = savedPortScaleY;
    dr->offsetX = savedOffsetX;
    dr->offsetY = savedOffsetY;
    dr->portOffsetX = savedPortOffsetX;
    dr->portOffsetY = savedPortOffsetY;
}

void D3D9Renderer_present(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);
    Dev(dr)->EndScene();
    Dev(dr)->Present(NULL, NULL, NULL, NULL);
}

static void d3d9BeginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH,
                           int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);
    (void)viewAngle;

    int32_t targetW = dr->screenW;
    int32_t targetH = dr->screenH;
    if (dr->currentSurfaceTarget >= 0 && dr->currentSurfaceTarget < D3D9_MAX_SURFACES &&
        dr->surfaceActive[dr->currentSurfaceTarget]) {
        targetW = dr->surfaceWidths[dr->currentSurfaceTarget];
        targetH = dr->surfaceHeights[dr->currentSurfaceTarget];
    }

    D3DVIEWPORT9 vp;
    vp.X = 0;
    vp.Y = 0;
    vp.Width = (DWORD)targetW;
    vp.Height = (DWORD)targetH;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    dev->SetViewport(&vp);

    // Store view transform for point mapping
    dr->offsetX = (float)viewX;
    dr->offsetY = (float)viewY;
    dr->portScaleX = (float)portW / (float)viewW;
    dr->portScaleY = (float)portH / (float)viewH;
    dr->portOffsetX = (float)portX;
    dr->portOffsetY = (float)portY;

    // No projection matrix needed ?vertices are pre-transformed screen coords.
    // D3DRS_VIEWPORTENABLE=FALSE means the GPU uses positions directly as pixels.
}

static void d3d9BeginGUI(Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, int32_t targetSurfaceId) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);

    if (!bindRenderTarget(dr, targetSurfaceId)) return;

    dr->savedPortScaleX = dr->portScaleX;
    dr->savedPortScaleY = dr->portScaleY;
    dr->savedOffsetX = dr->offsetX;
    dr->savedOffsetY = dr->offsetY;
    dr->savedPortOffsetX = dr->portOffsetX;
    dr->savedPortOffsetY = dr->portOffsetY;

    dr->portScaleX = (float)portW / (float)guiW;
    dr->portScaleY = (float)portH / (float)guiH;
    dr->offsetX = 0.0f;
    dr->offsetY = 0.0f;
    dr->portOffsetX = (float)portX;
    dr->portOffsetY = (float)portY;
    dr->inGUI = true;
}

static void d3d9EndGUI(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);

    dr->portScaleX = dr->savedPortScaleX;
    dr->portScaleY = dr->savedPortScaleY;
    dr->offsetX = dr->savedOffsetX;
    dr->offsetY = dr->savedOffsetY;
    dr->portOffsetX = dr->savedPortOffsetX;
    dr->portOffsetY = dr->savedPortOffsetY;
    dr->inGUI = false;
}

static void d3d9SetGuiProjection(Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portW, int32_t portH, bool renderingToUserSurface) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    (void)renderingToUserSurface;
    flushBatch(dr);
    dr->portScaleX = guiW > 0 ? (float)portW / (float)guiW : 1.0f;
    dr->portScaleY = guiH > 0 ? (float)portH / (float)guiH : 1.0f;
    dr->offsetX = 0.0f;
    dr->offsetY = 0.0f;
}

static void d3d9ApplyProjection(Renderer* renderer, const Matrix4f* viewMatrix, const Matrix4f* projectionMatrix) {
    (void)renderer;
    (void)viewMatrix;
    (void)projectionMatrix;
}

static void d3d9EndView(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);
}

// ===[ Sprite Drawing ]===

static void d3d9DrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y,
                            float originX, float originY, float xscale, float yscale,
                            float angleDeg, uint32_t color, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int32_t texPageId = tpag->texturePageId;
    if (0 > texPageId || (uint32_t)texPageId >= dr->textureCount) return;

    ensureTexture(dr, texPageId);
    if (!dr->textures[texPageId]) return;

    float texW = (float)dr->textureWidths[texPageId];
    float texH = (float)dr->textureHeights[texPageId];
    if (texW <= 0 || texH <= 0) return;

    // UV coordinates on the texture atlas
    float u0 = (float)tpag->sourceX / texW;
    float v0 = (float)tpag->sourceY / texH;
    float u1 = (float)(tpag->sourceX + tpag->sourceWidth) / texW;
    float v1 = (float)(tpag->sourceY + tpag->sourceHeight) / texH;

    // Quad corners in local space (before transform)
    // Use targetWidth/Height (draw size in bounding rect), not sourceWidth/Height (texture sample size).
    // They differ when the texture was auto-downscaled by GMS to fit a texture page.
    float localX0 = (float)tpag->targetX - originX;
    float localY0 = (float)tpag->targetY - originY;
    float localX1 = localX0 + (float)tpag->targetWidth;
    float localY1 = localY0 + (float)tpag->targetHeight;

    // Scale
    localX0 *= xscale; localY0 *= yscale;
    localX1 *= xscale; localY1 *= yscale;

    DWORD d3dColor = bgrToD3DColor(color, alpha);

    // Build 4 corners
    float cx[4], cy[4];
    if (angleDeg != 0.0f) {
        float rad = -angleDeg * (3.14159265f / 180.0f);
        float cosA = cosf(rad);
        float sinA = sinf(rad);

        float lx[4] = { localX0, localX1, localX1, localX0 };
        float ly[4] = { localY0, localY0, localY1, localY1 };
        for (int i = 0; i < 4; i++) {
            cx[i] = lx[i] * cosA - ly[i] * sinA;
            cy[i] = lx[i] * sinA + ly[i] * cosA;
        }
    } else {
        cx[0] = localX0; cy[0] = localY0;
        cx[1] = localX1; cy[1] = localY0;
        cx[2] = localX1; cy[2] = localY1;
        cx[3] = localX0; cy[3] = localY1;
    }

    // Transform to screen space
    SpriteVertex* v = allocQuad(dr);
    float sx, sy;
    for (int i = 0; i < 4; i++) {
        transformPoint(dr, x + cx[i], y + cy[i], &sx, &sy);
        v[i].x = sx;
        v[i].y = sy;
        v[i].z = 0.0f;
        v[i].w = 1.0f;
        v[i].color = d3dColor;
    }
    v[0].u = u0; v[0].v = v0;
    v[1].u = u1; v[1].v = v0;
    v[2].u = u1; v[2].v = v1;
    v[3].u = u0; v[3].v = v1;
}

static void d3d9DrawSpritePart(Renderer* renderer, int32_t tpagIndex,
                                int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH,
                                float x, float y, float xscale, float yscale,
                                float angleDeg, float pivotX, float pivotY,
                                uint32_t color, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int32_t texPageId = tpag->texturePageId;
    if (0 > texPageId || (uint32_t)texPageId >= dr->textureCount) return;

    ensureTexture(dr, texPageId);
    if (!dr->textures[texPageId]) return;

    float texW = (float)dr->textureWidths[texPageId];
    float texH = (float)dr->textureHeights[texPageId];
    if (texW <= 0 || texH <= 0) return;

    float u0 = (float)(tpag->sourceX + srcOffX) / texW;
    float v0 = (float)(tpag->sourceY + srcOffY) / texH;
    float u1 = (float)(tpag->sourceX + srcOffX + srcW) / texW;
    float v1 = (float)(tpag->sourceY + srcOffY + srcH) / texH;

    DWORD d3dColor = bgrToD3DColor(color, alpha);

    float cx[4], cy[4];
    if (angleDeg == 0.0f) {
        cx[0] = x;                        cy[0] = y;
        cx[1] = x + (float)srcW * xscale; cy[1] = y;
        cx[2] = x + (float)srcW * xscale; cy[2] = y + (float)srcH * yscale;
        cx[3] = x;                        cy[3] = y + (float)srcH * yscale;
    } else {
        float rad = -angleDeg * (3.14159265f / 180.0f);
        float cosA = cosf(rad);
        float sinA = sinf(rad);
        float qx[4] = { x, x + (float)srcW * xscale, x + (float)srcW * xscale, x };
        float qy[4] = { y, y, y + (float)srcH * yscale, y + (float)srcH * yscale };
        for (int i = 0; i < 4; i++) {
            float dx = qx[i] - pivotX;
            float dy = qy[i] - pivotY;
            cx[i] = cosA * dx - sinA * dy + pivotX;
            cy[i] = sinA * dx + cosA * dy + pivotY;
        }
    }

    SpriteVertex* v = allocQuad(dr);
    float sx, sy;
    for (int i = 0; i < 4; i++) {
        transformPoint(dr, cx[i], cy[i], &sx, &sy);
        v[i].x = sx;
        v[i].y = sy;
        v[i].z = 0.0f;
        v[i].w = 1.0f;
        v[i].color = d3dColor;
    }
    v[0].u = u0; v[0].v = v0;
    v[1].u = u1; v[1].v = v0;
    v[2].u = u1; v[2].v = v1;
    v[3].u = u0; v[3].v = v1;
}

// Draws a textured quad with 4 arbitrary corner positions (used for perspective/distorted sprites).
// Matches original gl_renderer.c:565 glDrawSpritePos: corners are TL, TR, BR, BL.
static void d3d9DrawSpritePos(Renderer* renderer, int32_t tpagIndex,
                               float x1, float y1, float x2, float y2,
                               float x3, float y3, float x4, float y4,
                               float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int32_t texPageId = tpag->texturePageId;
    if (0 > texPageId || (uint32_t)texPageId >= dr->textureCount) return;

    ensureTexture(dr, texPageId);

    float texW = (float)dr->textureWidths[texPageId];
    float texH = (float)dr->textureHeights[texPageId];
    if (texW <= 0 || texH <= 0) return;

    float u0 = (float)tpag->sourceX / texW;
    float v0 = (float)tpag->sourceY / texH;
    float u1 = (float)(tpag->sourceX + tpag->sourceWidth) / texW;
    float v1 = (float)(tpag->sourceY + tpag->sourceHeight) / texH;

    SpriteVertex* v = allocQuad(dr);
    float sx, sy;
    // Corner order: (x1,y1)=TL, (x2,y2)=TR, (x3,y3)=BR, (x4,y4)=BL
    transformPoint(dr, x1, y1, &sx, &sy);
    v[0].x = sx; v[0].y = sy; v[0].z = 0.0f; v[0].w = 1.0f;
    transformPoint(dr, x2, y2, &sx, &sy);
    v[1].x = sx; v[1].y = sy; v[1].z = 0.0f; v[1].w = 1.0f;
    transformPoint(dr, x3, y3, &sx, &sy);
    v[2].x = sx; v[2].y = sy; v[2].z = 0.0f; v[2].w = 1.0f;
    transformPoint(dr, x4, y4, &sx, &sy);
    v[3].x = sx; v[3].y = sy; v[3].z = 0.0f; v[3].w = 1.0f;

    v[0].u = u0; v[0].v = v0;
    v[1].u = u1; v[1].v = v0;
    v[2].u = u1; v[2].v = v1;
    v[3].u = u0; v[3].v = v1;

    DWORD d3dColor = bgrToD3DColor(0x00FFFFFF, alpha);  // white tint
    for (int i = 0; i < 4; i++) {
        v[i].color = d3dColor;
    }
}

static void d3d9DrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2,
                               uint32_t color, float alpha, bool outline) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;

    if (outline) {
        // Draw 4 lines as thin rectangles
        float lw = 1.0f;
        d3d9DrawRectangle(renderer, x1, y1, x2, y1 + lw, color, alpha, false); // top
        d3d9DrawRectangle(renderer, x1, y2 - lw, x2, y2, color, alpha, false); // bottom
        d3d9DrawRectangle(renderer, x1, y1, x1 + lw, y2, color, alpha, false); // left
        d3d9DrawRectangle(renderer, x2 - lw, y1, x2, y2, color, alpha, false); // right
        return;
    }

    ensureTexture(dr, -1); // white texture

    DWORD d3dColor = bgrToD3DColor(color, alpha);
    SpriteVertex* v = allocQuad(dr);

    float sx0, sy0, sx1, sy1;
    transformPoint(dr, x1, y1, &sx0, &sy0);
    transformPoint(dr, x2, y2, &sx1, &sy1);

    setVertex(&v[0], sx0, sy0, 0, 0, d3dColor);
    setVertex(&v[1], sx1, sy0, 1, 0, d3dColor);
    setVertex(&v[2], sx1, sy1, 1, 1, d3dColor);
    setVertex(&v[3], sx0, sy1, 0, 1, d3dColor);
}

// Draws a rectangle with 4 corner colors (gradient). Matches original gl_renderer.c:729 glDrawRectangleColor.
// Filled: GML adds +1 to width/height (matches original gl_renderer.c:774,778,782).
static void d3d9DrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2,
                               float width, uint32_t color1, uint32_t color2, float alpha);
static void d3d9DrawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2,
                                    uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4,
                                    float alpha, bool outline) {
    if (outline) {
        // Draw 4 one-pixel-wide edges with per-edge color gradients
        d3d9DrawLineColor(renderer, x1, y1, x2, y1, 1.0f, color1, color2, alpha); // top
        d3d9DrawLineColor(renderer, x2, y1, x2, y2, 1.0f, color2, color3, alpha); // right
        d3d9DrawLineColor(renderer, x2, y2, x1, y2, 1.0f, color3, color4, alpha); // bottom
        d3d9DrawLineColor(renderer, x1, y2, x1, y1, 1.0f, color4, color1, alpha); // left
        return;
    }

    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    ensureTexture(dr, -1); // white texture

    DWORD dc1, dc2, dc3, dc4;
    bgrToD3DColor4(color1, color2, color3, color4, alpha, &dc1, &dc2, &dc3, &dc4);

    SpriteVertex* v = allocQuad(dr);
    float sx, sy;
    // Vertex order: TL, TR, BR, BL ?matches original gl_renderer.c:770-783
    // GML filled-rect convention: +1 to right and bottom edges
    transformPoint(dr, x1,    y1,    &sx, &sy); setVertex(&v[0], sx, sy, 0.5f, 0.5f, dc1);
    transformPoint(dr, x2+1,  y1,    &sx, &sy); setVertex(&v[1], sx, sy, 0.5f, 0.5f, dc2);
    transformPoint(dr, x2+1,  y2+1,  &sx, &sy); setVertex(&v[2], sx, sy, 0.5f, 0.5f, dc3);
    transformPoint(dr, x1,    y2+1,  &sx, &sy); setVertex(&v[3], sx, sy, 0.5f, 0.5f, dc4);
}

static void d3d9DrawLine(Renderer* renderer, float x1, float y1, float x2, float y2,
                          float width, uint32_t color, float alpha) {
    // Draw line as a thin rotated rectangle
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;

    float nx = -dy / len * width * 0.5f;
    float ny = dx / len * width * 0.5f;

    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    ensureTexture(dr, -1);
    DWORD d3dColor = bgrToD3DColor(color, alpha);

    SpriteVertex* v = allocQuad(dr);
    float sx, sy;

    transformPoint(dr, x1 + nx, y1 + ny, &sx, &sy); setVertex(&v[0], sx, sy, 0, 0, d3dColor);
    transformPoint(dr, x2 + nx, y2 + ny, &sx, &sy); setVertex(&v[1], sx, sy, 1, 0, d3dColor);
    transformPoint(dr, x2 - nx, y2 - ny, &sx, &sy); setVertex(&v[2], sx, sy, 1, 1, d3dColor);
    transformPoint(dr, x1 - nx, y1 - ny, &sx, &sy); setVertex(&v[3], sx, sy, 0, 1, d3dColor);
}

static void d3d9DrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2,
                               float width, uint32_t color1, uint32_t color2, float alpha) {
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;

    float nx = -dy / len * width * 0.5f;
    float ny = dx / len * width * 0.5f;

    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    ensureTexture(dr, -1);
    DWORD dc1 = bgrToD3DColor(color1, alpha);
    DWORD dc2 = bgrToD3DColor(color2, alpha);

    SpriteVertex* v = allocQuad(dr);
    float sx, sy;

    transformPoint(dr, x1 + nx, y1 + ny, &sx, &sy); setVertex(&v[0], sx, sy, 0, 0, dc1);
    transformPoint(dr, x2 + nx, y2 + ny, &sx, &sy); setVertex(&v[1], sx, sy, 1, 0, dc2);
    transformPoint(dr, x2 - nx, y2 - ny, &sx, &sy); setVertex(&v[2], sx, sy, 1, 1, dc2);
    transformPoint(dr, x1 - nx, y1 - ny, &sx, &sy); setVertex(&v[3], sx, sy, 0, 1, dc1);
}

// Draws a triangle. Matches original gl_renderer.c:790 glDrawTriangle.
// Outline: 3 lines with renderer->drawColor. Filled: D3DPT_TRIANGLELIST with white texture.
static void d3d9DrawTriangle(Renderer* renderer, float x1, float y1, float x2, float y2,
                              float x3, float y3, uint32_t color1, uint32_t color2, uint32_t color3, float alpha, bool outline) {
    if (outline) {
        d3d9DrawLine(renderer, x1, y1, x2, y2, 1.0f, color1, alpha);
        d3d9DrawLine(renderer, x2, y2, x3, y3, 1.0f, color2, alpha);
        d3d9DrawLine(renderer, x3, y3, x1, y1, 1.0f, color3, alpha);
        return;
    }

    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);

    // Flush any pending quads ?triangles use a different primitive topology
    flushBatch(dr);

    // Bind white texture so triangle color comes from vertex colors
    dev->SetTexture(0, (IDirect3DBaseTexture9*)dr->whiteTexture);

    DWORD dc1 = bgrToD3DColor(color1, alpha);
    DWORD dc2 = bgrToD3DColor(color2, alpha);
    DWORD dc3 = bgrToD3DColor(color3, alpha);

    // 3 vertices, triangle list ?matches original gl_renderer.c:806-810
    SpriteVertex verts[3];
    float sx, sy;
    transformPoint(dr, x1, y1, &sx, &sy); setVertex(&verts[0], sx, sy, 0.5f, 0.5f, dc1);
    transformPoint(dr, x2, y2, &sx, &sy); setVertex(&verts[1], sx, sy, 0.5f, 0.5f, dc2);
    transformPoint(dr, x3, y3, &sx, &sy); setVertex(&verts[2], sx, sy, 0.5f, 0.5f, dc3);

    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, verts, sizeof(SpriteVertex));

    // DrawPrimitiveUP may invalidate vertex declaration on Xbox 360 D3D9.
    // Invalidate currentTextureIndex so the next batch rebinds the correct texture,
    // and let flushBatch handle the vertex declaration restore.
    dr->currentTextureIndex = -1;
}

static void d3d9DrawText(Renderer* renderer, const char* text, float x, float y,
                          float xscale, float yscale, float angleDeg, float lineSeparation) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;
    int32_t fontIndex = renderer->drawFont;

    {
        static int _drawTextDebug = 0;
        if (_drawTextDebug < 3) {
            DbgPrint("BS: d3d9DrawText fontIdx=%d fontCount=%d text='%.30s' x=%.1f y=%.1f\n",
                fontIndex, (int)dw->font.count, text ? text : "(null)", x, y);
            _drawTextDebug++;
        }
    }

    if (0 > fontIndex || (uint32_t)fontIndex >= dw->font.count) {
        static int _err1 = 0;
        if (_err1 < 3) { DbgPrint("BS: d3d9DrawText EARLY RETURN: fontIndex=%d count=%d\n", fontIndex, (int)dw->font.count); _err1++; }
        return;
    }

    Font* font = &dw->font.fonts[fontIndex];
    uint32_t color = renderer->drawColor;
    float alpha = renderer->drawAlpha;

    // Resolve font texture state (matches GL's glResolveFontState)
    bool isSpriteFont = font->isSpriteFont;
    TexturePageItem* fontTpag = nullptr;
    float texW = 0, texH = 0;

    if (!isSpriteFont) {
        int32_t fontTpagIndex = font->tpagIndex;
        if (0 > fontTpagIndex) return;

        fontTpag = &dw->tpag.items[fontTpagIndex];
        int16_t pageId = fontTpag->texturePageId;
        if (0 > pageId || dr->textureCount <= (uint32_t)pageId) return;

        ensureTexture(dr, (int32_t)pageId);
        if (!dr->textures[pageId]) return;

        texW = (float)dr->textureWidths[pageId];
        texH = (float)dr->textureHeights[pageId];
        if (texW <= 0 || texH <= 0) return;
    }

    DWORD d3dColor = bgrToD3DColor(color, alpha);

    int32_t textLen = (int32_t)strlen(text);

    // Count lines
    int32_t lineCount = TextUtils_countLines(text, textLen);

    float lineStride = lineSeparation < 0.0f
        ? TextUtils_lineStride(font)
        : lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f);

    // Vertical alignment offset
    float totalHeight = (float)lineCount * lineStride;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float fontScaleX = xscale * font->scaleX;
    float fontScaleY = yscale * font->scaleY;

    // Build rotation transform (if needed)
    float cosA = 1.0f, sinA = 0.0f;
    bool hasRotation = (angleDeg != 0.0f);
    if (hasRotation) {
        float rad = -angleDeg * (3.14159265f / 180.0f);
        cosA = cosf(rad);
        sinA = sinf(rad);
    }

    // Iterate through lines
    float cursorY = valignOffset - (float)font->ascenderOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        // Find end of current line
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(text[lineEnd])) {
            lineEnd++;
        }
        int32_t lineLen = lineEnd - lineStart;

        // Horizontal alignment offset for this line
        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;

        // Render each glyph
        int32_t pos = 0;
        while (lineLen > pos) {
            uint16_t ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (!glyph) continue;
            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) {
                cursorX += glyph->shift;
                continue;
            }

            float u0, v0, u1, v1;
            float localX0, localY0;

            if (isSpriteFont) {
                // Sprite font: each glyph is a separate texture page item
                // Matches GL's glResolveGlyph for sprite fonts
                Sprite* sprite = &dw->sprt.sprites[font->spriteIndex];
                int32_t glyphIndex = (int32_t)(glyph - font->glyphs);
                if (0 > glyphIndex || glyphIndex >= (int32_t)sprite->textureCount) {
                    cursorX += glyph->shift;
                    continue;
                }

                int32_t tpagIdx = sprite->tpagIndices[glyphIndex];
                if (0 > tpagIdx) {
                    cursorX += glyph->shift;
                    continue;
                }

                TexturePageItem* glyphTpag = &dw->tpag.items[tpagIdx];
                int16_t pid = glyphTpag->texturePageId;
                if (0 > pid || (uint32_t)pid >= dr->textureCount) {
                    cursorX += glyph->shift;
                    continue;
                }

                ensureTexture(dr, (int32_t)pid);
                if (!dr->textures[pid]) {
                    cursorX += glyph->shift;
                    continue;
                }

                float glyphTexW = (float)dr->textureWidths[pid];
                float glyphTexH = (float)dr->textureHeights[pid];

                u0 = (float)glyphTpag->sourceX / glyphTexW;
                v0 = (float)glyphTpag->sourceY / glyphTexH;
                u1 = (float)(glyphTpag->sourceX + glyphTpag->sourceWidth) / glyphTexW;
                v1 = (float)(glyphTpag->sourceY + glyphTpag->sourceHeight) / glyphTexH;

                localX0 = cursorX + (float)glyph->offset;
                localY0 = cursorY + (float)(int32_t)glyphTpag->targetY - (float)font->spriteOriginYAdjust;
            } else {
                // Regular atlas font
                u0 = (float)(fontTpag->sourceX + glyph->sourceX) / texW;
                v0 = (float)(fontTpag->sourceY + glyph->sourceY) / texH;
                u1 = (float)(fontTpag->sourceX + glyph->sourceX + glyph->sourceWidth) / texW;
                v1 = (float)(fontTpag->sourceY + glyph->sourceY + glyph->sourceHeight) / texH;

                localX0 = cursorX + glyph->offset;
                localY0 = cursorY;
            }

            // Local quad size
            float localX1 = localX0 + (float)glyph->sourceWidth;
            float localY1 = localY0 + (float)glyph->sourceHeight;

            // Scale
            float sx0 = localX0 * fontScaleX;
            float sy0 = localY0 * fontScaleY;
            float sx1 = localX1 * fontScaleX;
            float sy1 = localY1 * fontScaleY;

            // Build 4 corners (with optional rotation)
            float cx[4], cy[4];
            if (hasRotation) {
                float lx[4] = { sx0, sx1, sx1, sx0 };
                float ly[4] = { sy0, sy0, sy1, sy1 };
                for (int i = 0; i < 4; i++) {
                    cx[i] = lx[i] * cosA - ly[i] * sinA;
                    cy[i] = lx[i] * sinA + ly[i] * cosA;
                }
            } else {
                cx[0] = sx0; cy[0] = sy0;
                cx[1] = sx1; cy[1] = sy0;
                cx[2] = sx1; cy[2] = sy1;
                cx[3] = sx0; cy[3] = sy1;
            }

            SpriteVertex* v = allocQuad(dr);
            float screenX, screenY;
            for (int i = 0; i < 4; i++) {
                transformPoint(dr, x + cx[i], y + cy[i], &screenX, &screenY);
                v[i].x = screenX;
                v[i].y = screenY;
                v[i].z = 0.0f;
                v[i].w = 1.0f;
                v[i].color = d3dColor;
            }
            v[0].u = u0; v[0].v = v0;
            v[1].u = u1; v[1].v = v0;
            v[2].u = u1; v[2].v = v1;
            v[3].u = u0; v[3].v = v1;

            // Advance cursor (shift + kerning)
            cursorX += glyph->shift;
            if (lineLen > pos) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }

        cursorY += lineStride;

        // Advance past the newline
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(text, lineEnd, textLen);
        } else {
            lineStart = lineEnd;
        }
    }
}

static void d3d9DrawTextColor(Renderer* renderer, const char* text, float x, float y,
                               float xscale, float yscale, float angleDeg,
                               int32_t c1, int32_t c2, int32_t c3, int32_t c4, float alpha, float lineSeparation) {
    uint32_t savedColor = renderer->drawColor;
    float savedAlpha = renderer->drawAlpha;

    renderer->drawColor = (uint32_t)c1;
    renderer->drawAlpha = alpha;
    d3d9DrawText(renderer, text, x, y, xscale, yscale, angleDeg, lineSeparation);
    renderer->drawColor = savedColor;
    renderer->drawAlpha = savedAlpha;
}

static void d3d9Flush(Renderer* renderer) {
    flushBatch((D3D9Renderer*)renderer);
}

static uint32_t findOrAllocTexturePageSlot(D3D9Renderer* dr) {
    for (uint32_t i = dr->originalTexturePageCount; dr->textureCount > i; i++) {
        if (dr->textures[i] == NULL) return i;
    }
    uint32_t newPageId = dr->textureCount;
    dr->textureCount++;
    dr->textures = (void**)realloc(dr->textures, dr->textureCount * sizeof(void*));
    dr->textureWidths = (int32_t*)realloc(dr->textureWidths, dr->textureCount * sizeof(int32_t));
    dr->textureHeights = (int32_t*)realloc(dr->textureHeights, dr->textureCount * sizeof(int32_t));
    dr->textureLoaded = (bool*)realloc(dr->textureLoaded, dr->textureCount * sizeof(bool));
    dr->textureBytes = (size_t*)realloc(dr->textureBytes, dr->textureCount * sizeof(size_t));
    dr->textureLastUsedFrame = (uint32_t*)realloc(dr->textureLastUsedFrame, dr->textureCount * sizeof(uint32_t));
    dr->textureFailureFrame = (uint32_t*)realloc(dr->textureFailureFrame, dr->textureCount * sizeof(uint32_t));
    dr->textures[newPageId] = NULL;
    dr->textureWidths[newPageId] = 0;
    dr->textureHeights[newPageId] = 0;
    dr->textureLoaded[newPageId] = false;
    dr->textureBytes[newPageId] = 0;
    dr->textureLastUsedFrame[newPageId] = dr->textureFrame;
    dr->textureFailureFrame[newPageId] = 0;
    return newPageId;
}

static uint32_t findOrAllocTpagSlot(DataWin* dw, uint32_t originalTpagCount) {
    for (uint32_t i = originalTpagCount; dw->tpag.count > i; i++) {
        if (dw->tpag.items[i].texturePageId == -1) return i;
    }
    uint32_t newIndex = dw->tpag.count;
    dw->tpag.count++;
    dw->tpag.items = (TexturePageItem*)realloc(dw->tpag.items, dw->tpag.count * sizeof(TexturePageItem));
    memset(&dw->tpag.items[newIndex], 0, sizeof(TexturePageItem));
    dw->tpag.items[newIndex].texturePageId = -1;
    return newIndex;
}

static int32_t d3d9CreateSpriteFromSurface(Renderer* renderer, int32_t surfaceID, int32_t x, int32_t y,
                                            int32_t w, int32_t h, bool removeback,
                                            bool smooth, int32_t xorig, int32_t yorig) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;
    IDirect3DDevice9* dev = Dev(dr);

    (void)smooth;
    if (0 >= w || 0 >= h) return -1;

    flushBatch(dr);

    if (surfaceID < 0 || surfaceID >= D3D9_MAX_SURFACES ||
        !dr->surfaceActive[surfaceID]) return -1;
    int32_t srcW = dr->surfaceWidths[surfaceID];
    int32_t srcH = dr->surfaceHeights[surfaceID];
    if (x < 0 || y < 0 || x + w > srcW || y + h > srcH) return -1;

    size_t fullBytes = (size_t)srcW * (size_t)srcH * 4u;
    uint8_t* fullPixels = (uint8_t*)malloc(fullBytes);
    if (!fullPixels) return -1;
    if (!readSurfacePixels(dr, surfaceID, fullPixels)) {
        free(fullPixels);
        return -1;
    }

    size_t spriteRowBytes = (size_t)w * 4u;
    uint8_t* pixels = (uint8_t*)malloc(spriteRowBytes * (size_t)h);
    if (!pixels) {
        free(fullPixels);
        return -1;
    }
    for (int32_t row = 0; row < h; row++) {
        const uint8_t* srcRow = fullPixels +
            ((size_t)(y + row) * (size_t)srcW + (size_t)x) * 4u;
        memcpy(pixels + (size_t)row * spriteRowBytes, srcRow, spriteRowBytes);
    }
    free(fullPixels);

    if (removeback) {
        uint32_t bottomLeft = *(uint32_t*)(pixels + (h - 1) * w * 4);
        uint8_t bkR = bottomLeft & 0xFF;
        uint8_t bkG = (bottomLeft >> 8) & 0xFF;
        uint8_t bkB = (bottomLeft >> 16) & 0xFF;
        uint8_t bkA = (bottomLeft >> 24) & 0xFF;
        for (int32_t i = 0; i < w * h; i++) {
            uint8_t* p = pixels + i * 4;
            if (p[0] == bkR && p[1] == bkG && p[2] == bkB && p[3] == bkA) {
                p[3] = 0;
            }
        }
    }

    IDirect3DTexture9* newTex = NULL;
    HRESULT hr = dev->CreateTexture(w, h, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &newTex, NULL);
    if (FAILED(hr) || !newTex) {
        free(pixels);
        return -1;
    }

    D3DLOCKED_RECT lr2;
    hr = newTex->LockRect(0, &lr2, NULL, 0);
    if (FAILED(hr)) {
        newTex->Release();
        free(pixels);
        return -1;
    }
    for (int32_t row = 0; row < h; row++) {
        memcpy((uint8_t*)lr2.pBits + row * lr2.Pitch, pixels + row * w * 4, w * 4);
    }
    newTex->UnlockRect(0);
    free(pixels);

    uint32_t pageId = findOrAllocTexturePageSlot(dr);
    dr->textures[pageId] = newTex;
    dr->textureWidths[pageId] = w;
    dr->textureHeights[pageId] = h;
    dr->textureLoaded[pageId] = true; // Already uploaded, skip lazy load

    uint32_t tpagIndex = findOrAllocTpagSlot(dw, dr->originalTpagCount);
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    tpag->sourceX = 0;
    tpag->sourceY = 0;
    tpag->sourceWidth = (uint16_t)w;
    tpag->sourceHeight = (uint16_t)h;
    tpag->targetX = 0;
    tpag->targetY = 0;
    tpag->targetWidth = (uint16_t)w;
    tpag->targetHeight = (uint16_t)h;
    tpag->boundingWidth = (uint16_t)w;
    tpag->boundingHeight = (uint16_t)h;
    tpag->texturePageId = (int16_t)pageId;

    uint32_t spriteIndex = DataWin_allocSpriteSlot(dw, dr->originalSpriteCount);
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    sprite->width = (uint32_t)w;
    sprite->height = (uint32_t)h;
    sprite->originX = xorig;
    sprite->originY = yorig;
    sprite->textureCount = 1;
    if (sprite->tpagIndices) free(sprite->tpagIndices);
    sprite->tpagIndices = (int32_t*)malloc(sizeof(int32_t));
    sprite->tpagIndices[0] = (int32_t)tpagIndex;
    sprite->maskCount = 0;
    sprite->masks = NULL;

    return (int32_t)spriteIndex;
}

static void d3d9DeleteSprite(Renderer* renderer, int32_t spriteIndex) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > spriteIndex || dw->sprt.count <= (uint32_t)spriteIndex) return;
    if (dr->originalSpriteCount > (uint32_t)spriteIndex) return;

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return;

    for (uint32_t i = 0; i < sprite->textureCount; i++) {
        int32_t tpagIdx = sprite->tpagIndices[i];
        if (tpagIdx >= 0 && (uint32_t)tpagIdx >= dr->originalTpagCount) {
            TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
            int16_t pageId = tpag->texturePageId;
            if (pageId >= 0 && dr->textureCount > (uint32_t)pageId) {
                if (dr->textures[pageId]) {
                    ((IDirect3DTexture9*)dr->textures[pageId])->Release();
                    dr->textures[pageId] = NULL;
                }
            }
            tpag->texturePageId = -1;
        }
    }

    free(sprite->tpagIndices);
    const char* keepName = sprite->name;
    memset(sprite, 0, sizeof(Sprite));
    sprite->name = keepName;
}

// ===[ GML Blend Factor Conversion ]===

static DWORD gmlBlendFactorToD3D(int32_t gmlFactor) {
    switch (gmlFactor) {
        case 1:  return D3DBLEND_ZERO;
        case 2:  return D3DBLEND_ONE;
        case 3:  return D3DBLEND_SRCCOLOR;
        case 4:  return D3DBLEND_INVSRCCOLOR;
        case 5:  return D3DBLEND_SRCALPHA;
        case 6:  return D3DBLEND_INVSRCALPHA;
        case 7:  return D3DBLEND_DESTALPHA;
        case 8:  return D3DBLEND_INVDESTALPHA;
        case 9:  return D3DBLEND_DESTCOLOR;
        case 10: return D3DBLEND_INVDESTCOLOR;
        case 11: return D3DBLEND_SRCALPHASAT;
        default: return D3DBLEND_SRCALPHA;
    }
}

// ===[ GPU State ]===
// Dirty-flag optimization: only call SetRenderState for values that actually changed.

void D3D9Renderer_applyGpuState(D3D9Renderer* dr) {
    // Flush any pending batched quads before changing GPU state.
    // Without this, GML code that interleaves draw_sprite() with gpu_set_blendmode()
    // would draw both sprites with the new blend mode, causing flickering on
    // semi-transparent elements.
    flushBatch(dr);

    IDirect3DDevice9* dev = Dev(dr);

    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, dr->blendEnable ? TRUE : FALSE);
    dev->SetRenderState(D3DRS_SRCBLEND, dr->srcBlend);
    dev->SetRenderState(D3DRS_DESTBLEND, dr->destBlend);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLENDALPHA, dr->srcBlendAlpha);
    dev->SetRenderState(D3DRS_DESTBLENDALPHA, dr->destBlendAlpha);

    dev->SetRenderState(D3DRS_ALPHATESTENABLE, dr->alphaTestEnable ? TRUE : FALSE);
    dev->SetRenderState(D3DRS_ALPHAREF, (DWORD)dr->alphaTestRef);
    dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);

    DWORD colorWrite = 0;
    if (dr->colorWriteR) colorWrite |= D3DCOLORWRITEENABLE_RED;
    if (dr->colorWriteG) colorWrite |= D3DCOLORWRITEENABLE_GREEN;
    if (dr->colorWriteB) colorWrite |= D3DCOLORWRITEENABLE_BLUE;
    if (dr->colorWriteA) colorWrite |= D3DCOLORWRITEENABLE_ALPHA;
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, colorWrite);

#ifdef D3DRS_FOGENABLE
    dev->SetRenderState(D3DRS_FOGENABLE, dr->fogEnable ? TRUE : FALSE);
    if (dr->fogEnable) {
        dev->SetRenderState(D3DRS_FOGCOLOR, dr->fogColor);
        dev->SetRenderState(D3DRS_FOGSTART, *(DWORD*)&dr->fogStart);
        dev->SetRenderState(D3DRS_FOGEND, *(DWORD*)&dr->fogEnd);
        dev->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
    }
#endif
}

// ===[ Surface Functions ]===

static int32_t d3d9CreateSurface(Renderer* renderer, int32_t width, int32_t height) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);
    flushBatch(dr);

    if (width <= 0 || height <= 0) return -1;

    int32_t slot = -1;
    for (int32_t i = 0; i < D3D9_MAX_SURFACES; i++) {
        if (!dr->surfaceActive[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;

    IDirect3DTexture9* tex = NULL;
    HRESULT hr = dev->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
                                     D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, NULL);
    if (FAILED(hr) || !tex) return -1;

    IDirect3DSurface9* surf = NULL;
    hr = tex->GetSurfaceLevel(0, &surf);
    if (FAILED(hr) || !surf) {
        tex->Release();
        return -1;
    }

    dr->surfaceTextures[slot] = tex;
    dr->surfaceSurfaces[slot] = surf;
    dr->surfaceWidths[slot] = width;
    dr->surfaceHeights[slot] = height;
    dr->surfaceActive[slot] = true;

    return slot;
}

static bool d3d9SurfaceExists(Renderer* renderer, int32_t surfaceID) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    if (surfaceID < 0 || surfaceID >= D3D9_MAX_SURFACES) return false;
    return dr->surfaceActive[surfaceID];
}

static bool d3d9SetRenderTarget(Renderer* renderer, int32_t surfaceID, bool implicitApplicationSurface) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);
    if (!bindRenderTarget(dr, surfaceID)) return false;

    int32_t viewCurrent = 0;
    if (renderer->runner && renderer->runner->viewsEnabled) {
        viewCurrent = renderer->runner->viewCurrent;
    }
    RuntimeView* view = renderer->runner ? &renderer->runner->views[viewCurrent] : NULL;
    if (renderer->runner && (!view || surfaceID != view->surfaceId)) {
        for (int32_t i = 0; i < MAX_VIEWS; i++) {
            RuntimeView* candidate = &renderer->runner->views[i];
            if (candidate->enabled && candidate->surfaceId == surfaceID) {
                view = candidate;
                break;
            }
        }
    }
    GMLCamera* camera = view && renderer->runner
        ? Runner_getCameraById(renderer->runner, view->cameraId)
        : NULL;
    bool implicitApp = view && implicitApplicationSurface &&
        surfaceID == renderer->runner->applicationSurfaceId;
    bool viewSurface = view && surfaceID == view->surfaceId;

    if ((implicitApp || viewSurface) && camera &&
        camera->viewWidth != 0 && camera->viewHeight != 0) {
        dr->offsetX = camera->viewX;
        dr->offsetY = camera->viewY;
        if (viewSurface) {
            dr->portScaleX = (float)dr->surfaceWidths[surfaceID] / (float)camera->viewWidth;
            dr->portScaleY = (float)dr->surfaceHeights[surfaceID] / (float)camera->viewHeight;
            dr->portOffsetX = 0.0f;
            dr->portOffsetY = 0.0f;
        } else {
            dr->portScaleX = ((float)view->portWidth * renderer->runner->displayScaleX) /
                             (float)camera->viewWidth;
            dr->portScaleY = ((float)view->portHeight * renderer->runner->displayScaleY) /
                             (float)camera->viewHeight;
            dr->portOffsetX = (float)view->portX * renderer->runner->displayScaleX;
            dr->portOffsetY = (float)view->portY * renderer->runner->displayScaleY;
        }
    } else {
        // Explicit user surfaces use a full-surface, top-left-origin camera.
        dr->offsetX = 0.0f;
        dr->offsetY = 0.0f;
        dr->portScaleX = 1.0f;
        dr->portScaleY = 1.0f;
        dr->portOffsetX = 0.0f;
        dr->portOffsetY = 0.0f;
    }
    return true;
}

static float d3d9GetSurfaceWidth(Renderer* renderer, int32_t surfaceID) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    if (surfaceID < 0 || surfaceID >= D3D9_MAX_SURFACES) return 0.0f;
    if (!dr->surfaceActive[surfaceID]) return 0.0f;
    return (float)dr->surfaceWidths[surfaceID];
}

static float d3d9GetSurfaceHeight(Renderer* renderer, int32_t surfaceID) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    if (surfaceID < 0 || surfaceID >= D3D9_MAX_SURFACES) return 0.0f;
    if (!dr->surfaceActive[surfaceID]) return 0.0f;
    return (float)dr->surfaceHeights[surfaceID];
}

static void d3d9DrawSurface(Renderer* renderer, int32_t surfaceID, int32_t srcLeft, int32_t srcTop,
                             int32_t srcWidth, int32_t srcHeight, float x, float y,
                             float xscale, float yscale, float angleDeg,
                             uint32_t color, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);

    if (surfaceID < 0 || surfaceID >= D3D9_MAX_SURFACES) return;
    if (!dr->surfaceActive[surfaceID]) return;

    IDirect3DTexture9* surfTex = (IDirect3DTexture9*)dr->surfaceTextures[surfaceID];
    int32_t texW = dr->surfaceWidths[surfaceID];
    int32_t texH = dr->surfaceHeights[surfaceID];
    if (texW <= 0 || texH <= 0) return;

    if (srcWidth < 0) {
        srcLeft = 0; srcTop = 0;
        srcWidth = texW; srcHeight = texH;
    }

    flushBatch(dr);

    dev->SetTexture(0, (IDirect3DBaseTexture9*)surfTex);

    float u0 = (float)srcLeft / (float)texW;
    float v0 = (float)srcTop / (float)texH;
    float u1 = (float)(srcLeft + srcWidth) / (float)texW;
    float v1 = (float)(srcTop + srcHeight) / (float)texH;

    DWORD d3dColor = bgrToD3DColor(color, alpha);

    float localX0 = 0.0f;
    float localY0 = 0.0f;
    float localX1 = (float)srcWidth * xscale;
    float localY1 = (float)srcHeight * yscale;

    float cx[4], cy[4];
    if (angleDeg != 0.0f) {
        float rad = -angleDeg * (3.14159265f / 180.0f);
        float cosA = cosf(rad);
        float sinA = sinf(rad);
        float lx[4] = { localX0, localX1, localX1, localX0 };
        float ly[4] = { localY0, localY0, localY1, localY1 };
        for (int i = 0; i < 4; i++) {
            cx[i] = lx[i] * cosA - ly[i] * sinA;
            cy[i] = lx[i] * sinA + ly[i] * cosA;
        }
    } else {
        cx[0] = localX0; cy[0] = localY0;
        cx[1] = localX1; cy[1] = localY0;
        cx[2] = localX1; cy[2] = localY1;
        cx[3] = localX0; cy[3] = localY1;
    }

    SpriteVertex verts[4];
    float sx, sy;
    for (int i = 0; i < 4; i++) {
        transformPoint(dr, x + cx[i], y + cy[i], &sx, &sy);
        setVertex(&verts[i], sx, sy, 0, 0, d3dColor);
    }
    verts[0].u = u0; verts[0].v = v0;
    verts[1].u = u1; verts[1].v = v0;
    verts[2].u = u1; verts[2].v = v1;
    verts[3].u = u0; verts[3].v = v1;

    dev->DrawPrimitiveUP(D3DPT_QUADLIST, 1, verts, sizeof(SpriteVertex));

    // d3d9DrawSurface bypasses the batch system via DrawPrimitiveUP, which may
    // cause the GPU to lose the vertex declaration and shader state on Xbox 360 D3D9.
    // Re-apply them and invalidate currentTextureIndex so the next batched draw
    // rebinds the correct texture.
    dev->SetVertexDeclaration((IDirect3DVertexDeclaration9*)dr->pVertexDecl);
    dr->currentTextureIndex = -1;
}

static void d3d9SurfaceResize(Renderer* renderer, int32_t surfaceID, int32_t width, int32_t height) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);
    flushBatch(dr);

    if (width <= 0 || height <= 0) return;
    if (surfaceID < 0 || surfaceID >= D3D9_MAX_SURFACES) return;
    if (!dr->surfaceActive[surfaceID]) return;
    if (dr->surfaceWidths[surfaceID] == width && dr->surfaceHeights[surfaceID] == height) return;

    IDirect3DTexture9* tex = NULL;
    HRESULT hr = dev->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
                                     D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, NULL);
    if (FAILED(hr) || !tex) return;

    IDirect3DSurface9* surf = NULL;
    hr = tex->GetSurfaceLevel(0, &surf);
    if (FAILED(hr) || !surf) {
        tex->Release();
        return;
    }

    bool wasCurrentTarget = dr->currentSurfaceTarget == surfaceID;
    if (wasCurrentTarget) bindRenderTarget(dr, RENDER_TARGET_HOST_FRAMEBUFFER);

    IDirect3DSurface9* oldSurface = (IDirect3DSurface9*)dr->surfaceSurfaces[surfaceID];
    IDirect3DTexture9* oldTexture = (IDirect3DTexture9*)dr->surfaceTextures[surfaceID];
    dr->surfaceTextures[surfaceID] = tex;
    dr->surfaceSurfaces[surfaceID] = surf;
    dr->surfaceWidths[surfaceID] = width;
    dr->surfaceHeights[surfaceID] = height;

    if (oldSurface) oldSurface->Release();
    if (oldTexture) oldTexture->Release();
    if (wasCurrentTarget) bindRenderTarget(dr, surfaceID);
}

static void d3d9SurfaceFree(Renderer* renderer, int32_t surfaceID) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);

    if (surfaceID < 0 || surfaceID >= D3D9_MAX_SURFACES) return;
    if (renderer->runner && surfaceID == renderer->runner->applicationSurfaceId) return;

    if (dr->currentSurfaceTarget == surfaceID) {
        int32_t appSurfaceID = renderer->runner ? renderer->runner->applicationSurfaceId : -1;
        if (appSurfaceID == surfaceID || !bindRenderTarget(dr, appSurfaceID)) {
            bindRenderTarget(dr, RENDER_TARGET_HOST_FRAMEBUFFER);
        }
    }

    if (dr->surfaceSurfaces[surfaceID]) {
        ((IDirect3DSurface9*)dr->surfaceSurfaces[surfaceID])->Release();
        dr->surfaceSurfaces[surfaceID] = NULL;
    }
    if (dr->surfaceTextures[surfaceID]) {
        ((IDirect3DTexture9*)dr->surfaceTextures[surfaceID])->Release();
        dr->surfaceTextures[surfaceID] = NULL;
    }
    dr->surfaceWidths[surfaceID] = 0;
    dr->surfaceHeights[surfaceID] = 0;
    dr->surfaceActive[surfaceID] = false;
}

static void d3d9SurfaceCopy(Renderer* renderer, int32_t destSurfaceID, int32_t destX, int32_t destY,
                              int32_t srcSurfaceID, int32_t srcX, int32_t srcY,
                              int32_t srcW, int32_t srcH, bool part) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);
    flushBatch(dr);

    IDirect3DSurface9* srcSurf = NULL;
    int32_t srcWFull = 0;
    int32_t srcHFull = 0;
    bool srcIsBackBuffer = false;

    if (srcSurfaceID == APPLICATION_SURFACE_ID) {
        dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &srcSurf);
        srcWFull = dr->screenW;
        srcHFull = dr->screenH;
        srcIsBackBuffer = true;
    } else {
        if (srcSurfaceID < 0 || srcSurfaceID >= D3D9_MAX_SURFACES) return;
        if (!dr->surfaceActive[srcSurfaceID]) return;
        srcSurf = (IDirect3DSurface9*)dr->surfaceSurfaces[srcSurfaceID];
        srcWFull = dr->surfaceWidths[srcSurfaceID];
        srcHFull = dr->surfaceHeights[srcSurfaceID];
    }

    IDirect3DSurface9* dstSurf = NULL;
    bool dstIsBackBuffer = false;

    if (destSurfaceID == APPLICATION_SURFACE_ID) {
        dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &dstSurf);
        dstIsBackBuffer = true;
    } else {
        if (destSurfaceID < 0 || destSurfaceID >= D3D9_MAX_SURFACES) return;
        if (!dr->surfaceActive[destSurfaceID]) {
            if (srcIsBackBuffer && srcSurf) srcSurf->Release();
            return;
        }
        dstSurf = (IDirect3DSurface9*)dr->surfaceSurfaces[destSurfaceID];
    }

    if (!srcSurf || !dstSurf) {
        if (srcIsBackBuffer && srcSurf) srcSurf->Release();
        if (dstIsBackBuffer && dstSurf) dstSurf->Release();
        return;
    }

    if (part) {
        RECT srcRect = { srcX, srcY, srcX + srcW, srcY + srcH };
        RECT dstRect = { destX, destY, destX + srcW, destY + srcH };
        D3DXLoadSurfaceFromSurface(dstSurf, NULL, &dstRect, srcSurf, NULL, &srcRect, D3DX_FILTER_POINT, 0);
    } else {
        RECT srcRect = { 0, 0, srcWFull, srcHFull };
        RECT dstRect = { destX, destY, destX + srcWFull, destY + srcHFull };
        D3DXLoadSurfaceFromSurface(dstSurf, NULL, &dstRect, srcSurf, NULL, &srcRect, D3DX_FILTER_POINT, 0);
    }

    if (srcIsBackBuffer) srcSurf->Release();
    if (dstIsBackBuffer) dstSurf->Release();
}

static void d3d9ClearScreen(Renderer* renderer, uint32_t color, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);
    flushBatch(dr);

    uint8_t r = color & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = (color >> 16) & 0xFF;
    uint8_t a = alphaToByte(alpha);
    D3DCOLOR clearColor = D3DCOLOR_ARGB(a, r, g, b);

    dev->Clear(0, NULL, D3DCLEAR_TARGET, clearColor, 1.0f, 0);
}

static void d3d9ApplyGpuState(Renderer* renderer) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    D3D9Renderer_applyGpuState(dr);
}

static BlendFactors d3d9GpuGetBlendFactors(Renderer* renderer) {
    return renderer->blendFactors;
}

static int32_t d3d9GpuGetBlendMode(Renderer* renderer) {
    return ((D3D9Renderer*)renderer)->blendMode;
}

static void d3d9GpuSetBlendMode(Renderer* renderer, int32_t mode) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    IDirect3DDevice9* dev = Dev(dr);
    flushBatch(dr);
    dr->blendMode = mode;
    renderer->blendFactors.src = bm_src_alpha;
    renderer->blendFactors.dst = bm_inv_src_alpha;
    renderer->blendFactors.srcAlpha = bm_src_alpha;
    renderer->blendFactors.dstAlpha = bm_inv_src_alpha;
    dr->srcBlend = D3DBLEND_SRCALPHA;
    dr->destBlend = D3DBLEND_INVSRCALPHA;
    dr->srcBlendAlpha = D3DBLEND_SRCALPHA;
    dr->destBlendAlpha = D3DBLEND_INVSRCALPHA;
    DWORD operation = D3DBLENDOP_ADD;

    switch (mode) {
        case bm_add:
            renderer->blendFactors.dst = bm_one;
            renderer->blendFactors.dstAlpha = bm_one;
            dr->destBlend = D3DBLEND_ONE;
            dr->destBlendAlpha = D3DBLEND_ONE;
            break;
        case bm_max:
            operation = D3DBLENDOP_MAX;
            break;
        case bm_subtract:
            operation = D3DBLENDOP_SUBTRACT;
            break;
        case bm_min:
            operation = D3DBLENDOP_MIN;
            break;
        case bm_reverse_subtract:
            operation = D3DBLENDOP_REVSUBTRACT;
            break;
        default:
            break;
    }

    dev->SetRenderState(D3DRS_BLENDOP, operation);
    dev->SetRenderState(D3DRS_BLENDOPALPHA, operation);
    D3D9Renderer_applyGpuState(dr);
}

static void d3d9GpuSetBlendModeExt(Renderer* renderer, int32_t sfactor, int32_t dfactor, int32_t sfactorAlpha, int32_t dfactorAlpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    flushBatch(dr);
    dr->blendMode = bm_complex;
    renderer->blendFactors.src = sfactor;
    renderer->blendFactors.dst = dfactor;
    renderer->blendFactors.srcAlpha = sfactorAlpha;
    renderer->blendFactors.dstAlpha = dfactorAlpha;
    dr->srcBlend = gmlBlendFactorToD3D(sfactor);
    dr->destBlend = gmlBlendFactorToD3D(dfactor);
    dr->srcBlendAlpha = gmlBlendFactorToD3D(sfactorAlpha);
    dr->destBlendAlpha = gmlBlendFactorToD3D(dfactorAlpha);
    Dev(dr)->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    Dev(dr)->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    D3D9Renderer_applyGpuState(dr);
}

static void d3d9GpuSetBlendEnable(Renderer* renderer, bool enable) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    dr->blendEnable = enable;
    D3D9Renderer_applyGpuState(dr);
}

static bool d3d9GpuGetBlendEnable(Renderer* renderer) {
    return ((D3D9Renderer*)renderer)->blendEnable;
}

static void d3d9GpuSetAlphaTestEnable(Renderer* renderer, bool enable) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    dr->alphaTestEnable = enable;
    D3D9Renderer_applyGpuState(dr);
}

static void d3d9GpuSetAlphaTestRef(Renderer* renderer, uint8_t ref) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    dr->alphaTestRef = ref;
    D3D9Renderer_applyGpuState(dr);
}

static void d3d9GpuSetColorWriteEnable(Renderer* renderer, bool red, bool green, bool blue, bool alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    dr->colorWriteR = red;
    dr->colorWriteG = green;
    dr->colorWriteB = blue;
    dr->colorWriteA = alpha;
    D3D9Renderer_applyGpuState(dr);
}

static void d3d9GpuGetColorWriteEnable(Renderer* renderer, bool* red, bool* green, bool* blue, bool* alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    *red = dr->colorWriteR;
    *green = dr->colorWriteG;
    *blue = dr->colorWriteB;
    *alpha = dr->colorWriteA;
}

static void d3d9GpuSetFog(Renderer* renderer, bool enable, uint32_t color) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    dr->fogEnable = enable;
    dr->fogColor = bgrToD3DColor(color, 1.0f);
    D3D9Renderer_applyGpuState(dr);
}

static int32_t d3d9EnsureApplicationSurface(Renderer* renderer, int32_t width, int32_t height) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    int32_t surfaceID = renderer->runner ? renderer->runner->applicationSurfaceId : APPLICATION_SURFACE_ID;

    if (surfaceID < 0 || surfaceID >= D3D9_MAX_SURFACES || !dr->surfaceActive[surfaceID]) {
        surfaceID = d3d9CreateSurface(renderer, width, height);
        if (surfaceID >= 0 && renderer->runner) renderer->runner->applicationSurfaceId = surfaceID;
        return surfaceID;
    }

    if (dr->surfaceWidths[surfaceID] != width || dr->surfaceHeights[surfaceID] != height) {
        d3d9SurfaceResize(renderer, surfaceID, width, height);
    }
    return surfaceID;
}

static void d3d9DrawSpriteTiled(Renderer* renderer, int32_t tpagIndex, float originX, float originY, float x, float y, float xscale, float yscale, bool tileX, bool tileY, float roomW, float roomH, uint32_t color, float alpha) {
    DataWin* dw = renderer->dataWin;
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    float stepX = (float)tpag->boundingWidth * fabsf(xscale);
    float stepY = (float)tpag->boundingHeight * fabsf(yscale);
    if (stepX <= 0.0f || stepY <= 0.0f) return;
    float startX = x;
    float startY = y;
    if (tileX) while (startX - originX * xscale > 0.0f) startX -= stepX;
    if (tileY) while (startY - originY * yscale > 0.0f) startY -= stepY;
    float endX = tileX ? roomW + stepX : startX + 1.0f;
    float endY = tileY ? roomH + stepY : startY + 1.0f;
    for (float drawY = startY; drawY < endY; drawY += stepY) {
        for (float drawX = startX; drawX < endX; drawX += stepX) {
            d3d9DrawSprite(renderer, tpagIndex, drawX, drawY, originX, originY, xscale, yscale, 0.0f, color, alpha);
        }
    }
}

static void d3d9DrawSurfaceTiled(Renderer* renderer, int32_t surfaceID, float x, float y, float xscale, float yscale, float roomW, float roomH, uint32_t color, float alpha) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    if (!d3d9SurfaceExists(renderer, surfaceID)) return;
    float stepX = (float)dr->surfaceWidths[surfaceID] * fabsf(xscale);
    float stepY = (float)dr->surfaceHeights[surfaceID] * fabsf(yscale);
    if (stepX <= 0.0f || stepY <= 0.0f) return;
    while (x > 0.0f) x -= stepX;
    while (y > 0.0f) y -= stepY;
    for (float drawY = y; drawY < roomH + stepY; drawY += stepY) {
        for (float drawX = x; drawX < roomW + stepX; drawX += stepX) {
            d3d9DrawSurface(renderer, surfaceID, 0, 0, -1, -1, drawX, drawY, xscale, yscale, 0.0f, color, alpha);
        }
    }
}

static bool d3d9SurfaceGetPixels(Renderer* renderer, int32_t surfaceID, uint8_t* outRGBA) {
    return readSurfacePixels((D3D9Renderer*)renderer, surfaceID, outRGBA);
}

#define D3D9_SURFACE_TEXTURE_FLAG 0x80000000u

static bool d3d9ResolveTextureHandle(D3D9Renderer* dr, uint32_t textureHandle, TexturePageItem** outTpag, int32_t* outWidth, int32_t* outHeight) {
    if (textureHandle == 0) return false;
    if ((textureHandle & D3D9_SURFACE_TEXTURE_FLAG) != 0) {
        uint32_t surfaceID = textureHandle & ~D3D9_SURFACE_TEXTURE_FLAG;
        if (surfaceID >= D3D9_MAX_SURFACES || !dr->surfaceActive[surfaceID]) return false;
        if (outTpag) *outTpag = NULL;
        *outWidth = dr->surfaceWidths[surfaceID];
        *outHeight = dr->surfaceHeights[surfaceID];
        return true;
    }
    int32_t tpagIndex = (int32_t)textureHandle - 1;
    DataWin* dw = dr->base.dataWin;
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return false;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int32_t pageID = tpag->texturePageId;
    if (pageID < 0 || (uint32_t)pageID >= dr->textureCount) return false;
    ensureTextureLoaded(dr, pageID);
    if (!dr->textures[pageID]) return false;
    if (outTpag) *outTpag = tpag;
    *outWidth = dr->textureWidths[pageID];
    *outHeight = dr->textureHeights[pageID];
    return true;
}

static uint32_t d3d9SpriteGetTexture(Renderer* renderer, int32_t tpagIndex) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    DataWin* dw = renderer->dataWin;
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return 0;
    int32_t pageID = dw->tpag.items[tpagIndex].texturePageId;
    if (pageID < 0 || (uint32_t)pageID >= dr->textureCount) return 0;
    ensureTextureLoaded(dr, pageID);
    return dr->textures[pageID] ? (uint32_t)(tpagIndex + 1) : 0;
}

static uint32_t d3d9SurfaceGetTexture(Renderer* renderer, int32_t surfaceID) {
    if (!d3d9SurfaceExists(renderer, surfaceID)) return 0;
    return D3D9_SURFACE_TEXTURE_FLAG | (uint32_t)surfaceID;
}

static float d3d9TextureGetTexelWidth(Renderer* renderer, uint32_t textureHandle) {
    int32_t width = 0, height = 0;
    if (!d3d9ResolveTextureHandle((D3D9Renderer*)renderer, textureHandle, NULL, &width, &height) || width <= 0) return 1.0f;
    return 1.0f / (float)width;
}

static float d3d9TextureGetTexelHeight(Renderer* renderer, uint32_t textureHandle) {
    int32_t width = 0, height = 0;
    if (!d3d9ResolveTextureHandle((D3D9Renderer*)renderer, textureHandle, NULL, &width, &height) || height <= 0) return 1.0f;
    return 1.0f / (float)height;
}

static bool d3d9TextureGetUVs(Renderer* renderer, uint32_t textureHandle, float* outUVs) {
    TexturePageItem* tpag = NULL;
    int32_t width = 0, height = 0;
    if (!d3d9ResolveTextureHandle((D3D9Renderer*)renderer, textureHandle, &tpag, &width, &height) || width <= 0 || height <= 0) return false;
    if (!tpag) {
        outUVs[0] = 0.0f; outUVs[1] = 0.0f; outUVs[2] = 1.0f; outUVs[3] = 1.0f;
        return true;
    }
    outUVs[0] = (float)tpag->sourceX / (float)width;
    outUVs[1] = (float)tpag->sourceY / (float)height;
    outUVs[2] = (float)(tpag->sourceX + tpag->sourceWidth) / (float)width;
    outUVs[3] = (float)(tpag->sourceY + tpag->sourceHeight) / (float)height;
    return true;
}

static void d3d9TextureSetStage(Renderer* renderer, int32_t slot, uint32_t textureHandle) {
    D3D9Renderer* dr = (D3D9Renderer*)renderer;
    if (slot < 0 || slot >= MAX_TEXTURE_STAGES) return;
    if ((textureHandle & D3D9_SURFACE_TEXTURE_FLAG) != 0) {
        uint32_t surfaceID = textureHandle & ~D3D9_SURFACE_TEXTURE_FLAG;
        if (surfaceID < D3D9_MAX_SURFACES && dr->surfaceActive[surfaceID]) {
            flushBatch(dr);
            Dev(dr)->SetTexture((DWORD)slot, (IDirect3DBaseTexture9*)dr->surfaceTextures[surfaceID]);
        }
        return;
    }
    int32_t tpagIndex = (int32_t)textureHandle - 1;
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= renderer->dataWin->tpag.count) return;
    int32_t pageID = renderer->dataWin->tpag.items[tpagIndex].texturePageId;
    if (pageID < 0 || (uint32_t)pageID >= dr->textureCount) return;
    ensureTextureLoaded(dr, pageID);
    flushBatch(dr);
    Dev(dr)->SetTexture((DWORD)slot, (IDirect3DBaseTexture9*)dr->textures[pageID]);
}

static void d3d9GpuSetShader(Renderer* renderer, int32_t shaderIndex) { renderer->currentShader = shaderIndex; }
static void d3d9GpuResetShader(Renderer* renderer) { renderer->currentShader = -1; }
static int32_t d3d9ShaderGetUniform(Renderer* renderer, int32_t shaderIndex, char* uniform) { (void)renderer; (void)shaderIndex; (void)uniform; return -1; }
static int32_t d3d9ShaderGetSamplerIndex(Renderer* renderer, int32_t shaderIndex, char* uniform) { (void)renderer; (void)shaderIndex; (void)uniform; return -1; }
static void d3d9ShaderSetUniformF(Renderer* renderer, int32_t handle, int32_t count, float value1, float value2, float value3, float value4) { (void)renderer; (void)handle; (void)count; (void)value1; (void)value2; (void)value3; (void)value4; }
static void d3d9ShaderSetUniformFArray(Renderer* renderer, int32_t handle, float* values, uint32_t count) { (void)renderer; (void)handle; (void)values; (void)count; }
static void d3d9ShaderSetUniformI(Renderer* renderer, int32_t handle, int32_t count, int32_t value1, int32_t value2, int32_t value3, int32_t value4) { (void)renderer; (void)handle; (void)count; (void)value1; (void)value2; (void)value3; (void)value4; }
static bool d3d9ShaderIsCompiled(Renderer* renderer, int32_t shader) { (void)renderer; (void)shader; return false; }
static bool d3d9ShadersSupported(void) { return false; }
static void d3d9SetMatrix(Renderer* renderer, int32_t matrixType, Matrix4f matrix) { if (matrixType >= 0 && matrixType < MATRICES_MAX) renderer->gmlMatrices[matrixType] = matrix; }

// ===[ Vtable ]===

static RendererVtable d3d9RendererVtable;

// ===[ Public API ]===

Renderer* D3D9Renderer_create(void* pd3dDevice) {
    D3D9Renderer* dr = (D3D9Renderer*)calloc(1, sizeof(D3D9Renderer));
    dr->base.vtable = &d3d9RendererVtable;
    d3d9RendererVtable.init = d3d9Init;
    d3d9RendererVtable.destroy = d3d9Destroy;
    d3d9RendererVtable.beginFrame = d3d9BeginFrame;
    d3d9RendererVtable.endFrameInit = d3d9EndFrameInit;
    d3d9RendererVtable.endFrameEnd = d3d9EndFrameEnd;
    d3d9RendererVtable.beginView = d3d9BeginView;
    d3d9RendererVtable.endView = d3d9EndView;
    d3d9RendererVtable.applyProjection = d3d9ApplyProjection;
    d3d9RendererVtable.beginGUI = d3d9BeginGUI;
    d3d9RendererVtable.setGuiProjection = d3d9SetGuiProjection;
    d3d9RendererVtable.endGUI = d3d9EndGUI;
    d3d9RendererVtable.drawSprite = d3d9DrawSprite;
    d3d9RendererVtable.drawSpritePart = d3d9DrawSpritePart;
    d3d9RendererVtable.drawSpritePos = d3d9DrawSpritePos;
    d3d9RendererVtable.drawRectangle = d3d9DrawRectangle;
    d3d9RendererVtable.drawRectangleColor = d3d9DrawRectangleColor;
    d3d9RendererVtable.drawLine = d3d9DrawLine;
    d3d9RendererVtable.drawTriangle = d3d9DrawTriangle;
    d3d9RendererVtable.drawLineColor = d3d9DrawLineColor;
    d3d9RendererVtable.drawText = d3d9DrawText;
    d3d9RendererVtable.drawTextColor = d3d9DrawTextColor;
    d3d9RendererVtable.flush = d3d9Flush;
    d3d9RendererVtable.clearScreen = d3d9ClearScreen;
    d3d9RendererVtable.createSpriteFromSurface = d3d9CreateSpriteFromSurface;
    d3d9RendererVtable.deleteSprite = d3d9DeleteSprite;
    d3d9RendererVtable.gpuGetBlendFactors = d3d9GpuGetBlendFactors;
    d3d9RendererVtable.gpuGetBlendMode = d3d9GpuGetBlendMode;
    d3d9RendererVtable.gpuSetBlendMode = d3d9GpuSetBlendMode;
    d3d9RendererVtable.gpuSetBlendModeExt = d3d9GpuSetBlendModeExt;
    d3d9RendererVtable.gpuSetBlendEnable = d3d9GpuSetBlendEnable;
    d3d9RendererVtable.gpuSetAlphaTestEnable = d3d9GpuSetAlphaTestEnable;
    d3d9RendererVtable.gpuSetAlphaTestRef = d3d9GpuSetAlphaTestRef;
    d3d9RendererVtable.gpuSetColorWriteEnable = d3d9GpuSetColorWriteEnable;
    d3d9RendererVtable.gpuGetColorWriteEnable = d3d9GpuGetColorWriteEnable;
    d3d9RendererVtable.gpuGetBlendEnable = d3d9GpuGetBlendEnable;
    d3d9RendererVtable.gpuSetFog = d3d9GpuSetFog;
    d3d9RendererVtable.drawTile = NULL;
    d3d9RendererVtable.drawSpriteTiled = d3d9DrawSpriteTiled;
    d3d9RendererVtable.createSurface = d3d9CreateSurface;
    d3d9RendererVtable.surfaceExists = d3d9SurfaceExists;
    d3d9RendererVtable.setRenderTarget = d3d9SetRenderTarget;
    d3d9RendererVtable.ensureApplicationSurface = d3d9EnsureApplicationSurface;
    d3d9RendererVtable.getSurfaceWidth = d3d9GetSurfaceWidth;
    d3d9RendererVtable.getSurfaceHeight = d3d9GetSurfaceHeight;
    d3d9RendererVtable.drawSurface = d3d9DrawSurface;
    d3d9RendererVtable.drawSurfaceTiled = d3d9DrawSurfaceTiled;
    d3d9RendererVtable.surfaceResize = d3d9SurfaceResize;
    d3d9RendererVtable.surfaceFree = d3d9SurfaceFree;
    d3d9RendererVtable.surfaceCopy = d3d9SurfaceCopy;
    d3d9RendererVtable.surfaceGetPixels = d3d9SurfaceGetPixels;
    d3d9RendererVtable.drawTiledPart = NULL;
    d3d9RendererVtable.gpuSetShader = d3d9GpuSetShader;
    d3d9RendererVtable.gpuResetShader = d3d9GpuResetShader;
    d3d9RendererVtable.shaderGetUniform = d3d9ShaderGetUniform;
    d3d9RendererVtable.shaderGetSamplerIndex = d3d9ShaderGetSamplerIndex;
    d3d9RendererVtable.shaderSetUniformF = d3d9ShaderSetUniformF;
    d3d9RendererVtable.shaderSetUniformFArray = d3d9ShaderSetUniformFArray;
    d3d9RendererVtable.shaderSetUniformI = d3d9ShaderSetUniformI;
    d3d9RendererVtable.spriteGetTexture = d3d9SpriteGetTexture;
    d3d9RendererVtable.surfaceGetTexture = d3d9SurfaceGetTexture;
    d3d9RendererVtable.textureGetTexelWidth = d3d9TextureGetTexelWidth;
    d3d9RendererVtable.textureGetTexelHeight = d3d9TextureGetTexelHeight;
    d3d9RendererVtable.textureGetUVs = d3d9TextureGetUVs;
    d3d9RendererVtable.textureSetStage = d3d9TextureSetStage;
    d3d9RendererVtable.shaderIsCompiled = d3d9ShaderIsCompiled;
    d3d9RendererVtable.shadersSupported = d3d9ShadersSupported;
    d3d9RendererVtable.setMatrix = d3d9SetMatrix;
    dr->base.drawColor = 0xFFFFFF;
    dr->base.drawAlpha = 1.0f;
    dr->base.drawFont = -1;
    dr->base.circlePrecision = 24;
    dr->base.currentShader = -1;
    dr->base.blendFactors.src = bm_src_alpha;
    dr->base.blendFactors.dst = bm_inv_src_alpha;
    dr->base.blendFactors.srcAlpha = bm_src_alpha;
    dr->base.blendFactors.dstAlpha = bm_inv_src_alpha;
    dr->blendMode = bm_normal;
    dr->pd3dDevice = pd3dDevice;
    dr->currentTextureIndex = -1;
    return (Renderer*)dr;
}
