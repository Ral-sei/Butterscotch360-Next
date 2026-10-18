// Butterscotch Xbox 360 port - platform log sink.
//
// src/log.c calls platformLog(logType, fmt, va_list); every platform must
// provide it. On Xbox 360 the output goes to an attached XBDM debugger, which
// is what OXDK's `xbdm.py <host> listen` shows.
//
// Port-owned. No public source is touched.

#include "log.h"

#include <xtl.h>
#include <stdarg.h>

void platformLog(const logType type, const char *format, va_list va) {
    char buffer[1024];
    const char *prefix = "";

    switch (type) {
        case LOG_TYPE_NORMAL:  break;
        case LOG_TYPE_WARNING: prefix = "Warning: "; break;
        case LOG_TYPE_ERROR:   prefix = "Error: ";   break;
        case LOG_TYPE_DEBUG:   prefix = "Debug: ";   break;
    }

    int written = snprintf(buffer, sizeof(buffer), "%s", prefix);
    if (written < 0 || written >= (int) sizeof(buffer)) {
        written = 0;
    }
    vsnprintf(buffer + written, (int) sizeof(buffer) - written, format, va);
    buffer[sizeof(buffer) - 1] = '\0';

    OutputDebugStringA(buffer);
}
