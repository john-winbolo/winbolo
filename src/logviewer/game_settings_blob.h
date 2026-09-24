/*
 * game_settings_blob.h - decode of the log_GameSettings payload
 *
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The replay's lobby settings arrive as a framed blob of binary bytes
 * (layout in docs/replay-format.md). This turns one into a struct.
 *
 * Deliberately plain C with no bolo and no ImGui headers: the Game
 * Information panel that draws the result includes neither, and keeping
 * the decode out here is what lets a unit test drive the same code the
 * panel runs rather than a copy of it.
 */

#ifndef LV_GAME_SETTINGS_BLOB_H
#define LV_GAME_SETTINGS_BLOB_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Payload bytes a blob needs before it carries every field the panel drew
 * when the event was first written. Recordings from then are still read,
 * so this is the minimum a decode accepts, not the current length. */
#define LV_GAME_SETTINGS_MIN_LEN 14

/* Payload bytes the current writer emits: the 14 above, the high half of
 * the lock mask, and the settings-flags byte. */
#define LV_GAME_SETTINGS_FULL_LEN 17

/* Bits of the settings-flags byte (payload offset 16). Matches
 * LOG_SETTINGS_FLAG_* in src/server/server_dedicated_log.h. */
#define LV_GAME_SETTINGS_FLAG_SMART_PINGS_OFF 0x01u

typedef struct {
    int      viewPolicy[3];      /* pill, base, ally */
    int      viewDecay[3];       /* seconds, meaningful only for decay */
    bool     classicMode;
    bool     alliesInTrees;
    int      gameType;           /* 1 open, 2 tournament, 3 strict, 4 scripted */
    int      aiType;             /* 0 none .. 3 full */
    unsigned flags;              /* the byte at offset 9, verbatim */
    int      timeMinutes;        /* meaningless when the time-limit bit is clear */
    /* The whole 32-bit LOBBY_LOCK_* mask. A recording that predates the
     * high half reads it as zero, which is no lock at bit 16 or above. */
    uint32_t lobbyLocks;
    /* Absent reads as false, which means smart pings were allowed — the
     * behaviour of every server that predates the byte. */
    bool     smartPingsOff;
} LvGameSettings;

/* Decode payload[0..len-1] — the framed bytes, with the length byte
 * already consumed. Returns false and leaves *out untouched when the
 * payload is missing or shorter than LV_GAME_SETTINGS_MIN_LEN. Fields
 * added after the writer that produced a short-but-valid payload come
 * back at their absent value rather than as garbage. */
bool lvGameSettingsDecode(const unsigned char *payload, int len,
                          LvGameSettings *out);

#ifdef __cplusplus
}
#endif

#endif /* LV_GAME_SETTINGS_BLOB_H */
