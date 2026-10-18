// Butterscotch Xbox 360 port - XInput backend.
//
// See xbox360_input.h. Behaviour that matters:
//
//   * Wireless controllers are not active at boot and XInputGetState returns
//     ERROR_DEVICE_NOT_CONNECTED until the user presses START or the Guide
//     button, so "no state" is a normal condition, not an error.
//   * On a disconnect edge every key the pad was holding is released, so a game
//     cannot be left with a stuck direction.
//   * On a reconnect edge the previous button state is seeded from the current
//     one, so a button held through the reconnect does not fire a phantom press.
//
// Port-owned. No public source is touched.

#include "xbox360_input.h"

#include "runner.h"
#include "runner_keyboard.h"
#include "runner_gamepad.h"
#include "log.h"

#include "stdio_compat.h"
#include "string_compat.h"

#include <xtl.h>

#ifndef ERROR_SUCCESS
#define ERROR_SUCCESS 0L
#endif
#ifndef ERROR_DEVICE_NOT_CONNECTED
#define ERROR_DEVICE_NOT_CONNECTED 1167L
#endif

#define X360_PAD_COUNT 4

// Digital threshold for turning a stick deflection into a d-pad key. The
// analogue values themselves are handed to the runner raw; it applies the
// configurable deadzone (GamepadSlot.deadzone).
#define X360_STICK_KEY_THRESHOLD 0.5f

// Button indices in GamepadSlot, matching the SDL2/Switch backends and
// runner_gamepad.c's gmlButtonToIndex.
enum {
    X360_GP_FACE1 = 0, X360_GP_FACE2, X360_GP_FACE3, X360_GP_FACE4,
    X360_GP_SHOULDERL, X360_GP_SHOULDERR,
    X360_GP_SHOULDERLB, X360_GP_SHOULDERRB,
    X360_GP_SELECT, X360_GP_START,
    X360_GP_STICKL, X360_GP_STICKR,
    X360_GP_PADU, X360_GP_PADD, X360_GP_PADL, X360_GP_PADR,
    X360_GP_HOME,
};

typedef struct {
    WORD mask;
    int gamepadIndex;
    int32_t gmlKey;
    const char* name;
} X360ButtonMapping;

// Pad button -> (gamepad slot index, keyboard VK).
static const X360ButtonMapping X360_BUTTONS[] = {
    { XINPUT_GAMEPAD_A,              X360_GP_FACE1,     'Z',          "A" },        // confirm
    { XINPUT_GAMEPAD_B,              X360_GP_FACE2,     'X',          "B" },        // cancel
    { XINPUT_GAMEPAD_X,              X360_GP_FACE3,     'C',          "X" },        // menu
    { XINPUT_GAMEPAD_Y,              X360_GP_FACE4,     'V',          "Y" },
    { XINPUT_GAMEPAD_LEFT_SHOULDER,  X360_GP_SHOULDERL, VK_PAGEDOWN,  "LB" },
    { XINPUT_GAMEPAD_RIGHT_SHOULDER, X360_GP_SHOULDERR, VK_PAGEUP,    "RB" },
    { XINPUT_GAMEPAD_BACK,           X360_GP_SELECT,    VK_ESCAPE,    "Back" },
    { XINPUT_GAMEPAD_START,          X360_GP_START,     VK_ENTER,     "Start" },
    { XINPUT_GAMEPAD_LEFT_THUMB,     X360_GP_STICKL,    VK_F10,       "LS" },
    { XINPUT_GAMEPAD_RIGHT_THUMB,    X360_GP_STICKR,    VK_F12,       "RS" },
    { XINPUT_GAMEPAD_DPAD_UP,        X360_GP_PADU,      VK_UP,        "Up" },
    { XINPUT_GAMEPAD_DPAD_DOWN,      X360_GP_PADD,      VK_DOWN,      "Down" },
    { XINPUT_GAMEPAD_DPAD_LEFT,      X360_GP_PADL,      VK_LEFT,      "Left" },
    { XINPUT_GAMEPAD_DPAD_RIGHT,     X360_GP_PADR,      VK_RIGHT,     "Right" },
};
#define X360_BUTTON_COUNT (sizeof(X360_BUTTONS) / sizeof(X360_BUTTONS[0]))

// Left stick axes -> d-pad keys, so keyboard-only games steer with the stick.
typedef struct {
    int axisIndex;   // index into GamepadSlot.axisValue
    float sign;      // +1: positive value triggers, -1: negative value triggers
    int32_t gmlKey;
} X360StickMapping;

