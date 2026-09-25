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
/* Steam Input device callback IDs (k_iSteamControllerCallbacks = 2800).
   Connected/Disconnected track real physical controllers (opt-in via
   EnableDeviceCallbacks); ConfigurationLoaded fires when a device's config
   is ready and tells us to (re)activate the action set against it. */
static const int k_iSteamInputDeviceConnected_id     = k_iSteamControllerCallbacks + 1;
static const int k_iSteamInputDeviceDisconnected_id  = k_iSteamControllerCallbacks + 2;
static const int k_iSteamInputConfigurationLoaded_id = k_iSteamControllerCallbacks + 3;
/* Workshop (UGC) callback IDs (k_iSteamUGCCallbacks = 3400). */
static const int k_iItemInstalled_id      = k_iSteamUGCCallbacks + 5;
static const int k_iDownloadItemResult_id = k_iSteamUGCCallbacks + 6;
/* SteamAPICallCompleted_t callback ID (k_iSteamUtilsCallbacks = 700).  Unlike
   the ids above this does not carry a result: it says that a SteamAPICall_t
   handle has finished, and the payload is read back separately with
   SteamAPI_ManualDispatch_GetAPICallResult. */
static const int k_iSteamAPICallCompleted_id = k_iSteamUtilsCallbacks + 3;

static bool s_initialized = false;
static SteamJoinCallback s_join_callback = nullptr;
static HAuthTicket s_authTicket = k_HAuthTicketInvalid;
/* Steam in-game overlay open/closed state, updated from the
   GameOverlayActivated_t callback during steam_run_callbacks(). */
static bool s_overlay_active = false;

/* Real physical-controller tracking via the device hot-plug callbacks
   (EnableDeviceCallbacks), maintained in steam_run_callbacks().  These fire
   only for actual hardware — the always-present keyboard/mouse virtual
   controller that pins GetConnectedControllers at >=1 does NOT fire them —
   so this is the trustworthy "is a real controller connected" signal.  The
   count is balanced one-for-one by SteamInputDeviceConnected/Disconnected. */
static int  s_real_controller_count   = 0;
static bool s_real_disconnect_pending = false;
/* Handle of the real controller from the most recent DeviceConnected
   callback.  Used to point s_active_controller at the *physical* pad rather
   than handles[0], which can be the keyboard/mouse virtual controller when
   it registered first (no pad at launch) — that left the real pad's input
   unread in the menus. */
static InputHandle_t s_real_controller_handle = 0;
/* Set when a real device connects / its config loads.  The active handle is
   shared between the keyboard/mouse virtual controller and the physical pad,
   so it doesn't change when the pad takes over — the normal "handle changed"
   re-activation never fires, leaving the action set bound to the phantom and
   the pad's input dead in the menus.  Force a re-activation to re-bind it. */
static bool s_force_actionset_reactivate = false;
/* Set when a Workshop item finishes installing or downloading; consumed by
   steam_workshop_consume_installed_event() so the skin picker can rescan. */
static bool s_workshop_installed_event = false;

/* Workshop publish.  CreateItem and SubmitItemUpdate hand back a
   SteamAPICall_t whose result lands on a later frame, so a publish is spread
   over several frames: steam_workshop_publish_begin() starts the first call,
   the call-result arm in steam_run_callbacks() advances the state as each
   result arrives, and callers watch it with steam_workshop_publish_poll(). */
namespace {
enum PublishState {
  kPublishIdle = 0,   /* nothing started, or reset by steam_shutdown */
  kPublishCreating,   /* CreateItem in flight (new items only) */
  kPublishUpdating,   /* SubmitItemUpdate in flight */
  kPublishDone,       /* finished, s_pubFileId holds the item */
  kPublishFailed      /* Steam refused a call or returned an error */
};
}
static PublishState s_pubState = kPublishIdle;
/* The call result being waited on.  The dispatch arm compares every completed
   handle against this and ignores the ones that don't match, so the wrapper
   never consumes a call result it didn't start. */
static SteamAPICall_t s_pubCall = k_uAPICallInvalid;
/* Live between StartItemUpdate and the SubmitItemUpdate result; read by
   steam_workshop_publish_progress() for the byte counts. */
