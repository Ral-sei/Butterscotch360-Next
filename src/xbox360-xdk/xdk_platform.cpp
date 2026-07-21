#include "platform_xdk.h"

#include "runner.h"

static void xdkSetWindowTitle(const char* title) {
    if (title != nullptr && *title != '\0') {
        Butterscotch_xdkLog("title: %s", title);
    }
}

static bool xdkGetWindowSize(int32_t* outWidth, int32_t* outHeight) {
    if (outWidth != nullptr) *outWidth = XDK_FRAMEBUFFER_WIDTH;
    if (outHeight != nullptr) *outHeight = XDK_FRAMEBUFFER_HEIGHT;
    return true;
}

static void xdkSetWindowSize(MAYBE_UNUSED int32_t width, MAYBE_UNUSED int32_t height) {
}

static bool xdkWindowHasFocus(void) {
    return true;
}

static void xdkSetCursor(MAYBE_UNUSED int32_t cursorType) {
}

extern "C" void XdkPlatform_attachRunnerCallbacks(Runner* runner) {
    if (runner == nullptr) return;
    runner->setWindowTitle = xdkSetWindowTitle;
    runner->getWindowSize = xdkGetWindowSize;
    runner->setWindowSize = xdkSetWindowSize;
    runner->windowHasFocus = xdkWindowHasFocus;
    runner->setCursor = xdkSetCursor;
}

extern "C" void XdkPlatform_getFramebufferSize(int32_t* outWidth, int32_t* outHeight) {
    xdkGetWindowSize(outWidth, outHeight);
}
