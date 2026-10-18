// Butterscotch Xbox 360 port - monotonic clock.
//
// src/gettime.c selects its backend with platform #if blocks and has no Xbox
// 360 branch. Editing that file is a protected-path change, so the port excludes
// it from its own source list and supplies nowNanos() here instead. See
// docs/xbox360-capability-inventory.md, gap G1.
//
// Port-owned. No public source is touched.

#include "gettime.h"

#include <xtl.h>

uint64_t nowNanos(void) {
    static LARGE_INTEGER frequency;
    if (frequency.QuadPart == 0) {
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart == 0) {
            // Fall back to the 1 ms system timer if the high-resolution counter
            // is unavailable.
            return (uint64_t) GetTickCount() * 1000000ull;
        }
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    uint64_t count = (uint64_t) now.QuadPart;
    uint64_t freq = (uint64_t) frequency.QuadPart;
    return (count / freq) * 1000000000ull + (count % freq) * 1000000000ull / freq;
}
