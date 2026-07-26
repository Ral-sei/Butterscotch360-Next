/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Loading and diagnostic UI adapted from flaf1x/Butterscotch360-Refresh.
 */

#include "platform_xdk.h"

#include "xdk_ui.h"
#include "d3d9_renderer.h"
#include "debug_font/debug_font.h"
#include "stb_ds.h"

#include <d3d9.h>
#include <d3dx9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    float x, y, z, w;
    float u, v;
    float r, g, b, a;
} XdkUiVertex;

struct XdkUi {
    IDirect3DDevice9* device;
    IDirect3DTexture9* fontTexture;
    IDirect3DTexture9* whiteTexture;
    IDirect3DVertexShader9* vertexShader;
    IDirect3DPixelShader9* pixelShader;
    IDirect3DVertexDeclaration9* vertexDeclaration;
    bool available;
    bool overlayVisible;
    bool overlayComboWasDown;
    float fps;
    float deltaMs;
    int32_t steps;
    uint32_t frameCount;
    double fpsWindowSeconds;
    SIZE_T totalPhysical;
    SIZE_T availablePhysical;
    char loadingStage[128];
};

static IDirect3DTexture9* createTexture(
    IDirect3DDevice9* device,
    const uint8_t* rgba,
    int32_t width,
    int32_t height
) {
    IDirect3DTexture9* texture = NULL;
    HRESULT result = device->CreateTexture(
        width, height, 1, 0, D3DFMT_LIN_A8R8G8B8,
        D3DPOOL_DEFAULT, &texture, NULL
    );
    if (FAILED(result) || !texture) return NULL;

    D3DLOCKED_RECT locked;
    if (FAILED(texture->LockRect(0, &locked, NULL, 0))) {
        texture->Release();
        return NULL;
    }
    for (int32_t y = 0; y < height; y++) {
        const uint8_t* source = rgba + (size_t)y * (size_t)width * 4u;
        DWORD* destination = (DWORD*)((uint8_t*)locked.pBits + (size_t)y * locked.Pitch);
        for (int32_t x = 0; x < width; x++) {
            destination[x] = D3DCOLOR_ARGB(
                source[x * 4 + 3], source[x * 4],
                source[x * 4 + 1], source[x * 4 + 2]
            );
        }
    }
    texture->UnlockRect(0);
    return texture;
}

static IDirect3DTexture9* createFontTexture(IDirect3DDevice9* device) {
    size_t pixelCount = (size_t)DEBUGFONT_ATLAS_W * DEBUGFONT_ATLAS_H;
    uint8_t* rgba = (uint8_t*)malloc(pixelCount * 4u);
    if (!rgba) return NULL;
    for (size_t i = 0; i < pixelCount; i++) {
        rgba[i * 4] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = debugFontPixels[i];
    }
    IDirect3DTexture9* texture = createTexture(
        device, rgba, DEBUGFONT_ATLAS_W, DEBUGFONT_ATLAS_H
    );
    free(rgba);
    return texture;
}

static void applyState(XdkUi* ui) {
    D3DVIEWPORT9 viewport;
    viewport.X = 0;
    viewport.Y = 0;
    viewport.Width = XDK_FRAMEBUFFER_WIDTH;
    viewport.Height = XDK_FRAMEBUFFER_HEIGHT;
    viewport.MinZ = 0.0f;
    viewport.MaxZ = 1.0f;
    ui->device->SetViewport(&viewport);
    ui->device->SetVertexShader(ui->vertexShader);
    ui->device->SetPixelShader(ui->pixelShader);
    ui->device->SetVertexDeclaration(ui->vertexDeclaration);
    ui->device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    ui->device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    ui->device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    ui->device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    ui->device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    ui->device->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
        D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    ui->device->SetRenderState(D3DRS_ZENABLE, FALSE);
    ui->device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    ui->device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    ui->device->SetRenderState(D3DRS_VIEWPORTENABLE, FALSE);
    ui->device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    ui->device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    ui->device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
    ui->device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    ui->device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
}

static void setVertex(
    XdkUiVertex* vertex, float x, float y, float u, float v,
    float r, float g, float b, float a
) {
    vertex->x = x - 0.5f;
    vertex->y = y - 0.5f;
    vertex->z = 0.0f;
    vertex->w = 1.0f;
    vertex->u = u;
    vertex->v = v;
    vertex->r = r;
    vertex->g = g;
    vertex->b = b;
    vertex->a = a;
}

