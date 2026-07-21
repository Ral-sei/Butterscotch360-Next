/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Derived from ceilingtilefan/Butterscotch-360 commit
 * 7f8f1ea6044dbc55560dfbd2ca9a2f45e472c02e, with later work from
 * flaf1x/Butterscotch360-Refresh and Ral-sei.
 */

#pragma once

#include "runner_gamepad.h"
#include "runner_keyboard.h"

typedef struct {
    bool keyboardDown[GML_KEY_COUNT];
} XdkInputState;

void XdkInput_initialize(XdkInputState* input);
void XdkInput_beginFrame(
    XdkInputState* input,
    RunnerGamepadState* gamepads,
    RunnerKeyboardState* keyboard
);
void XdkGamepad_poll(RunnerGamepadState* gamepads);
