/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * steam_wrapper.cpp — real Steamworks implementation.
 * Compiled only when HAVE_STEAM is defined (SDK detected by CMake).
 *
 * Built as C++ because the Steamworks headers require it, but all
 * public functions are extern "C" so the rest of the C codebase
 * can call them via steam_wrapper.h.
 *
 * Uses manual callback dispatch so we can intercept specific callbacks
 * (e.g. GameRichPresenceJoinRequested_t) from plain C callers.
 */

#include <steam/steam_api.h>
#include <steam/steam_api_flat.h>
#include <cstdio>
#include <cstring>

extern "C" {
#include "steam_wrapper.h"
#include "../common/wb_log.h"
}

/* GameRichPresenceJoinRequested_t callback ID */
static const int k_iGameRichPresenceJoinRequested_id = k_iSteamFriendsCallbacks + 37;
/* GameOverlayActivated_t callback ID */
static const int k_iGameOverlayActivated_id = k_iSteamFriendsCallbacks + 31;

static bool s_initialized = false;
static SteamJoinCallback s_join_callback = nullptr;
static HAuthTicket s_authTicket = k_HAuthTicketInvalid;
/* Steam in-game overlay open/closed state, updated from the
   GameOverlayActivated_t callback during steam_run_callbacks(). */
static bool s_overlay_active = false;

extern "C" bool steam_init(void) {
  if (s_initialized) return true;
  if (!SteamAPI_Init()) {
    fprintf(stderr, "steam_init: SteamAPI_Init failed\n");
    return false;
  }
  /* ManualDispatch_Init must follow SteamAPI_Init — the dispatcher
     needs the library live before it can hook in.  Inverting these
     causes SteamAPI_Init() to fail on Steam Deck (silent breakage
     of Steam Input, achievements, and rich presence). */
  SteamAPI_ManualDispatch_Init();
  s_initialized = true;
  return true;
}

extern "C" void steam_shutdown(void) {
  if (!s_initialized) return;
  SteamAPI_Shutdown();
  s_initialized = false;
  s_join_callback = nullptr;
  s_authTicket = k_HAuthTicketInvalid;
  s_overlay_active = false;
}

extern "C" void steam_run_callbacks(void) {
  if (!s_initialized) return;

  HSteamPipe pipe = SteamAPI_GetHSteamPipe();
  SteamAPI_ManualDispatch_RunFrame(pipe);

  CallbackMsg_t msg;
  while (SteamAPI_ManualDispatch_GetNextCallback(pipe, &msg)) {
    if (msg.m_iCallback == k_iGameRichPresenceJoinRequested_id &&
        s_join_callback != nullptr) {
      auto *data = reinterpret_cast<GameRichPresenceJoinRequested_t *>(msg.m_pubParam);
      s_join_callback(data->m_rgchConnect);
    } else if (msg.m_iCallback == k_iGameOverlayActivated_id) {
      auto *data = reinterpret_cast<GameOverlayActivated_t *>(msg.m_pubParam);
      s_overlay_active = (data->m_bActive != 0);
    }
    SteamAPI_ManualDispatch_FreeLastCallback(pipe);
  }
}

extern "C" bool steam_overlay_is_active(void) {
  return s_initialized && s_overlay_active;
}

extern "C" void steam_set_rich_presence(const char *key, const char *value) {
  if (!s_initialized) return;
  SteamFriends()->SetRichPresence(key, value);
}

extern "C" void steam_clear_rich_presence(void) {
  if (!s_initialized) return;
  SteamFriends()->ClearRichPresence();
}

extern "C" bool steam_get_auth_ticket(uint8_t *buf, uint32_t buf_size,
                                      uint32_t *out_len) {
  if (!s_initialized) return false;
  if (s_authTicket != k_HAuthTicketInvalid) {
    SteamUser()->CancelAuthTicket(s_authTicket);
    s_authTicket = k_HAuthTicketInvalid;
  }
  uint32 len = 0;
  HAuthTicket ticket =
      SteamUser()->GetAuthSessionTicket(buf, buf_size, &len, nullptr);
  if (ticket == k_HAuthTicketInvalid) return false;
  s_authTicket = ticket;
  if (out_len) *out_len = len;
  return true;
}

