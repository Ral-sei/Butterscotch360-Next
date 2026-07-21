/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Derived from ceilingtilefan/Butterscotch-360 commit
 * 7f8f1ea6044dbc55560dfbd2ca9a2f45e472c02e, with later work from
 * flaf1x/Butterscotch360-Refresh and Ral-sei.
 */

#include "xdk_file_system.h"

#include <xtl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "stb_ds.h"
#include "utils.h"

#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD) -1)
#endif

typedef struct {
    HANDLE file;
    bool writable;
} XdkBinaryHandle;

static void normalizeSeparators(char* path) {
    for (char* cursor = path; *cursor != '\0'; cursor++) {
        if (*cursor == '/') *cursor = '\\';
    }
}

static bool hasDevicePrefix(const char* path) {
    const char* colon = strchr(path, ':');
    const char* slash = strpbrk(path, "/\\");
    return colon != nullptr && (slash == nullptr || colon < slash);
}

static char* xdkResolvePath(FileSystem* fs, const char* relativePath) {
    if (relativePath == nullptr) return nullptr;

    XdkFileSystem* xfs = (XdkFileSystem*) fs;
    if (hasDevicePrefix(relativePath)) {
        char* absolutePath = safeStrdup(relativePath);
        normalizeSeparators(absolutePath);
        return absolutePath;
    }

    size_t baseLength = strlen(xfs->basePath);
    size_t relativeOffset = 0;
    while (relativePath[relativeOffset] == '/' || relativePath[relativeOffset] == '\\') {
        relativeOffset++;
    }
    size_t relativeLength = strlen(relativePath + relativeOffset);
    bool needsSeparator = baseLength > 0 && xfs->basePath[baseLength - 1] != '\\';
    size_t totalLength = baseLength + (needsSeparator ? 1 : 0) + relativeLength;

    char* result = (char*) safeMalloc(totalLength + 1);
    memcpy(result, xfs->basePath, baseLength);
    size_t position = baseLength;
    if (needsSeparator) result[position++] = '\\';
    memcpy(result + position, relativePath + relativeOffset, relativeLength + 1);
    normalizeSeparators(result);
    return result;
}

