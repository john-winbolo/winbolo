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
#include <stdint.h>

bool     steam_init(void);
void     steam_shutdown(void);
void     steam_run_callbacks(void);
void     steam_set_rich_presence(const char *key, const char *value);
void     steam_clear_rich_presence(void);
bool     steam_get_auth_ticket(uint8_t *buf, uint32_t buf_size, uint32_t *out_len);

/* Stats & achievements */
void     steam_request_stats(void);
void     steam_increment_stat(const char *name, int amount);
void     steam_set_achievement(const char *id);
void     steam_store_stats(void);

/* Register a callback invoked when a friend clicks "Join Game" in Steam.
 * The connect_str is the value previously set via steam_set_rich_presence("connect", ...). */
typedef void (*SteamJoinCallback)(const char *connect_str);
void     steam_set_join_callback(SteamJoinCallback cb);

#endif /* STEAM_WRAPPER_H */
