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

static bool s_initialized = false;
static SteamJoinCallback s_join_callback = nullptr;

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
    }
    SteamAPI_ManualDispatch_FreeLastCallback(pipe);
  }
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
  uint32 len = 0;
  HAuthTicket ticket =
      SteamUser()->GetAuthSessionTicket(buf, buf_size, &len, nullptr);
  if (ticket == k_HAuthTicketInvalid) return false;
  if (out_len) *out_len = len;
  return true;
}

extern "C" void steam_increment_stat(const char *name, int amount) {
  if (!s_initialized) return;
  int32 current = 0;
  SteamUserStats()->GetStat(name, &current);
  SteamUserStats()->SetStat(name, current + amount);
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
