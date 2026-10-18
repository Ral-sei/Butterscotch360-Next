// Sidecar reader/writer for precompressed Xbox 360 texture pages.
//
// Port-owned. No public source is touched.

#include "xbox360_textures.h"

#include "utils.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

static const uint8_t kTextureMagic[8] = { 'B', 'S', 'X', '3', 'T', 'E', 'X', 0 };
// Version 3: DXT pages are generated after transparent-texel colour bleeding.
// Version 2 sidecars are rejected so an old green-fringed cache is regenerated.
#define X360_TEX_SIDECAR_VERSION 3u
#define X360_TEX_HEADER_SIZE_V1  32u
#define X360_TEX_HEADER_SIZE     36u
#define X360_TEX_ENTRY_SIZE      24u
#define X360_TEX_EMPTY_PAGE      0xFFFFFFFFu
#define X360_TEX_FLAG_COMPLETE   0x00000001u

// ===[ Big-endian helpers ]===

static uint32_t x360TexReadU32(const uint8_t *p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16)
         | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

static uint64_t x360TexReadU64(const uint8_t *p) {
    return ((uint64_t) x360TexReadU32(p) << 32) | (uint64_t) x360TexReadU32(p + 4);
}

static void x360TexWriteU32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) v;
}

static void x360TexWriteU64(uint8_t *p, uint64_t v) {
    x360TexWriteU32(p, (uint32_t) (v >> 32));
    x360TexWriteU32(p + 4, (uint32_t) v);
}

// ===[ Reader ]===

struct X360TextureSet {
    FileSystem *fs;
    void *handle;
    X360TextureEntry *entries;
    uint32_t tableCount;
    uint32_t presentCount;
    bool complete;
};

