/*
 * game_settings_blob.c - decode of the log_GameSettings payload
 *
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "game_settings_blob.h"

#include <string.h>

bool lvGameSettingsDecode(const unsigned char *payload, int len,
                          LvGameSettings *out) {
    LvGameSettings s;

    if (payload == NULL || out == NULL || len < LV_GAME_SETTINGS_MIN_LEN) {
        return false;
    }

    memset(&s, 0, sizeof(s));

    /* Byte 0: two bits per category, then the two mode flags. */
    s.viewPolicy[0] = payload[0] & 0x03;
    s.viewPolicy[1] = (payload[0] >> 2) & 0x03;
    s.viewPolicy[2] = (payload[0] >> 4) & 0x03;
    s.classicMode = (payload[0] & 0x40) != 0;
    s.alliesInTrees = (payload[0] & 0x80) != 0;

    s.viewDecay[0] = (payload[1] << 8) | payload[2];
    s.viewDecay[1] = (payload[3] << 8) | payload[4];
    s.viewDecay[2] = (payload[5] << 8) | payload[6];

    s.gameType = payload[7];
    s.aiType = payload[8];
    s.flags = payload[9];
    s.timeMinutes = (payload[10] << 8) | payload[11];

    /* Bytes 12-13 are the low half of the lock mask and have been in the
     * blob since the event existed. */
    s.lobbyLocks = (uint32_t)((payload[12] << 8) | payload[13]);

    /* Bytes 14-15 are its high half, appended later. A recording that
     * stops before them leaves those bits clear, which is what a server
     * old enough to write that recording actually had. */
    if (len >= 16) {
        s.lobbyLocks |= ((uint32_t)payload[14] << 24) |
                        ((uint32_t)payload[15] << 16);
    }

    /* Byte 16 is the settings-flags byte, appended with it. Absent means
     * smart pings were allowed. */
    if (len >= 17) {
        s.smartPingsOff =
            (payload[16] & LV_GAME_SETTINGS_FLAG_SMART_PINGS_OFF) != 0;
    }

    *out = s;
    return true;
}
