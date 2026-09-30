/*
 * log_GameSettings blob: the writer and the reader over the same bytes.
 *
 * The blob is the only record of what a round was played under, and it is
 * append-only — a field lands after the last one and every reader older
 * than it has to keep working. Two things can break that and neither shows
 * up in a build:
 *
 *   - the writer moving a field that an old reader still reads by offset,
 *   - a new reader taking a field an old recording does not carry.
 *
 * So this drives the real writer (serverDedicatedLogBuildSettings) and the
 * real reader (lvGameSettingsDecode, the code the log viewer's Game
 * Information panel runs) rather than a copy of either, and asserts the
 * byte offsets the doc names as well as the decoded values. The short
 * payload case stands in for a recording made before these fields existed.
 *
 * Layout: docs/replay-format.md, "log_GameSettings payload".
 */

#include <stdint.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_lifecycle.h"  /* serverSimSetServerLocks */
#include "wire_limits.h"          /* LOBBY_LOCK_* */
#include "server_dedicated_log.h" /* the writer + LOG_SETTINGS_FLAG_* */
#include "game_settings_blob.h"  /* the log viewer's decode, linked here */
#include "test_harness.h"

/* Room for the count byte plus the payload, with slack so a writer that
 * grows past the current length overruns the assert below rather than the
 * buffer. */
#define BLOB_CAP 64

