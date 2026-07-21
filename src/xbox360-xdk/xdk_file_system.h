#pragma once

#include "file_system.h"

typedef struct {
    FileSystem base;
    char basePath[512];
} XdkFileSystem;

XdkFileSystem* XdkFileSystem_create(const char* dataWinPath);
void XdkFileSystem_destroy(XdkFileSystem* fileSystem);
