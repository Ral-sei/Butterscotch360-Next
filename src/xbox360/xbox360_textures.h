#ifndef BS_XBOX360_TEXTURES_H
#define BS_XBOX360_TEXTURES_H

#include <stdbool.h>
#include <stdint.h>

#include "file_system.h"

// Texture compression sidecar for the Xbox 360 port.
//
// The renderer can compress TXTR pages at runtime with XGCompressSurface, but
// a title can also ship a precomputed "data.x360tex" sidecar. When the sidecar
// exists and matches the data.win it was generated from, pages are uploaded
// straight from it (no decode/compress cost); pages missing from the sidecar
// fall back to the runtime path.
//
// The container is big-endian, like data.x360shaders, so the Xenon reader does
// no byte swapping. See docs/xbox360-renderer-design.md section 5.

typedef enum {
    X360_TEXFMT_RGBA8 = 0, // linear 0xAARRGGBB words, row pitch = width * 4
    X360_TEXFMT_DXT1  = 1, // linear DXT1 blocks, row pitch = (width / 4) * 8
    X360_TEXFMT_DXT5  = 2, // linear DXT5 blocks, row pitch = (width / 4) * 16
} X360TextureFormat;

typedef struct {
    uint32_t pageId;
    uint8_t format;       // X360TextureFormat
    uint32_t width;
    uint32_t height;
    uint32_t dataOffset;  // absolute offset in the sidecar
    uint32_t dataSize;
    bool present;
} X360TextureEntry;

typedef struct X360TextureSet X360TextureSet;
typedef struct X360TextureDump X360TextureDump;

// Loads the sidecar through the port file system. Returns NULL when the file is
// absent, malformed, or was generated for a different data.win.
X360TextureSet *X360TextureSet_load(FileSystem *fs, const char *relativePath,
                                    uint32_t gameId, uint64_t dataWinSize);
void X360TextureSet_destroy(X360TextureSet *set);

// True when the sidecar was closed after a full generation pass. Version 1
// sidecars (written before the completeness flag existed) report false.
bool X360TextureSet_isComplete(X360TextureSet *set);

// Returns the entry for a page, or NULL when the sidecar does not cover it.
const X360TextureEntry *X360TextureSet_find(X360TextureSet *set, int32_t pageId);

// Reads a page's payload into a freshly allocated buffer (caller frees).
uint8_t *X360TextureSet_readData(X360TextureSet *set, const X360TextureEntry *entry);

uint32_t X360TextureSet_count(X360TextureSet *set);

// Writes a sidecar incrementally as pages are loaded. Used by the
// BS_X360_DUMP_TEXTURES build to generate data.x360tex on a devkit. The table
// is reserved up front so the loader can seek directly to a page.
X360TextureDump *X360TextureDump_create(FileSystem *fs, const char *relativePath,
                                        uint32_t maxPages, uint32_t gameId,
                                        uint64_t dataWinSize);
void X360TextureDump_add(X360TextureDump *dump, int32_t pageId, X360TextureFormat format,
                         uint32_t width, uint32_t height, const uint8_t *data,
                         uint32_t dataSize);
void X360TextureDump_close(X360TextureDump *dump, bool complete);

#endif /* BS_XBOX360_TEXTURES_H */