int run_game_settings_blob(void) {
    ServerSim *sim;
    char blob[BLOB_CAP];
    const unsigned char *payload;
    int len;
    LvGameSettings got;
    /* Bit 16 is LOBBY_LOCK_SMART_PINGS — the first lock above the low half
     * the blob has always carried — paired with one low bit so a writer
     * that dropped either half is caught. */
    const uint32_t wantLocks = LOBBY_LOCK_SMART_PINGS | LOBBY_LOCK_MINES;

    sim = ut_make_running_sim("Tester");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim failed");

    serverSimSetSmartPingsOff(sim, true);
    serverSimSetServerLocks(sim, wantLocks);

    memset(blob, 0x5A, sizeof(blob));
    serverDedicatedLogBuildSettings(sim, blob);

    len = (unsigned char)blob[0];
    payload = (const unsigned char *)blob + 1;

    UT_ASSERT_MSG(len == LV_GAME_SETTINGS_FULL_LEN,
                  "settings length byte = %d (want %d)",
                  len, LV_GAME_SETTINGS_FULL_LEN);

    /* The offsets, not just the decoded values: an old reader indexes the
     * low half of the lock mask at 12-13 and must keep finding it there. */
    UT_ASSERT_MSG(payload[12] == (unsigned char)((wantLocks >> 8) & 0xFF) &&
                      payload[13] == (unsigned char)(wantLocks & 0xFF),
                  "lock low half at 12-13 = %02X %02X (want %02X %02X)",
                  payload[12], payload[13],
                  (unsigned)((wantLocks >> 8) & 0xFF),
                  (unsigned)(wantLocks & 0xFF));
    UT_ASSERT_MSG(payload[14] == (unsigned char)((wantLocks >> 24) & 0xFF) &&
                      payload[15] == (unsigned char)((wantLocks >> 16) & 0xFF),
                  "lock high half at 14-15 = %02X %02X (want %02X %02X)",
                  payload[14], payload[15],
                  (unsigned)((wantLocks >> 24) & 0xFF),
                  (unsigned)((wantLocks >> 16) & 0xFF));
    UT_ASSERT_MSG((payload[16] & LOG_SETTINGS_FLAG_SMART_PINGS_OFF) != 0,
                  "settings flags at 16 = %02X, smart-pings-off bit clear",
                  payload[16]);

    /* And back through the reader the panel uses. */
    memset(&got, 0, sizeof(got));
    UT_ASSERT_MSG(lvGameSettingsDecode(payload, len, &got),
                  "decode of a full-length payload failed");
    UT_ASSERT_MSG(got.lobbyLocks == wantLocks,
                  "decoded locks = 0x%08X (want 0x%08X)",
                  (unsigned)got.lobbyLocks, (unsigned)wantLocks);
    UT_ASSERT_MSG(got.smartPingsOff,
                  "decoded smartPingsOff false, want true");
    /* The sim starts with positional sound off, so bit 1 is clear. */
    UT_ASSERT_MSG(!got.positionalSound,
                  "decoded positionalSound true with the setting off");

    /* Smart pings back on: the bit clears and the mask is untouched. */
    serverSimSetSmartPingsOff(sim, false);
    serverDedicatedLogBuildSettings(sim, blob);
    len = (unsigned char)blob[0];
    payload = (const unsigned char *)blob + 1;
    memset(&got, 0, sizeof(got));
    UT_ASSERT_MSG(lvGameSettingsDecode(payload, len, &got), "decode failed");
    UT_ASSERT_MSG(!got.smartPingsOff,
                  "decoded smartPingsOff true after turning pings back on");
    UT_ASSERT_MSG(got.lobbyLocks == wantLocks,
                  "locks moved with the pings flag: 0x%08X",
                  (unsigned)got.lobbyLocks);

    /* Positional sound on: bit 1 of byte 16, and nothing else moves. The
     * reader's own copy of the bit has to be the writer's. */
    UT_ASSERT(LV_GAME_SETTINGS_FLAG_POSITIONAL_SOUND ==
              LOG_SETTINGS_FLAG_POSITIONAL_SOUND);
    serverSimSetPositionalSound(sim, true);
    serverDedicatedLogBuildSettings(sim, blob);
    len = (unsigned char)blob[0];
    payload = (const unsigned char *)blob + 1;
    UT_ASSERT_MSG((payload[16] & LOG_SETTINGS_FLAG_POSITIONAL_SOUND) != 0,
                  "settings flags at 16 = %02X, positional-sound bit clear",
                  payload[16]);
    memset(&got, 0, sizeof(got));
    UT_ASSERT_MSG(lvGameSettingsDecode(payload, len, &got), "decode failed");
    UT_ASSERT_MSG(got.positionalSound,
                  "decoded positionalSound false, want true");
    UT_ASSERT_MSG(!got.smartPingsOff,
                  "the positional-sound bit read as smart pings off");

    serverSimDestroy(sim);

    /* An old recording: the 14 bytes that existed before either field, and
     * nothing after them. The reader must take it, and must read the two
     * missing fields as zero rather than off the end of the payload. */
    {
        static const unsigned char oldPayload[LV_GAME_SETTINGS_MIN_LEN] = {
            0xF6,        /* view policies + classic + trees */
            0x00, 0x2D,  /* pill decay 45s */
            0x01, 0x2C,  /* base decay 300s */
            0x00, 0x05,  /* ally decay 5s */
            0x03,        /* gameStrictTournament */
            0x02,        /* aiYesAdvantage */
            0x2A,        /* time limit + ranked + allow new players */
            0x00, 0x1E,  /* 30 minutes */
            0xFF, 0xFF   /* every low lock bit set */
        };
        LvGameSettings old;

        memset(&old, 0, sizeof(old));
        UT_ASSERT_MSG(lvGameSettingsDecode(oldPayload, (int)sizeof(oldPayload),
                                           &old),
                      "decode rejected a 14-byte payload");
        UT_ASSERT_MSG(old.lobbyLocks == 0x0000FFFFu,
                      "short payload locks = 0x%08X (want 0x0000FFFF)",
                      (unsigned)old.lobbyLocks);
        UT_ASSERT_MSG(!old.smartPingsOff,
                      "short payload read smart pings as banned; absent means "
                      "allowed");
        UT_ASSERT_MSG(!old.positionalSound,
                      "short payload read positional sound as on; absent "
                      "means off");
        /* The fields it does carry still land where they always did. */
        UT_ASSERT_MSG(old.gameType == 3 && old.aiType == 2 &&
                          old.timeMinutes == 30 && old.classicMode,
                      "short payload decoded wrong: type=%d ai=%d mins=%d "
                      "classic=%d",
                      old.gameType, old.aiType, old.timeMinutes,
                      (int)old.classicMode);
    }

    /* Shorter than the oldest writer ever produced is not a recording this
     * reader can make sense of, and it says so rather than guessing. */
    {
        static const unsigned char tooShort[4] = {0, 0, 0, 0};
        LvGameSettings never;
        memset(&never, 0, sizeof(never));
        UT_ASSERT_MSG(!lvGameSettingsDecode(tooShort, (int)sizeof(tooShort),
                                            &never),
                      "decode accepted a 4-byte payload");
        UT_ASSERT_MSG(!lvGameSettingsDecode(NULL, LV_GAME_SETTINGS_FULL_LEN,
                                            &never),
                      "decode accepted a NULL payload");
    }

    return 0;
}