extern "C" bool steam_get_persona_name(char *out, size_t outSize) {
  if (!s_initialized) return false;
  if (!out || outSize == 0) return false;
  const char *name = SteamFriends()->GetPersonaName();
  if (!name) return false;
  strncpy(out, name, outSize - 1);
  out[outSize - 1] = '\0';
  return true;
}

extern "C" void steam_cancel_auth_ticket(void) {
  if (!s_initialized || s_authTicket == k_HAuthTicketInvalid) return;
  SteamUser()->CancelAuthTicket(s_authTicket);
  s_authTicket = k_HAuthTicketInvalid;
}

/* Stat -> threshold achievement map.  Each lifetime stat unlocks tiered
   achievements at 1 / 10 / 100.  Steam never auto-unlocks from stat values,
   so steam_increment_stat() checks these on every increment and fires the
   matching achievement the moment the running total crosses a threshold.
   Unlocks are persisted by the steam_store_stats() that callers already
   issue after a batch of increments. */
namespace {
struct StatTier { const char *stat; int32 threshold; const char *ach; };
const StatTier kStatTiers[] = {
    {"STAT_BASES_CAPTURED_NEUTRAL",   1, "ACH_BASES_NEUTRAL_1"},
    {"STAT_BASES_CAPTURED_NEUTRAL",  10, "ACH_BASES_NEUTRAL_10"},
    {"STAT_BASES_CAPTURED_NEUTRAL", 100, "ACH_BASES_NEUTRAL_100"},
    {"STAT_BASES_CAPTURED_ENEMY",     1, "ACH_BASES_ENEMY_1"},
    {"STAT_BASES_CAPTURED_ENEMY",    10, "ACH_BASES_ENEMY_10"},
    {"STAT_BASES_CAPTURED_ENEMY",   100, "ACH_BASES_ENEMY_100"},
    {"STAT_PILLS_CAPTURED_NEUTRAL",   1, "ACH_PILLS_NEUTRAL_1"},
    {"STAT_PILLS_CAPTURED_NEUTRAL",  10, "ACH_PILLS_NEUTRAL_10"},
    {"STAT_PILLS_CAPTURED_NEUTRAL", 100, "ACH_PILLS_NEUTRAL_100"},
    {"STAT_PILLS_CAPTURED_ENEMY",     1, "ACH_PILLS_ENEMY_1"},
    {"STAT_PILLS_CAPTURED_ENEMY",    10, "ACH_PILLS_ENEMY_10"},
    {"STAT_PILLS_CAPTURED_ENEMY",   100, "ACH_PILLS_ENEMY_100"},
    {"STAT_TANK_KILLS",               1, "ACH_TANK_KILLS_1"},
    {"STAT_TANK_KILLS",              10, "ACH_TANK_KILLS_10"},
    {"STAT_TANK_KILLS",             100, "ACH_TANK_KILLS_100"},
    {"STAT_TOURN_WINS",               1, "ACH_TOURN_WINS_1"},
    {"STAT_TOURN_WINS",              10, "ACH_TOURN_WINS_10"},
    {"STAT_TOURN_WINS",             100, "ACH_TOURN_WINS_100"},
    {"STAT_TOURN_LOSSES",             1, "ACH_TOURN_LOSSES_1"},
    {"STAT_TOURN_LOSSES",            10, "ACH_TOURN_LOSSES_10"},
    {"STAT_TOURN_LOSSES",           100, "ACH_TOURN_LOSSES_100"},
    {"STAT_LGM_LOSSES",               1, "ACH_LGM_LOSSES_1"},
    {"STAT_LGM_LOSSES",              10, "ACH_LGM_LOSSES_10"},
    {"STAT_LGM_LOSSES",             100, "ACH_LGM_LOSSES_100"},
    {"STAT_LGM_KILLS",                1, "ACH_LGM_KILLS_1"},
    {"STAT_LGM_KILLS",               10, "ACH_LGM_KILLS_10"},
    {"STAT_LGM_KILLS",              100, "ACH_LGM_KILLS_100"},
};
}  // namespace

