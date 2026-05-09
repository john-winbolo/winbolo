/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
*Name:          Gamepad Input
*Filename:      input_gamepad.c
*Purpose:
*  Gamepad input handling. Polls SDL3 gamepad state each
*  frame, converting axis/button state into tank
*  movement, fire, mine, gunsight, and scroll commands.
*
*  Single-controller policy: first-connected wins. On
*  disconnect, walks SDL_GetGamepads() to promote the
*  next available gamepad. Caches the active handle
*  so haptic / queries don't pay open/close overhead.
*********************************************************/

#include <math.h>
#include <SDL3/SDL.h>
#include "input_gamepad.h"
#include "input_joystick.h"
#include "../../steam/steam_wrapper.h"
#include "../../steam/steam_input_actions.h"

/* Axis normalisation: int16 range to -1..1 */
#define AXIS_NORM (1.0f / 32767.0f)

/* Movement and scroll deadzones (radial, normalised) */
#define MOVE_DEADZONE  0.20f
#define MOVE_MAX_REACH 0.95f
#define SCROLL_DEADZONE  0.20f
#define SCROLL_MAX_REACH 0.95f

/* Trigger threshold (normalised 0..1) */
#define TRIGGER_THRESHOLD 0.5f

/* --- State --- */

static SDL_Gamepad     *s_activeGamepad = NULL;
static SDL_JoystickID   s_activeId      = 0;
static SDL_GamepadType  s_activeType    = SDL_GAMEPAD_TYPE_UNKNOWN;

/* Latch so joystickResetState() fires once per stick-centred transition. */
static bool s_moveWasActive = false;

/* Builder UX edge-triggered state, consumed on read. */
static int  s_buildSelectChange  = 0;
static bool s_viewToggleEdge     = false;
static bool s_builderConfirmEdge = false;
static bool s_pauseEdge          = false;
static bool s_quickChatEdge      = false;
static bool s_buildCursorToggleEdge = false;
static bool s_statusToggleEdge   = false;
static bool s_activeDisconnectedEdge = false;

/* Per-trigger last-axis state for edge synthesis when a trigger is
   bound to an edge action (view, pause, etc.).  Indexed by axis -
   SDL_GAMEPAD_AXIS_LEFT_TRIGGER. */
static bool s_triggerWasPressed[2] = { false, false };

/* Path B rebindable action table.  Seeded with defaults the first
   time it is read or written; gameFront's prefs load may pre-populate
   it before inputGamepadInit() runs. */
static GamepadBindings s_bindings;
static bool            s_bindings_seeded = false;

static void seedBindingsIfNeeded(void) {
  if (s_bindings_seeded) return;
  inputGamepadBindingsResetDefaults(&s_bindings);
  s_bindings_seeded = true;
}

/* Right-stick scroll sensitivity multiplier (also referenced from UI/prefs). */
float g_gamepadScrollSensitivity = 1.0f;

/* --- Path A (Steam Input) state --- */

/* Tracks whether Steam Input was driving the controller last frame.
   On a transition (B->A or A->B) we reset edge state so a held button
   doesn't synthesize a spurious rising edge after the switch. */
static bool s_path_a_was_active = false;

/* Latched on a Path A active->inactive transition so the auto-pause
   trigger surfaces it via inputGamepadConsumeActiveDisconnect.  Steam
   Input doesn't fire SDL_EVENT_GAMEPAD_REMOVED, so this is the
   equivalent signal for the Path A side. */
static bool s_path_a_just_disconnected = false;

/* Path A last-frame button states.  Steam Input only exposes bState
   (current pressed/not), so we synthesize rising edges by comparing
   to the previous frame.  All cleared by reset_path_a_edges(). */
