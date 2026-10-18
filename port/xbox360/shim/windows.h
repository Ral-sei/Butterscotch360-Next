// Butterscotch Xbox 360 port - `windows.h` shim.
//
// The public sources take Windows code paths whenever _WIN32 is defined, and
// _WIN32 must be defined for the XDK's CRT headers to parse. Two of those paths
// call Win32 APIs that Xenon does not provide:
//
//   * data_win.c  mapFile()         -> CreateFileMappingA / MapViewOfFile / UnmapViewOfFile
//   * utils.h     dropMappedRange() -> GetModuleHandleW + GetProcAddress("DiscardVirtualMemory")
//
// Returning NULL/0 makes mapFile() fail, which sends the loader down its
// non-mapped path, and makes dropMappedRange() a no-op. Nothing else changes:
// the stubs are only reached through the _WIN32 branches the public code
// already has.
//
// This is a port-owned header that sits on the port's include path. No public
// source is modified, and no other platform sees it.

#ifndef _BS_X360_WINDOWS_SHIM_H_
#define _BS_X360_WINDOWS_SHIM_H_

// xtl.h is the XDK's umbrella header and is where the real Windows-compatible
// types (HANDLE, DWORD, BOOL, HMODULE, PVOID, WINAPI, the VK_ constants) come
// from. The public code expects them via <windows.h>; route it here.
#include <xtl.h>

// Some Xenon CRT headers declare a Windows name under a slightly different
// spelling. Nothing here redefines an existing symbol: each stub is only
// compiled if the XDK did not already provide the API.
#ifndef CreateFileMappingA
static __inline HANDLE CreateFileMappingA(HANDLE hFile, void *lpFileMappingAttributes,
                                          DWORD flProtect, DWORD dwMaximumSizeHigh,
                                          DWORD dwMaximumSizeLow, const char *lpName) {
    (void) hFile; (void) lpFileMappingAttributes; (void) flProtect;
    (void) dwMaximumSizeHigh; (void) dwMaximumSizeLow; (void) lpName;
    return NULL;
}
#endif

#ifndef MapViewOfFile
static __inline void *MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess,
                                    DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow,
                                    size_t dwNumberOfBytesToMap) {
    (void) hFileMappingObject; (void) dwDesiredAccess;
    (void) dwFileOffsetHigh; (void) dwFileOffsetLow; (void) dwNumberOfBytesToMap;
    return NULL;
}
#endif

#ifndef UnmapViewOfFile
static __inline BOOL UnmapViewOfFile(const void *lpBaseAddress) {
    (void) lpBaseAddress;
    return FALSE;
}
#endif

#ifndef GetModuleHandleW
static __inline HMODULE GetModuleHandleW(const wchar_t *lpModuleName) {
    (void) lpModuleName;
    return NULL;
}
#endif

// data_win.c's Windows branch passes FILE_MAP_READ to MapViewOfFile. The XDK
// defines PAGE_READONLY but not this one.
#ifndef FILE_MAP_READ
#define FILE_MAP_READ 0x0004
#endif

#endif /* _BS_X360_WINDOWS_SHIM_H_ */
