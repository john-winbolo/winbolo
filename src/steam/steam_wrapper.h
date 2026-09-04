/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * steam_wrapper.h — thin C wrapper around the Steamworks SDK.
 *
 * Always include this header; the build system selects either the real
 * implementation (steam_wrapper.c, when the SDK is present) or the stub
 * (steam_wrapper_stub.c).
 */
#ifndef STEAM_WRAPPER_H
#define STEAM_WRAPPER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool     steam_init(void);
void     steam_shutdown(void);
void     steam_run_callbacks(void);

/* True iff the Steam in-game overlay (Shift+Tab, Big Picture, or the
 * Steam Deck menu) is currently open. Updated from GameOverlayActivated_t
 * during steam_run_callbacks(). Always false in stub builds. */
bool     steam_overlay_is_active(void);

void     steam_set_rich_presence(const char *key, const char *value);
void     steam_clear_rich_presence(void);
bool     steam_get_auth_ticket(uint8_t *buf, uint32_t buf_size, uint32_t *out_len);

/* Copy the local Steam user's persona (display) name into out. Returns
 * false (out untouched/empty) when Steam is not initialized — callers use
 * this both to pre-fill a signup name and to detect "running under Steam". */
bool     steam_get_persona_name(char *out, size_t outSize);

/* The local user's SteamID64, 0 when Steam is not initialized. This is a
 * public identifier — it is the number in every Steam profile URL — and not a
 * credential: it is written into a published skin so a later publish can tell
 * the author's own item from someone else's. */
uint64_t steam_get_steam_id(void);

/* Cancel the auth-session ticket acquired by the most recent
 * steam_get_auth_ticket call, releasing its HAuthTicket handle.
 * No-op if no ticket is outstanding or Steam is not initialized. */
void     steam_cancel_auth_ticket(void);

/* True iff Steam is initialized and the local user owns the given DLC.
 * A licence check (BIsSubscribedApp), independent of whether any DLC
 * content is installed — required for a content-less cosmetic DLC, which
 * BIsDlcInstalled would report as absent even for owners. False in stub
 * builds and when Steam is not initialized. */
bool     steam_owns_dlc(uint32_t dlc_app_id);

/* True iff Steam is initialized and currently running on a Steam Deck. */
bool     steam_is_steam_deck(void);

/* True iff Steam is initialized and the app was launched into Big Picture /
 * Gamepad UI mode (the couch/controller-first context). */
bool     steam_is_big_picture(void);

/* -------- Steam virtual keyboard --------
 * Floating on-screen keyboard for controller text entry.  Steam injects the
 * typed characters into the focused window as ordinary keystrokes, so they
 * arrive through the normal SDL text-input path and land in whatever ImGui
 * field has focus — no per-field plumbing.  The x/y/w/h describe the text
 * field's screen rect so Steam can position the panel without covering it.
 *
 * Returns true if the keyboard was shown.  Returns false (no-op) when Steam
 * is not initialized or the current context can't display it (e.g. a desktop
 * launch without Steam Input / Big Picture) — callers treat false as "Steam
 * can't help here". */
bool     steam_show_floating_keyboard(int x, int y, int w, int h);

/* Dismiss the floating keyboard shown by steam_show_floating_keyboard.  Must
 * be called when the text field loses focus — Steam does not auto-dismiss it. */
void     steam_dismiss_floating_keyboard(void);

/* -------- Steam Workshop (UGC) --------
 * Read side of the Workshop: what the local user is subscribed to, where
 * Steam unpacked it, and a way into the overlay's Workshop page.  Everything
 * here reports "no Workshop" in stub builds and when Steam is not
 * initialized, so callers need no #ifdef. */

/* True iff Steam is initialized and the UGC interface is live. */
bool     steam_workshop_available(void);

/* How many Workshop items the local user is subscribed to for this app.
 * 0 when the Workshop is unavailable. */
int      steam_workshop_subscribed_count(void);

/* Details of the idx'th subscribed item.  Writes the item's published file
 * id to *id whether or not the item is installed, and returns true only when
 * the item IS installed and *folder holds its install path.  A false return
 * with a non-zero *id therefore reads as "subscribed, still downloading" —
 * the caller can list it as pending and ask for it with
 * steam_workshop_request_download.  Both outs are cleared on entry, so a
 * false return with a zero *id is "no such item". */
bool     steam_workshop_item(int idx, uint64_t *id, char *folder,
                             size_t folderSize);

/* Ask Steam to download (or update) a subscribed item.  Completion shows up
 * as the install event below, not as a return value.  No-op when the
 * Workshop is unavailable or id is 0. */
void     steam_workshop_request_download(uint64_t id);

/* Consume the "a Workshop item finished installing" edge.  Returns true once
 * if ItemInstalled_t or DownloadItemResult_t has fired since the previous
 * call, and clears the flag.  Callers rescan when it fires. */
bool     steam_workshop_consume_installed_event(void);

/* Open the Steam overlay on this app's Workshop page.  No-op when the
 * Workshop is unavailable. */
void     steam_workshop_open_browse_page(void);

