/* Preserve short keyboard presses between simulation samples. */
#ifndef WINBOLO_TURN_TAP_H
#define WINBOLO_TURN_TAP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct TurnTapState {
	bool physicalDown;
	bool lastOutput;
	uint8_t pending;
} TurnTapState;

static inline void turnTapReset(TurnTapState *state) {
	state->physicalDown = false;
	state->lastOutput = false;
	state->pending = 0;
}

static inline void turnTapEvent(TurnTapState *state, bool down, bool accept) {
	if (down && !state->physicalDown && accept && state->pending < UINT8_MAX) {
		state->pending++;
	}
	state->physicalDown = down;
}

static inline bool turnTapRead(TurnTapState *state, bool held) {
	/* Preserve a release between distinct presses, even if both events
	 * arrived between samples. This also resets the tank's turn ramp. */
	if (state->pending && state->lastOutput) {
		state->lastOutput = false;
		return false;
	}
	if (state->pending) {
		state->pending--;
		held = true;
	}
	state->lastOutput = held;
	return held;
}

#endif
