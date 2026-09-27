/*
 * The scripted game type as the log viewer reads it.
 *
 * A scripted round writes game type 4 in two places: the .wbv header's game
 * type byte and byte 7 of the log_GameSettings payload. The Game Information
 * panel names the type from either, and a value its enum does not know reads
 * "Unknown". These cases hold both readers to 4.
 *
 * The first records a real round through the replay harness and decodes it
 * with the production viewer, so the header byte comes from the writer the
 * dedicated server uses. The second hands the settings decoder a payload
 * written by hand; the layout is docs/replay-format.md, "log_GameSettings
 * payload".
 */

#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "game_settings_blob.h"  /* the log viewer's decode, linked here */
#include "replay_harness.h"
#include "test_harness.h"

int run_lv_scripted_game_type_header(void) {
    ReplayHarness h;
    ReplayWorld *w;
    ReplayFileInfo info;
    bool decoded;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessPrepare(&h, "lvScriptedGameType", "Tester"),
                  "could not stand the round up");

    /* No scenario is loaded, so the round has no declared base game and
       plays strict. The header records the lobby's type all the same. */
    serverSimSetGameType(h.sim, gameScripted);

    UT_ASSERT_MSG(replayHarnessBeginRecording(&h), "could not start recording");
    replayHarnessTick(&h, 6);
    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");

    w = (ReplayWorld *) malloc(sizeof(ReplayWorld));
    UT_ASSERT_MSG(w != NULL, "could not allocate the decoded world");
    memset(&info, 0, sizeof(info));
    decoded = replayHarnessDecodeFile(h.path, w, &info);
    free(w);
    replayHarnessStop(&h);

    UT_ASSERT_MSG(decoded, "replay did not decode to end-of-log");
    UT_ASSERT_MSG(info.gameType == (int) gameScripted,
                  "header game type decoded as %d (want %d)",
                  info.gameType, (int) gameScripted);
    return 0;
}

int run_lv_scripted_game_type_settings(void) {
    /* The 14 bytes every writer has produced, with byte 7 the scripted
       type. The other fields are ordinary values so the decode has nothing
       else to refuse. */
    static const unsigned char payload[LV_GAME_SETTINGS_MIN_LEN] = {
        0x00,        /* view policies + classic + trees */
        0x00, 0x00,  /* pill decay */
        0x00, 0x00,  /* base decay */
        0x00, 0x00,  /* ally decay */
        0x04,        /* gameScripted */
        0x00,        /* aiNone */
        0x00,        /* flags */
        0x00, 0x00,  /* minutes */
        0x00, 0x00   /* low lock bits */
    };
    LvGameSettings got;

    memset(&got, 0, sizeof(got));
    UT_ASSERT_MSG(lvGameSettingsDecode(payload, (int) sizeof(payload), &got),
                  "decode rejected a 14-byte payload");
    UT_ASSERT_MSG(got.gameType == (int) gameScripted,
                  "settings game type decoded as %d (want %d)",
                  got.gameType, (int) gameScripted);
    return 0;
}