static bool s_path_a_last_pause           = false;
static bool s_path_a_last_quickchat       = false;
static bool s_path_a_last_view            = false;
static bool s_path_a_last_builder_confirm = false;
static bool s_path_a_last_build_prev      = false;
static bool s_path_a_last_build_next      = false;
static bool s_path_a_last_build_cursor_toggle = false;
static bool s_path_a_last_status_toggle   = false;

static void reset_path_a_edges(void) {
  s_path_a_last_pause           = false;
  s_path_a_last_quickchat       = false;
  s_path_a_last_view            = false;
  s_path_a_last_builder_confirm = false;
  s_path_a_last_build_prev      = false;
  s_path_a_last_build_next      = false;
  s_path_a_last_build_cursor_toggle = false;
  s_path_a_last_status_toggle   = false;
}

static bool path_a_active(void) {
  bool now = steam_input_has_active_controller();
  if (now != s_path_a_was_active) {
    if (s_path_a_was_active && !now) {
      /* Path A just lost its controller — surface as a disconnect
         so auto-pause-on-disconnect works on Steam launches too. */
      s_path_a_just_disconnected = true;
    }
    reset_path_a_edges();
    s_path_a_was_active = now;
  }
  return now;
}

/* --- Binding table --- */

static const char *kActionNames[GP_ACT_COUNT] = {
  "fire",
  "mine",
  "build_confirm",
  "view_cycle",
  "gunsight_dec",
  "gunsight_inc",
  "build_prev",
  "build_next",
  "build_cursor_toggle",
  "quick_chat",
  "pause",
  "view_players",
};

const char *inputGamepadActionName(GamepadAction a) {
  if ((unsigned)a >= GP_ACT_COUNT) return "";
  return kActionNames[a];
}

/* Defaults reproduce the historical hardcoded mapping in this file.
   FIRE keeps its dual binding (RT primary + SOUTH secondary) so the
   pre-refactor "RT or A" behaviour is preserved.  MINE remains LT-only
   because EAST (B) is reserved for ImGui cancel.  Every other action
   defaults secondary to NONE.

   fire                = RT     + SOUTH
   mine                = LT     + NONE   (B reserved for cancel)
   build_confirm       = WEST   + NONE
   view_cycle          = NORTH  + NONE
   gunsight_dec        = LB     + NONE
   gunsight_inc        = RB     + NONE
   build_prev          = DPAD_UP    + NONE
   build_next          = DPAD_DOWN  + NONE
   build_cursor_toggle = R3     + NONE
   quick_chat          = DPAD_LEFT  + NONE
   pause               = START  + NONE
   view_players        = DPAD_RIGHT + NONE */
void inputGamepadBindingsResetDefaults(GamepadBindings *out) {
  if (!out) return;
  static const GamepadBinding kNone = { GP_BIND_NONE, 0 };
  for (int i = 0; i < GP_ACT_COUNT; ++i) {
    out->b[i].pri = kNone;
    out->b[i].sec = kNone;
  }
  out->b[GP_ACT_FIRE].pri                = (GamepadBinding){ GP_BIND_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER };
  out->b[GP_ACT_FIRE].sec                = (GamepadBinding){ GP_BIND_BUTTON,  SDL_GAMEPAD_BUTTON_SOUTH };
  out->b[GP_ACT_MINE].pri                = (GamepadBinding){ GP_BIND_TRIGGER, SDL_GAMEPAD_AXIS_LEFT_TRIGGER };
  out->b[GP_ACT_BUILD_CONFIRM].pri       = (GamepadBinding){ GP_BIND_BUTTON,  SDL_GAMEPAD_BUTTON_WEST };
  out->b[GP_ACT_VIEW_CYCLE].pri          = (GamepadBinding){ GP_BIND_BUTTON,  SDL_GAMEPAD_BUTTON_NORTH };
  out->b[GP_ACT_GUNSIGHT_DEC].pri        = (GamepadBinding){ GP_BIND_BUTTON,  SDL_GAMEPAD_BUTTON_LEFT_SHOULDER };
  out->b[GP_ACT_GUNSIGHT_INC].pri        = (GamepadBinding){ GP_BIND_BUTTON,  SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER };
  out->b[GP_ACT_BUILD_PREV].pri          = (GamepadBinding){ GP_BIND_BUTTON,  SDL_GAMEPAD_BUTTON_DPAD_UP };
  out->b[GP_ACT_BUILD_NEXT].pri          = (GamepadBinding){ GP_BIND_BUTTON,  SDL_GAMEPAD_BUTTON_DPAD_DOWN };
  out->b[GP_ACT_BUILD_CURSOR_TOGGLE].pri = (GamepadBinding){ GP_BIND_BUTTON,  SDL_GAMEPAD_BUTTON_RIGHT_STICK };
  out->b[GP_ACT_QUICK_CHAT].pri          = (GamepadBinding){ GP_BIND_BUTTON,  SDL_GAMEPAD_BUTTON_DPAD_LEFT };
  out->b[GP_ACT_PAUSE].pri               = (GamepadBinding){ GP_BIND_BUTTON,  SDL_GAMEPAD_BUTTON_START };
  out->b[GP_ACT_STATUS_TOGGLE].pri       = (GamepadBinding){ GP_BIND_BUTTON,  SDL_GAMEPAD_BUTTON_DPAD_RIGHT };
}