static bool pathIsDirectory(const char* fullPath) {
    DWORD attributes = GetFileAttributesA(fullPath);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool ensureDirectories(const char* fullPath, bool includeLastComponent) {
    char* path = safeStrdup(fullPath);
    normalizeSeparators(path);

    char* firstSeparator = strchr(path, ':');
    firstSeparator = firstSeparator != nullptr ? firstSeparator + 1 : path;

    for (char* cursor = firstSeparator; *cursor != '\0'; cursor++) {
        if (*cursor != '\\' || cursor == firstSeparator) continue;
        if (!includeLastComponent && cursor[1] == '\0') continue;

        char saved = *cursor;
        *cursor = '\0';
        if (*path != '\0' && !pathIsDirectory(path)) {
            CreateDirectoryA(path, nullptr);
        }
        *cursor = saved;
    }

    bool result = true;
    if (includeLastComponent && !pathIsDirectory(path)) {
        result = CreateDirectoryA(path, nullptr) != FALSE || pathIsDirectory(path);
    }
    free(path);
    return result;
}

static bool ensureParentDirectories(const char* fullPath) {
    char* parent = safeStrdup(fullPath);
    normalizeSeparators(parent);
    char* lastSeparator = strrchr(parent, '\\');
    char* deviceSeparator = strchr(parent, ':');
    if (lastSeparator == nullptr ||
        (deviceSeparator != nullptr && lastSeparator == deviceSeparator + 1)) {
        free(parent);
        return true;
    }
    *lastSeparator = '\0';
    bool result = ensureDirectories(parent, true);
    free(parent);
    return result;
}

static HANDLE openResolvedFile(
    FileSystem* fs,
    const char* relativePath,
    DWORD access,
    DWORD shareMode,
    DWORD creationDisposition,
    bool createParents
) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (fullPath == nullptr) return INVALID_HANDLE_VALUE;
    if (createParents && !ensureParentDirectories(fullPath)) {
        free(fullPath);
        return INVALID_HANDLE_VALUE;
    }

    HANDLE file = CreateFileA(
        fullPath,
        access,
        shareMode,
        nullptr,
        creationDisposition,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    free(fullPath);
    return file;
}

static bool getFileSize32(HANDLE file, int32_t* outSize) {
    DWORD high = 0;
    DWORD low = GetFileSize(file, &high);
    if (low == INVALID_FILE_SIZE || high != 0 || low > INT32_MAX) return false;
    *outSize = (int32_t) low;
    return true;
}

static bool readAll(HANDLE file, uint8_t* buffer, int32_t size) {
    int32_t totalRead = 0;
    while (totalRead < size) {
        DWORD bytesRead = 0;
        DWORD remaining = (DWORD) (size - totalRead);
        if (!ReadFile(file, buffer + totalRead, remaining, &bytesRead, nullptr)) return false;
        if (bytesRead == 0) return false;
        totalRead += (int32_t) bytesRead;
    }
    return true;
}

static bool writeAll(HANDLE file, const uint8_t* buffer, int32_t size) {
    int32_t totalWritten = 0;
    while (totalWritten < size) {
        DWORD bytesWritten = 0;
        DWORD remaining = (DWORD) (size - totalWritten);
        if (!WriteFile(file, buffer + totalWritten, remaining, &bytesWritten, nullptr)) return false;
        if (bytesWritten == 0) return false;
        totalWritten += (int32_t) bytesWritten;
    }
    return true;
}

static bool xdkFileExists(FileSystem* fs, const char* relativePath) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (fullPath == nullptr) return false;
    DWORD attributes = GetFileAttributesA(fullPath);
    free(fullPath);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static char* xdkReadFileText(FileSystem* fs, const char* relativePath) {
    HANDLE file = openResolvedFile(
        fs, relativePath, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, false
    );
    if (file == INVALID_HANDLE_VALUE) return nullptr;

    int32_t size = 0;
    if (!getFileSize32(file, &size)) {
        CloseHandle(file);
        return nullptr;
    }

    char* text = (char*) safeMalloc((size_t) size + 1);
    bool ok = readAll(file, (uint8_t*) text, size);
    CloseHandle(file);
    if (!ok) {
        free(text);
        return nullptr;
    }
    text[size] = '\0';
    return text;
}

static bool xdkWriteFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    if (contents == nullptr) return false;
    size_t length = strlen(contents);
    if (length > INT32_MAX) return false;

    HANDLE file = openResolvedFile(
        fs, relativePath, GENERIC_WRITE, 0, CREATE_ALWAYS, true
    );
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = writeAll(file, (const uint8_t*) contents, (int32_t) length);
    CloseHandle(file);
    return ok;
}

static bool xdkDeleteFile(FileSystem* fs, const char* relativePath) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (fullPath == nullptr) return false;
    bool result = DeleteFileA(fullPath) != FALSE;
    free(fullPath);
    return result;
}

static bool xdkReadFileBinary(
    FileSystem* fs,
    const char* relativePath,
    uint8_t** outData,
    int32_t* outSize
) {
    if (outData == nullptr || outSize == nullptr) return false;
    *outData = nullptr;
    *outSize = 0;

    HANDLE file = openResolvedFile(
        fs, relativePath, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, false
    );
    if (file == INVALID_HANDLE_VALUE) return false;

    int32_t size = 0;
    if (!getFileSize32(file, &size)) {
        CloseHandle(file);
        return false;
    }

    uint8_t* data = (uint8_t*) safeMalloc(size > 0 ? (size_t) size : 1);
    bool ok = readAll(file, data, size);
    CloseHandle(file);
    if (!ok) {
        free(data);
        return false;
    }

    *outData = data;
    *outSize = size;
    return true;
}

static bool xdkWriteFileBinary(
    FileSystem* fs,
    const char* relativePath,
    const uint8_t* data,
    int32_t size
) {
    if (size < 0 || (size > 0 && data == nullptr)) return false;
    HANDLE file = openResolvedFile(
        fs, relativePath, GENERIC_WRITE, 0, CREATE_ALWAYS, true
    );
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = writeAll(file, data, size);
    CloseHandle(file);
    return ok;
}

