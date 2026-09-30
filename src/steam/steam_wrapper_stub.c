/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * steam_wrapper_stub.c — no-op stubs when the Steamworks SDK is absent.
 */
#include <stddef.h>

#include "steam_wrapper.h"

bool steam_init(void)          { return false; }
void steam_shutdown(void)      {}
void steam_run_callbacks(void) {}

bool steam_overlay_is_active(void) { return false; }

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

uint64_t steam_get_steam_id(void) { return 0; }

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

bool steam_owns_dlc(uint32_t dlc_app_id) { (void)dlc_app_id; return false; }

bool steam_is_steam_deck(void) { return false; }

bool steam_is_big_picture(void) { return false; }

bool steam_show_floating_keyboard(int x, int y, int w, int h) {
  (void)x; (void)y; (void)w; (void)h;
  return false;
}

void steam_dismiss_floating_keyboard(void) {}

/* -------- Steam Workshop (UGC) stubs -------- */

bool steam_workshop_available(void)        { return false; }
int  steam_workshop_subscribed_count(void) { return 0; }

bool steam_workshop_item(int idx, uint64_t *id, char *folder,
                         size_t folderSize) {
  (void)idx;
  if (id) *id = 0;
  if (folder && folderSize) folder[0] = '\0';
  return false;
}

bool steam_workshop_item_disabled(uint64_t id) {
  (void)id;
  return false;
}

void steam_workshop_request_download(uint64_t id) {
  (void)id;
}

bool steam_workshop_consume_installed_event(void) { return false; }

void steam_workshop_open_browse_page(void) {}

bool steam_workshop_publish_begin(const char *contentFolder, const char *title,
                                  const char *description, const char *previewPng,
                                  uint64_t existingId, const char *tag) {
  (void)contentFolder;
  (void)title;
  (void)description;
  (void)previewPng;
  (void)existingId;
  (void)tag;
  return false;
}

/* -1 (failed) rather than 0 (in progress): without a Steam client no publish
   can ever complete, and answering 0 would leave a caller waiting on one. */
int steam_workshop_publish_poll(uint64_t *outId, bool *needsLegalAgreement) {
  if (outId) *outId = 0;
  if (needsLegalAgreement) *needsLegalAgreement = false;
  return -1;
}

int steam_workshop_publish_progress(uint64_t *bytesDone, uint64_t *bytesTotal) {
  if (bytesDone) *bytesDone = 0;
  if (bytesTotal) *bytesTotal = 0;
  return 0;
}

void steam_workshop_open_item_page(uint64_t id) {
  (void)id;
}

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

bool steam_input_real_controller_connected(void) { return false; }

bool steam_input_consume_real_disconnect(void) { return false; }

void steam_input_trigger_vibration(uint16_t left_speed, uint16_t right_speed) {
  (void)left_speed;
  (void)right_speed;
}
