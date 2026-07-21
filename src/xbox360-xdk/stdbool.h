#pragma once

// VS2010 does not ship stdbool.h. Shared sources are compiled as C++ by the
// XDK project, where bool, true, and false are language keywords.
#ifndef __cplusplus
typedef int bool;
#define true 1
#define false 0
#endif
