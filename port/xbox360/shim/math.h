// Butterscotch Xbox 360 port - `math.h` shim.
//
// The XDK's math.h is MSVC's: it is missing several C99 functions that public
// and vendored code uses. `#include_next` pulls in the real header first, then
// this file supplies only what is genuinely absent. Nothing here overrides an
// existing declaration, and the public NO_* switches in real_type.h /
// math_compat.h are still used for the functions they cover.
//
// Port-owned header on the port's include path; no public source is touched.

#ifndef _BS_X360_MATH_SHIM_H_
#define _BS_X360_MATH_SHIM_H_

#include_next <math.h>

// box2d's math_functions.h calls remainderf. The XDK does not declare it.
#ifndef remainderf
static __inline float remainderf(float x, float y) {
    if (y == 0.0f) {
        // IEEE remainder is undefined for y == 0.
        return (x != x) ? x : (0.0f / 0.0f);
    }
    if (x != x) return x;
    float q = x / y;
    float n = (q >= 0.0f) ? (float) (long) (q + 0.5f) : (float) (long) (q - 0.5f);
    return x - n * y;
}
#endif

#endif /* _BS_X360_MATH_SHIM_H_ */