static GamepadBinding *slotPtr(GamepadActionBindings *ab, GamepadSlot s) {
  return (s == GP_SLOT_SECONDARY) ? &ab->sec : &ab->pri;
}

const GamepadBinding *inputGamepadBindingsGet(GamepadAction a, GamepadSlot s) {
  if ((unsigned)a >= GP_ACT_COUNT) return NULL;
  if ((unsigned)s >= GP_SLOT_COUNT) return NULL;
  seedBindingsIfNeeded();
  return (s == GP_SLOT_SECONDARY) ? &s_bindings.b[a].sec : &s_bindings.b[a].pri;
}

void inputGamepadBindingsSet(GamepadAction a, GamepadSlot s, GamepadBinding b) {
  if ((unsigned)a >= GP_ACT_COUNT) return;
  if ((unsigned)s >= GP_SLOT_COUNT) return;
  seedBindingsIfNeeded();
  *slotPtr(&s_bindings.b[a], s) = b;
}

void inputGamepadBindingsGetAll(GamepadBindings *out) {
  if (!out) return;
  seedBindingsIfNeeded();
  *out = s_bindings;
}

void inputGamepadBindingsSetAll(const GamepadBindings *in) {
  if (!in) return;
  s_bindings = *in;
  s_bindings_seeded = true;
}

/* True if the binding identifies a held button or a trigger above the
   threshold.  Returns false when the binding is NONE or no gamepad. */
static bool bindingIsHeld(const GamepadBinding *b) {
  if (!b || !s_activeGamepad) return false;
  if (b->kind == GP_BIND_BUTTON) {
    return SDL_GetGamepadButton(s_activeGamepad, (SDL_GamepadButton)b->code);
  }
  if (b->kind == GP_BIND_TRIGGER) {
    float v = (float)SDL_GetGamepadAxis(s_activeGamepad, (SDL_GamepadAxis)b->code) * AXIS_NORM;
    return v > TRIGGER_THRESHOLD;
  }
  return false;
}

/* Held = either slot held.  Both slots are checked; NONE slots
   short-circuit to false inside bindingIsHeld via the kind switch. */
static bool actionIsHeld(GamepadAction a) {
  if (!s_activeGamepad) return false;
  seedBindingsIfNeeded();
  const GamepadActionBindings *ab = &s_bindings.b[a];
  return bindingIsHeld(&ab->pri) || bindingIsHeld(&ab->sec);
}

