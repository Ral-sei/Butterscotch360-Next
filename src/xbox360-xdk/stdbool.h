/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Derived from ceilingtilefan/Butterscotch-360 commit
 * 7f8f1ea6044dbc55560dfbd2ca9a2f45e472c02e, with later work from
 * flaf1x/Butterscotch360-Refresh and Ral-sei.
 */

#pragma once

// VS2010 does not ship stdbool.h. Shared sources are compiled as C++ by the
// XDK project, where bool, true, and false are language keywords.
#ifndef __cplusplus
typedef int bool;
#define true 1
#define false 0
#endif