static void drawQuad(
    XdkUi* ui, IDirect3DTexture9* texture,
    float x0, float y0, float x1, float y1,
    float u0, float v0, float u1, float v1,
    float r, float g, float b, float a
) {
    XdkUiVertex vertices[4];
    setVertex(&vertices[0], x0, y0, u0, v0, r, g, b, a);
    setVertex(&vertices[1], x1, y0, u1, v0, r, g, b, a);
    setVertex(&vertices[2], x1, y1, u1, v1, r, g, b, a);
    setVertex(&vertices[3], x0, y1, u0, v1, r, g, b, a);
    ui->device->SetTexture(0, texture ? texture : ui->whiteTexture);
    ui->device->DrawPrimitiveUP(D3DPT_QUADLIST, 1, vertices, sizeof(XdkUiVertex));
}

static void drawText(
    XdkUi* ui, const char* text, float x, float y, float scale,
    float r, float g, float b, float a
) {
    if (!text || !ui->fontTexture) return;
    float startX = x;
    for (const char* cursor = text; *cursor; cursor++) {
        unsigned char codePoint = (unsigned char)*cursor;
        if (codePoint == '\n') {
            x = startX;
            y += DEBUGFONT_LINE_HEIGHT * scale;
            continue;
        }
        if (codePoint < DEBUGFONT_FIRST_CP || codePoint > DEBUGFONT_LAST_CP) codePoint = '?';
        const DebugFontGlyphEntry* glyph = &debugFontGlyphs[codePoint - DEBUGFONT_FIRST_CP];
        float gx0 = x + glyph->xoffset * scale;
        float gy0 = y + glyph->yoffset * scale;
        float gx1 = gx0 + glyph->w * scale;
        float gy1 = gy0 + glyph->h * scale;
        float u0 = ((float)glyph->x + 0.5f) / DEBUGFONT_ATLAS_W;
        float v0 = ((float)glyph->y + 0.5f) / DEBUGFONT_ATLAS_H;
        float u1 = ((float)glyph->x + glyph->w - 0.5f) / DEBUGFONT_ATLAS_W;
        float v1 = ((float)glyph->y + glyph->h - 0.5f) / DEBUGFONT_ATLAS_H;
        drawQuad(ui, ui->fontTexture, gx0, gy0, gx1, gy1,
                 u0, v0, u1, v1, r, g, b, a);
        x += glyph->xadvance * scale;
    }
}

static float textWidth(const char* text, float scale) {
    float width = 0.0f;
    if (!text) return width;
    for (const char* cursor = text; *cursor; cursor++) {
        unsigned char codePoint = (unsigned char)*cursor;
        if (codePoint < DEBUGFONT_FIRST_CP || codePoint > DEBUGFONT_LAST_CP) codePoint = '?';
        width += debugFontGlyphs[codePoint - DEBUGFONT_FIRST_CP].xadvance * scale;
    }
    return width;
}

static void drawLine(
    XdkUi* ui, const char* text, float* y,
    float r, float g, float b
) {
    const float scale = 0.36f;
    drawText(ui, text, 26.0f, *y, scale, r, g, b, 0.96f);
    *y += DEBUGFONT_LINE_HEIGHT * scale + 3.0f;
}

static float bytesToMb(SIZE_T bytes) {
    return (float)((double)bytes / (1024.0 * 1024.0));
}