/* Edge-action dispatch — call from an input event when the bound
   button/trigger crosses to pressed.  Sets exactly the s_*Edge flags
   the queries below consume.  Build prev/next set the integer
   accumulator instead.  Held actions (fire, mine, gunsight inc/dec)
   are intentionally absent: their queries poll live state. */
static void fireEdgeForAction(GamepadAction a) {
  switch (a) {
    case GP_ACT_BUILD_PREV:          s_buildSelectChange     = -1; break;
    case GP_ACT_BUILD_NEXT:          s_buildSelectChange     = +1; break;
    case GP_ACT_VIEW_CYCLE:          s_viewToggleEdge        = true; break;
    case GP_ACT_BUILD_CONFIRM:       s_builderConfirmEdge    = true; break;
    case GP_ACT_PAUSE:               s_pauseEdge             = true; break;
    case GP_ACT_QUICK_CHAT:          s_quickChatEdge         = true; break;
    case GP_ACT_BUILD_CURSOR_TOGGLE: s_buildCursorToggleEdge = true; break;
    case GP_ACT_STATUS_TOGGLE:       s_statusToggleEdge      = true; break;
    default: break;
  }
}

/* --- Helpers --- */

static void openGamepadById(SDL_JoystickID id) {
  SDL_Gamepad *gp = SDL_OpenGamepad(id);
  if (!gp) return;
  s_activeGamepad = gp;
  s_activeId      = id;
  s_activeType    = SDL_GetGamepadType(gp);
}

static void clearActive(void) {
  s_activeGamepad = NULL;
  s_activeId      = 0;
  s_activeType    = SDL_GAMEPAD_TYPE_UNKNOWN;
}

static void promoteNextGamepad(void) {
  int count = 0;
  SDL_JoystickID *list = SDL_GetGamepads(&count);
  if (list) {
    for (int i = 0; i < count; i++) {
      if (list[i] != 0) {
        openGamepadById(list[i]);
        if (s_activeGamepad) break;
      }
    }
    SDL_free(list);
  }
}

/* --- Init / Shutdown --- */

void inputGamepadInit(void) {
  clearActive();
  s_moveWasActive      = false;
  s_buildSelectChange  = 0;
  s_viewToggleEdge     = false;
  s_builderConfirmEdge = false;
  s_pauseEdge          = false;
  s_quickChatEdge      = false;
  s_buildCursorToggleEdge = false;
  s_statusToggleEdge   = false;
  s_activeDisconnectedEdge = false;
  s_triggerWasPressed[0] = false;
  s_triggerWasPressed[1] = false;
  /* Bindings are seeded on first access (or by gameFront's prefs
     load if it ran first); do not reset here. */
  seedBindingsIfNeeded();
  /* Reset Path A edge tracking so first-frame reads start from a
     known zero state regardless of which path eventually drives. */
  s_path_a_was_active        = false;
  s_path_a_just_disconnected = false;
  reset_path_a_edges();

  /* Steam Deck built-in controller HIDAPI access. With a real Steam
     App ID, Steam Input handles this automatically; this hint covers
     development and non-Steam builds. */
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK, "1");

  if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
    return;
  }

  promoteNextGamepad();
}

void inputGamepadShutdown(void) {
  if (s_activeGamepad) {
    SDL_CloseGamepad(s_activeGamepad);
  }
  clearActive();
}

/* --- Event processing --- */