static const X360StickMapping X360_STICKS[] = {
    { 0, -1.0f, VK_LEFT },
    { 0, +1.0f, VK_RIGHT },
    { 1, -1.0f, VK_UP },
    { 1, +1.0f, VK_DOWN },
};
#define X360_STICK_COUNT (sizeof(X360_STICKS) / sizeof(X360_STICKS[0]))

static WORD s_prevButtons[X360_PAD_COUNT];
static bool s_prevStick[X360_PAD_COUNT][X360_STICK_COUNT];
static bool s_padConnected[X360_PAD_COUNT];

// LB+RB on controller 0 is the port's debug-overlay shortcut. It is consumed
// by the port's main loop and never injected into the runner's key state.
static bool s_overlayComboPrev;
static bool s_overlayTogglePending;

// Set when START is used to skip texture generation. START is masked out of the
// game's input until the physical button is released, so the skip press does
// not open a menu or confirm a dialogue on the first game frame.
static bool s_suppressStartUntilRelease;

static void x360ReleaseMappedKeys(Runner* runner) {
    for (int i = 0; i < (int) X360_BUTTON_COUNT; i++) {
        RunnerKeyboard_onKeyUp(runner->keyboard, X360_BUTTONS[i].gmlKey);
    }
    for (int i = 0; i < (int) X360_STICK_COUNT; i++) {
        RunnerKeyboard_onKeyUp(runner->keyboard, X360_STICKS[i].gmlKey);
    }
}

static void x360ClearGamepadSlot(GamepadSlot* slot) {
    memcpy(slot->buttonDownPrev, slot->buttonDown, sizeof(slot->buttonDownPrev));
    memset(slot->buttonDown, 0, sizeof(slot->buttonDown));
    memset(slot->buttonPressed, 0, sizeof(slot->buttonPressed));
    memset(slot->buttonReleased, 0, sizeof(slot->buttonReleased));
    memset(slot->buttonValue, 0, sizeof(slot->buttonValue));
    memset(slot->axisValue, 0, sizeof(slot->axisValue));
}

