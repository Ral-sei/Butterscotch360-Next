// Butterscotch Xbox 360 port - file system backend.
//
// The XDK CRT requires backslash separators; the public OverlayFileSystem
// normalises every path to '/' and therefore cannot open anything on the
// console. This is the port's own implementation with the same two-area
// semantics:
//
//   * reads  : the save area wins if the file exists there, else the bundle
//   * writes : the save area only
//   * delete : the save area only
//
// The bundle is read-only game data (the directory data.win came from); the save
// area is where GML's file and INI functions write. A real save policy (XContent
// content packages, user selection, quota) is a separate, policy-gated step and
// is not implemented here.
//
// Port-owned. No public source is touched.

#include "xbox360_file_system.h"
#include "utils.h"
#include "stb_ds.h"

#include "stdio_compat.h"
#include <stdlib.h>
#include "string_compat.h"
#include <sys/stat.h>
#include <direct.h>
#include <windows.h>

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#endif

#define X360_FS_SEP '\\'

typedef struct {
    FileSystem base;
    char* bundlePath; // ends with '\'
    char* savePath;   // ends with '\'
} X360FileSystem;

// ===[ Path helpers ]===

// Copies `path` with every '/' turned into '\'. Caller frees.
static char* x360Normalize(const char* path) {
    if (path == nullptr) return nullptr;
    char* out = safeStrdup(path);
    for (int i = 0; out[i] != '\0'; i++) {
        if (out[i] == '/') out[i] = X360_FS_SEP;
    }
    return out;
}

// A path is absolute if it names a drive ("game:") or starts at a root.
static bool x360IsAbsolute(const char* path) {
    return path != nullptr && (path[0] == X360_FS_SEP || strchr(path, ':') != nullptr);
}

// Joins base (ends with '\') and an already-normalised relative path. Caller frees.
static char* x360JoinPath(const char* base, const char* relative) {
    size_t baseLen = strlen(base);
    size_t relLen = strlen(relative);
    char* full = (char*) safeMalloc(baseLen + relLen + 1);
    memcpy(full, base, baseLen);
    memcpy(full + baseLen, relative, relLen);
    full[baseLen + relLen] = '\0';
    return full;
}

// Returns `path` guaranteed to end with '\'. Empty input becomes ".\\". Caller frees.
static char* x360WithTrailingSlash(const char* path) {
    if (path == nullptr || path[0] == '\0') return safeStrdup(".\\");
    char* normalized = x360Normalize(path);
    size_t len = strlen(normalized);
    if (normalized[len - 1] == X360_FS_SEP) return normalized;
    char* out = (char*) safeMalloc(len + 2);
    memcpy(out, normalized, len);
    out[len] = X360_FS_SEP;
    out[len + 1] = '\0';
    free(normalized);
    return out;
}

static bool x360PathExists(const char* fullPath) {
    struct stat st;
    return stat(fullPath, &st) == 0;
}

// Full path for reads. Absolute inputs pass through; otherwise the save area
// wins when the file is there, else the bundle.
static char* x360ResolveForRead(X360FileSystem* fs, const char* relativePath) {
    char* normalized = x360Normalize(relativePath);
    if (normalized == nullptr) return nullptr;
    if (x360IsAbsolute(normalized)) return normalized;

    if (strncmp(normalized, fs->savePath, strlen(fs->savePath)) == 0) return normalized;
    if (strncmp(normalized, fs->bundlePath, strlen(fs->bundlePath)) == 0) return normalized;

    char* saveFull = x360JoinPath(fs->savePath, normalized);
    if (x360PathExists(saveFull)) {
        free(normalized);
        return saveFull;
    }
    free(saveFull);
    char* bundleFull = x360JoinPath(fs->bundlePath, normalized);
    free(normalized);
    return bundleFull;
}

// Full path for writes. Always the save area for relative inputs.
static char* x360ResolveForWrite(X360FileSystem* fs, const char* relativePath) {
    char* normalized = x360Normalize(relativePath);
    if (normalized == nullptr) return nullptr;
    if (x360IsAbsolute(normalized)) return normalized;

    if (strncmp(normalized, fs->savePath, strlen(fs->savePath)) == 0) return normalized;
    if (strncmp(normalized, fs->bundlePath, strlen(fs->bundlePath)) == 0) return normalized;

    char* full = x360JoinPath(fs->savePath, normalized);
    free(normalized);
    return full;
}

