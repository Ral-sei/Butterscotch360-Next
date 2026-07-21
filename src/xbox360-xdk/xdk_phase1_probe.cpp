#include "xdk_phase1_probe.h"

#include <stdlib.h>
#include <string.h>

#include "stb_ds.h"

static const char* probeDirectory = "__butterscotch_phase1_probe";
static const char* probeTextPath = "__butterscotch_phase1_probe\\text.txt";
static const char* probeBinaryPath = "__butterscotch_phase1_probe\\whole.bin";
static const char* probeStreamPath = "__butterscotch_phase1_probe\\stream.bin";

static bool hasCompleteVtable(FileSystemVtable* vtable) {
    return vtable != nullptr &&
           vtable->resolvePath != nullptr &&
           vtable->fileExists != nullptr &&
           vtable->readFileText != nullptr &&
           vtable->writeFileText != nullptr &&
           vtable->deleteFile != nullptr &&
           vtable->readFileBinary != nullptr &&
           vtable->writeFileBinary != nullptr &&
           vtable->binaryOpen != nullptr &&
           vtable->binaryClose != nullptr &&
           vtable->binaryRead != nullptr &&
           vtable->binaryWrite != nullptr &&
           vtable->binaryTell != nullptr &&
           vtable->binarySeek != nullptr &&
           vtable->binarySize != nullptr &&
           vtable->binaryRewrite != nullptr &&
           vtable->directoryExists != nullptr &&
           vtable->createDirectory != nullptr &&
           vtable->deleteDirectory != nullptr &&
           vtable->listDirectory != nullptr;
}

static void cleanupProbe(FileSystem* fileSystem) {
    FileSystemVtable* vtable = fileSystem->vtable;
    vtable->deleteFile(fileSystem, probeTextPath);
    vtable->deleteFile(fileSystem, probeBinaryPath);
    vtable->deleteFile(fileSystem, probeStreamPath);
    vtable->deleteDirectory(fileSystem, probeDirectory);
}

static bool directoryContainsProbeFiles(FileSystem* fileSystem) {
    FileSystemDirEntry* entries = fileSystem->vtable->listDirectory(
        fileSystem,
        probeDirectory
    );
    bool foundText = false;
    bool foundBinary = false;
    bool foundStream = false;
    for (int index = 0; index < arrlen(entries); index++) {
        if (strcmp(entries[index].name, "text.txt") == 0) foundText = true;
        if (strcmp(entries[index].name, "whole.bin") == 0) foundBinary = true;
        if (strcmp(entries[index].name, "stream.bin") == 0) foundStream = true;
        free(entries[index].name);
    }
    arrfree(entries);
    return foundText && foundBinary && foundStream;
}

bool XdkPhase1_runFileSystemProbe(FileSystem* fileSystem) {
    if (fileSystem == nullptr || !hasCompleteVtable(fileSystem->vtable)) return false;
    FileSystemVtable* vtable = fileSystem->vtable;
    cleanupProbe(fileSystem);

    bool ok = vtable->createDirectory(fileSystem, probeDirectory) &&
              vtable->directoryExists(fileSystem, probeDirectory);

    const char* expectedText = "Butterscotch Xbox 360 Phase 1";
    ok = vtable->writeFileText(fileSystem, probeTextPath, expectedText) && ok;
    char* actualText = vtable->readFileText(fileSystem, probeTextPath);
    ok = actualText != nullptr && strcmp(actualText, expectedText) == 0 && ok;
    free(actualText);

    const uint8_t expectedBinary[] = {0x00, 0x7F, 0x80, 0xFF};
    ok = vtable->writeFileBinary(
        fileSystem,
        probeBinaryPath,
        expectedBinary,
        (int32_t) sizeof(expectedBinary)
    ) && ok;
    uint8_t* actualBinary = nullptr;
    int32_t actualBinarySize = 0;
    bool binaryRead = vtable->readFileBinary(
        fileSystem,
        probeBinaryPath,
        &actualBinary,
        &actualBinarySize
    );
    ok = binaryRead &&
         actualBinarySize == (int32_t) sizeof(expectedBinary) &&
         memcmp(actualBinary, expectedBinary, sizeof(expectedBinary)) == 0 &&
         ok;
    free(actualBinary);

    const uint8_t initialStream[] = {1, 2, 3, 4};
    void* stream = vtable->binaryOpen(fileSystem, probeStreamPath, GML_FILE_BIN_WRITE);
    if (stream == nullptr) {
        ok = false;
    } else {
        ok = vtable->binaryWrite(
            fileSystem,
            stream,
            initialStream,
            (int32_t) sizeof(initialStream)
        ) == (int32_t) sizeof(initialStream) && ok;
        ok = vtable->binaryTell(fileSystem, stream) == 4 && ok;
        ok = vtable->binarySeek(fileSystem, stream, 1) && ok;
        const uint8_t replacement = 9;
        ok = vtable->binaryWrite(fileSystem, stream, &replacement, 1) == 1 && ok;
        ok = vtable->binarySize(fileSystem, stream) == 4 && ok;
        vtable->binaryClose(fileSystem, stream);
    }

    stream = vtable->binaryOpen(fileSystem, probeStreamPath, GML_FILE_BIN_READ);
    if (stream == nullptr) {
        ok = false;
    } else {
        uint8_t streamData[4] = {0};
        const uint8_t expectedStream[] = {1, 9, 3, 4};
        ok = vtable->binaryRead(fileSystem, stream, streamData, 4) == 4 &&
             memcmp(streamData, expectedStream, sizeof(expectedStream)) == 0 &&
             ok;
        vtable->binaryClose(fileSystem, stream);
    }

    stream = vtable->binaryOpen(fileSystem, probeStreamPath, GML_FILE_BIN_READWRITE);
    if (stream == nullptr) {
        ok = false;
    } else {
        vtable->binaryRewrite(fileSystem, stream);
        ok = vtable->binaryTell(fileSystem, stream) == 0 &&
             vtable->binarySize(fileSystem, stream) == 0 &&
             ok;
        const uint8_t rewritten = 0x5A;
        ok = vtable->binaryWrite(fileSystem, stream, &rewritten, 1) == 1 && ok;
        vtable->binaryClose(fileSystem, stream);
    }

    ok = directoryContainsProbeFiles(fileSystem) && ok;
    cleanupProbe(fileSystem);
    ok = !vtable->directoryExists(fileSystem, probeDirectory) && ok;
    return ok;
}
