#ifndef _BS_X360_FILE_SYSTEM_H_
#define _BS_X360_FILE_SYSTEM_H_

#include "file_system.h"

// Xbox 360 file system.
//
// Why this exists instead of the public OverlayFileSystem: the XDK CRT only
// accepts backslash separators. Measured on a devkit, fopen("game:\\x") succeeds
// while fopen("game:/x") fails, and OverlayFileSystem normalises every path to
// '/', so it cannot reach console storage. This implementation keeps the same
// two-area semantics (read: save first, then bundle; write: save only) but
// builds backslash paths.
//
// Port-owned. No public source is touched.

// bundlePath: read-only game data (the directory data.win came from).
// savePath:   writable area. Neither is required to exist yet.
FileSystem* X360FileSystem_create(const char* bundlePath, const char* savePath);
void X360FileSystem_destroy(FileSystem* fs);

#endif /* _BS_X360_FILE_SYSTEM_H_ */
