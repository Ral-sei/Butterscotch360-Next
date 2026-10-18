// Butterscotch Xbox 360 port - minimal XEX (toolchain probe).
//
// This translation unit links nothing from the Butterscotch runtime. Its only
// job is to prove, end to end, that:
//   * OXDK's patched clang compiles for powerpc-unknown-none-elf with the Xenon
//     ABI,
//   * oxdklink resolves the console imports (xapilib, libcMT),
//   * the result is a loadable XEX2.
//
// Step 2.4 of docs/xbox360-platform-plan.md (a runtime that boots and reads a
// test game with no graphics and no audio) is a later, separate target.
//
// Port-owned. No public Butterscotch source is involved.

#include <xtl.h>

int main(void) {
    // Delivered to an attached XBDM debugger ("xbdm.py <host> listen").
    OutputDebugStringA("Butterscotch Xbox 360: minimal XEX up (toolchain probe)\n");

    // Stay resident briefly so the debug string is captured, then let the CRT
    // exit the title. A real title would drive its own frame loop instead.
    Sleep(1000);
    return 0;
}
