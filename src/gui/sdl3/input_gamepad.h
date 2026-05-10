/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
*Name:          Gamepad Input
*Filename:      input_gamepad.h
*Purpose:
*  Gamepad input handling. Polls SDL3 gamepad state each
*  frame for movement, actions, gunsight, and map scroll.
*  Single-controller policy: first-connected wins; on
*  disconnect, promotes the next-connected gamepad.
*  Works on all platforms whenever a gamepad is connected.
*********************************************************/

#ifndef INPUT_GAMEPAD_H
#define INPUT_GAMEPAD_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include "../../bolo/global.h"
#include "../../bolo/screen.h"

#ifdef __cplusplus
extern "C" {
#endif

void inputGamepadInit(void);
void inputGamepadShutdown(void);
void inputGamepadProcessEvent(const SDL_Event *e);
bool inputGamepadIsConnected(void);

/* --- Path B rebindable action set ---
 * Twelve gameplay actions the player can rebind from the Configure
 * Keys dialog.  Path A (Steam Input) ignores this table — Steam's
 * own configurator owns binding for Steam launches.  Defaults are
 * applied by inputGamepadBindingsResetDefaults and reproduce the
 * historical hardcoded mapping. */
typedef enum {
  GP_ACT_FIRE = 0,
  GP_ACT_MINE,
  GP_ACT_BUILD_CONFIRM,
  GP_ACT_VIEW_CYCLE,
  GP_ACT_GUNSIGHT_DEC,
  GP_ACT_GUNSIGHT_INC,
  GP_ACT_BUILD_PREV,
  GP_ACT_BUILD_NEXT,
  GP_ACT_BUILD_CURSOR_TOGGLE,
  GP_ACT_QUICK_CHAT,
  GP_ACT_PAUSE,
  GP_ACT_VIEW_PLAYERS,
  GP_ACT_COUNT
} GamepadAction;

typedef enum {
  GP_BIND_NONE    = 0,
  GP_BIND_BUTTON  = 1,    /* code is SDL_GamepadButton */
  GP_BIND_TRIGGER = 2     /* code is SDL_GamepadAxis (LEFT_TRIGGER / RIGHT_TRIGGER) */
} GamepadBindKind;

typedef struct {
  GamepadBindKind kind;
  int             code;
} GamepadBinding;

/* Two binding slots per action.  Held = either slot held; edge events
   fire from either slot.  Slot order is purely for UI display.  Only
   FIRE ships with a non-NONE secondary by default (RT primary, SOUTH
   secondary) — every other action defaults secondary to NONE. */
typedef enum {
  GP_SLOT_PRIMARY   = 0,
  GP_SLOT_SECONDARY = 1,
  GP_SLOT_COUNT
} GamepadSlot;

typedef struct {
  GamepadBinding pri;
  GamepadBinding sec;
} GamepadActionBindings;

typedef struct {
  GamepadActionBindings b[GP_ACT_COUNT];
} GamepadBindings;

void                  inputGamepadBindingsResetDefaults(GamepadBindings *out);
const GamepadBinding *inputGamepadBindingsGet(GamepadAction a, GamepadSlot s);
void                  inputGamepadBindingsSet(GamepadAction a, GamepadSlot s, GamepadBinding b);
void                  inputGamepadBindingsGetAll(GamepadBindings *out);
void                  inputGamepadBindingsSetAll(const GamepadBindings *in);
const char           *inputGamepadActionName(GamepadAction a);

tankButton inputGamepadGetMovement(BYTE tankAngle);
bool inputGamepadIsFireHeld(void);
bool inputGamepadIsMineHeld(void);
int  inputGamepadGetGunsightChange(void);  /* -1, 0, +1; live held state — caller rate-limits */
bool inputGamepadGetScrollDirection(float *dx, float *dy);

/* Builder UX edge-triggered getters (consume on read). */
int  inputGamepadGetBuildSelectChange(void);  /* -1 (D-pad UP), +1 (D-pad DOWN), 0 */
bool inputGamepadIsViewToggleEdge(void);       /* Y press, consumed on read */
bool inputGamepadIsBuilderConfirmEdge(void);   /* X press, consumed on read */
bool inputGamepadIsPauseEdge(void);            /* Start press, consumed on read */
bool inputGamepadIsQuickChatEdge(void);        /* D-pad LEFT press, consumed on read */
bool inputGamepadIsBuildCursorToggleEdge(void); /* R3 press, consumed on read */
bool inputGamepadIsViewPlayersEdge(void);      /* D-pad Right press, consumed on read */
bool inputGamepadConsumeActiveDisconnect(void);  /* Active controller disconnect, consumed on read */

/* Right-stick scroll sensitivity multiplier (clamped 0.25..4.0 by UI). */
extern float g_gamepadScrollSensitivity;

SDL_Gamepad     *inputGamepadGetActiveHandle(void);  /* may be NULL */
SDL_GamepadType  inputGamepadGetActiveType(void);
void             inputGamepadRumble(float strength, Uint32 durationMs);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_GAMEPAD_H */