/* Publish side of the Workshop.  A publish runs across several frames — the
 * Steam calls behind it complete asynchronously — so it is started once and
 * then polled.  One publish at a time. */

/* Start publishing an item.  contentFolder is a directory whose entire
 * contents become the item's content; the caller creates it and cleans it up
 * afterwards.  previewPng may be NULL for no preview image, and must be under
 * 1MB when given.  existingId 0 creates a new item; non-zero updates that
 * item rather than making a duplicate.  Returns false immediately when the
 * Workshop is unavailable or a publish is already in flight.  The strings are
 * copied, so the caller need not keep them alive past the call. */
bool     steam_workshop_publish_begin(const char *contentFolder, const char *title,
                                      const char *description, const char *previewPng,
                                      uint64_t existingId /* 0 = new item */);

/* State of the publish: 0 still in progress, 1 done (*outId holds the
 * published file id), -1 failed.  Both outs may be NULL and are cleared
 * before anything is written.  Only meaningful after a
 * steam_workshop_publish_begin() that returned true — with nothing in flight
 * this answers -1, so a caller that polls out of order gets a definite answer
 * rather than waiting forever on a publish that was never started.  The
 * terminal value stays put until the next steam_workshop_publish_begin(). */
int      steam_workshop_publish_poll(uint64_t *outId, bool *needsLegalAgreement);

/* Upload progress.  0 when nothing is uploading, otherwise the Steam update
 * status: 1 preparing config, 2 preparing content, 3 uploading content,
 * 4 uploading preview, 5 committing.  The byte counts are filled once the
 * upload has started and are 0 before then.  Either out may be NULL. */
int      steam_workshop_publish_progress(uint64_t *bytesDone, uint64_t *bytesTotal);

/* Open the Steam overlay on one item's Workshop page — where the author
 * accepts the Workshop legal agreement and changes the item's visibility.
 * No-op when the Workshop is unavailable. */
void     steam_workshop_open_item_page(uint64_t id);

/* Stats & achievements */
void     steam_increment_stat(const char *name, int amount);
void     steam_set_achievement(const char *id);
void     steam_store_stats(void);

/* Register a callback invoked when a friend clicks "Join Game" in Steam.
 * The connect_str is the value previously set via steam_set_rich_presence("connect", ...). */
typedef void (*SteamJoinCallback)(const char *connect_str);
void     steam_set_join_callback(SteamJoinCallback cb);

/* -------- Steam Input (controller actions + glyphs) --------
 * Path A of the two-path input model.  Returns false / null in the
 * stub build, so callers should always have a Path B (raw SDL_Gamepad)
 * fallback.  Action and set names match the strings in the action
 * manifest VDF — see steam_input_actions.h. */

bool steam_input_init(void);
void steam_input_shutdown(void);
void steam_input_run_frame(void);

/* Activate one action set on the active controller.  Action sets group
 * actions by context — e.g. "InGame" enables fire/move/build, "Menu"
 * enables nav, "Build" overlays builder-specific bindings.  Game code
 * calls this on context transitions. */
void steam_input_activate_action_set(const char *set_name);

/* Digital action: returns true if the player is currently pressing
 * any input mapped to this action.  Returns false if Steam Input is
 * uninitialised, no controller is connected, or the action name is
 * unknown to the manifest. */
bool steam_input_is_action_pressed(const char *action_name);

/* Analog action: writes -1.0..+1.0 components to *x, *y.  Both are
 * set to 0.0 if Steam Input is uninitialised, no controller is
 * connected, or the action is unknown.  Pass NULL for components
 * you don't care about. */
void steam_input_get_analog_action(const char *action_name, float *x, float *y);

/* Glyph lookup: returns an absolute filesystem path to a PNG of the
 * appropriate icon for the action's currently-bound origin (e.g.
 * "A button" on Xbox, "Cross" on PS5, "Steam Deck A" on Deck).  The
 * path is owned by Steam and remains valid until the next call to
 * steam_input_run_frame.  Returns NULL if no controller is connected
 * or the action is unknown. */
const char *steam_input_get_glyph_path(const char *action_name);

/* True iff Steam Input is initialised AND has an active connected
 * controller.  Single source of truth for the Path A / Path B
 * selector in the input layer.  Returns false in stub builds. */
bool steam_input_has_active_controller(void);

/* True iff a real physical controller is connected, tracked via the
 * device hot-plug callbacks (EnableDeviceCallbacks).  Unlike
 * steam_input_has_active_controller(), this is NOT fooled by the
 * always-present keyboard/mouse virtual controller.  Returns false in
 * stub builds. */
bool steam_input_real_controller_connected(void);

/* Consume the one-shot "a real controller just disconnected" edge.
 * Returns true once per physical disconnect.  False in stub builds. */
bool steam_input_consume_real_disconnect(void);

/* Route haptic rumble through Steam Input's vibration API.  When
 * Steam Input is intercepting the controller, SDL_RumbleGamepad
 * is silent — this lets us still feel the rumble on Path A.
 * Magnitudes are 0..65535.  No-op in stub builds and when no
 * controller is active. */
void steam_input_trigger_vibration(uint16_t left_speed, uint16_t right_speed);

#endif /* STEAM_WRAPPER_H */