X360TextureSet *X360TextureSet_load(FileSystem *fs, const char *relativePath,
                                    uint32_t gameId, uint64_t dataWinSize) {
    if (fs == NULL || fs->vtable == NULL || fs->vtable->binaryOpen == NULL
        || fs->vtable->binaryRead == NULL || fs->vtable->binarySeek == NULL
        || fs->vtable->binaryClose == NULL || relativePath == NULL) {
        return NULL;
    }

    void *handle = fs->vtable->binaryOpen(fs, relativePath, GML_FILE_BIN_READ);
    if (handle == NULL) {
        logInfo("X360Tex: no sidecar at \"%s\"; pages use runtime compression\n", relativePath);
        return NULL;
    }

    uint8_t header[X360_TEX_HEADER_SIZE];
    memset(header, 0, sizeof(header));
    if (fs->vtable->binaryRead(fs, handle, header, (int32_t) X360_TEX_HEADER_SIZE_V1)
        != (int32_t) X360_TEX_HEADER_SIZE_V1) {
        logWarn("X360Tex: \"%s\" is truncated; ignoring\n", relativePath);
        fs->vtable->binaryClose(fs, handle);
        return NULL;
    }

    if (memcmp(header, kTextureMagic, sizeof(kTextureMagic)) != 0) {
        logWarn("X360Tex: \"%s\" has a bad magic; ignoring\n", relativePath);
        fs->vtable->binaryClose(fs, handle);
        return NULL;
    }

    uint32_t version = x360TexReadU32(header + 8);
    uint32_t headerSize;
    uint32_t flags = 0;
    if (version == 1u) {
        headerSize = X360_TEX_HEADER_SIZE_V1;
    } else if (version == X360_TEX_SIDECAR_VERSION) {
        headerSize = X360_TEX_HEADER_SIZE;
        if (fs->vtable->binaryRead(fs, handle, header + X360_TEX_HEADER_SIZE_V1, 4) != 4) {
            logWarn("X360Tex: \"%s\" has a truncated v3 header; ignoring\n", relativePath);
            fs->vtable->binaryClose(fs, handle);
            return NULL;
        }
        flags = x360TexReadU32(header + X360_TEX_HEADER_SIZE_V1);
    } else {
        logWarn("X360Tex: \"%s\" has an unsupported version (%u); ignoring\n",
                relativePath, version);
        fs->vtable->binaryClose(fs, handle);
        return NULL;
    }

    uint32_t pageCount = x360TexReadU32(header + 12);
    uint32_t tableCount = x360TexReadU32(header + 16);
    uint32_t sidecarGameId = x360TexReadU32(header + 20);
    uint64_t sidecarDataWinSize = x360TexReadU64(header + 24);

    if (sidecarGameId != gameId || sidecarDataWinSize != dataWinSize) {
        logWarn("X360Tex: \"%s\" was built for a different data.win (game %u/%u, want %u/%u); ignoring\n",
                relativePath, sidecarGameId, (unsigned) sidecarDataWinSize,
                gameId, (unsigned) dataWinSize);
        fs->vtable->binaryClose(fs, handle);
        return NULL;
    }

    if (tableCount == 0 || tableCount > 65536u) {
        logWarn("X360Tex: \"%s\" has an invalid table count (%u); ignoring\n",
                relativePath, tableCount);
        fs->vtable->binaryClose(fs, handle);
        return NULL;
    }

    size_t tableBytes = (size_t) tableCount * X360_TEX_ENTRY_SIZE;
    uint8_t *table = (uint8_t *) safeMalloc(tableBytes);
    if (table == NULL
        || fs->vtable->binaryRead(fs, handle, table, (int32_t) tableBytes)
           != (int32_t) tableBytes) {
        logWarn("X360Tex: \"%s\" has a truncated page table; ignoring\n", relativePath);
        free(table);
        fs->vtable->binaryClose(fs, handle);
        return NULL;
    }

    int32_t fileSize = fs->vtable->binarySize != NULL ? fs->vtable->binarySize(fs, handle) : -1;
    X360TextureSet *set = (X360TextureSet *) safeCalloc(1, sizeof(X360TextureSet));
    set->fs = fs;
    set->handle = handle;
    set->tableCount = tableCount;
    set->entries = (X360TextureEntry *) safeCalloc(tableCount, sizeof(X360TextureEntry));
    set->complete = (flags & X360_TEX_FLAG_COMPLETE) != 0;

    uint64_t payloadStart = (uint64_t) headerSize + (uint64_t) tableBytes;
    for (uint32_t i = 0; i < tableCount; i++) {
        const uint8_t *p = table + (size_t) i * X360_TEX_ENTRY_SIZE;
        uint32_t pageId = x360TexReadU32(p);
        if (pageId == X360_TEX_EMPTY_PAGE || pageId >= tableCount) continue;

        X360TextureEntry *entry = &set->entries[pageId];
        entry->pageId = pageId;
        entry->format = p[4];
        entry->width = x360TexReadU32(p + 8);
        entry->height = x360TexReadU32(p + 12);
        entry->dataOffset = x360TexReadU32(p + 16);
        entry->dataSize = x360TexReadU32(p + 20);

        bool valid = entry->format <= (uint8_t) X360_TEXFMT_DXT5
                  && entry->width > 0 && entry->height > 0
                  && entry->dataSize > 0
                  && (uint64_t) entry->dataOffset >= payloadStart
                  && (uint64_t) entry->dataOffset + entry->dataSize <= 0x7FFFFFFFull;
        if (valid && fileSize >= 0) {
            valid = (uint64_t) entry->dataOffset + entry->dataSize <= (uint64_t) fileSize;
        }
        if (!valid) {
            memset(entry, 0, sizeof(*entry));
            continue;
        }
        entry->present = true;
        set->presentCount++;
    }
    free(table);

    if (set->presentCount == 0) {
        logWarn("X360Tex: \"%s\" has no usable pages; ignoring\n", relativePath);
        X360TextureSet_destroy(set);
        return NULL;
    }

    logInfo("X360Tex: loaded \"%s\" (%u/%u pages present, complete=%d, game %u)\n",
            relativePath, set->presentCount,
            pageCount != 0 ? pageCount : tableCount,
            set->complete ? 1 : 0, sidecarGameId);
    return set;
}

void X360TextureSet_destroy(X360TextureSet *set) {
    if (set == NULL) return;
    if (set->handle != NULL && set->fs != NULL && set->fs->vtable != NULL
        && set->fs->vtable->binaryClose != NULL) {
        set->fs->vtable->binaryClose(set->fs, set->handle);
    }
    free(set->entries);
    free(set);
}

