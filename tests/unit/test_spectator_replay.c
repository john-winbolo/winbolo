/*
 * Spectator replay translator (src/bolo/spectator_replay.c) byte-level framing
 * gate. The translator turns spectator-ring records into the LOG_*-marked
 * plaintext byte stream the log-viewer decoder consumes, plus the synthesized
 * v2 header.
 *
 * This test asserts the framing byte for byte:
 *   - specReplayWriteHeader lays the v2 header out field by field;
 *   - specReplayTranslateKeyframe emits [LOG_EVENT_SNAPSHOT][body] and exposes
 *     the control-snapshot slice;
 *   - specReplayTranslateEvents frames idle, small and malformed event ticks.
 *
 * End-to-end decode of a translated stream is covered by composition:
 * stream_load proves lv_screenLoadFromStream decodes a real LOG_* stream, and
 * the synthesized header is byte-identical to the writer's header whose
 * round-trip wbv_reader_v1/v2 prove. The live-client decode path is exercised
 * in the spectator client-integration slices.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "log.h"            /* LOG_* markers, LOG_VERSION */
#include "spectator_replay.h"
#include "test_harness.h"

int run_spectator_replay(void) {
  /* Header: known info, asserted field by field. */
  {
    SpecReplayHeaderInfo info;
    BYTE buf[256];
    int n;
    int expectLen;
    const char *mapName = "HdrMap";   /* 6 chars */
    int mapLen = (int) strlen(mapName);

    memset(&info, 0, sizeof(info));
    info.mapName = mapName;
    info.gameType = 7;
    info.allowHiddenMines = 1;
    info.ai = 2;
    info.usePassword = 1;
    info.maxPlayers = 8;
    info.versionMajor = 4;
    info.versionMinor = 5;
    info.versionRevision = 6;

    n = specReplayWriteHeader(&info, buf, (int) sizeof(buf));
    /* 8 id + 1 version + 1 maplen + mapLen + 8 gameinfo + 4 addr + 2 port
       + 4 time + 32 key. */
    expectLen = 8 + 1 + 1 + mapLen + 8 + 4 + 2 + 4 + 32;
    UT_ASSERT_MSG(n == expectLen, "header len = %d (want %d)", n, expectLen);

    UT_ASSERT_MSG(memcmp(buf, "WBOLOMOV", 8) == 0, "header id mismatch");
    UT_ASSERT_MSG(buf[8] == LOG_VERSION, "version byte = %d (want %d)",
                  buf[8], LOG_VERSION);
    UT_ASSERT_MSG(buf[9] == (BYTE) mapLen, "map len byte = %d (want %d)",
                  buf[9], mapLen);
    UT_ASSERT_MSG(memcmp(buf + 10, mapName, (size_t) mapLen) == 0,
                  "map name mismatch");
    /* The 8 gameinfo bytes land in order straight after the map name. */
    UT_ASSERT_MSG(buf[10 + mapLen + 0] == 7, "gameType byte");
    UT_ASSERT_MSG(buf[10 + mapLen + 1] == 1, "allowHiddenMines byte");
    UT_ASSERT_MSG(buf[10 + mapLen + 2] == 2, "ai byte");
    UT_ASSERT_MSG(buf[10 + mapLen + 3] == 1, "usePassword byte");
    UT_ASSERT_MSG(buf[10 + mapLen + 4] == 8, "maxPlayers byte");
    UT_ASSERT_MSG(buf[10 + mapLen + 5] == 4, "versionMajor byte");
    UT_ASSERT_MSG(buf[10 + mapLen + 6] == 5, "versionMinor byte");
    UT_ASSERT_MSG(buf[10 + mapLen + 7] == 6, "versionRevision byte");

    /* Overflow: a cap one byte short of the layout returns -1. */
    UT_ASSERT_MSG(specReplayWriteHeader(&info, buf, expectLen - 1) == -1,
                  "header overflow not rejected");
  }

  /* Events: a 2-event payload frames as [LOG_EVENT][2] + payload verbatim. */
  {
    BYTE payload[] = { 0x07, 0x00, 0x03, 'a', 'b', 'c',
                       0x09, 0x00, 0x02, 'd', 'e' };
    int payloadLen = (int) sizeof(payload);
    BYTE out[64];
    int n = specReplayTranslateEvents(payload, payloadLen, out,
                                      (int) sizeof(out));
    UT_ASSERT_MSG(n == 2 + payloadLen, "events len = %d (want %d)",
                  n, 2 + payloadLen);
    UT_ASSERT_MSG(out[0] == LOG_EVENT, "events marker = %d (want LOG_EVENT)",
                  out[0]);
    UT_ASSERT_MSG(out[1] == 2, "event count = %d (want 2)", out[1]);
    UT_ASSERT_MSG(memcmp(out + 2, payload, (size_t) payloadLen) == 0,
                  "event payload not copied verbatim");
  }

  /* Empty event tick -> [LOG_NOEVENTS][1]. */
  {
    BYTE out[8];
    int n = specReplayTranslateEvents(NULL, 0, out, (int) sizeof(out));
    UT_ASSERT_MSG(n == 2, "empty events len = %d (want 2)", n);
    UT_ASSERT_MSG(out[0] == LOG_NOEVENTS, "empty marker = %d (want LOG_NOEVENTS)",
                  out[0]);
    UT_ASSERT_MSG(out[1] == 1, "empty count byte = %d (want 1)", out[1]);
  }

  /* Malformed events: a length field that overruns the payload -> -1. */
  {
    BYTE bad[] = { 0x07, 0x00, 0x05, 'a', 'b' };  /* claims 5 body bytes, has 2 */
    BYTE out[64];
    UT_ASSERT_MSG(specReplayTranslateEvents(bad, (int) sizeof(bad), out,
                                            (int) sizeof(out)) == -1,
                  "overrunning event len not rejected");
  }

  /* Keyframe: [u32 bodyLen=4][BB BB BB BB][u32 ctrlLen=3][CC CC CC]. */
  {
    BYTE payload[] = {
      0x00, 0x00, 0x00, 0x04,           /* bodyLen = 4   */
      0xBB, 0xBB, 0xBB, 0xBB,           /* body          */
      0x00, 0x00, 0x00, 0x03,           /* ctrlLen = 3   */
      0xCC, 0xCC, 0xCC                  /* control slice */
    };
    BYTE out[64];
    const BYTE *ctrl = NULL;
    int ctrlLen = -1;
    int n = specReplayTranslateKeyframe(payload, (int) sizeof(payload), out,
                                        (int) sizeof(out), &ctrl, &ctrlLen);
    UT_ASSERT_MSG(n == 5, "keyframe len = %d (want 5)", n);
    UT_ASSERT_MSG(out[0] == LOG_EVENT_SNAPSHOT,
                  "keyframe marker = %d (want LOG_EVENT_SNAPSHOT)", out[0]);
    UT_ASSERT_MSG(out[1] == 0xBB && out[2] == 0xBB && out[3] == 0xBB &&
                      out[4] == 0xBB,
                  "keyframe body bytes mismatch");
    UT_ASSERT_MSG(ctrl == payload + 12, "control slice pointer wrong");
    UT_ASSERT_MSG(ctrlLen == 3, "control slice len = %d (want 3)", ctrlLen);
    UT_ASSERT_MSG(ctrl != NULL && ctrl[0] == 0xCC && ctrl[1] == 0xCC &&
                      ctrl[2] == 0xCC,
                  "control slice bytes mismatch");
  }

  return 0;
}