extern "C" void steam_increment_stat(const char *name, int amount) {
  if (!s_initialized) return;
  int32 current = 0;
  SteamUserStats()->GetStat(name, &current);
  int32 updated = current + amount;
  SteamUserStats()->SetStat(name, updated);

  /* Fire any tiered achievement whose threshold this increment just crossed. */
  for (const StatTier &t : kStatTiers) {
    if (current < t.threshold && updated >= t.threshold &&
        std::strcmp(t.stat, name) == 0) {
      SteamUserStats()->SetAchievement(t.ach);
    }
  }
}

extern "C" void steam_set_achievement(const char *id) {
  if (!s_initialized) return;
  SteamUserStats()->SetAchievement(id);
}

extern "C" void steam_store_stats(void) {
  if (!s_initialized) return;
  SteamUserStats()->StoreStats();
}

extern "C" void steam_set_join_callback(SteamJoinCallback cb) {
  s_join_callback = cb;
}

extern "C" bool steam_is_steam_deck(void) {
  if (!s_initialized) return false;
  ISteamUtils *utils = SteamUtils();
  return utils && utils->IsSteamRunningOnSteamDeck();
}

extern "C" bool steam_is_big_picture(void) {
  if (!s_initialized) return false;
  ISteamUtils *utils = SteamUtils();
  return utils && utils->IsSteamInBigPictureMode();
}

extern "C" bool steam_show_floating_keyboard(int x, int y, int w, int h) {
  if (!s_initialized) return false;
  ISteamUtils *utils = SteamUtils();
  if (!utils) return false;
  return utils->ShowFloatingGamepadTextInput(
      k_EFloatingGamepadTextInputModeModeSingleLine, x, y, w, h);
}

extern "C" void steam_dismiss_floating_keyboard(void) {
  if (!s_initialized) return;
  ISteamUtils *utils = SteamUtils();
  if (utils) utils->DismissFloatingGamepadTextInput();
}

/* -------- Steam Input --------
 * Path A of the two-path input model.  All lookups go through lazy
 * caches keyed by the action / set name string literal pointers from
 * steam_input_actions.h, so each name hits Steam at most once. */

static bool                     s_input_initialized   = false;
static InputHandle_t            s_active_controller   = 0; /* 0 = none */
static InputActionSetHandle_t   s_active_set_handle   = 0;

namespace {
struct DigitalCache   { const char *name; InputDigitalActionHandle_t handle; };
struct AnalogCache    { const char *name; InputAnalogActionHandle_t  handle; };
struct ActionSetCache { const char *name; InputActionSetHandle_t     handle; };
}

static DigitalCache   s_digital_cache[32]  = {};
static AnalogCache    s_analog_cache[16]   = {};
static ActionSetCache s_actionset_cache[8] = {};
static int s_digital_count   = 0;
static int s_analog_count    = 0;
static int s_actionset_count = 0;

static InputDigitalActionHandle_t cache_digital(ISteamInput *input,
                                                const char *name) {
  if (!input || !name) return 0;
  for (int i = 0; i < s_digital_count; ++i) {
    if (s_digital_cache[i].name == name ||
        (s_digital_cache[i].name && std::strcmp(s_digital_cache[i].name, name) == 0)) {
      return s_digital_cache[i].handle;
    }
  }
  if (s_digital_count >= (int)(sizeof(s_digital_cache) / sizeof(s_digital_cache[0]))) {
    fprintf(stderr, "steam_input: digital action cache full, dropping '%s'\n", name);
    return 0;
  }
  InputDigitalActionHandle_t h =
      SteamAPI_ISteamInput_GetDigitalActionHandle(input, name);
  s_digital_cache[s_digital_count].name   = name;
  s_digital_cache[s_digital_count].handle = h;
  ++s_digital_count;
  return h;
}

