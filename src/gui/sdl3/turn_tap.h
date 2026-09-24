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

/* Sample an opposed pair of keys. When the first key delivers a queued tap
 * this sample, a tap queued on the second key waits one sample instead of
 * being consumed, since the button priority in inputGetKeys can only turn
 * one way per sample and would drop it. A physically held first key does
 * not hold the second's tap back: that is the same priority the two held
 * keys already have, and a tap kept until the hold ends would turn the
 * tank the other way long after it was pressed. */
static inline void turnTapReadPair(TurnTapState *first, bool firstHeld,
                                   TurnTapState *second, bool secondHeld,
                                   bool *firstOut, bool *secondOut) {
  *firstOut = turnTapRead(first, firstHeld);
  if (*firstOut && !firstHeld && !secondHeld && second->pending &&
      !second->lastOutput) {
    *secondOut = false;
    return;
  }
  *secondOut = turnTapRead(second, secondHeld);
}

#endif