// Creates the parent directory chain of fullPath so nested saves work.
static void x360EnsureParentDir(const char* fullPath) {
    char buf[1024];
    size_t len = strlen(fullPath);
    if (len >= sizeof(buf)) return;
    memcpy(buf, fullPath, len + 1);

    char* lastSep = strrchr(buf, X360_FS_SEP);
    if (lastSep == nullptr || lastSep == buf) return;
    // Do not try to create a drive root ("game:" has no trailing separator to trim).
    if (lastSep[-1] == ':') return;
    *lastSep = '\0';

    for (size_t i = 1; len > i; i++) {
        if (buf[i] == X360_FS_SEP) {
            buf[i] = '\0';
            _mkdir(buf);
            buf[i] = X360_FS_SEP;
        }
    }
    _mkdir(buf);
}

// ===[ Vtable ]===

static char* x360ResolvePath(FileSystem* fs, const char* relativePath) {
    X360FileSystem* xfs = (X360FileSystem*) fs;
    char* normalized = x360Normalize(relativePath);
    if (normalized == nullptr) return nullptr;
    if (x360IsAbsolute(normalized)) return normalized;

    if (strncmp(normalized, xfs->savePath, strlen(xfs->savePath)) == 0) return normalized;
    if (strncmp(normalized, xfs->bundlePath, strlen(xfs->bundlePath)) == 0) return normalized;

    char* full = x360JoinPath(xfs->bundlePath, normalized);
    free(normalized);
    return full;
}

static bool x360FileExists(FileSystem* fs, const char* relativePath) {
    char* fullPath = x360ResolveForRead((X360FileSystem*) fs, relativePath);
    if (fullPath == nullptr) return false;
    bool exists = x360PathExists(fullPath);
    free(fullPath);
    return exists;
}

static char* x360ReadFileText(FileSystem* fs, const char* relativePath) {
    char* fullPath = x360ResolveForRead((X360FileSystem*) fs, relativePath);
    if (fullPath == nullptr) return nullptr;
    FILE* f = fopen(fullPath, "rb");
    free(fullPath);
    if (f == nullptr) return nullptr;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) size = 0;

    char* content = (char*) safeMalloc((size_t) size + 1);
    size_t bytesRead = fread(content, 1, (size_t) size, f);
    content[bytesRead] = '\0';
    fclose(f);
    return content;
}

static bool x360WriteFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    char* fullPath = x360ResolveForWrite((X360FileSystem*) fs, relativePath);
    if (fullPath == nullptr) return false;
    x360EnsureParentDir(fullPath);
    FILE* f = fopen(fullPath, "wb");
    free(fullPath);
    if (f == nullptr) return false;

    size_t len = strlen(contents);
    size_t written = fwrite(contents, 1, len, f);
    fclose(f);
    return written == len;
}

static bool x360DeleteFile(FileSystem* fs, const char* relativePath) {
    char* fullPath = x360ResolveForWrite((X360FileSystem*) fs, relativePath);
    if (fullPath == nullptr) return false;
    int result = remove(fullPath);
    free(fullPath);
    return result == 0;
}

static bool x360RenameFile(FileSystem* fs, const char* oldRelativePath, const char* newRelativePath) {
    char* oldFullPath = x360ResolveForWrite((X360FileSystem*) fs, oldRelativePath);
    char* newFullPath = x360ResolveForWrite((X360FileSystem*) fs, newRelativePath);
    if (oldFullPath == nullptr || newFullPath == nullptr) {
        free(oldFullPath);
        free(newFullPath);
        return false;
    }
    int result = rename(oldFullPath, newFullPath);
    free(oldFullPath);
    free(newFullPath);
    return result == 0;
}

static bool x360ReadFileBinary(FileSystem* fs, const char* relativePath, uint8_t** outData, int32_t* outSize) {
    char* fullPath = x360ResolveForRead((X360FileSystem*) fs, relativePath);
    if (fullPath == nullptr) return false;
    FILE* f = fopen(fullPath, "rb");
    free(fullPath);
    if (f == nullptr) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) size = 0;

    uint8_t* data = (uint8_t*) safeMalloc((size_t) size);
    size_t bytesRead = fread(data, 1, (size_t) size, f);
    fclose(f);

    *outData = data;
    *outSize = (int32_t) bytesRead;
    return true;
}

static bool x360WriteFileBinary(FileSystem* fs, const char* relativePath, const uint8_t* data, int32_t size) {
    char* fullPath = x360ResolveForWrite((X360FileSystem*) fs, relativePath);
    if (fullPath == nullptr) return false;
    x360EnsureParentDir(fullPath);
    FILE* f = fopen(fullPath, "wb");
    free(fullPath);
    if (f == nullptr) return false;

    size_t written = fwrite(data, 1, (size_t) size, f);
    fclose(f);
    return written == (size_t) size;
}

// ===[ Streaming binary I/O ]===

