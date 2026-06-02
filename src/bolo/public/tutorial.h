/*********************************************************
 *Filename:      tutorial.h
 *Purpose:       Shared definitions for the built-in Bolo
 *               tutorial. Pairs each Y-row trigger with the
 *               sequence of message strings to display.
 *               Used by both the server-authoritative tank
 *               stop logic (tank.c) and the client-side
 *               dialog sequencer (winbolo.c).
 *********************************************************/
#ifndef TUTORIAL_H
#define TUTORIAL_H

#include "global.h"
#include <stdint.h>

/* 4 covers the worst case: the opening intro fires 4 welcome dialogs
 * in a row (STR_TUTORIAL_START01..04). Positional steps use up to 3. */
#define TUTORIAL_MAX_MSGS 4

/* Sentinel pos value: step fires on any tick (regardless of tank row).
 * Used for the intro, which is a pre-game briefing independent of
 * where the tank is. 0xFF is safe: valid tank rows never reach 255. */
#define TUTORIAL_POS_ANY  0xFF

typedef struct {
    BYTE     pos;                        /* Y-row trigger, or TUTORIAL_POS_ANY. */
    uint16_t msgs[TUTORIAL_MAX_MSGS];    /* STR_TUTORIAL* IDs; 0 ends the list. */
} TutorialStep;

extern const TutorialStep tutorialSteps[];
extern const int          tutorialStepCount;

/* Returns TRUE if `pos` is one of the tutorial's stop rows. */
bool tutorialIsStopPos(BYTE pos);

#endif