static UGCUpdateHandle_t s_pubUpdate = k_UGCUpdateHandleInvalid;
static PublishedFileId_t s_pubFileId = 0;
/* Steam reporting that the author has yet to accept the Workshop legal
   agreement.  The item stays hidden until they do, so callers send them to
   the item page. */
static bool s_pubNeedsLegal = false;
/* The item's fields, copied rather than aliased: for a new item CreateItem
   completes frames later and only then are SetItemTitle and friends called,
   by which point the caller's strings are long gone.  The two text sizes are
   the SDK's published-document limits. */
static char s_pubTitle[k_cchPublishedDocumentTitleMax];
static char s_pubDesc[k_cchPublishedDocumentDescriptionMax];
static char s_pubPreview[1024];
static char s_pubFolder[1024];
static char s_pubTag[256];

/* StartItemUpdate through SubmitItemUpdate for one item, using the fields
   copied above.  Defined with the other publish functions at the foot of the
   file; the call-result dispatch below runs it when CreateItem completes. */
static bool steam_publish_submit_update(PublishedFileId_t id);

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
  s_workshop_installed_event = false;
  s_pubState = kPublishIdle;
  s_pubCall = k_uAPICallInvalid;
  s_pubUpdate = k_UGCUpdateHandleInvalid;
  s_pubFileId = 0;
  s_pubNeedsLegal = false;
  s_pubTitle[0] = '\0';
  s_pubDesc[0] = '\0';
  s_pubPreview[0] = '\0';
  s_pubFolder[0] = '\0';
  s_pubTag[0] = '\0';
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
    } else if (msg.m_iCallback == k_iSteamInputDeviceConnected_id) {
      auto *d = reinterpret_cast<SteamInputDeviceConnected_t *>(msg.m_pubParam);
      s_real_controller_count++;
      s_real_controller_handle = d->m_ulConnectedDeviceHandle;
      s_force_actionset_reactivate = true;
      ESteamInputType t = SteamAPI_ISteamInput_GetInputTypeForHandle(
          SteamAPI_SteamInput(), d->m_ulConnectedDeviceHandle);
      WB_LOG_INFO(WB_LOG_CAT_GUI,
                  "steam_input: real controller connected handle=%llu type=%d (count=%d)",
                  (unsigned long long)d->m_ulConnectedDeviceHandle, (int)t,
                  s_real_controller_count);
    } else if (msg.m_iCallback == k_iSteamInputDeviceDisconnected_id) {
      auto *d = reinterpret_cast<SteamInputDeviceDisconnected_t *>(msg.m_pubParam);
      if (s_real_controller_count > 0) s_real_controller_count--;
      if (d->m_ulDisconnectedDeviceHandle == s_real_controller_handle)
        s_real_controller_handle = 0;
      s_real_disconnect_pending = true;
      WB_LOG_INFO(WB_LOG_CAT_GUI,
                  "steam_input: real controller disconnected handle=%llu (count=%d)",
                  (unsigned long long)d->m_ulDisconnectedDeviceHandle,
                  s_real_controller_count);
    } else if (msg.m_iCallback == k_iSteamInputConfigurationLoaded_id) {
      /* A real device's config just loaded.  The action handles resolve and
         the action set must be (re)activated against it — see
         steam_input_run_frame.  (Fires when a pad takes over the virtual
         controller's handle, where the handle alone doesn't change.) */
      s_force_actionset_reactivate = true;
    } else if (msg.m_iCallback == k_iItemInstalled_id) {
      auto *d = reinterpret_cast<ItemInstalled_t *>(msg.m_pubParam);
      s_workshop_installed_event = true;
      WB_LOG_INFO(WB_LOG_CAT_GUI, "steam_workshop: item installed id=%llu",
                  (unsigned long long)d->m_nPublishedFileId);
    } else if (msg.m_iCallback == k_iDownloadItemResult_id) {
      auto *d = reinterpret_cast<DownloadItemResult_t *>(msg.m_pubParam);
      s_workshop_installed_event = true;
      WB_LOG_INFO(WB_LOG_CAT_GUI,
                  "steam_workshop: item download finished id=%llu result=%d",
                  (unsigned long long)d->m_nPublishedFileId, (int)d->m_eResult);
    } else if (msg.m_iCallback == k_iSteamAPICallCompleted_id) {
      /* A SteamAPICall_t finished.  The message says only which handle it was
         and how big its payload is — the payload itself has to be fetched with
         GetAPICallResult.  Only the publish call is ours to take: anything
         else belongs to code that started its own call and is waiting for it. */
      auto *d = reinterpret_cast<SteamAPICallCompleted_t *>(msg.m_pubParam);
      if (s_pubCall != k_uAPICallInvalid && d->m_hAsyncCall == s_pubCall) {
        unsigned char *buf = new unsigned char[d->m_cubParam];
        bool failed = false;
        bool got = SteamAPI_ManualDispatch_GetAPICallResult(
            pipe, d->m_hAsyncCall, buf, (int)d->m_cubParam, d->m_iCallback,
            &failed);
        /* Cleared before dispatching: the create step starts the next call
           and stores its handle here. */
        s_pubCall = k_uAPICallInvalid;
        if (!got || failed) {
          s_pubState = kPublishFailed;
          WB_LOG_ERROR(WB_LOG_CAT_GUI,
                       "steam_workshop: publish call result unavailable "
                       "(callback=%d)",
                       d->m_iCallback);
        } else if (s_pubState == kPublishCreating) {
          auto *r = reinterpret_cast<CreateItemResult_t *>(buf);
          if (r->m_eResult != k_EResultOK) {
            s_pubState = kPublishFailed;
            WB_LOG_ERROR(WB_LOG_CAT_GUI,
                         "steam_workshop: CreateItem failed result=%d",
                         (int)r->m_eResult);
          } else {
            s_pubFileId = r->m_nPublishedFileId;
            if (r->m_bUserNeedsToAcceptWorkshopLegalAgreement)
              s_pubNeedsLegal = true;
            WB_LOG_INFO(WB_LOG_CAT_GUI, "steam_workshop: item created id=%llu",
                        (unsigned long long)s_pubFileId);
            if (steam_publish_submit_update(s_pubFileId)) {
              s_pubState = kPublishUpdating;
            } else {
              s_pubState = kPublishFailed;
              WB_LOG_ERROR(WB_LOG_CAT_GUI,
                           "steam_workshop: could not start update for id=%llu",
                           (unsigned long long)s_pubFileId);
            }
          }
        } else if (s_pubState == kPublishUpdating) {
          auto *r = reinterpret_cast<SubmitItemUpdateResult_t *>(buf);
          if (r->m_eResult != k_EResultOK) {
            s_pubState = kPublishFailed;
            WB_LOG_ERROR(WB_LOG_CAT_GUI,
                         "steam_workshop: SubmitItemUpdate failed id=%llu "
                         "result=%d",
                         (unsigned long long)s_pubFileId, (int)r->m_eResult);
          } else {
            if (r->m_bUserNeedsToAcceptWorkshopLegalAgreement)
              s_pubNeedsLegal = true;
            s_pubState = kPublishDone;
            WB_LOG_INFO(WB_LOG_CAT_GUI,
                        "steam_workshop: item published id=%llu",
                        (unsigned long long)s_pubFileId);
          }
        }
        delete[] buf;
      }
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

extern "C" uint64_t steam_get_steam_id(void) {
  if (!s_initialized) return 0;
  ISteamUser *u = SteamUser();
  if (!u) return 0;
  return u->GetSteamID().ConvertToUint64();
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

extern "C" bool steam_owns_dlc(uint32_t dlc_app_id) {
  if (!s_initialized) return false;
  ISteamApps *apps = SteamApps();
  return apps && apps->BIsSubscribedApp((AppId_t)dlc_app_id);
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
/* Name of the desired action set (static literal from steam_input_actions.h).
   Kept so run_frame can re-resolve s_active_set_handle once the config loads —
   GetActionSetHandle returns 0 until a controller is present, and the nav
   layer only requests a set on change, so it would otherwise never retry. */
static const char *             s_active_set_name     = nullptr;

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
  /* Don't cache an unresolved (0) handle.  Steam returns 0 until the
     in-game-actions config is loaded — which only happens once a real
     controller is present — so caching the 0 would leave the action dead
     forever even after the config loads.  Retry on the next call. */
  if (h == 0) return 0;
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
  if (h == 0) return 0;   /* don't cache an unresolved handle — see cache_digital */
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
  if (h == 0) return 0;   /* don't cache an unresolved handle — see cache_digital */
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
  /* Opt in to SteamInputDeviceConnected_t / Disconnected_t — the only signal
     for *real* physical controller presence (GetConnectedControllers is pinned
     at >=1 by the always-present keyboard/mouse virtual controller). */
  SteamAPI_ISteamInput_EnableDeviceCallbacks(input);
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
  s_active_set_name     = nullptr;
  s_digital_count       = 0;
  s_analog_count        = 0;
  s_actionset_count     = 0;
  s_real_controller_count   = 0;
  s_real_disconnect_pending = false;
  s_real_controller_handle  = 0;
  s_force_actionset_reactivate = false;
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

  /* Re-resolve the action-set handle if it wasn't available when first
     requested (GetActionSetHandle returns 0 until the config loads, which
     needs a controller present).  Force a re-activation once it resolves. */
  if (s_active_set_handle == 0 && s_active_set_name) {
    s_active_set_handle = cache_actionset(input, s_active_set_name);
    if (s_active_set_handle != 0) s_force_actionset_reactivate = true;
  }

  /* Re-activate the action set when the active controller changes OR when a
     real device just connected / loaded its config — the latter is required
     because the pad shares the virtual controller's handle, so the handle
     alone doesn't change when the pad takes over. */
  if (s_active_set_handle != 0 &&
      s_active_controller != 0 &&
      (s_active_controller != prev || s_force_actionset_reactivate)) {
    SteamAPI_ISteamInput_ActivateActionSet(input, s_active_controller,
                                           s_active_set_handle);
    s_force_actionset_reactivate = false;
  }
}

extern "C" void steam_input_activate_action_set(const char *set_name) {
  if (!s_input_initialized) return;
  ISteamInput *input = SteamAPI_SteamInput();
  if (!input) return;
  s_active_set_name = set_name;   /* remembered for re-resolution in run_frame */
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

/* Real physical controller present, per the device hot-plug callbacks —
   immune to the keyboard/mouse virtual controller (the phantom). */
extern "C" bool steam_input_real_controller_connected(void) {
  return s_real_controller_count > 0;
}

/* Consume the "a real controller just disconnected" edge (one-shot). */
extern "C" bool steam_input_consume_real_disconnect(void) {
  bool v = s_real_disconnect_pending;
  s_real_disconnect_pending = false;
  return v;
}

extern "C" void steam_input_trigger_vibration(uint16_t left_speed,
                                              uint16_t right_speed) {
  if (!s_input_initialized || s_active_controller == 0) return;
  ISteamInput *input = SteamAPI_SteamInput();
  if (!input) return;
  SteamAPI_ISteamInput_TriggerVibration(input, s_active_controller,
                                        left_speed, right_speed);
}

/* -------- Steam Workshop (UGC) --------
 * Read side only: enumerate what the local user is subscribed to and hand
 * the install folders to the skin scan. */

extern "C" bool steam_workshop_available(void) {
  return s_initialized && SteamUGC() != nullptr;
}

extern "C" int steam_workshop_subscribed_count(void) {
  if (!steam_workshop_available()) return 0;
  return (int)SteamUGC()->GetNumSubscribedItems();
}

extern "C" bool steam_workshop_item(int idx, uint64_t *id, char *folder,
                                    size_t folderSize) {
  if (id) *id = 0;
  if (folder && folderSize) folder[0] = '\0';
  if (!steam_workshop_available() || idx < 0) return false;

  ISteamUGC *ugc = SteamUGC();
  uint32 n = ugc->GetNumSubscribedItems();
  if ((uint32)idx >= n) return false;

  /* The subscribed list is re-fetched on every call rather than cached.
     n is a handful of items, and a cache would only pay off if callers
     walked idx in ascending order — a contract nothing in the signature
     states and nothing would enforce. */
  PublishedFileId_t *ids = new PublishedFileId_t[n];
  uint32 got = ugc->GetSubscribedItems(ids, n);
  if ((uint32)idx >= got) {
    delete[] ids;
    return false;
  }

  PublishedFileId_t fid = ids[idx];
  delete[] ids;
  if (id) *id = (uint64_t)fid;

  /* Installed but locally disabled is the user hiding the item, so treat it
     as not usable even though the files are on disk. */
  uint32 state = ugc->GetItemState(fid);
  if (!(state & k_EItemStateInstalled) ||
      (state & k_EItemStateDisabledLocally)) {
    return false;
  }

  if (!folder || folderSize == 0) return false;

  uint64 sizeOnDisk = 0;
  uint32 timeStamp = 0;
  char buf[1024];
  buf[0] = '\0';
  if (!ugc->GetItemInstallInfo(fid, &sizeOnDisk, buf, (uint32)sizeof(buf),
                               &timeStamp)) {
    return false;
  }
  if (buf[0] == '\0') return false;
  strncpy(folder, buf, folderSize - 1);
  folder[folderSize - 1] = '\0';
  return true;
}

extern "C" void steam_workshop_request_download(uint64_t id) {
  if (!steam_workshop_available() || id == 0) return;
  SteamUGC()->DownloadItem((PublishedFileId_t)id, true);
}

extern "C" bool steam_workshop_consume_installed_event(void) {
  bool v = s_workshop_installed_event;
  s_workshop_installed_event = false;
  return v;
}

extern "C" void steam_workshop_open_browse_page(void) {
  if (!steam_workshop_available()) return;
  ISteamUtils *utils = SteamUtils();
  ISteamFriends *friends = SteamFriends();
  if (!utils || !friends) return;
  /* App id read at runtime: a dev build can be running against a different
     one than ships. */
  char url[128];
  snprintf(url, sizeof(url), "https://steamcommunity.com/app/%u/workshop/",
           (unsigned)utils->GetAppID());
  friends->ActivateGameOverlayToWebPage(url);
}

/* -------- Steam Workshop publish --------
 * A state machine over the call-result path in steam_run_callbacks(): begin
 * starts a call, each result advances the state, poll reports it. */

/* Copy one publish field into its fixed buffer, NUL-terminated.  A NULL
 * source means "not set" and leaves an empty string. */
static void steam_publish_copy(char *dst, size_t dstSize, const char *src) {
  if (!dst || dstSize == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = '\0';
}

static bool steam_publish_submit_update(PublishedFileId_t id) {
  ISteamUGC *ugc = SteamUGC();
  ISteamUtils *utils = SteamUtils();
  if (!ugc || !utils) return false;

  UGCUpdateHandle_t h = ugc->StartItemUpdate(utils->GetAppID(), id);
  if (h == k_UGCUpdateHandleInvalid) return false;

  ugc->SetItemTitle(h, s_pubTitle);
  ugc->SetItemDescription(h, s_pubDesc);
  ugc->SetItemContent(h, s_pubFolder);
  /* An empty preview path is "no preview image"; handing that to
     SetItemPreview would fail the whole update. */
  if (s_pubPreview[0] != '\0') ugc->SetItemPreview(h, s_pubPreview);

  /* The item's one tag — what the app's Workshop page filters on.  An empty
     tag leaves the item's tags alone. */
  if (s_pubTag[0] != '\0') {
    const char *tags[] = {s_pubTag};
    SteamParamStringArray_t arr;
    arr.m_ppStrings = tags;
    arr.m_nNumStrings = 1;
    ugc->SetItemTags(h, &arr);
  }

  /* Visibility is deliberately left alone.  A new item starts private, and
     the author makes it public from the item page — where this flow sends
     them once the upload finishes. */

  s_pubUpdate = h;
  s_pubCall = ugc->SubmitItemUpdate(h, "Published from WinBolo");
  return s_pubCall != k_uAPICallInvalid;
}

extern "C" bool steam_workshop_publish_begin(const char *contentFolder,
                                             const char *title,
                                             const char *description,
                                             const char *previewPng,
                                             uint64_t existingId,
                                             const char *tag) {
  if (!steam_workshop_available()) return false;
  /* One publish at a time — a second begin would overwrite the handle the
     dispatch arm is matching against and strand the first. */
  if (s_pubState == kPublishCreating || s_pubState == kPublishUpdating)
    return false;
  if (!contentFolder || contentFolder[0] == '\0') return false;

  ISteamUGC *ugc = SteamUGC();
  ISteamUtils *utils = SteamUtils();
  if (!ugc || !utils) return false;

  steam_publish_copy(s_pubFolder, sizeof(s_pubFolder), contentFolder);
  steam_publish_copy(s_pubTitle, sizeof(s_pubTitle), title);
  steam_publish_copy(s_pubDesc, sizeof(s_pubDesc), description);
  steam_publish_copy(s_pubPreview, sizeof(s_pubPreview), previewPng);
  steam_publish_copy(s_pubTag, sizeof(s_pubTag), tag);
  s_pubNeedsLegal = false;
  s_pubFileId = 0;
  s_pubUpdate = k_UGCUpdateHandleInvalid;
  s_pubCall = k_uAPICallInvalid;

  if (existingId == 0) {
    s_pubCall = ugc->CreateItem(utils->GetAppID(), k_EWorkshopFileTypeCommunity);
    if (s_pubCall == k_uAPICallInvalid) {
      s_pubState = kPublishFailed;
      WB_LOG_ERROR(WB_LOG_CAT_GUI, "steam_workshop: CreateItem did not start");
      return false;
    }
    s_pubState = kPublishCreating;
  } else {
    /* Re-publishing a known item: no CreateItem, straight to the upload. */
    s_pubFileId = (PublishedFileId_t)existingId;
    if (!steam_publish_submit_update(s_pubFileId)) {
      s_pubState = kPublishFailed;
      WB_LOG_ERROR(WB_LOG_CAT_GUI,
                   "steam_workshop: could not start update for id=%llu",
                   (unsigned long long)s_pubFileId);
      return false;
    }
    s_pubState = kPublishUpdating;
  }
  return true;
}

extern "C" int steam_workshop_publish_poll(uint64_t *outId,
                                           bool *needsLegalAgreement) {
  if (outId) *outId = 0;
  if (needsLegalAgreement) *needsLegalAgreement = false;
  switch (s_pubState) {
    case kPublishDone:
      if (outId) *outId = (uint64_t)s_pubFileId;
      if (needsLegalAgreement) *needsLegalAgreement = s_pubNeedsLegal;
      return 1;
    case kPublishCreating:
    case kPublishUpdating:
      return 0;
    default:
      /* Idle counts as failure, not as progress: a caller polling without a
         publish in flight would otherwise wait on one that never comes. */
      return -1;
  }
}

extern "C" int steam_workshop_publish_progress(uint64_t *bytesDone,
                                               uint64_t *bytesTotal) {
  if (bytesDone) *bytesDone = 0;
  if (bytesTotal) *bytesTotal = 0;
  if (!steam_workshop_available()) return 0;
  if (s_pubState != kPublishUpdating || s_pubUpdate == k_UGCUpdateHandleInvalid)
    return 0;

  uint64 done = 0;
  uint64 total = 0;
  EItemUpdateStatus status =
      SteamUGC()->GetItemUpdateProgress(s_pubUpdate, &done, &total);
  if (bytesDone) *bytesDone = (uint64_t)done;
  if (bytesTotal) *bytesTotal = (uint64_t)total;
  return (int)status;
}

extern "C" void steam_workshop_open_item_page(uint64_t id) {
  if (!steam_workshop_available() || id == 0) return;
  ISteamFriends *friends = SteamFriends();
  if (!friends) return;
  char url[128];
  snprintf(url, sizeof(url),
           "https://steamcommunity.com/sharedfiles/filedetails/?id=%llu",
           (unsigned long long)id);
  friends->ActivateGameOverlayToWebPage(url);
}
