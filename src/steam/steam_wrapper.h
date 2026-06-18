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

/* Cancel the auth-session ticket acquired by the most recent
 * steam_get_auth_ticket call, releasing its HAuthTicket handle.
 * No-op if no ticket is outstanding or Steam is not initialized. */
void     steam_cancel_auth_ticket(void);

/* True iff Steam is initialized and currently running on a Steam Deck. */
bool     steam_is_steam_deck(void);

/* True iff Steam is initialized and the app was launched into Big Picture /
 * Gamepad UI mode (the couch/controller-first context). */
bool     steam_is_big_picture(void);

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

/* Route haptic rumble through Steam Input's vibration API.  When
 * Steam Input is intercepting the controller, SDL_RumbleGamepad
 * is silent — this lets us still feel the rumble on Path A.
 * Magnitudes are 0..65535.  No-op in stub builds and when no
 * controller is active. */
void steam_input_trigger_vibration(uint16_t left_speed, uint16_t right_speed);

#endif /* STEAM_WRAPPER_H */
