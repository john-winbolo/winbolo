/*
 * Copyright (c) 1998-2026 John Morrison.
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
#include "global.h"
#include "client_enums.h"  /* tankButton */

/* Set to 1 to enable controller tuning debug: the on-screen stick/turn/
   build-cursor magnitude overlay and the build_cursor controller.log file.
   Leave 0 for normal builds. */
#define WB_CONTROLLER_DEBUG 0

#ifdef __cplusplus
extern "C" {
#endif

void inputGamepadInit(void);
void inputGamepadShutdown(void);
void inputGamepadProcessEvent(const SDL_Event *e);
bool inputGamepadIsConnected(void);
/* true = Steam Input (Path A) is driving the controller; false = native
 * SDL gamepad (Path B). Useful for diagnostics / glyph selection. */
bool inputGamepadIsSteamInput(void);
/* true iff a REAL physical controller is present — native SDL pad, or a
 * Steam Input device per the hot-plug callbacks.  Unlike
 * inputGamepadIsConnected(), this is not fooled by Steam Input's
 * always-present keyboard/mouse virtual controller.  Use for UI mode
 * decisions (controller-mode vs keyboard); gameplay input still uses the
 * Path A/B read functions. */
bool inputGamepadRealControllerConnected(void);
/* true if the active controller has any live input this frame (button held
 * or stick past the deadzone), across both Path A and Path B. Polled by
 * input_source so controller use registers even on a Steam launch. */
bool inputGamepadActivityDetected(void);

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
  GP_ACT_BUILD_CANCEL,    /* exit build cursor mode without building */
  GP_ACT_LOCK_HEADING,    /* toggle: freeze tank facing, stick only drives */
  GP_ACT_TANK_VIEW,       /* recentre on / return to tank view */
  /* Smart ping. The menu one is held — the pie opens under it and the right
     stick or the d-pad picks a sector — and the six below it send one kind
     outright with no menu. All unbound by default.
     The six MUST stay contiguous and in PING_KIND_* order: the direct
     dispatch turns a kind into an action with
     GP_ACT_PING_DIRECT_FIRST + kind. */
  GP_ACT_PING_MENU,
  GP_ACT_PING_STANDARD,
  GP_ACT_PING_CAUTION,
  GP_ACT_PING_ASSIST,
  GP_ACT_PING_ATTACK,
  GP_ACT_PING_ON_MY_WAY,
  GP_ACT_PING_BOT_COMMAND,
  GP_ACT_COUNT
} GamepadAction;

/* The action the direct ping for PING_KIND_STANDARD sits on; add the kind to
   reach the rest. */
#define GP_ACT_PING_DIRECT_FIRST GP_ACT_PING_STANDARD

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
bool inputGamepadIsBuildCursorToggleHeld(void); /* R3 live held state (gesture timing) */
bool inputGamepadIsBuildCancelEdge(void);       /* Cancel-build press, consumed on read */
bool inputGamepadIsLockHeadingHeld(void);       /* Lock-direction held (live state) */
bool inputGamepadIsTankViewEdge(void);          /* Tank-view press, consumed on read */
bool inputGamepadIsViewPlayersEdge(void);      /* D-pad Right press, consumed on read */

/* --- Smart ping --- */
/* Is the pie-menu binding held? The pie opens while it is and sends what the
   aim below is pointing at when it goes. */
bool inputGamepadIsPingMenuHeld(void);
/* A direct-ping binding just went down: writes its PING_KIND_* to *outKind
   and returns true, consuming the edge. Only one per call — a pad cannot
   press two in the same frame in any way worth answering. */
bool inputGamepadConsumePingDirect(int *outKind);
/* Which way the pad is pointing for an open pie: the right stick past its
   deadzone, or the d-pad if the stick is idle. Writes a unit-ish vector with
   +y down, the way the pie's own geometry measures. False when the pad is
   pointing nowhere, which is the standard ping. */
bool inputGamepadGetPingAim(float *outX, float *outY);
bool inputGamepadConsumeActiveDisconnect(void);  /* Active controller disconnect, consumed on read */

/* Right-stick scroll sensitivity multiplier (clamped 0.25..4.0 by UI). */
extern float g_gamepadScrollSensitivity;
/* Left-stick tank-move sensitivity (clamped 0.25..2.0 by UI). Lower = finer
   steering: small stick movements turn the tank more slowly. */
extern float g_gamepadTankSensitivity;
/* Right-stick build-cursor move sensitivity (clamped 0.25..2.0 by UI). Lower =
   finer building: the build cursor moves more slowly per stick deflection. */
extern float g_gamepadBuildCursorSensitivity;

/* DEBUG on-screen readouts (0..1). */
extern float g_dbgStickMag;
extern float g_dbgTurnMag;
extern float g_dbgCursorStickMag;
extern float g_dbgCursorMoveMag;

SDL_Gamepad     *inputGamepadGetActiveHandle(void);  /* may be NULL */
SDL_GamepadType  inputGamepadGetActiveType(void);
void             inputGamepadRumble(float strength, Uint32 durationMs);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_GAMEPAD_H */