static void x360PollPad(Runner* runner, DWORD user, GamepadSlot* slot) {
    XINPUT_STATE state;
    DWORD result = XInputGetState(user, &state);
    bool connected = (result == ERROR_SUCCESS);

    if (!connected && s_padConnected[user]) {
        // Disconnect edge: let go of everything this pad was holding.
        x360ReleaseMappedKeys(runner);
        s_prevButtons[user] = 0;
        memset(s_prevStick[user], 0, sizeof(s_prevStick[user]));
        s_padConnected[user] = false;
        if (user == 0) s_overlayComboPrev = false;
        logInfo("Input: controller %u disconnected\n", (unsigned) user);
    }

    x360ClearGamepadSlot(slot);

    if (!connected) {
        slot->connected = false;
        slot->guid[0] = '\0';
        return;
    }

    if (!s_padConnected[user]) {
        // Reconnect edge: seed the previous state from the current one so a
        // button held through the reconnect does not look like a new press.
        s_prevButtons[user] = state.Gamepad.wButtons;
        memset(s_prevStick[user], 0, sizeof(s_prevStick[user]));
        s_padConnected[user] = true;
        logInfo("Input: controller %u connected\n", (unsigned) user);
    }

    slot->connected = true;
    slot->jid = (int) user;
    strncpy(slot->description, "Xbox 360 Controller", sizeof(slot->description) - 1);
    snprintf(slot->guid, sizeof(slot->guid), "xinput-%u", (unsigned) user);

    WORD buttons = state.Gamepad.wButtons;
    if (user == 0 && s_suppressStartUntilRelease) {
        if ((buttons & XINPUT_GAMEPAD_START) == 0) {
            s_suppressStartUntilRelease = false;
        } else {
            buttons &= (WORD) ~XINPUT_GAMEPAD_START;
        }
    }

    // Debug-overlay shortcut: LB+RB on controller 0. Edge-triggered, and not
    // fed into the keyboard mapping below.
    if (user == 0) {
        const WORD comboMask = XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER;
        bool combo = (buttons & comboMask) == comboMask;
        if (combo && !s_overlayComboPrev) s_overlayTogglePending = true;
        s_overlayComboPrev = combo;
    }

    for (int i = 0; i < (int) X360_BUTTON_COUNT; i++) {
        if (buttons & X360_BUTTONS[i].mask) {
            slot->buttonDown[X360_BUTTONS[i].gamepadIndex] = true;
        }
    }

    // Triggers are analogue; they also count as pressed past the threshold,
    // matching the SDL2 backend.
    float leftTrigger = state.Gamepad.bLeftTrigger / 255.0f;
    float rightTrigger = state.Gamepad.bRightTrigger / 255.0f;
    slot->buttonValue[X360_GP_SHOULDERLB] = leftTrigger;
    slot->buttonValue[X360_GP_SHOULDERRB] = rightTrigger;
    if (leftTrigger >= slot->triggerThreshold) slot->buttonDown[X360_GP_SHOULDERLB] = true;
    if (rightTrigger >= slot->triggerThreshold) slot->buttonDown[X360_GP_SHOULDERRB] = true;

    for (int b = 0; b < GP_BUTTON_COUNT; b++) {
        if (b == X360_GP_SHOULDERLB || b == X360_GP_SHOULDERRB) continue;
        slot->buttonValue[b] = slot->buttonDown[b] ? 1.0f : 0.0f;
    }

    // Sticks: XInput +Y is up, and the runner's convention (shared with the
    // SDL2 and Switch backends) is +Y down, so Y is negated.
    slot->axisValue[0] = state.Gamepad.sThumbLX / 32767.0f;
    slot->axisValue[1] = -state.Gamepad.sThumbLY / 32767.0f;
    slot->axisValue[2] = state.Gamepad.sThumbRX / 32767.0f;
    slot->axisValue[3] = -state.Gamepad.sThumbRY / 32767.0f;

    for (int b = 0; b < GP_BUTTON_COUNT; b++) {
        bool wasDown = slot->buttonDownPrev[b];
        if (slot->buttonDown[b] && !wasDown) slot->buttonPressed[b] = true;
        if (!slot->buttonDown[b] && wasDown) slot->buttonReleased[b] = true;
    }

    runner->gamepads->connectedCount++;

    // Keyboard mapping: emit edges so held keys are not re-pressed every frame.
    WORD previous = s_prevButtons[user];
    for (int i = 0; i < (int) X360_BUTTON_COUNT; i++) {
        bool now = (buttons & X360_BUTTONS[i].mask) != 0;
        bool before = (previous & X360_BUTTONS[i].mask) != 0;
        if (now && !before) {
            RunnerKeyboard_onKeyDown(runner->keyboard, X360_BUTTONS[i].gmlKey);
#ifdef BS_X360_INPUT_LOG
            logInfo("Input: pad %u %s down -> key %d\n",
                    (unsigned) user, X360_BUTTONS[i].name, X360_BUTTONS[i].gmlKey);
#endif
        } else if (!now && before) {
            RunnerKeyboard_onKeyUp(runner->keyboard, X360_BUTTONS[i].gmlKey);
        }
    }
    s_prevButtons[user] = buttons;

    for (int i = 0; i < (int) X360_STICK_COUNT; i++) {
        float value = slot->axisValue[X360_STICKS[i].axisIndex];
        bool pressed = (X360_STICKS[i].sign > 0.0f)
                       ? (value > X360_STICK_KEY_THRESHOLD)
                       : (value < -X360_STICK_KEY_THRESHOLD);
        bool wasPressed = s_prevStick[user][i];
        if (pressed && !wasPressed) {
            RunnerKeyboard_onKeyDown(runner->keyboard, X360_STICKS[i].gmlKey);
        } else if (!pressed && wasPressed) {
            RunnerKeyboard_onKeyUp(runner->keyboard, X360_STICKS[i].gmlKey);
        }
        s_prevStick[user][i] = pressed;
    }
}

void X360Input_poll(Runner* runner) {
    for (DWORD user = 0; user < X360_PAD_COUNT; user++) {
        x360PollPad(runner, user, &runner->gamepads->slots[user]);
    }
}

bool X360Input_consumeOverlayToggle(void) {
    bool pressed = s_overlayTogglePending;
    s_overlayTogglePending = false;
    return pressed;
}

bool X360Input_consumeStartPress(void) {
    XINPUT_STATE state;
    if (XInputGetState(0, &state) != ERROR_SUCCESS) return false;
    if ((state.Gamepad.wButtons & XINPUT_GAMEPAD_START) == 0) return false;

    s_suppressStartUntilRelease = true;
    return true;
}