static void* xdkBinaryOpen(FileSystem* fs, const char* relativePath, int32_t mode) {
    DWORD access;
    DWORD creationDisposition;
    bool writable;

    switch (mode) {
        case GML_FILE_BIN_READ:
            access = GENERIC_READ;
            creationDisposition = OPEN_EXISTING;
            writable = false;
            break;
        case GML_FILE_BIN_WRITE:
            access = GENERIC_READ | GENERIC_WRITE;
            creationDisposition = CREATE_ALWAYS;
            writable = true;
            break;
        case GML_FILE_BIN_READWRITE:
            access = GENERIC_READ | GENERIC_WRITE;
            creationDisposition = OPEN_ALWAYS;
            writable = true;
            break;
        default:
            return nullptr;
    }

    HANDLE file = openResolvedFile(
        fs,
        relativePath,
        access,
        writable ? 0 : FILE_SHARE_READ,
        creationDisposition,
        writable
    );
    if (file == INVALID_HANDLE_VALUE) return nullptr;

    XdkBinaryHandle* handle = (XdkBinaryHandle*) safeCalloc(1, sizeof(XdkBinaryHandle));
    handle->file = file;
    handle->writable = writable;
    return handle;
}

static void xdkBinaryClose(MAYBE_UNUSED FileSystem* fs, void* opaqueHandle) {
    if (opaqueHandle == nullptr) return;
    XdkBinaryHandle* handle = (XdkBinaryHandle*) opaqueHandle;
    CloseHandle(handle->file);
    free(handle);
}

static int32_t xdkBinaryRead(
    MAYBE_UNUSED FileSystem* fs,
    void* opaqueHandle,
    void* destination,
    int32_t count
) {
    if (opaqueHandle == nullptr || destination == nullptr || count <= 0) return 0;
    XdkBinaryHandle* handle = (XdkBinaryHandle*) opaqueHandle;
    DWORD bytesRead = 0;
    if (!ReadFile(handle->file, destination, (DWORD) count, &bytesRead, nullptr)) return 0;
    return (int32_t) bytesRead;
}

static int32_t xdkBinaryWrite(
    MAYBE_UNUSED FileSystem* fs,
    void* opaqueHandle,
    const void* source,
    int32_t count
) {
    if (opaqueHandle == nullptr || source == nullptr || count <= 0) return 0;
    XdkBinaryHandle* handle = (XdkBinaryHandle*) opaqueHandle;
    if (!handle->writable) return 0;
    DWORD bytesWritten = 0;
    if (!WriteFile(handle->file, source, (DWORD) count, &bytesWritten, nullptr)) return 0;
    return (int32_t) bytesWritten;
}

static int32_t xdkBinaryTell(MAYBE_UNUSED FileSystem* fs, void* opaqueHandle) {
    if (opaqueHandle == nullptr) return -1;
    XdkBinaryHandle* handle = (XdkBinaryHandle*) opaqueHandle;
    DWORD position = SetFilePointer(handle->file, 0, nullptr, FILE_CURRENT);
    if (position == INVALID_SET_FILE_POINTER || position > INT32_MAX) return -1;
    return (int32_t) position;
}

static bool xdkBinarySeek(
    MAYBE_UNUSED FileSystem* fs,
    void* opaqueHandle,
    int32_t position
) {
    if (opaqueHandle == nullptr || position < 0) return false;
    XdkBinaryHandle* handle = (XdkBinaryHandle*) opaqueHandle;
    DWORD result = SetFilePointer(handle->file, position, nullptr, FILE_BEGIN);
    return result != INVALID_SET_FILE_POINTER;
}

static int32_t xdkBinarySize(MAYBE_UNUSED FileSystem* fs, void* opaqueHandle) {
    if (opaqueHandle == nullptr) return -1;
    XdkBinaryHandle* handle = (XdkBinaryHandle*) opaqueHandle;
    int32_t size = 0;
    return getFileSize32(handle->file, &size) ? size : -1;
}

static void xdkBinaryRewrite(MAYBE_UNUSED FileSystem* fs, void* opaqueHandle) {
    if (opaqueHandle == nullptr) return;
    XdkBinaryHandle* handle = (XdkBinaryHandle*) opaqueHandle;
    if (!handle->writable) return;
    if (SetFilePointer(handle->file, 0, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER) return;
    SetEndOfFile(handle->file);
}

static bool xdkDirectoryExists(FileSystem* fs, const char* relativePath) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (fullPath == nullptr) return false;
    bool result = pathIsDirectory(fullPath);
    free(fullPath);
    return result;
}

static bool xdkCreateDirectory(FileSystem* fs, const char* relativePath) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (fullPath == nullptr) return false;
    bool result = ensureDirectories(fullPath, true);
    free(fullPath);
    return result;
}

