#pragma once

#include <xtl.h>
#include <stdint.h>

#define XDK_FRAMEBUFFER_WIDTH 1280
#define XDK_FRAMEBUFFER_HEIGHT 720

typedef struct Runner Runner;

#ifdef __cplusplus
extern "C" {
#endif

void Butterscotch_xdkAbort(const char* file, int line);
void Butterscotch_xdkLog(const char* format, ...);
void XdkPlatform_attachRunnerCallbacks(Runner* runner);
void XdkPlatform_getFramebufferSize(int32_t* outWidth, int32_t* outHeight);

#ifdef __cplusplus
}
#endif
