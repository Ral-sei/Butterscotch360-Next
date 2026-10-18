// Butterscotch Xbox 360 port - small libc shims the XDK declares but does not
// implement.
//
// Port-owned. No public source is touched.

#include <stdlib.h>

// The XDK's stdlib.h declares getenv but marks it _XBOX_CRT_UNSUPPORTED, so
// libcMT does not define it and the link fails. vm_builtins.c calls it for
// GML's environment-variable builtin. There is no environment on the console,
// so returning NULL is the honest answer.
char *getenv(const char *name) {
    (void) name;
    return NULL;
}
