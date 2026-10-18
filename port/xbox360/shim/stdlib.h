// Butterscotch Xbox 360 port - `stdlib.h` shim.
//
// The XDK's stdlib.h hides the getenv declaration behind `#ifndef _XBSTRICT`
// and marks it _XBOX_CRT_UNSUPPORTED. The port supplies getenv (returning NULL,
// since Xenon has no environment) in src/xbox360/xbox360_compat.c, but the
// declaration must be visible to callers such as vm_builtins.c. At -O2 clang
// treats the missing prototype as an error, so declare it here after the real
// header. Port-owned; no public source is touched.

#ifndef _BS_X360_STDLIB_SHIM_H_
#define _BS_X360_STDLIB_SHIM_H_

#include_next <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

char *getenv(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* _BS_X360_STDLIB_SHIM_H_ */
