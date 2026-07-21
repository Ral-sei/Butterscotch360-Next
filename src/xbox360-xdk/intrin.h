#pragma once

// common.h includes the desktop MSVC header, but its x86/ARM pause intrinsics
// are not used on PowerPC. Byte-swap intrinsics are declared by XDK stdlib.h.
