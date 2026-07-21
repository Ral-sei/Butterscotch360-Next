/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Derived from ceilingtilefan/Butterscotch-360 commit
 * 7f8f1ea6044dbc55560dfbd2ca9a2f45e472c02e, with later work from
 * flaf1x/Butterscotch360-Refresh and Ral-sei.
 */

#pragma once

// The Xbox XDK exposes its Win32-compatible API through xtl.h.
#include <xtl.h>

// XDK has no desktop file-mapping API. Returning failure makes DataWin use
// its existing buffered-read path without changing the shared parser.
#ifndef FILE_MAP_READ
#define FILE_MAP_READ 0x0004
#endif

static __inline HANDLE CreateFileMappingA(
    HANDLE file,
    void* attributes,
    DWORD protect,
    DWORD maximumSizeHigh,
    DWORD maximumSizeLow,
    const char* name
) {
    (void)file;
    (void)attributes;
    (void)protect;
    (void)maximumSizeHigh;
    (void)maximumSizeLow;
    (void)name;
    return NULL;
}

static __inline void* MapViewOfFile(
    HANDLE mapping,
    DWORD desiredAccess,
    DWORD fileOffsetHigh,
    DWORD fileOffsetLow,
    SIZE_T bytesToMap
) {
    (void)mapping;
    (void)desiredAccess;
    (void)fileOffsetHigh;
    (void)fileOffsetLow;
    (void)bytesToMap;
    return NULL;
}

static __inline BOOL UnmapViewOfFile(const void* address) {
    (void)address;
    return FALSE;
}