static InputAnalogActionHandle_t cache_analog(ISteamInput *input,
                                              const char *name) {
  if (!input || !name) return 0;
  for (int i = 0; i < s_analog_count; ++i) {
    if (s_analog_cache[i].name == name ||
        (s_analog_cache[i].name && std::strcmp(s_analog_cache[i].name, name) == 0)) {
      return s_analog_cache[i].handle;
    }
  }
  if (s_analog_count >= (int)(sizeof(s_analog_cache) / sizeof(s_analog_cache[0]))) {
    fprintf(stderr, "steam_input: analog action cache full, dropping '%s'\n", name);
    return 0;
  }
  InputAnalogActionHandle_t h =
      SteamAPI_ISteamInput_GetAnalogActionHandle(input, name);
  s_analog_cache[s_analog_count].name   = name;
  s_analog_cache[s_analog_count].handle = h;
  ++s_analog_count;
  return h;
}

static InputActionSetHandle_t cache_actionset(ISteamInput *input,
                                              const char *name) {
  if (!input || !name) return 0;
  for (int i = 0; i < s_actionset_count; ++i) {
    if (s_actionset_cache[i].name == name ||
        (s_actionset_cache[i].name && std::strcmp(s_actionset_cache[i].name, name) == 0)) {
      return s_actionset_cache[i].handle;
    }
  }
  if (s_actionset_count >= (int)(sizeof(s_actionset_cache) / sizeof(s_actionset_cache[0]))) {
    fprintf(stderr, "steam_input: action-set cache full, dropping '%s'\n", name);
    return 0;
  }
  InputActionSetHandle_t h =
      SteamAPI_ISteamInput_GetActionSetHandle(input, name);
  s_actionset_cache[s_actionset_count].name   = name;
  s_actionset_cache[s_actionset_count].handle = h;
  ++s_actionset_count;
  return h;
}

extern "C" bool steam_input_init(void) {
  if (s_input_initialized) return true;
  if (!s_initialized) return false;
  ISteamInput *input = SteamAPI_SteamInput();
  if (!input) return false;
  if (!SteamAPI_ISteamInput_Init(input, false)) {
    WB_LOG_WARN(WB_LOG_CAT_GUI, "steam_input_init: ISteamInput_Init failed");
    return false;
  }
  s_input_initialized = true;
  WB_LOG_INFO(WB_LOG_CAT_GUI, "steam_input_init: ISteamInput active");
  return true;
}

extern "C" void steam_input_shutdown(void) {
  if (!s_input_initialized) return;
  ISteamInput *input = SteamAPI_SteamInput();
  if (input) SteamAPI_ISteamInput_Shutdown(input);
  s_input_initialized   = false;
  s_active_controller   = 0;
  s_active_set_handle   = 0;
  s_digital_count       = 0;
  s_analog_count        = 0;
  s_actionset_count     = 0;
}

