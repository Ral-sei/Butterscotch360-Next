// D3D9 display bring-up, NOT a RendererVtable implementation.
// Kept separate from the runtime so an incomplete renderer cannot masquerade
// as one that handles surfaces, sprites or shaders. The probe draws three
// horizontal clear rectangles, optionally draws a shader triangle, presents a
// bounded number of frames, then exits.

#include <xtl.h>
#include <d3d9.h>
#include <xgraphics.h>
#include <stdint.h>
#include "data_win.h"
#include "image_decoder.h"
#include "xbox360_d3d_device.h"

#ifdef BS_X360_D3D_SHADER_PROBE
#include "probe_vs.h"
#include "probe_ps.h"

typedef struct {
    float position[4];
    float color[4];
    float texcoord[2];
} ProbeVertex;

static const D3DVERTEXELEMENT9 probeElements[] = {
    {0, 0,  D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
    {0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
    {0, 32, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
    D3DDECL_END()
};

static ProbeVertex probeTriangle[] = {
    {{-0.75f,  0.75f, 0.5f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f}},
    {{ 0.75f,  0.75f, 0.5f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 0.0f}},
    {{ 0.75f, -0.75f, 0.5f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 1.0f}},
    {{-0.75f, -0.75f, 0.5f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}}
};

static const uint16_t probeIndices[] = {0, 1, 2, 0, 2, 3};
#endif

#ifndef BS_X360_TEST_FRAMES
#define BS_X360_TEST_FRAMES 120
#endif

#define PROBE_WIDTH 1280
#define PROBE_HEIGHT 720

static const char *probeDataWinPath(void) {
    static const char *paths[] = {"game:\\data.win", "data.win", "D:\\data.win"};
    for (unsigned int i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *file = fopen(paths[i], "rb");
        if (file) { fclose(file); return paths[i]; }
    }
    return NULL;
}

#ifndef BS_X360_PROBE_TPAG_INDEX
#define BS_X360_PROBE_TPAG_INDEX (-1)
#endif

static bool probeLoadRealTexture(DataWin **outData, uint8_t **outPixels,
                                 int *outWidth, int *outHeight,
                                 TexturePageItem **outPage) {
    const char *path = probeDataWinPath();
    if (!path) {
        OutputDebugStringA("D3D9 probe: real data.win not found\n");
        return false;
    }
    DataWinParserOptions options = {0};
    options.parseTpag = true;
    options.parseTxtr = true;
    options.loadType = DATAWINLOADTYPE_LOAD_IN_MEMORY_AHEAD_OF_TIME;
    DataWin *data = DataWin_parse(path, options);
    if (!data || data->tpag.count == 0 || data->txtr.count == 0) {
        OutputDebugStringA("D3D9 probe: data.win has no TPAG/TXTR\n");
        DataWin_free(data);
        return false;
    }

    for (uint32_t i = 0; i < data->tpag.count; i++) {
        if (BS_X360_PROBE_TPAG_INDEX >= 0 &&
            i != (uint32_t) BS_X360_PROBE_TPAG_INDEX) continue;
        TexturePageItem *page = &data->tpag.items[i];
        if (!page->present || page->texturePageId < 0 ||
            (uint32_t)page->texturePageId >= data->txtr.count) continue;
        Texture *texture = &data->txtr.textures[page->texturePageId];
        int width = 0, height = 0;
        uint8_t *pixels = ImageDecoder_decodeToRgba(
            texture->blobData, texture->blobSize,
            DataWin_isVersionAtLeast(data, 2022, 5, 0, 0), &width, &height);
        if (!pixels) continue;
        char message[160];
        snprintf(message, sizeof(message),
                 "D3D9 probe: real TPAG %u page %d decoded %dx%d\n",
                 (unsigned int)i, (int)page->texturePageId, width, height);
        OutputDebugStringA(message);
        // XGTileTextureLevel's 32bpp linear source is one 0xAARRGGBB word per
        // texel, but ImageDecoder_decodeToRgba returns RGBA bytes. Permute the
        // bytes in place (verified on hardware by the runtime renderer).
        for (size_t t = 0; t < (size_t)width * (size_t)height; t++) {
            uint8_t *p = pixels + t * 4;
            uint8_t rr = p[0], gg = p[1], bb = p[2], aa = p[3];
            p[0] = aa; p[1] = rr; p[2] = gg; p[3] = bb;
        }
        *outData = data;
        *outPixels = pixels;
        *outWidth = width;
        *outHeight = height;
        *outPage = page;
        return true;
    }
    OutputDebugStringA("D3D9 probe: no decodable TPAG texture\n");
    DataWin_free(data);
    return false;
}

static void probeClear(D3DDevice *device, LONG top, LONG bottom, D3DCOLOR color) {
    D3DRECT rect;
    rect.x1 = 0;
    rect.y1 = top;
    rect.x2 = PROBE_WIDTH;
    rect.y2 = bottom;
    IDirect3DDevice9_Clear(device, 1, &rect, D3DCLEAR_TARGET, color, 1.0f, 0);
}

int main(void) {
    OutputDebugStringA("D3D9 probe: creating device (1280x720, interval one)\n");

    Xbox360D3DDevice graphics = {0};
    if (!Xbox360D3DDevice_create(&graphics, PROBE_WIDTH, PROBE_HEIGHT)) return 2;
    D3DDevice *device = graphics.device;
    OutputDebugStringA("D3D9 probe: device created\n");

#ifdef BS_X360_D3D_SHADER_PROBE
    D3DVertexDeclaration *declaration = NULL;
    D3DVertexShader *vertexShader = NULL;
    D3DPixelShader *pixelShader = NULL;
    D3DTexture *texture = NULL;
    DataWin *realData = NULL;
    uint8_t *realPixels = NULL;
    int realWidth = 64;
    int realHeight = 64;
    float realU0 = 0.0f;
    float realV0 = 0.0f;
    float realU1 = 1.0f;
    float realV1 = 1.0f;
#ifdef BS_X360_REAL_RESOURCE_PROBE
    TexturePageItem *realPage = NULL;
    if (!probeLoadRealTexture(&realData, &realPixels, &realWidth, &realHeight, &realPage)) {
        OutputDebugStringA("D3D9 probe: real resource probe failed\n");
        Xbox360D3DDevice_destroy(&graphics);
        return 5;
    }
    realU0 = (float) realPage->sourceX / (float) realWidth;
    realV0 = (float) realPage->sourceY / (float) realHeight;
    realU1 = (float) (realPage->sourceX + realPage->sourceWidth) / (float) realWidth;
    realV1 = (float) (realPage->sourceY + realPage->sourceHeight) / (float) realHeight;
    char pageMessage[220];
    snprintf(pageMessage, sizeof(pageMessage),
             "D3D9 probe: TPAG crop src=(%u,%u %ux%u) dst=(%u,%u %ux%u) UV=(%.4f,%.4f)-(%.4f,%.4f)\n",
             (unsigned int)realPage->sourceX, (unsigned int)realPage->sourceY,
             (unsigned int)realPage->sourceWidth, (unsigned int)realPage->sourceHeight,
             (unsigned int)realPage->targetX, (unsigned int)realPage->targetY,
             (unsigned int)realPage->targetWidth, (unsigned int)realPage->targetHeight,
             realU0, realV0, realU1, realV1);
    OutputDebugStringA(pageMessage);
#endif
    probeTriangle[0].texcoord[0] = realU0;
    probeTriangle[0].texcoord[1] = realV0;
    probeTriangle[1].texcoord[0] = realU1;
    probeTriangle[1].texcoord[1] = realV0;
    probeTriangle[2].texcoord[0] = realU1;
    probeTriangle[2].texcoord[1] = realV1;
    probeTriangle[3].texcoord[0] = realU0;
    probeTriangle[3].texcoord[1] = realV1;
    if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(device, probeElements, &declaration)) ||
        FAILED(IDirect3DDevice9_CreateVertexShader(device, bs_probe_vs, &vertexShader)) ||
        FAILED(IDirect3DDevice9_CreatePixelShader(device, bs_probe_ps, &pixelShader))) {
        OutputDebugStringA("D3D9 probe: shader or declaration creation failed\n");
        if (pixelShader) IDirect3DPixelShader9_Release(pixelShader);
        if (vertexShader) IDirect3DVertexShader9_Release(vertexShader);
        if (declaration) IDirect3DVertexDeclaration9_Release(declaration);
        Xbox360D3DDevice_destroy(&graphics);
        return 3;
    }
    OutputDebugStringA("D3D9 probe: shaders and vertex declaration created\n");
    IDirect3DDevice9_SetVertexDeclaration(device, declaration);
    IDirect3DDevice9_SetVertexShader(device, vertexShader);
    IDirect3DDevice9_SetPixelShader(device, pixelShader);
    IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    if (FAILED(IDirect3DDevice9_CreateTexture(device, realWidth, realHeight, 1, 0,
                                               D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                               &texture, NULL))) {
        OutputDebugStringA("D3D9 probe: texture creation failed\n");
        IDirect3DDevice9_SetPixelShader(device, NULL);
        IDirect3DDevice9_SetVertexShader(device, NULL);
        IDirect3DDevice9_SetVertexDeclaration(device, NULL);
        IDirect3DPixelShader9_Release(pixelShader);
        IDirect3DVertexShader9_Release(vertexShader);
        IDirect3DVertexDeclaration9_Release(declaration);
        Xbox360D3DDevice_destroy(&graphics);
        return 4;
    }

    D3DLOCKED_RECT locked;
    IDirect3DTexture9_LockRect(texture, 0, &locked, NULL, 0);
    uint32_t source[64 * 64];
#ifdef BS_X360_REAL_RESOURCE_PROBE
    const uint32_t *sourcePixels = (const uint32_t *)realPixels;
    const uint32_t sourcePitch = (uint32_t)realWidth * sizeof(uint32_t);
#else
    const uint32_t *sourcePixels = source;
    const uint32_t sourcePitch = 64 * sizeof(source[0]);
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            bool right = x >= 32;
            bool bottom = y >= 32;
            source[y * 64 + x] = bottom
                ? (right ? 0x80FFFFFF : 0x800000FF)
                : (right ? 0xFF00FF00 : 0xFFFF0000);
        }
    }
