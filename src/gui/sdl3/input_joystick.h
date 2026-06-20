/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
*Name:          Joystick Input
*Filename:      input_joystick.h
*Purpose:
*  Shared joystick-to-movement math used by both touch
*  and gamepad input modules.
*********************************************************/

#ifndef INPUT_JOYSTICK_H
#define INPUT_JOYSTICK_H

#include <stdbool.h>
#include "global.h"
#include "client_enums.h"  /* tankButton */

#ifdef __cplusplus
extern "C" {
#endif

BYTE joystickAngleToBolo(float atan2Deg);
tankButton joystickGetMovementRelative(float dx, float dy, float dist, float deadzone, float maxReach);
tankButton joystickGetMovementAbsolute(float dx, float dy, float dist, float deadzone, float maxReach, BYTE tankAngle);
void joystickSetAbsoluteSteering(bool enabled);
bool joystickGetAbsoluteSteering(void);
void joystickResetState(void);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_JOYSTICK_H */
