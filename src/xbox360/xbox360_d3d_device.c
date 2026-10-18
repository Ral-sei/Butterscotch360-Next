#include <xtl.h>
#include "xbox360_d3d_device.h"

#include <string.h>

bool Xbox360D3DDevice_createEx(Xbox360D3DDevice *graphics, UINT width, UINT height, bool depthStencil) {
    if (!graphics || width == 0 || height == 0) return false;
    memset(graphics, 0, sizeof(*graphics));

    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth = width;
    pp.BackBufferHeight = height;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.BackBufferCount = 1;
    pp.MultiSampleType = D3DMULTISAMPLE_NONE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.Windowed = FALSE;
    pp.EnableAutoDepthStencil = depthStencil ? TRUE : FALSE;
    pp.AutoDepthStencilFormat = depthStencil ? D3DFMT_D24S8 : D3DFMT_UNKNOWN;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    graphics->d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!graphics->d3d) {
        OutputDebugStringA("D3D9: Direct3DCreate9 failed\n");
        return false;
    }

    HRESULT hr = IDirect3D9_CreateDevice(graphics->d3d, D3DADAPTER_DEFAULT,
                                         D3DDEVTYPE_HAL, NULL,
                                         D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                         &pp, &graphics->device);
    if (FAILED(hr) || !graphics->device) {
        OutputDebugStringA("D3D9: CreateDevice failed\n");
        Xbox360D3DDevice_destroy(graphics);
        return false;
    }
    return true;
}

bool Xbox360D3DDevice_create(Xbox360D3DDevice *graphics, UINT width, UINT height) {
    return Xbox360D3DDevice_createEx(graphics, width, height, true);
}

void Xbox360D3DDevice_present(Xbox360D3DDevice *graphics) {
    // Xbox 360 Present performs presentation-interval synchronization internally.
    // The renderer must redraw/resolve its targets on every frame: EDRAM content
    // is not guaranteed to survive Present.
    IDirect3DDevice9_Present(graphics->device, NULL, NULL, NULL, NULL);
}

void Xbox360D3DDevice_destroy(Xbox360D3DDevice *graphics) {
    if (!graphics) return;
    if (graphics->device) IDirect3DDevice9_Release(graphics->device);
    if (graphics->d3d) IDirect3D9_Release(graphics->d3d);
    memset(graphics, 0, sizeof(*graphics));
}
