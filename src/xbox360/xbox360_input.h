#ifndef _BS_X360_INPUT_H_
#define _BS_X360_INPUT_H_

// Xbox 360 input backend.
//
// Polls the four XInput user slots once per frame and drives the runner's two
// input contracts:
//
//   * RunnerGamepadState - the GameMaker gamepad API (gamepad_button_check,
//     gamepad_axis_value, ...). This is the primary path for a console title.
//   * RunnerKeyboardState - a VK mapping so keyboard-driven games are playable
//     with a pad. Uses the public onKeyDown/onKeyUp entry points only.
//
// Neither public mapping table is modified; the port owns the mapping.
//
// Port-owned. No public source is touched.

#include <stdbool.h>

struct Runner;

// Call once per frame, after RunnerKeyboard_beginFrame and
// RunnerGamepad_beginFrame and before Runner_step.
void X360Input_poll(struct Runner* runner);

// One-shot LB+RB edge on controller 0, used by the port's debug overlay.
// It is deliberately not injected into RunnerKeyboardState, so the game never
// sees the combo. Returns true once per press and clears the pending flag.
bool X360Input_consumeOverlayToggle(void);

// Detects START on controller 0 for the texture-generation skip. The START
// press is suppressed from the game's input until the button is released, so
// the first game frame does not see the skip press.
bool X360Input_consumeStartPress(void);

#endif /* _BS_X360_INPUT_H_ */
