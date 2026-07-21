#include "xdk_gamepad.h"

#include <xtl.h>
#include <string.h>

enum {
    XDK_GAMEPAD_SLOT = 0,
    XDK_LEFT_TRIGGER_INDEX = 6,
    XDK_RIGHT_TRIGGER_INDEX = 7,
    XDK_STICK_KEYBOARD_DEADZONE = 16384
};

typedef struct {
    WORD mask;
    int32_t key;
} XdkKeyboardMapping;

static const XdkKeyboardMapping keyboardMappings[] = {
    {XINPUT_GAMEPAD_DPAD_UP, VK_UP},
    {XINPUT_GAMEPAD_DPAD_DOWN, VK_DOWN},
    {XINPUT_GAMEPAD_DPAD_LEFT, VK_LEFT},
    {XINPUT_GAMEPAD_DPAD_RIGHT, VK_RIGHT},
    {XINPUT_GAMEPAD_A, VK_ENTER},
    {XINPUT_GAMEPAD_B, VK_SHIFT},
    {XINPUT_GAMEPAD_X, VK_CONTROL},
    {XINPUT_GAMEPAD_Y, 'X'},
    {XINPUT_GAMEPAD_START, VK_ESCAPE},
    {XINPUT_GAMEPAD_BACK, VK_ESCAPE}
};

static float normalizeThumb(SHORT value) {
    return value < 0 ? (float) value / 32768.0f : (float) value / 32767.0f;
}

static bool pollController(RunnerGamepadState* gamepads, XINPUT_STATE* state) {
    GamepadSlot* slot = &gamepads->slots[XDK_GAMEPAD_SLOT];
    memcpy(slot->buttonDownPrev, slot->buttonDown, sizeof(slot->buttonDown));
    memset(slot->buttonDown, 0, sizeof(slot->buttonDown));
    memset(slot->buttonValue, 0, sizeof(slot->buttonValue));
    memset(slot->axisValue, 0, sizeof(slot->axisValue));
    memset(state, 0, sizeof(*state));

    bool connected = XInputGetState(XDK_GAMEPAD_SLOT, state) == ERROR_SUCCESS;
    slot->connected = connected;
    if (!connected) {
        slot->guid[0] = '\0';
        slot->description[0] = '\0';
    } else {
        slot->jid = XDK_GAMEPAD_SLOT;
        strcpy(slot->description, "Xbox 360 Controller");
        strcpy(slot->guid, "xinput-0");

        WORD buttons = state->Gamepad.wButtons;
        if (buttons & XINPUT_GAMEPAD_A) slot->buttonDown[0] = true;
        if (buttons & XINPUT_GAMEPAD_B) slot->buttonDown[1] = true;
        if (buttons & XINPUT_GAMEPAD_X) slot->buttonDown[2] = true;
        if (buttons & XINPUT_GAMEPAD_Y) slot->buttonDown[3] = true;
        if (buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) slot->buttonDown[4] = true;
        if (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) slot->buttonDown[5] = true;
        if (buttons & XINPUT_GAMEPAD_BACK) slot->buttonDown[8] = true;
        if (buttons & XINPUT_GAMEPAD_START) slot->buttonDown[9] = true;
        if (buttons & XINPUT_GAMEPAD_LEFT_THUMB) slot->buttonDown[10] = true;
        if (buttons & XINPUT_GAMEPAD_RIGHT_THUMB) slot->buttonDown[11] = true;
        if (buttons & XINPUT_GAMEPAD_DPAD_UP) slot->buttonDown[12] = true;
        if (buttons & XINPUT_GAMEPAD_DPAD_DOWN) slot->buttonDown[13] = true;
        if (buttons & XINPUT_GAMEPAD_DPAD_LEFT) slot->buttonDown[14] = true;
        if (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) slot->buttonDown[15] = true;

        float leftTrigger = (float) state->Gamepad.bLeftTrigger / 255.0f;
        float rightTrigger = (float) state->Gamepad.bRightTrigger / 255.0f;
        slot->buttonValue[XDK_LEFT_TRIGGER_INDEX] = leftTrigger;
        slot->buttonValue[XDK_RIGHT_TRIGGER_INDEX] = rightTrigger;
        slot->buttonDown[XDK_LEFT_TRIGGER_INDEX] = leftTrigger >= slot->triggerThreshold;
        slot->buttonDown[XDK_RIGHT_TRIGGER_INDEX] = rightTrigger >= slot->triggerThreshold;

        slot->axisValue[0] = normalizeThumb(state->Gamepad.sThumbLX);
        slot->axisValue[1] = -normalizeThumb(state->Gamepad.sThumbLY);
        slot->axisValue[2] = normalizeThumb(state->Gamepad.sThumbRX);
        slot->axisValue[3] = -normalizeThumb(state->Gamepad.sThumbRY);

        for (int button = 0; button < GP_BUTTON_COUNT; button++) {
            if (button == XDK_LEFT_TRIGGER_INDEX || button == XDK_RIGHT_TRIGGER_INDEX) continue;
            slot->buttonValue[button] = slot->buttonDown[button] ? 1.0f : 0.0f;
        }
        gamepads->connectedCount++;
    }

    for (int button = 0; button < GP_BUTTON_COUNT; button++) {
        bool wasDown = slot->buttonDownPrev[button];
        if (slot->buttonDown[button] && !wasDown) slot->buttonPressed[button] = true;
        if (!slot->buttonDown[button] && wasDown) slot->buttonReleased[button] = true;
    }
    return connected;
}

