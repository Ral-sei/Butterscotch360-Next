#ifndef BS_XBOX360_D3D_DEVICE_H
#define BS_XBOX360_D3D_DEVICE_H

#include <stdbool.h>
#include <d3d9.h>

// Device/Present lifecycle shared by the D3D9 probe and the future renderer.
// Does not implement RendererVtable, render targets, or EDRAM resolve.
typedef struct {
    IDirect3D9 *d3d;
    D3DDevice *device;
} Xbox360D3DDevice;

// On failure the device is left empty and can be safely destroyed.
bool Xbox360D3DDevice_create(Xbox360D3DDevice *graphics, UINT width, UINT height);
// Same, with an explicit depth-stencil choice. The renderer disables depth:
// it never uses z and the depth buffer would consume EDRAM it needs for RTs.
bool Xbox360D3DDevice_createEx(Xbox360D3DDevice *graphics, UINT width, UINT height, bool depthStencil);
void Xbox360D3DDevice_present(Xbox360D3DDevice *graphics);
void Xbox360D3DDevice_destroy(Xbox360D3DDevice *graphics);

#endif
