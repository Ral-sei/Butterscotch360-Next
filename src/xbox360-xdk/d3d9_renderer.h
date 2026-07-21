#pragma once

#include "renderer.h"

#define D3D9_MAX_QUADS 4096
#define D3D9_VERTS_PER_QUAD 4
#define D3D9_INDICES_PER_QUAD 6
#define D3D9_VERTEX_STRIDE 28
#define D3D9_MAX_SURFACES 16
#define D3D9_TXTR_CACHE_BUDGET (240u * 1024u * 1024u)

typedef struct {
    Renderer base; // Must be first field

    void* pd3dDevice; // IDirect3DDevice9* (opaque in C header)

    // Shader programs (compiled at init from HLSL source)
    void* pVertexShader;
    void* pPixelShader;
    void* pVertexDecl;

    // Dynamic vertex buffer for batched rendering (avoids DrawPrimitiveUP per-flush CPUPU copy)
    void* pVertexBuffer;
    int32_t vbSize; // current allocated size in bytes

    // Sprite batch state
    int32_t quadCount;
    int32_t currentTextureIndex;
    uint8_t* vertexData; // CPU-side staging (D3D9_MAX_QUADS * D3D9_VERTS_PER_QUAD * D3D9_VERTEX_STRIDE)

    // Textures loaded from TXTR pages (decoded PNG -> D3D textures)
    void** textures;     // IDirect3DTexture9*[]
    int32_t* textureWidths;
    int32_t* textureHeights;
    uint32_t textureCount;
    bool* textureLoaded; // Per-page lazy-load flag (false = not yet decoded/uploaded)
    size_t* textureBytes;
    uint32_t* textureLastUsedFrame;
    uint32_t* textureFailureFrame;
    size_t textureResidentBytes;
    uint32_t textureFrame;

    // 1x1 white texture for primitives
    void* whiteTexture;

    // View transform state
    float portScaleX, portScaleY; // portW/viewW, portH/viewH
    float offsetX, offsetY;       // viewX, viewY
    float portOffsetX, portOffsetY; // portX, portY (game coords)

    // Saved view transform (for GUI restore)
    float savedPortScaleX, savedPortScaleY;
    float savedOffsetX, savedOffsetY;
    float savedPortOffsetX, savedPortOffsetY;
    bool inGUI;

    // Frame dimensions
    int32_t gameW, gameH;
    int32_t screenW, screenH;

    // Letterbox: uniform-scaled render area within screen
    float renderScale;   // uniform scale factor
    float renderOffsetX; // pixel offset for centering
    float renderOffsetY;

    // Dynamic sprite tracking
    uint32_t originalTexturePageCount;
    uint32_t originalTpagCount;
    uint32_t originalSpriteCount;

    // Surface data
    void* surfaceTextures[D3D9_MAX_SURFACES];   // IDirect3DTexture9*
    void* surfaceSurfaces[D3D9_MAX_SURFACES];   // IDirect3DSurface9*
    int32_t surfaceWidths[D3D9_MAX_SURFACES];
    int32_t surfaceHeights[D3D9_MAX_SURFACES];
    bool surfaceActive[D3D9_MAX_SURFACES];
    void* savedRenderTarget;                     // IDirect3DSurface9*
    int32_t currentSurfaceTarget;                // -1 = screen

    // GPU state (current values)
    bool blendEnable;
    int32_t blendMode;
    uint32_t srcBlend;
    uint32_t destBlend;
    uint32_t srcBlendAlpha;
    uint32_t destBlendAlpha;
    bool alphaTestEnable;
    uint8_t alphaTestRef;
    bool colorWriteR, colorWriteG, colorWriteB, colorWriteA;
    bool fogEnable;
    uint32_t fogColor;
    float fogStart;
    float fogEnd;

    // GPU state dirty flags ?only call SetRenderState when a value actually changed
    bool gpuStateDirty;

    // Runner reference for GPU state sync
    void* runner;
} D3D9Renderer;

Renderer* D3D9Renderer_create(void* pd3dDevice);
void D3D9Renderer_applyGpuState(D3D9Renderer* dr);
void D3D9Renderer_present(Renderer* renderer);