#endif
    XGTileTextureLevel((UINT)realWidth, (UINT)realHeight, 0,
                       XGGetGpuFormat(D3DFMT_A8R8G8B8), XGTILE_NONPACKED,
                       locked.pBits, NULL, sourcePixels, sourcePitch, NULL);
    IDirect3DTexture9_UnlockRect(texture, 0);
#ifdef BS_X360_REAL_RESOURCE_PROBE
    free(realPixels);
    realPixels = NULL;
    DataWin_free(realData);
    realData = NULL;
#endif
    IDirect3DDevice9_SetTexture(device, 0, (D3DBaseTexture *) texture);
    IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    OutputDebugStringA("D3D9 probe: tiled texture uploaded and bound\n");
#endif

    for (int frame = 0; frame < BS_X360_TEST_FRAMES; frame++) {
        // No EDRAM persistence is assumed: redraw all pixels after each Present.
        IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET,
                               0xFF000000, 1.0f, 0);
        probeClear(device,   0, 240, 0xFFCC3030);
        probeClear(device, 240, 480, 0xFF30CC30);
        probeClear(device, 480, PROBE_HEIGHT, 0xFF3030CC);
#ifdef BS_X360_D3D_SHADER_PROBE
        IDirect3DDevice9_DrawIndexedPrimitiveUP(device, D3DPT_TRIANGLELIST, 0, 4,
                                                2, probeIndices, D3DFMT_INDEX16,
                                                probeTriangle, sizeof(probeTriangle[0]));
#endif
        Xbox360D3DDevice_present(&graphics);
        if (frame == 0) OutputDebugStringA("D3D9 probe: first Present returned\n");
    }

#ifdef BS_X360_D3D_SHADER_PROBE
    IDirect3DDevice9_SetTexture(device, 0, NULL);
    IDirect3DTexture9_Release(texture);
    IDirect3DDevice9_SetPixelShader(device, NULL);
    IDirect3DDevice9_SetVertexShader(device, NULL);
    IDirect3DDevice9_SetVertexDeclaration(device, NULL);
    IDirect3DPixelShader9_Release(pixelShader);
    IDirect3DVertexShader9_Release(vertexShader);
    IDirect3DVertexDeclaration9_Release(declaration);
#endif
    Xbox360D3DDevice_destroy(&graphics);
    OutputDebugStringA("D3D9 probe: finished and released device\n");
    return 0;
}
