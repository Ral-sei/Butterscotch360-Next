/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Derived from ceilingtilefan/Butterscotch-360 commit
 * 7f8f1ea6044dbc55560dfbd2ca9a2f45e472c02e, with later work from
 * flaf1x/Butterscotch360-Refresh and Ral-sei.
 */

#include "platform_xdk.h"

#include "runner.h"

static Runner* xdkRunner = nullptr;
static int32_t xdkWindowWidth = 640;
static int32_t xdkWindowHeight = 480;

static void xdkSetWindowTitle(const char* title) {
    if (title != nullptr && *title != '\0') {
        Butterscotch_xdkLog("title: %s", title);
    }
}

static bool xdkGetWindowSize(int32_t* outWidth, int32_t* outHeight) {
    int32_t width = xdkWindowWidth;
    int32_t height = xdkWindowHeight;
    if (xdkRunner != nullptr) {
        if (xdkRunner->applicationWidth > 0) width = xdkRunner->applicationWidth;
        if (xdkRunner->applicationHeight > 0) height = xdkRunner->applicationHeight;
    }
    if (outWidth != nullptr) *outWidth = width;
    if (outHeight != nullptr) *outHeight = height;
    return true;
}

static void xdkSetWindowSize(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) return;
    if (xdkWindowWidth == width && xdkWindowHeight == height) return;

    xdkWindowWidth = width;
    xdkWindowHeight = height;
    if (xdkRunner != nullptr) {
        xdkRunner->applicationWidth = width;
        xdkRunner->applicationHeight = height;
    }
    Butterscotch_xdkLog("logical window resize requested: %dx%d", width, height);
}

static bool xdkWindowHasFocus(void) {
    return true;
}

static void xdkSetCursor(MAYBE_UNUSED int32_t cursorType) {
}

extern "C" void XdkPlatform_attachRunnerCallbacks(Runner* runner) {
    if (runner == nullptr) return;
    xdkRunner = runner;
    if (runner->applicationWidth > 0) xdkWindowWidth = runner->applicationWidth;
    if (runner->applicationHeight > 0) xdkWindowHeight = runner->applicationHeight;
    runner->setWindowTitle = xdkSetWindowTitle;
    runner->getWindowSize = xdkGetWindowSize;
    runner->setWindowSize = xdkSetWindowSize;
    runner->windowHasFocus = xdkWindowHasFocus;
    runner->setCursor = xdkSetCursor;
}

extern "C" void XdkPlatform_getFramebufferSize(int32_t* outWidth, int32_t* outHeight) {
    if (outWidth != nullptr) *outWidth = XDK_FRAMEBUFFER_WIDTH;
    if (outHeight != nullptr) *outHeight = XDK_FRAMEBUFFER_HEIGHT;
}