const X360TextureEntry *X360TextureSet_find(X360TextureSet *set, int32_t pageId) {
    if (set == NULL || pageId < 0 || (uint32_t) pageId >= set->tableCount) return NULL;
    const X360TextureEntry *entry = &set->entries[pageId];
    return entry->present ? entry : NULL;
}

uint8_t *X360TextureSet_readData(X360TextureSet *set, const X360TextureEntry *entry) {
    if (set == NULL || entry == NULL || !entry->present || entry->dataSize == 0) return NULL;
    if (set->fs == NULL || set->fs->vtable == NULL
        || set->fs->vtable->binarySeek == NULL || set->fs->vtable->binaryRead == NULL) {
        return NULL;
    }
    if (!set->fs->vtable->binarySeek(set->fs, set->handle, (int32_t) entry->dataOffset)) {
        return NULL;
    }
    uint8_t *data = (uint8_t *) safeMalloc(entry->dataSize);
    if (data == NULL) return NULL;
    int32_t read = set->fs->vtable->binaryRead(set->fs, set->handle, data,
                                               (int32_t) entry->dataSize);
    if (read != (int32_t) entry->dataSize) {
        free(data);
        return NULL;
    }
    return data;
}

uint32_t X360TextureSet_count(X360TextureSet *set) {
    return set != NULL ? set->presentCount : 0;
}

bool X360TextureSet_isComplete(X360TextureSet *set) {
    return set != NULL && set->complete;
}

// ===[ Writer (devkit sidecar generation) ]===

struct X360TextureDump {
    FileSystem *fs;
    void *handle;
    uint32_t maxPages;
    uint32_t count;
    uint32_t writePos;
    bool *written;
};

static bool x360TexDumpWriteEntry(X360TextureDump *dump, uint32_t pageId,
                                  X360TextureFormat format, uint32_t width,
                                  uint32_t height, uint32_t dataOffset,
                                  uint32_t dataSize) {
    uint8_t entry[X360_TEX_ENTRY_SIZE];
    memset(entry, 0, sizeof(entry));
    x360TexWriteU32(entry, pageId);
    entry[4] = (uint8_t) format;
    x360TexWriteU32(entry + 8, width);
    x360TexWriteU32(entry + 12, height);
    x360TexWriteU32(entry + 16, dataOffset);
    x360TexWriteU32(entry + 20, dataSize);

    uint32_t entryOffset = X360_TEX_HEADER_SIZE + pageId * X360_TEX_ENTRY_SIZE;
    if (!dump->fs->vtable->binarySeek(dump->fs, dump->handle, (int32_t) entryOffset)) {
        return false;
    }
    return dump->fs->vtable->binaryWrite(dump->fs, dump->handle, entry, sizeof(entry))
           == (int32_t) sizeof(entry);
}

