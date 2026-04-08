/*
 * steam_wrapper_stub.c — no-op stubs when the Steamworks SDK is absent.
 */
#include "steam_wrapper.h"

bool steam_init(void)          { return false; }
void steam_shutdown(void)      {}
void steam_run_callbacks(void) {}

void steam_set_rich_presence(const char *key, const char *value) {
  (void)key; (void)value;
}

void steam_clear_rich_presence(void) {}

bool steam_get_auth_ticket(uint8_t *buf, uint32_t buf_size, uint32_t *out_len) {
  (void)buf; (void)buf_size; (void)out_len;
  return false;
}

void steam_increment_stat(const char *name, int amount) {
  (void)name; (void)amount;
}

void steam_set_achievement(const char *id) {
  (void)id;
}

void steam_store_stats(void) {}

void steam_set_join_callback(SteamJoinCallback cb) {
  (void)cb;
}
