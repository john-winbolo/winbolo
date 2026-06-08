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
}

/* GameRichPresenceJoinRequested_t callback ID */
static const int k_iGameRichPresenceJoinRequested_id = k_iSteamFriendsCallbacks + 37;

static bool s_initialized = false;
static SteamJoinCallback s_join_callback = nullptr;
static HAuthTicket s_authTicket = k_HAuthTicketInvalid;

extern "C" bool steam_init(void) {
  if (s_initialized) return true;
  SteamAPI_ManualDispatch_Init();
  if (!SteamAPI_Init()) {
    fprintf(stderr, "steam_init: SteamAPI_Init failed\n");
    return false;
  }
  s_initialized = true;
  return true;
}

extern "C" void steam_shutdown(void) {
  if (!s_initialized) return;
  SteamAPI_Shutdown();
  s_initialized = false;
  s_join_callback = nullptr;
  s_authTicket = k_HAuthTicketInvalid;
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

extern "C" void steam_cancel_auth_ticket(void) {
  if (!s_initialized || s_authTicket == k_HAuthTicketInvalid) return;
  SteamUser()->CancelAuthTicket(s_authTicket);
  s_authTicket = k_HAuthTicketInvalid;
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