X360TextureDump *X360TextureDump_create(FileSystem *fs, const char *relativePath,
                                        uint32_t maxPages, uint32_t gameId,
                                        uint64_t dataWinSize) {
    if (fs == NULL || fs->vtable == NULL || relativePath == NULL || maxPages == 0
        || fs->vtable->binaryOpen == NULL || fs->vtable->binaryWrite == NULL
        || fs->vtable->binarySeek == NULL || fs->vtable->binaryClose == NULL) {
        return NULL;
    }

    void *handle = fs->vtable->binaryOpen(fs, relativePath, GML_FILE_BIN_WRITE);
    if (handle == NULL) {
        logWarn("X360Tex: cannot create \"%s\" for the texture dump\n", relativePath);
        return NULL;
    }

    uint8_t header[X360_TEX_HEADER_SIZE];
    memset(header, 0, sizeof(header));
    memcpy(header, kTextureMagic, sizeof(kTextureMagic));
    x360TexWriteU32(header + 8, X360_TEX_SIDECAR_VERSION);
    x360TexWriteU32(header + 12, 0); // filled in on close
    x360TexWriteU32(header + 16, maxPages);
    x360TexWriteU32(header + 20, gameId);
    x360TexWriteU64(header + 24, dataWinSize);
    x360TexWriteU32(header + 32, 0); // flags; set on close when complete

    if (fs->vtable->binaryWrite(fs, handle, header, sizeof(header)) != (int32_t) sizeof(header)) {
        fs->vtable->binaryClose(fs, handle);
        return NULL;
    }

    // Reserve the page table and mark every slot empty. The loader can then seek
    // straight to a page without scanning the payload.
    uint8_t empty[X360_TEX_ENTRY_SIZE];
    memset(empty, 0xFF, sizeof(empty));
    for (uint32_t i = 0; i < maxPages; i++) {
        if (fs->vtable->binaryWrite(fs, handle, empty, sizeof(empty)) != (int32_t) sizeof(empty)) {
            fs->vtable->binaryClose(fs, handle);
            return NULL;
        }
    }

    X360TextureDump *dump = (X360TextureDump *) safeCalloc(1, sizeof(X360TextureDump));
    dump->fs = fs;
    dump->handle = handle;
    dump->maxPages = maxPages;
    dump->writePos = X360_TEX_HEADER_SIZE + maxPages * X360_TEX_ENTRY_SIZE;
    dump->written = (bool *) safeCalloc(maxPages, sizeof(bool));
    logInfo("X360Tex: dumping compressed pages to \"%s\" (%u slots)\n", relativePath, maxPages);
    return dump;
}

void X360TextureDump_add(X360TextureDump *dump, int32_t pageId, X360TextureFormat format,
                         uint32_t width, uint32_t height, const uint8_t *data,
                         uint32_t dataSize) {
    if (dump == NULL || data == NULL || dataSize == 0) return;
    if (pageId < 0 || (uint32_t) pageId >= dump->maxPages || dump->written[pageId]) return;
    if ((uint64_t) dump->writePos + dataSize > 0x7FFFFFFFull) {
        logWarn("X360Tex: texture dump exceeds the 2 GB sidecar limit; skipping page %d\n", pageId);
        return;
    }

    if (!dump->fs->vtable->binarySeek(dump->fs, dump->handle, (int32_t) dump->writePos)) return;
    if (dump->fs->vtable->binaryWrite(dump->fs, dump->handle, data, (int32_t) dataSize)
        != (int32_t) dataSize) {
        logWarn("X360Tex: failed writing page %d to the texture dump\n", pageId);
        return;
    }

    if (!x360TexDumpWriteEntry(dump, (uint32_t) pageId, format, width, height,
                               dump->writePos, dataSize)) {
        logWarn("X360Tex: failed writing the page table entry for %d\n", pageId);
        return;
    }

    dump->written[pageId] = true;
    dump->count++;
    dump->writePos += dataSize;

    // Keep the header count current so a dump interrupted by a reboot still
    // describes the pages that made it to disk.
    uint8_t countBuf[4];
    x360TexWriteU32(countBuf, dump->count);
    if (dump->fs->vtable->binarySeek(dump->fs, dump->handle, 12)) {
        dump->fs->vtable->binaryWrite(dump->fs, dump->handle, countBuf, sizeof(countBuf));
    }

    // Leave the stream at the end of the payload for the next append.
    dump->fs->vtable->binarySeek(dump->fs, dump->handle, (int32_t) dump->writePos);
}

void X360TextureDump_close(X360TextureDump *dump, bool complete) {
    if (dump == NULL) return;
    uint8_t tail[8];
    x360TexWriteU32(tail, dump->count);
    x360TexWriteU32(tail + 4, complete ? X360_TEX_FLAG_COMPLETE : 0u);
    if (dump->fs->vtable->binarySeek(dump->fs, dump->handle, 12)) {
        dump->fs->vtable->binaryWrite(dump->fs, dump->handle, tail, 4);
    }
    if (dump->fs->vtable->binarySeek(dump->fs, dump->handle, 32)) {
        dump->fs->vtable->binaryWrite(dump->fs, dump->handle, tail + 4, 4);
    }
    logInfo("X360Tex: texture dump closed (%u pages, complete=%d)\n",
            dump->count, complete ? 1 : 0);
    dump->fs->vtable->binaryClose(dump->fs, dump->handle);
    free(dump->written);
    free(dump);
}
