/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Derived from ceilingtilefan/Butterscotch-360 commit
 * 7f8f1ea6044dbc55560dfbd2ca9a2f45e472c02e, with later work from
 * flaf1x/Butterscotch360-Refresh and Ral-sei.
 */

#pragma once

// common.h includes the desktop MSVC header, but its x86/ARM pause intrinsics
// are not used on PowerPC. Byte-swap intrinsics are declared by XDK stdlib.h.
