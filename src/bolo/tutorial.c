/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Filename:      tutorial.c
 *Purpose:       Tutorial step table and lookup.
 *********************************************************/
#include "tutorial.h"
#include "../gui/lang.h"

const TutorialStep tutorialSteps[] = {
    /* Intro: fires once at game start regardless of tank position. */
    { TUTORIAL_POS_ANY,
        { STR_TUTORIAL_START01, STR_TUTORIAL_START02,
          STR_TUTORIAL_START03, STR_TUTORIAL_START04 } },
    /* Positional steps. */
    { 210, { STR_TUTORIAL01, 0,              0,              0 } },
    { 199, { STR_TUTORIAL02, 0,              0,              0 } },
    { 192, { STR_TUTORIAL03, 0,              0,              0 } },
    { 186, { STR_TUTORIAL04, 0,              0,              0 } },
    { 181, { STR_TUTORIAL05, 0,              0,              0 } },
    { 175, { STR_TUTORIAL06, 0,              0,              0 } },
    { 166, { STR_TUTORIAL07, 0,              0,              0 } },
    { 159, { STR_TUTORIAL08, STR_TUTORIAL09, 0,              0 } },
    { 142, { STR_TUTORIAL10, STR_TUTORIAL11, 0,              0 } },
    { 122, { STR_TUTORIAL12, 0,              0,              0 } },
    { 120, { STR_TUTORIAL13, STR_TUTORIAL14, 0,              0 } },
    { 110, { STR_TUTORIAL15, 0,              0,              0 } },
    { 103, { STR_TUTORIAL16, 0,              0,              0 } },
    {  98, { STR_TUTORIAL17, 0,              0,              0 } },
    {  84, { STR_TUTORIAL18, STR_TUTORIAL19, 0,              0 } },
    {  66, { STR_TUTORIAL20, STR_TUTORIAL21, 0,              0 } },
    {  47, { STR_TUTORIAL22, STR_TUTORIAL23, STR_TUTORIAL24, 0 } },
    {  21, { STR_TUTORIAL25, 0,              0,              0 } },
};

const int tutorialStepCount =
    (int)(sizeof(tutorialSteps) / sizeof(tutorialSteps[0]));

bool tutorialIsStopPos(BYTE pos) {
    int i;
    for (i = 0; i < tutorialStepCount; i++) {
        if (tutorialSteps[i].pos == TUTORIAL_POS_ANY) continue;
        if (tutorialSteps[i].pos == pos) return TRUE;
    }
    return FALSE;
}