static void updateKeyboard(
    XdkInputState* input,
    RunnerKeyboardState* keyboard,
    const XINPUT_STATE* state,
    bool connected
) {
    bool currentKeys[GML_KEY_COUNT] = {false};
    WORD buttons = connected ? state->Gamepad.wButtons : 0;

    if (connected) {
        if (state->Gamepad.sThumbLX < -XDK_STICK_KEYBOARD_DEADZONE) {
            buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
        }
        if (state->Gamepad.sThumbLX > XDK_STICK_KEYBOARD_DEADZONE) {
            buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        }
        if (state->Gamepad.sThumbLY > XDK_STICK_KEYBOARD_DEADZONE) {
            buttons |= XINPUT_GAMEPAD_DPAD_UP;
        }
        if (state->Gamepad.sThumbLY < -XDK_STICK_KEYBOARD_DEADZONE) {
            buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
        }
    }

    int mappingCount = (int) (sizeof(keyboardMappings) / sizeof(keyboardMappings[0]));
    for (int index = 0; index < mappingCount; index++) {
        if ((buttons & keyboardMappings[index].mask) != 0) {
            currentKeys[keyboardMappings[index].key] = true;
        }
    }

    for (int key = 0; key < GML_KEY_COUNT; key++) {
        if (currentKeys[key] && !input->keyboardDown[key]) {
            RunnerKeyboard_onKeyDown(keyboard, key);
        } else if (!currentKeys[key] && input->keyboardDown[key]) {
            RunnerKeyboard_onKeyUp(keyboard, key);
        }
        input->keyboardDown[key] = currentKeys[key];
    }
}

void XdkInput_initialize(XdkInputState* input) {
    memset(input, 0, sizeof(*input));
}

void XdkInput_beginFrame(
    XdkInputState* input,
    RunnerGamepadState* gamepads,
    RunnerKeyboardState* keyboard
) {
    RunnerKeyboard_beginFrame(keyboard);
    RunnerGamepad_beginFrame(gamepads);
    XINPUT_STATE state;
    bool connected = pollController(gamepads, &state);
    updateKeyboard(input, keyboard, &state, connected);
}

void XdkGamepad_poll(RunnerGamepadState* gamepads) {
    XINPUT_STATE state;
    pollController(gamepads, &state);
}