void inputGamepadProcessEvent(const SDL_Event *e) {
  if (!e) return;

  switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
      if (s_activeGamepad == NULL) {
        openGamepadById(e->gdevice.which);
      }
      break;

    case SDL_EVENT_GAMEPAD_REMOVED:
      if (s_activeGamepad && e->gdevice.which == s_activeId) {
        SDL_CloseGamepad(s_activeGamepad);
        clearActive();
        s_activeDisconnectedEdge = true;
        promoteNextGamepad();
      }
      break;

    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
      if (s_activeGamepad && e->gbutton.which == s_activeId) {
        seedBindingsIfNeeded();
        for (int i = 0; i < GP_ACT_COUNT; ++i) {
          const GamepadBinding *slots[2] = {
            &s_bindings.b[i].pri, &s_bindings.b[i].sec
          };
          for (int s = 0; s < 2; ++s) {
            const GamepadBinding *b = slots[s];
            if (b->kind == GP_BIND_BUTTON && b->code == e->gbutton.button) {
              fireEdgeForAction((GamepadAction)i);
              break;
            }
          }
        }
      }
      break;

    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      /* No-op: held-state queries read live via SDL_GetGamepadButton. */
      break;

    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      if (s_activeGamepad && e->gaxis.which == s_activeId) {
        SDL_GamepadAxis axis = (SDL_GamepadAxis)e->gaxis.axis;
        if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
            axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
          seedBindingsIfNeeded();
          int slot = (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) ? 0 : 1;
          float v = (float)e->gaxis.value * AXIS_NORM;
          bool nowDown = v > TRIGGER_THRESHOLD;
          if (nowDown && !s_triggerWasPressed[slot]) {
            for (int i = 0; i < GP_ACT_COUNT; ++i) {
              const GamepadBinding *bs[2] = {
                &s_bindings.b[i].pri, &s_bindings.b[i].sec
              };
              for (int j = 0; j < 2; ++j) {
                if (bs[j]->kind == GP_BIND_TRIGGER && bs[j]->code == (int)axis) {
                  fireEdgeForAction((GamepadAction)i);
                  break;
                }
              }
            }
          }
          s_triggerWasPressed[slot] = nowDown;
        }
      }
      break;

    default:
      break;
  }
}

/* --- Queries --- */

bool inputGamepadIsConnected(void) {
  if (path_a_active()) return true;
  return s_activeGamepad != NULL;
}

/* Path B only: Steam Input doesn't expose an SDL_Gamepad handle.
   Returns NULL on Path A (callers that need raw SDL handles should
   gate on inputGamepadIsConnected and degrade gracefully). */
SDL_Gamepad *inputGamepadGetActiveHandle(void) {
  return s_activeGamepad;
}

/* Path B only for V1.  Phase 7B will translate Path A's
   ESteamInputType to a glyph atlas selector via a separate lookup;
   no need to forge an SDL_GamepadType here. */
SDL_GamepadType inputGamepadGetActiveType(void) {
  return s_activeType;
}

tankButton inputGamepadGetMovement(BYTE tankAngle) {
  if (path_a_active()) {
    float x = 0.0f, y = 0.0f;
    steam_input_get_analog_action(SI_ANALOG_TANK_MOVE, &x, &y);
    /* Steam Input's joystick_move convention: +Y = up.  SDL_Gamepad
       and the joystickGetMovement* helpers treat +Y = down.  Negate
       to match.  If movement turns out flipped on first Deck test,
       remove this negation. */
    y = -y;
    float dist = sqrtf(x * x + y * y);
    if (dist < MOVE_DEADZONE) {
      if (s_moveWasActive) {
        joystickResetState();
        s_moveWasActive = false;
      }
      return TNONE;
    }
    s_moveWasActive = true;
    if (joystickGetAbsoluteSteering()) {
      return joystickGetMovementAbsolute(x, y, dist, MOVE_DEADZONE, MOVE_MAX_REACH, tankAngle);
    }
    return joystickGetMovementRelative(x, y, dist, MOVE_DEADZONE, MOVE_MAX_REACH);
  }

  if (!s_activeGamepad) {
    if (s_moveWasActive) {
      joystickResetState();
      s_moveWasActive = false;
    }
    return TNONE;
  }

  float x = (float)SDL_GetGamepadAxis(s_activeGamepad, SDL_GAMEPAD_AXIS_LEFTX) * AXIS_NORM;
  float y = (float)SDL_GetGamepadAxis(s_activeGamepad, SDL_GAMEPAD_AXIS_LEFTY) * AXIS_NORM;
  float dist = sqrtf(x * x + y * y);

  if (dist < MOVE_DEADZONE) {
    if (s_moveWasActive) {
      joystickResetState();
      s_moveWasActive = false;
    }
    return TNONE;
  }

  s_moveWasActive = true;

  if (joystickGetAbsoluteSteering()) {
    return joystickGetMovementAbsolute(x, y, dist, MOVE_DEADZONE, MOVE_MAX_REACH, tankAngle);
  } else {
    return joystickGetMovementRelative(x, y, dist, MOVE_DEADZONE, MOVE_MAX_REACH);
  }
}