extern "C" void steam_input_run_frame(void) {
  if (!s_input_initialized) return;
  ISteamInput *input = SteamAPI_SteamInput();
  if (!input) return;

  SteamAPI_ISteamInput_RunFrame(input, false);

  InputHandle_t handles[STEAM_INPUT_MAX_COUNT] = {};
  int n = SteamAPI_ISteamInput_GetConnectedControllers(input, handles);
  InputHandle_t prev = s_active_controller;
  s_active_controller = (n > 0) ? handles[0] : 0;

  /* Diagnostic: log on every connect/disconnect transition.  Invaluable
     when debugging "Steam launched but no input" — confirms whether
     Steam Input sees a controller at all, and its type, before any
     binding works.  Goes to the log file (stderr is invisible under a
     Steam GUI launch). */
  if (s_active_controller != 0 && prev == 0) {
    ESteamInputType type =
        SteamAPI_ISteamInput_GetInputTypeForHandle(input, s_active_controller);
    WB_LOG_INFO(WB_LOG_CAT_GUI,
                "steam_input: controller connected, count=%d type=%d set=%s",
                n, (int)type,
                s_active_set_handle != 0 ? "active" : "none");
  } else if (s_active_controller == 0 && prev != 0) {
    WB_LOG_INFO(WB_LOG_CAT_GUI, "steam_input: controller disconnected");
  }

  if (s_active_set_handle != 0 &&
      s_active_controller != 0 &&
      s_active_controller != prev) {
    SteamAPI_ISteamInput_ActivateActionSet(input, s_active_controller,
                                           s_active_set_handle);
  }
}

extern "C" void steam_input_activate_action_set(const char *set_name) {
  if (!s_input_initialized) return;
  ISteamInput *input = SteamAPI_SteamInput();
  if (!input) return;
  InputActionSetHandle_t h = cache_actionset(input, set_name);
  s_active_set_handle = h;
  if (s_active_controller != 0 && h != 0) {
    SteamAPI_ISteamInput_ActivateActionSet(input, s_active_controller, h);
  }
}

extern "C" bool steam_input_is_action_pressed(const char *action_name) {
  if (!s_input_initialized || s_active_controller == 0) return false;
  ISteamInput *input = SteamAPI_SteamInput();
  if (!input) return false;
  InputDigitalActionHandle_t h = cache_digital(input, action_name);
  if (h == 0) return false;
  InputDigitalActionData_t data =
      SteamAPI_ISteamInput_GetDigitalActionData(input, s_active_controller, h);
  return data.bState && data.bActive;
}

extern "C" void steam_input_get_analog_action(const char *action_name,
                                              float *x, float *y) {
  if (x) *x = 0.0f;
  if (y) *y = 0.0f;
  if (!s_input_initialized || s_active_controller == 0) return;
  ISteamInput *input = SteamAPI_SteamInput();
  if (!input) return;
  InputAnalogActionHandle_t h = cache_analog(input, action_name);
  if (h == 0) return;
  InputAnalogActionData_t data =
      SteamAPI_ISteamInput_GetAnalogActionData(input, s_active_controller, h);
  if (!data.bActive) return;
  if (x) *x = data.x;
  if (y) *y = data.y;
}

extern "C" const char *steam_input_get_glyph_path(const char *action_name) {
  if (!s_input_initialized || s_active_controller == 0) return nullptr;
  ISteamInput *input = SteamAPI_SteamInput();
  if (!input) return nullptr;
  InputDigitalActionHandle_t h = cache_digital(input, action_name);
  if (h == 0 || s_active_set_handle == 0) return nullptr;

  EInputActionOrigin origins[STEAM_INPUT_MAX_ORIGINS] = {};
  int n = SteamAPI_ISteamInput_GetDigitalActionOrigins(
      input, s_active_controller, s_active_set_handle, h, origins);
  if (n <= 0) return nullptr;

  return SteamAPI_ISteamInput_GetGlyphPNGForActionOrigin(
      input, origins[0], k_ESteamInputGlyphSize_Medium, 0);
}

extern "C" bool steam_input_has_active_controller(void) {
  return s_input_initialized && s_active_controller != 0;
}

extern "C" void steam_input_trigger_vibration(uint16_t left_speed,
                                              uint16_t right_speed) {
  if (!s_input_initialized || s_active_controller == 0) return;
  ISteamInput *input = SteamAPI_SteamInput();
  if (!input) return;
  SteamAPI_ISteamInput_TriggerVibration(input, s_active_controller,
                                        left_speed, right_speed);
}
