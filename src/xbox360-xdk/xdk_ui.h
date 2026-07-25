/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once

#include "data_win.h"
#include "renderer.h"
#include "runner.h"

typedef struct XdkUi XdkUi;

XdkUi* XdkUi_create(void* d3dDevice);
void XdkUi_destroy(XdkUi* ui);

void XdkUi_drawLoading(XdkUi* ui, float progress, const char* stage);
void XdkUi_dataWinProgress(
    const char* chunkName,
    int chunkIndex,
    int totalChunks,
    DataWin* dataWin,
    void* userData
);

void XdkUi_updateDiagnostics(XdkUi* ui, double deltaSeconds, int32_t steps);
void XdkUi_drawDiagnostics(XdkUi* ui, Runner* runner, Renderer* renderer);