bool inputGamepadIsFireHeld(void) {
  if (path_a_active()) {
    return steam_input_is_action_pressed(SI_ACTION_FIRE);
  }
  return actionIsHeld(GP_ACT_FIRE);
}

bool inputGamepadIsMineHeld(void) {
  if (path_a_active()) {
    return steam_input_is_action_pressed(SI_ACTION_MINE);
  }
  return actionIsHeld(GP_ACT_MINE);
}

int inputGamepadGetGunsightChange(void) {
  /* Return live held state (-1/0/+1) on both paths so holding the
     bound buttons repeats at the caller's rate (INPUT_GUNSIGHT_WAIT_TIME),
     matching keyboard behavior.  Inc wins if both are held. */
  bool dec, inc;
  if (path_a_active()) {
    dec = steam_input_is_action_pressed(SI_ACTION_GUNSIGHT_DEC);
    inc = steam_input_is_action_pressed(SI_ACTION_GUNSIGHT_INC);
  } else if (s_activeGamepad) {
    dec = actionIsHeld(GP_ACT_GUNSIGHT_DEC);
    inc = actionIsHeld(GP_ACT_GUNSIGHT_INC);
  } else {
    return 0;
  }
  if (inc) return 1;
  if (dec) return -1;
  return 0;
}

bool inputGamepadGetScrollDirection(float *dx, float *dy) {
  if (dx) *dx = 0.0f;
  if (dy) *dy = 0.0f;

  float x, y;
  if (path_a_active()) {
    x = 0.0f;
    y = 0.0f;
    steam_input_get_analog_action(SI_ANALOG_MAP_SCROLL, &x, &y);
    /* Steam Input joystick_camera convention: +Y = up.  Path B (and
       the smooth-scroll consumers downstream) use +Y = down.  Negate
       to match.  Flip if scroll direction is inverted on first Deck
       test. */
    y = -y;
  } else {
    if (!s_activeGamepad) return false;
    x = (float)SDL_GetGamepadAxis(s_activeGamepad, SDL_GAMEPAD_AXIS_RIGHTX) * AXIS_NORM;
    y = (float)SDL_GetGamepadAxis(s_activeGamepad, SDL_GAMEPAD_AXIS_RIGHTY) * AXIS_NORM;
  }

  float dist = sqrtf(x * x + y * y);
  if (dist < SCROLL_DEADZONE) return false;

  /* Radial deadzone with outer-reach clamp to (-1..1). */
  float clampDist = dist > SCROLL_MAX_REACH ? SCROLL_MAX_REACH : dist;
  float reach = (clampDist - SCROLL_DEADZONE) / (SCROLL_MAX_REACH - SCROLL_DEADZONE);
  float scale = reach / dist;  /* re-scale unit vector by reach */

  if (dx) *dx = x * scale * g_gamepadScrollSensitivity;
  if (dy) *dy = y * scale * g_gamepadScrollSensitivity;
  return true;
}