XdkUi* XdkUi_create(void* d3dDevice) {
    XdkUi* ui = (XdkUi*)calloc(1, sizeof(XdkUi));
    if (!ui) return NULL;
    ui->device = (IDirect3DDevice9*)d3dDevice;
    strcpy(ui->loadingStage, "Starting");

    static const char* vertexSource =
        "struct I{float4 p:POSITION;float2 t:TEXCOORD0;float4 c:TEXCOORD1;};"
        "struct O{float4 p:POSITION;float2 t:TEXCOORD0;float4 c:TEXCOORD1;};"
        "O main(I i){O o;o.p=i.p;o.t=i.t;o.c=i.c;return o;}";
    static const char* pixelSource =
        "sampler2D s:register(s0);"
        "struct I{float2 t:TEXCOORD0;float4 c:TEXCOORD1;};"
        "float4 main(I i):COLOR0{return tex2D(s,i.t)*i.c;}";

    ID3DXBuffer* code = NULL;
    ID3DXBuffer* errors = NULL;
    HRESULT result = D3DXCompileShader(
        vertexSource, (UINT)strlen(vertexSource), NULL, NULL,
        "main", "vs_2_0", 0, &code, &errors, NULL
    );
    if (FAILED(result) || !code) goto failed;
    result = ui->device->CreateVertexShader(
        (const DWORD*)code->GetBufferPointer(), &ui->vertexShader
    );
    code->Release();
    code = NULL;
    if (errors) {
        errors->Release();
        errors = NULL;
    }
    if (FAILED(result)) goto failed;

    result = D3DXCompileShader(
        pixelSource, (UINT)strlen(pixelSource), NULL, NULL,
        "main", "ps_2_0", 0, &code, &errors, NULL
    );
    if (FAILED(result) || !code) goto failed;
    result = ui->device->CreatePixelShader(
        (const DWORD*)code->GetBufferPointer(), &ui->pixelShader
    );
    code->Release();
    code = NULL;
    if (errors) {
        errors->Release();
        errors = NULL;
    }
    if (FAILED(result)) goto failed;

    {
        static const D3DVERTEXELEMENT9 declaration[] = {
            {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            {0, 24, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
            D3DDECL_END()
        };
        if (FAILED(ui->device->CreateVertexDeclaration(
                declaration, &ui->vertexDeclaration))) goto failed;
    }

    ui->fontTexture = createFontTexture(ui->device);
    {
        const uint8_t white[4] = {255, 255, 255, 255};
        ui->whiteTexture = createTexture(ui->device, white, 1, 1);
    }
    ui->available = ui->fontTexture && ui->whiteTexture;
    if (!ui->available) goto failed;
    return ui;

failed:
    if (errors) errors->Release();
    if (code) code->Release();
    Butterscotch_xdkLog("Phase 5 UI initialization failed (hr=0x%08X)", result);
    XdkUi_destroy(ui);
    return NULL;
}

void XdkUi_destroy(XdkUi* ui) {
    if (!ui) return;
    if (ui->device) ui->device->SetTexture(0, NULL);
    if (ui->fontTexture) ui->fontTexture->Release();
    if (ui->whiteTexture) ui->whiteTexture->Release();
    if (ui->vertexShader) ui->vertexShader->Release();
    if (ui->pixelShader) ui->pixelShader->Release();
    if (ui->vertexDeclaration) ui->vertexDeclaration->Release();
    free(ui);
}

void XdkUi_drawLoading(XdkUi* ui, float progress, const char* stage) {
    if (!ui || !ui->available) return;
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    if (stage && stage[0]) {
        _snprintf(ui->loadingStage, sizeof(ui->loadingStage) - 1, "%s", stage);
        ui->loadingStage[sizeof(ui->loadingStage) - 1] = '\0';
    }

    ui->device->Clear(
        0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
        D3DCOLOR_XRGB(13, 16, 20), 1.0f, 0
    );
    if (FAILED(ui->device->BeginScene())) return;
    applyState(ui);

    drawText(ui, "BUTTERSCOTCH360-NEXT", 72.0f, 72.0f, 0.68f,
             1.0f, 0.84f, 0.34f, 1.0f);
    drawText(ui, "Xbox 360 platform", 74.0f, 112.0f, 0.38f,
             0.72f, 0.78f, 0.84f, 1.0f);

    const float barX = 72.0f;
    const float barY = XDK_FRAMEBUFFER_HEIGHT - 118.0f;
    const float barWidth = XDK_FRAMEBUFFER_WIDTH - 144.0f;
    drawQuad(ui, NULL, barX, barY, barX + barWidth, barY + 16.0f,
             0, 0, 1, 1, 0.12f, 0.14f, 0.17f, 1.0f);
    drawQuad(ui, NULL, barX, barY, barX + barWidth * progress, barY + 16.0f,
             0, 0, 1, 1, 1.0f, 0.68f, 0.16f, 1.0f);
    float stageWidth = textWidth(ui->loadingStage, 0.36f);
    drawText(ui, ui->loadingStage,
             ((float)XDK_FRAMEBUFFER_WIDTH - stageWidth) * 0.5f,
             barY + 29.0f, 0.36f, 0.90f, 0.93f, 0.96f, 1.0f);

    ui->device->EndScene();
    ui->device->Present(NULL, NULL, NULL, NULL);
}

void XdkUi_dataWinProgress(
    const char* chunkName, int chunkIndex, int totalChunks,
    DataWin* dataWin, void* userData
) {
    (void)dataWin;
    XdkUi* ui = (XdkUi*)userData;
    if (!ui) return;
    char stage[128];
    _snprintf(
        stage, sizeof(stage) - 1, "Loading data.win: %.4s  %d/%d",
        chunkName ? chunkName : "????", chunkIndex + 1, totalChunks
    );
    stage[sizeof(stage) - 1] = '\0';
    float progress = totalChunks > 0
        ? (float)(chunkIndex + 1) / (float)totalChunks
        : 0.0f;
    XdkUi_drawLoading(ui, 0.08f + progress * 0.84f, stage);
}

void XdkUi_updateDiagnostics(XdkUi* ui, double deltaSeconds, int32_t steps) {
    if (!ui) return;
    XINPUT_STATE state;
    ZeroMemory(&state, sizeof(state));
    bool connected = XInputGetState(0, &state) == ERROR_SUCCESS;
    bool comboDown = connected &&
        (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) &&
        (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER);
    if (comboDown && !ui->overlayComboWasDown) {
        ui->overlayVisible = !ui->overlayVisible;
        Butterscotch_xdkLog(
            "diagnostic overlay %s", ui->overlayVisible ? "enabled" : "disabled"
        );
    }
    ui->overlayComboWasDown = comboDown;
    ui->deltaMs = (float)(deltaSeconds * 1000.0);
    ui->steps = steps;
    if (steps > 0) ui->frameCount++;
    ui->fpsWindowSeconds += deltaSeconds;
    if (ui->fpsWindowSeconds >= 0.5) {
        ui->fps = (float)((double)ui->frameCount / ui->fpsWindowSeconds);
        ui->frameCount = 0;
        ui->fpsWindowSeconds = 0.0;
        MEMORYSTATUS memory;
        ZeroMemory(&memory, sizeof(memory));
        memory.dwLength = sizeof(memory);
        GlobalMemoryStatus(&memory);
        ui->totalPhysical = memory.dwTotalPhys;
        ui->availablePhysical = memory.dwAvailPhys;
    }
}

void XdkUi_drawDiagnostics(XdkUi* ui, Runner* runner, Renderer* renderer) {
    if (!ui || !ui->available || !ui->overlayVisible || !runner || !renderer) return;
    if (renderer->vtable->flush) renderer->vtable->flush(renderer);
    applyState(ui);

    D3D9Renderer* d3d = (D3D9Renderer*)renderer;
    int32_t activeSurfaces = 0;
    uint32_t residentTexturePages = 0;
    for (int32_t i = 0; i < D3D9_MAX_SURFACES; i++) {
        if (d3d->surfaceActive[i]) activeSurfaces++;
    }
    for (uint32_t i = 0; i < d3d->originalTexturePageCount; i++) {
        if (d3d->textures[i]) residentTexturePages++;
    }
    const char* roomName = runner->currentRoom && runner->currentRoom->name
        ? runner->currentRoom->name : "(none)";
    uint32_t roomSpeed = runner->currentRoom ? runner->currentRoom->speed : 0;
    SIZE_T usedPhysical = ui->totalPhysical > ui->availablePhysical
        ? ui->totalPhysical - ui->availablePhysical : 0;

    char line[256];
    float y = 28.0f;
    drawText(ui, "BUTTERSCOTCH360-NEXT DIAGNOSTICS  [LB+RB]",
             26.0f, y, 0.40f, 1.0f, 0.84f, 0.34f, 1.0f);
    y += 25.0f;

    _snprintf(line, sizeof(line) - 1, "FPS %.1f   dt %.2f ms   steps %d   room speed %u",
              ui->fps, ui->deltaMs, ui->steps, roomSpeed);
    line[sizeof(line) - 1] = '\0';
    drawLine(ui, line, &y, 0.94f, 0.96f, 0.98f);

    _snprintf(line, sizeof(line) - 1, "Room %d: %s   instances %d   pending %d",
              runner->currentRoomIndex, roomName, (int32_t)arrlen(runner->instances),
              runner->pendingRoom);
    line[sizeof(line) - 1] = '\0';
    drawLine(ui, line, &y, 0.80f, 0.90f, 1.0f);

    _snprintf(line, sizeof(line) - 1, "RAM %.1f / %.1f MB   free %.1f MB",
              bytesToMb(usedPhysical), bytesToMb(ui->totalPhysical),
              bytesToMb(ui->availablePhysical));
    line[sizeof(line) - 1] = '\0';
    drawLine(ui, line, &y, 0.70f, 1.0f, 0.72f);

    _snprintf(line, sizeof(line) - 1, "TXTR %.1f / %.0f MiB   resident %u/%u   frame %u",
              bytesToMb(d3d->textureResidentBytes),
              (double)D3D9_TXTR_CACHE_BUDGET / (1024.0 * 1024.0),
              residentTexturePages, d3d->originalTexturePageCount,
              d3d->textureFrame);
    line[sizeof(line) - 1] = '\0';
    drawLine(ui, line, &y, 0.70f, 1.0f, 0.72f);

    _snprintf(line, sizeof(line) - 1, "Surfaces %d/%d   target %d   app %d   auto %d",
              activeSurfaces, D3D9_MAX_SURFACES, d3d->currentSurfaceTarget,
              runner->applicationSurfaceId, runner->appSurfaceAutoDraw ? 1 : 0);
    line[sizeof(line) - 1] = '\0';
    drawLine(ui, line, &y, 0.80f, 0.90f, 1.0f);

    _snprintf(line, sizeof(line) - 1, "Game %dx%d   app %dx%d   GUI %dx%d",
              d3d->gameW, d3d->gameH, runner->applicationWidth,
              runner->applicationHeight, runner->guiWidth, runner->guiHeight);
    line[sizeof(line) - 1] = '\0';
    drawLine(ui, line, &y, 0.80f, 0.90f, 1.0f);
}
