/*
 * steam_wrapper_stub.c — no-op stubs when the Steamworks SDK is absent.
 */
#include <stddef.h>

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

bool steam_get_persona_name(char *out, size_t outSize) {
  if (out && outSize) out[0] = '\0';
  return false;
}

void steam_cancel_auth_ticket(void) {}

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

bool steam_is_steam_deck(void) { return false; }

/* -------- Steam Input stubs -------- */

bool steam_input_init(void)             { return false; }
void steam_input_shutdown(void)         {}
void steam_input_run_frame(void)        {}

void steam_input_activate_action_set(const char *set_name) {
  (void)set_name;
}

bool steam_input_is_action_pressed(const char *action_name) {
  (void)action_name;
  return false;
}

void steam_input_get_analog_action(const char *action_name, float *x, float *y) {
  (void)action_name;
  if (x) *x = 0.0f;
  if (y) *y = 0.0f;
}

const char *steam_input_get_glyph_path(const char *action_name) {
  (void)action_name;
  return NULL;
}

bool steam_input_has_active_controller(void) { return false; }

void steam_input_trigger_vibration(uint16_t left_speed, uint16_t right_speed) {
  (void)left_speed;
  (void)right_speed;
}