int inputGamepadGetBuildSelectChange(void) {
  if (path_a_active()) {
    bool now_prev = steam_input_is_action_pressed(SI_ACTION_BUILD_PREV);
    bool now_next = steam_input_is_action_pressed(SI_ACTION_BUILD_NEXT);
    int delta = 0;
    if (now_prev && !s_path_a_last_build_prev) delta -= 1;
    if (now_next && !s_path_a_last_build_next) delta += 1;
    s_path_a_last_build_prev = now_prev;
    s_path_a_last_build_next = now_next;
    return delta;
  }

  int v = s_buildSelectChange;
  s_buildSelectChange = 0;
  return v;
}

bool inputGamepadIsViewToggleEdge(void) {
  if (path_a_active()) {
    bool now = steam_input_is_action_pressed(SI_ACTION_VIEW_CYCLE);
    bool edge = now && !s_path_a_last_view;
    s_path_a_last_view = now;
    return edge;
  }

  bool v = s_viewToggleEdge;
  s_viewToggleEdge = false;
  return v;
}

bool inputGamepadIsBuilderConfirmEdge(void) {
  if (path_a_active()) {
    bool now = steam_input_is_action_pressed(SI_ACTION_BUILD_CONFIRM);
    bool edge = now && !s_path_a_last_builder_confirm;
    s_path_a_last_builder_confirm = now;
    return edge;
  }

  bool v = s_builderConfirmEdge;
  s_builderConfirmEdge = false;
  return v;
}

bool inputGamepadIsPauseEdge(void) {
  if (path_a_active()) {
    bool now = steam_input_is_action_pressed(SI_ACTION_PAUSE);
    bool edge = now && !s_path_a_last_pause;
    s_path_a_last_pause = now;
    return edge;
  }

  bool v = s_pauseEdge;
  s_pauseEdge = false;
  return v;
}

bool inputGamepadIsQuickChatEdge(void) {
  if (path_a_active()) {
    bool now = steam_input_is_action_pressed(SI_ACTION_QUICK_CHAT);
    bool edge = now && !s_path_a_last_quickchat;
    s_path_a_last_quickchat = now;
    return edge;
  }

  bool v = s_quickChatEdge;
  s_quickChatEdge = false;
  return v;
}

bool inputGamepadIsBuildCursorToggleEdge(void) {
  if (path_a_active()) {
    bool now = steam_input_is_action_pressed(SI_ACTION_BUILD_CURSOR_TOGGLE);
    bool edge = now && !s_path_a_last_build_cursor_toggle;
    s_path_a_last_build_cursor_toggle = now;
    return edge;
  }

  bool v = s_buildCursorToggleEdge;
  s_buildCursorToggleEdge = false;
  return v;
}

bool inputGamepadIsStatusToggleEdge(void) {
  if (path_a_active()) {
    bool now = steam_input_is_action_pressed(SI_ACTION_STATUS_TOGGLE);
    bool edge = now && !s_path_a_last_status_toggle;
    s_path_a_last_status_toggle = now;
    return edge;
  }

  bool v = s_statusToggleEdge;
  s_statusToggleEdge = false;
  return v;
}

bool inputGamepadConsumeActiveDisconnect(void) {
  /* Path A latch is checked first so the disconnect surfaces even
     after path_a_active() has flipped to false this frame. */
  if (s_path_a_just_disconnected) {
    s_path_a_just_disconnected = false;
    return true;
  }

  bool v = s_activeDisconnectedEdge;
  s_activeDisconnectedEdge = false;
  return v;
}


void inputGamepadRumble(float strength, Uint32 durationMs) {
  if (strength < 0.0f) strength = 0.0f;
  if (strength > 1.0f) strength = 1.0f;
  Uint16 mag = (Uint16)(strength * 65535.0f);

  if (path_a_active()) {
    /* Steam Input vibration takes no duration — Steam decides based
       on the binding config's haptic settings.  durationMs ignored. */
    (void)durationMs;
    steam_input_trigger_vibration(mag, mag);
    return;
  }

  if (!s_activeGamepad) return;
  SDL_RumbleGamepad(s_activeGamepad, mag, mag, durationMs);
}
