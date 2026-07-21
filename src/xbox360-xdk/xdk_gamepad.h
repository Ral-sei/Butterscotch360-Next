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
