/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Derived from ceilingtilefan/Butterscotch-360 commit
 * 7f8f1ea6044dbc55560dfbd2ca9a2f45e472c02e, with later work from
 * flaf1x/Butterscotch360-Refresh and Ral-sei.
 */

#pragma once

// Minimal compatibility layer for the VS2010-era Xbox 360 XDK compiler.
// Shared utility and allocation macros intentionally remain owned by Next.

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__cplusplus) && __cplusplus < 201103L && !defined(nullptr)
#define nullptr NULL
#endif

#ifndef strdup
#define strdup _strdup
#endif

#ifndef fileno
#define fileno _fileno
#endif

#ifndef isnan
#define isnan _isnan
#endif

#ifndef isinf
#define isinf(value) (!_finite(value))
#endif

#ifndef nextafter
#define nextafter _nextafter
#endif

static __inline char* Butterscotch_xdkGetenv(const char* name) {
    (void)name;
    return NULL;
}

#define getenv Butterscotch_xdkGetenv

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

#ifndef INFINITY
#define INFINITY ((float) HUGE_VAL)
#endif

#if defined(_MSC_VER) && _MSC_VER < 1900
static __inline int Butterscotch_xdkVsnprintf(
    char* buffer,
    size_t bufferSize,
    const char* format,
    va_list args
) {
    int required = _vscprintf(format, args);
    if (buffer != NULL && bufferSize > 0) {
        _vsnprintf(buffer, bufferSize, format, args);
        buffer[bufferSize - 1] = '\0';
    }
    return required;
}

static __inline int Butterscotch_xdkSnprintf(
    char* buffer,
    size_t bufferSize,
    const char* format,
    ...
) {
    int result;
    va_list args;
    va_start(args, format);
    result = Butterscotch_xdkVsnprintf(buffer, bufferSize, format, args);
    va_end(args);
    return result;
}

#define vsnprintf Butterscotch_xdkVsnprintf
#define snprintf Butterscotch_xdkSnprintf
#endif

#if defined(_XBOX)
#ifdef __cplusplus
extern "C" void Butterscotch_xdkAbort(const char* file, int line);
#else
void Butterscotch_xdkAbort(const char* file, int line);
#endif

#ifndef abort
#define abort() Butterscotch_xdkAbort(__FILE__, __LINE__)
#endif
#endif
