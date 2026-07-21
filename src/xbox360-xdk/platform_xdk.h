/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Derived from ceilingtilefan/Butterscotch-360 commit
 * 7f8f1ea6044dbc55560dfbd2ca9a2f45e472c02e, with later work from
 * flaf1x/Butterscotch360-Refresh and Ral-sei.
 */

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