static bool xdkDeleteDirectory(FileSystem* fs, const char* relativePath) {
    char* fullPath = xdkResolvePath(fs, relativePath);
    if (fullPath == nullptr) return false;
    bool result = RemoveDirectoryA(fullPath) != FALSE;
    free(fullPath);
    return result;
}

static FileSystemDirEntry* xdkListDirectory(FileSystem* fs, const char* relativeDirPath) {
    char* fullPath = xdkResolvePath(fs, relativeDirPath);
    if (fullPath == nullptr) return nullptr;

    size_t pathLength = strlen(fullPath);
    bool hasTrailingSeparator = pathLength > 0 && fullPath[pathLength - 1] == '\\';
    char* pattern = (char*) safeMalloc(pathLength + (hasTrailingSeparator ? 2 : 3));
    memcpy(pattern, fullPath, pathLength);
    size_t position = pathLength;
    if (!hasTrailingSeparator) pattern[position++] = '\\';
    pattern[position++] = '*';
    pattern[position] = '\0';
    free(fullPath);

    WIN32_FIND_DATAA findData;
    HANDLE search = FindFirstFileA(pattern, &findData);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) return nullptr;

    FileSystemDirEntry* entries = nullptr;
    do {
        const char* name = findData.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        FileSystemDirEntry entry = {0};
        entry.name = safeStrdup(name);
        entry.isDirectory = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        arrput(entries, entry);
    } while (FindNextFileA(search, &findData));
    FindClose(search);
    return entries;
}

static FileSystemVtable xdkFileSystemVtable;

static void initializeVtable(void) {
    static bool initialized = false;
    if (initialized) return;

    xdkFileSystemVtable.resolvePath = xdkResolvePath;
    xdkFileSystemVtable.fileExists = xdkFileExists;
    xdkFileSystemVtable.readFileText = xdkReadFileText;
    xdkFileSystemVtable.writeFileText = xdkWriteFileText;
    xdkFileSystemVtable.deleteFile = xdkDeleteFile;
    xdkFileSystemVtable.readFileBinary = xdkReadFileBinary;
    xdkFileSystemVtable.writeFileBinary = xdkWriteFileBinary;
    xdkFileSystemVtable.binaryOpen = xdkBinaryOpen;
    xdkFileSystemVtable.binaryClose = xdkBinaryClose;
    xdkFileSystemVtable.binaryRead = xdkBinaryRead;
    xdkFileSystemVtable.binaryWrite = xdkBinaryWrite;
    xdkFileSystemVtable.binaryTell = xdkBinaryTell;
    xdkFileSystemVtable.binarySeek = xdkBinarySeek;
    xdkFileSystemVtable.binarySize = xdkBinarySize;
    xdkFileSystemVtable.binaryRewrite = xdkBinaryRewrite;
    xdkFileSystemVtable.directoryExists = xdkDirectoryExists;
    xdkFileSystemVtable.createDirectory = xdkCreateDirectory;
    xdkFileSystemVtable.deleteDirectory = xdkDeleteDirectory;
    xdkFileSystemVtable.listDirectory = xdkListDirectory;
    initialized = true;
}

XdkFileSystem* XdkFileSystem_create(const char* dataWinPath) {
    initializeVtable();
    XdkFileSystem* xfs = (XdkFileSystem*) safeCalloc(1, sizeof(XdkFileSystem));
    xfs->base.vtable = &xdkFileSystemVtable;

    const char* path = dataWinPath != nullptr ? dataWinPath : "game:\\data.win";
    const char* lastBackslash = strrchr(path, '\\');
    const char* lastSlash = strrchr(path, '/');
    const char* lastSeparator = lastBackslash;
    if (lastSlash != nullptr && (lastSeparator == nullptr || lastSlash > lastSeparator)) {
        lastSeparator = lastSlash;
    }

    if (lastSeparator != nullptr) {
        size_t directoryLength = (size_t) (lastSeparator - path + 1);
        if (directoryLength >= sizeof(xfs->basePath)) {
            free(xfs);
            return nullptr;
        }
        memcpy(xfs->basePath, path, directoryLength);
        xfs->basePath[directoryLength] = '\0';
    } else {
        strcpy(xfs->basePath, "game:\\");
    }
    normalizeSeparators(xfs->basePath);
    return xfs;
}

void XdkFileSystem_destroy(XdkFileSystem* fileSystem) {
    free(fileSystem);
}