typedef struct {
    FILE* fp;
    char* fullPath;
} X360BinaryHandle;

static void* x360BinaryOpen(FileSystem* fs, const char* relativePath, int32_t mode) {
    X360FileSystem* xfs = (X360FileSystem*) fs;
    char* path = nullptr;
    FILE* f = nullptr;

    if (mode == GML_FILE_BIN_READ) {
        path = x360ResolveForRead(xfs, relativePath);
        if (path != nullptr) f = fopen(path, "rb");
    } else if (mode == GML_FILE_BIN_WRITE) {
        path = x360ResolveForWrite(xfs, relativePath);
        if (path != nullptr) {
            x360EnsureParentDir(path);
            f = fopen(path, "wb");
        }
    } else {
        // GML_FILE_BIN_READWRITE: preserve existing contents when possible.
        path = x360ResolveForRead(xfs, relativePath);
        if (path != nullptr) f = fopen(path, "r+b");
        if (f == nullptr) {
            free(path);
            path = x360ResolveForWrite(xfs, relativePath);
            if (path != nullptr) {
                x360EnsureParentDir(path);
                f = fopen(path, "w+b");
            }
        }
    }

    if (f == nullptr) {
        free(path);
        return nullptr;
    }

    X360BinaryHandle* h = (X360BinaryHandle*) safeMalloc(sizeof(X360BinaryHandle));
    h->fp = f;
    h->fullPath = path;
    return h;
}

static void x360BinaryClose(MAYBE_UNUSED FileSystem* fs, void* handle) {
    if (handle == nullptr) return;
    X360BinaryHandle* h = (X360BinaryHandle*) handle;
    if (h->fp != nullptr) fclose(h->fp);
    free(h->fullPath);
    free(h);
}

static int32_t x360BinaryRead(MAYBE_UNUSED FileSystem* fs, void* handle, void* dst, int32_t n) {
    if (handle == nullptr || n <= 0) return 0;
    return (int32_t) fread(dst, 1, (size_t) n, ((X360BinaryHandle*) handle)->fp);
}

static int32_t x360BinaryWrite(MAYBE_UNUSED FileSystem* fs, void* handle, const void* src, int32_t n) {
    if (handle == nullptr || n <= 0) return 0;
    return (int32_t) fwrite(src, 1, (size_t) n, ((X360BinaryHandle*) handle)->fp);
}

static int32_t x360BinaryTell(MAYBE_UNUSED FileSystem* fs, void* handle) {
    if (handle == nullptr) return 0;
    return (int32_t) ftell(((X360BinaryHandle*) handle)->fp);
}

static bool x360BinarySeek(MAYBE_UNUSED FileSystem* fs, void* handle, int32_t pos) {
    if (handle == nullptr) return false;
    return fseek(((X360BinaryHandle*) handle)->fp, pos, SEEK_SET) == 0;
}

static int32_t x360BinarySize(MAYBE_UNUSED FileSystem* fs, void* handle) {
    if (handle == nullptr) return 0;
    FILE* f = ((X360BinaryHandle*) handle)->fp;
    long saved = ftell(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, saved, SEEK_SET);
    return (int32_t) size;
}

static void x360BinaryRewrite(MAYBE_UNUSED FileSystem* fs, void* handle) {
    if (handle == nullptr) return;
    X360BinaryHandle* h = (X360BinaryHandle*) handle;
    if (h->fp != nullptr) fclose(h->fp);
    h->fp = fopen(h->fullPath, "wb+");
}

// ===[ Directories ]===

static bool x360DirectoryExists(FileSystem* fs, const char* relativePath) {
    char* fullPath = x360ResolveForRead((X360FileSystem*) fs, relativePath);
    if (fullPath == nullptr) return false;
    struct stat st;
    bool exists = (stat(fullPath, &st) == 0 && S_ISDIR(st.st_mode));
    free(fullPath);
    return exists;
}

static bool x360CreateDirectory(FileSystem* fs, const char* relativePath) {
    char* fullPath = x360ResolveForWrite((X360FileSystem*) fs, relativePath);
    if (fullPath == nullptr) return false;
    int result = _mkdir(fullPath);
    free(fullPath);
    return result == 0;
}

static bool x360DeleteDirectory(FileSystem* fs, const char* relativePath) {
    char* fullPath = x360ResolveForWrite((X360FileSystem*) fs, relativePath);
    if (fullPath == nullptr) return false;
    int result = _rmdir(fullPath);
    free(fullPath);
    return result == 0;
}

// ===[ Directory enumeration ]===

static void x360DirListPush(FileSystemDirEntry** list, const char* name, bool isDirectory) {
    repeat(arrlen(*list), i) {
        if (strcmp((*list)[i].name, name) == 0) return;
    }
    FileSystemDirEntry entry = {0};
    entry.name = safeStrdup(name);
    entry.isDirectory = isDirectory;
    arrput(*list, entry);
}

static void x360ListSingleDir(FileSystemDirEntry** list, const char* fullDir) {
    size_t dirLen = strlen(fullDir);
    // FindFirstFileA wants "<dir>\*"; add the separator only when the caller's
    // path does not already end with one (a drive root such as "game:" does not).
    bool needsSep = dirLen == 0 || fullDir[dirLen - 1] != X360_FS_SEP;
    char* search = (char*) safeMalloc(dirLen + (needsSep ? 2 : 1) + 1);
    memcpy(search, fullDir, dirLen);
    size_t at = dirLen;
    if (needsSep) search[at++] = X360_FS_SEP;
    search[at++] = '*';
    search[at] = '\0';

    WIN32_FIND_DATAA findData;
    HANDLE h = FindFirstFileA(search, &findData);
    free(search);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        const char* name = findData.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        bool isDir = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        x360DirListPush(list, name, isDir);
    } while (FindNextFileA(h, &findData));
    FindClose(h);
}

static FileSystemDirEntry* x360ListDirectory(FileSystem* fs, const char* relativeDirPath) {
    X360FileSystem* xfs = (X360FileSystem*) fs;
    char* normalized = x360Normalize(relativeDirPath);
    FileSystemDirEntry* list = nullptr;

    if (normalized != nullptr && x360IsAbsolute(normalized)) {
        x360ListSingleDir(&list, normalized);
    } else {
        // "." and "" mean the root of each area; joining them would produce
        // "game:\.\*", which FindFirstFileA does not accept.
        const char* rel = normalized;
        if (rel == nullptr || rel[0] == '\0' || strcmp(rel, ".") == 0) {
            rel = "";
        }

        char* saveFull = rel[0] == '\0' ? safeStrdup(xfs->savePath) : x360JoinPath(xfs->savePath, rel);
        x360ListSingleDir(&list, saveFull);
        free(saveFull);

        char* bundleFull = rel[0] == '\0' ? safeStrdup(xfs->bundlePath) : x360JoinPath(xfs->bundlePath, rel);
        x360ListSingleDir(&list, bundleFull);
        free(bundleFull);
    }

    free(normalized);
    return list;
}

// ===[ Lifecycle ]===

static FileSystemVtable x360FileSystemVtable;

FileSystem* X360FileSystem_create(const char* bundlePath, const char* savePath) {
    X360FileSystem* fs = (X360FileSystem*) safeCalloc(1, sizeof(X360FileSystem));
    fs->base.vtable = &x360FileSystemVtable;

    x360FileSystemVtable.resolvePath = x360ResolvePath;
    x360FileSystemVtable.fileExists = x360FileExists;
    x360FileSystemVtable.readFileText = x360ReadFileText;
    x360FileSystemVtable.writeFileText = x360WriteFileText;
    x360FileSystemVtable.deleteFile = x360DeleteFile;
    x360FileSystemVtable.renameFile = x360RenameFile;
    x360FileSystemVtable.readFileBinary = x360ReadFileBinary;
    x360FileSystemVtable.writeFileBinary = x360WriteFileBinary;
    x360FileSystemVtable.binaryOpen = x360BinaryOpen;
    x360FileSystemVtable.binaryClose = x360BinaryClose;
    x360FileSystemVtable.binaryRead = x360BinaryRead;
    x360FileSystemVtable.binaryWrite = x360BinaryWrite;
    x360FileSystemVtable.binaryTell = x360BinaryTell;
    x360FileSystemVtable.binarySeek = x360BinarySeek;
    x360FileSystemVtable.binarySize = x360BinarySize;
    x360FileSystemVtable.binaryRewrite = x360BinaryRewrite;
    x360FileSystemVtable.directoryExists = x360DirectoryExists;
    x360FileSystemVtable.createDirectory = x360CreateDirectory;
    x360FileSystemVtable.deleteDirectory = x360DeleteDirectory;
    x360FileSystemVtable.listDirectory = x360ListDirectory;

    fs->bundlePath = x360WithTrailingSlash(bundlePath);
    fs->savePath = x360WithTrailingSlash(savePath);
    return (FileSystem*) fs;
}

void X360FileSystem_destroy(FileSystem* fs) {
    if (fs == nullptr) return;
    X360FileSystem* xfs = (X360FileSystem*) fs;
    free(xfs->bundlePath);
    free(xfs->savePath);
    free(xfs);
}
