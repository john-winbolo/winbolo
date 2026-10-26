/*
 * Round-trip coverage for the dedicated-server replay log writer.
 *
 * Drives log.c's public API the same way the dedicated server does
 * (logStart → logAddEvent → logWriteTick → logStop), reads the
 * resulting .wbv back out of the zip, and walks the inner log.dat byte
 * stream by hand to assert every event we wrote comes back in order
 * with a clean LOG_QUIT terminator.
 *
 * The stream is format v2: plaintext (no XOR obfuscation), with each
 * event record framed as [type][u16 big-endian payload length][payload].
 *
 * Specifically asserts the snapshot continuity invariant:
 *   - When logWriteSnapshot fires WITH events queued, the events queued
 *     before it and the events written after it must both still decode in
 *     order — the snapshot marker and body must not knock the stream out
 *     of alignment. (Guards the regression behind the fix in commit
 *     18ad9be.)
 *
 * The walker here is intentionally independent of src/logviewer/'s
 * reader — it's the wire-format spec encoded as C. A writer-side bug
 * that produces bytes the production reader doesn't expect will fail
 * here too, but the failure mode is local to the walker so the
 * diagnostic is the actual byte position and decoded code, not a
 * generic "log is corrupt" dialog.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "input_packet.h"   /* PING_KIND_* — log_Ping's kind byte */
#include "log.h"
#include "server_sim.h"
#include "server_sim_internal.h"  /* serverSimLagLogDue, playerPing, statLastRewindTicks */
#include "server_lifecycle.h"      /* serverOverrunStatsDue */
#include "overrun_record.h"         /* log_OverrunStats payload */
#include "game_sim.h"       /* GameSim.tanks — set the stocks a snapshot records */
#include "tank.h"           /* tankSetShells / tankSetMines / ... */
#include "unzip.h"
#include "replay_harness.h"   /* replayHarnessDecodeFile: the viewer's read of a file */
#include "test_harness.h"

/* -------- Wire-format walker (self-contained on purpose) ---------- */

/* Per-event "bytes-after-code-byte" table. For variable-length
 * events (pstr suffix) the entry is -1 and the walker handles them
 * inline. Indexed by logitem code. The three spectator events (50..52)
 * are missing and decode as unknown; nothing emits them yet. */
static int eventFixedBytesAfterCode(uint8_t code) {
    switch (code) {
        case log_PlayerJoined:     return -1;  /* 5 + pstr */
        case log_PlayerQuit:       return 1;
        case log_PlayerLocation:   return 5;
        case log_LgmLocation:      return 4;
        case log_MapChange:        return 3;
        case log_Shell:            return 4;
        case log_SoundBuild:
        case log_SoundFarm:
        case log_SoundShoot:
        case log_SoundHitTank:
        case log_SoundHitTree:
        case log_SoundHitWall:
        case log_SoundMineLay:
        case log_SoundMineExplode:
        case log_SoundExplosion:
        case log_SoundBigExplosion:
        case log_SoundManDie:      return 2;
        case log_MessageServer:    return -1;  /* 0 + pstr */
        case log_MessageAll:       return -1;  /* 1 + pstr */
        case log_MessagePlayers:   return -1;  /* 2 + pstr */
        case log_ChangeName:       return -1;  /* 1 + pstr */
        case log_AllyRequest:      return 2;
        case log_AllyAccept:       return 2;
        case log_AllyLeave:        return 1;
        case log_BaseSetOwner:     return 3;
        case log_BaseSetStock:     return 4;
        case log_PillSetOwner:     return 3;
        case log_PillSetHealth:    return 2;   /* index, armour */
        case log_PillSetPlace:     return 3;
        case log_PillSetInTank:    return 1;
        case log_SaveMap:          return 0;
        case log_LostMan:          return 1;
        case log_KillPlayer:       return 2;
        case log_PlayerRejoin:     return 1;
        case log_PlayerLeaving:    return 1;
        case log_PlayerDied:       return 1;
        case log_LobbyEnter:       return 0;
        case log_LobbyExit:        return 0;
        case log_PlayerReady:      return 1;
        case log_PlayerUnready:    return 1;
        case log_TeamSet:          return 2;
        case log_CountdownStart:   return 0;
        case log_CountdownCancel:  return 0;
        case log_MapSkipVote:      return 1;
        case log_MapSkipApplied:   return -1;  /* 0 + pstr */
        case log_BalanceApplied:   return 0;
        case log_GameVoteStart:    return 3;
        case log_GameVoteCast:     return 3;
        case log_GameVoteEnd:      return 2;
        case log_GameSettings:     return -1;  /* 0 + pstr */
        case log_Ping:             return 6;   /* sender, kind, x (BE u16), y (BE u16) */
        case log_TankSetStock:     return 5;   /* player, shells, mines, armour, trees */
        case log_MapName:          return -1;  /* 0 + pstr */
        case log_PlayerLag:        return 4;   /* player, RTT (BE u16), rewind */
        case log_OverrunStats:     return -3;  /* 44 + 13 a bot, checked below */
        default:                   return -2;  /* unknown */
    }
}

static int variableEventPrefixBytes(uint8_t code) {
    switch (code) {
        case log_PlayerJoined:    return 5;
        case log_MessageServer:   return 0;
        case log_MessageAll:      return 1;
        case log_MessagePlayers:  return 2;
        case log_ChangeName:      return 1;
        case log_MapSkipApplied:  return 0;
        case log_GameSettings:    return 0;
        case log_MapName:         return 0;
        default:                  return -1;  /* not variable */
    }
}

/* Pull a single byte from buf[pos]. The v2 stream is plaintext (no XOR).
 * Returns -1 on EOF. */
static int readByte(const uint8_t *buf, size_t len, size_t pos) {
    if (pos >= len) return -1;
    return buf[pos];
}

/* Snapshot shape decoded for tests that need to verify the lobby
 * empty-world payload sizes. Filled by decodeSnapshot in addition to
 * the usual advance-pos behaviour of skipSnapshot. */
typedef struct {
    int pillsPayloadLen;   /* raw payload length after the 1-byte size */
    int basesPayloadLen;
    int startsPayloadLen;
    int mapRunCount;       /* number of runs read, NOT counting the terminator */
    int playerStubCount;   /* number of 2-byte (not-in-use) player blocks */
    int playerFullCount;   /* number of >2-byte player blocks */
    int slot0BlockLen;     /* payload length of slot 0's block */
    uint8_t slot0Block[256]; /* slot 0's block payload, for a field-by-field walk */
} snapshotShape;

/* Decode the snapshot at *pos, advancing *pos past it. Fills *shape with
 * the per-section sizes so the caller can assert on the lobby shape. The
 * snapshot body is NOT per-event length-framed; it is parsed exactly as in
 * v1, only without the XOR de-obfuscation. */
static bool decodeSnapshot(const uint8_t *buf, size_t len, size_t *pos,
                           snapshotShape *shape) {
    size_t p = *pos;
    int n;
    memset(shape, 0, sizeof(*shape));
    if (p + 8 > len) return false;
    p += 8;                                      /* startDelay + timeLimit */
    if ((n = readByte(buf, len, p)) < 0) return false;
    shape->pillsPayloadLen = n;
    p += 1 + (size_t)n;
    if ((n = readByte(buf, len, p)) < 0) return false;
    shape->basesPayloadLen = n;
    p += 1 + (size_t)n;
    if ((n = readByte(buf, len, p)) < 0) return false;
    shape->startsPayloadLen = n;
    p += 1 + (size_t)n;
    while (1) {
        if (p + 4 > len) return false;
        int dlen = readByte(buf, len, p);
        int y    = readByte(buf, len, p + 1);
        int sx   = readByte(buf, len, p + 2);
        int ex   = readByte(buf, len, p + 3);
        p += 4;
        if (dlen == 4 && y == 255 && sx == 255 && ex == 255) break;
        if (dlen < 4) return false;
        p += (size_t)(dlen - 4);
        shape->mapRunCount++;
    }
    for (int i = 0; i < MAX_TANKS; i++) {
        if ((n = readByte(buf, len, p)) < 0) return false;
        if (n == 2) shape->playerStubCount++;
        else        shape->playerFullCount++;
        if (i == 0) {
            if (p + 1 + (size_t)n > len) return false;
            shape->slot0BlockLen = n;
            memcpy(shape->slot0Block, buf + p + 1, (size_t)n);
        }
        p += 1 + (size_t)n;
    }
    *pos = p;
    return true;
}

/* Skip a full snapshot body (post-marker), updating pos in place.
 * Returns false on short read or malformed map-run terminator. The body
 * is plaintext and not length-framed — same shape as v1, minus the XOR. */
static bool skipSnapshot(const uint8_t *buf, size_t len, size_t *pos) {
    size_t p = *pos;
    if (p + 8 > len) return false;
    p += 8;                                      /* startDelay + timeLimit */
    int n;
    /* pills */
    if ((n = readByte(buf, len, p)) < 0) return false;
    p += 1 + (size_t)n;
    /* bases */
    if ((n = readByte(buf, len, p)) < 0) return false;
    p += 1 + (size_t)n;
    /* starts */
    if ((n = readByte(buf, len, p)) < 0) return false;
    p += 1 + (size_t)n;
    /* map runs until terminator (datalen=4, y=255, sx=255, ex=255) */
    while (1) {
        if (p + 4 > len) return false;
        int dlen = readByte(buf, len, p);
        int y    = readByte(buf, len, p + 1);
        int sx   = readByte(buf, len, p + 2);
        int ex   = readByte(buf, len, p + 3);
        p += 4;
        if (dlen == 4 && y == 255 && sx == 255 && ex == 255) break;
        if (dlen < 4) return false;
        p += (size_t)(dlen - 4);
    }
    /* 16 players */
    for (int i = 0; i < MAX_TANKS; i++) {
        if ((n = readByte(buf, len, p)) < 0) return false;
        p += 1 + (size_t)n;
    }
    *pos = p;
    return true;
}

/* Optional payload capture for walkLog. Set `code` to the event whose bytes
 * the caller wants; the walker copies the payload of the first event with
 * that code into `bytes` and records the length. `len` stays -1 when no such
 * event was seen, or when the payload is longer than `bytes`. */
typedef struct {
    uint8_t code;
    uint8_t bytes[512];  /* a log_OverrunStats record is up to 252 */
    int     len;
} eventPayloadCapture;

/* Walk the buffer from `startPos` (which must point at the first
 * outer byte AFTER the initial logStart header) and fill `eventCodes`
 * with every event code decoded in order. `capture` is optional (NULL to
 * skip) and grabs one event's payload bytes for callers that assert on the
 * payload as well as the framing. Returns 0 on a clean
 * LOG_QUIT terminator; -N on the first failure (N indicating which
 * stage broke, useful for human eyeballing on test failure).
 *
 * v2 stream: plaintext (no XOR). The outer markers (QUIT / NOEVENTS /
 * NOEVENTS_LONG / SNAPSHOT / EVENT / EVENT_LONG) are unframed, exactly as
 * before. Inside an EVENT block each record is [type][u16 BE payload len]
 * [payload]; the length field alone drives the advance, and the per-type
 * table is used as a cross-check that the framed length matches the payload
 * the writer's switch produces for known types. */
/* When true, walkLog skips an event the table does not know by its framed
 * length, as the reader does, instead of failing. For a test that walks a
 * whole recorded round, which holds events this file has no table entry for. */
static bool walkAllowUnknown = false;

static int walkLog(const uint8_t *buf, size_t len, size_t startPos,
                   uint8_t *eventCodes, int *outEventCount, int maxCodes,
                   int *outSnapshotCount, eventPayloadCapture *capture) {
    size_t pos = startPos;
    int ec = 0;
    int sc = 0;
    while (pos < len) {
        int code = readByte(buf, len, pos); pos++;
        if (code < 0) return -1;
        if (code == LOG_QUIT) {
            *outEventCount    = ec;
            *outSnapshotCount = sc;
            return 0;
        } else if (code == LOG_NOEVENTS) {
            if (readByte(buf, len, pos) < 0) return -2;
            pos += 1;
        } else if (code == LOG_NOEVENTS_LONG) {
            if (pos + 2 > len) return -2;
            pos += 2;
        } else if (code == LOG_EVENT_SNAPSHOT) {
            if (!skipSnapshot(buf, len, &pos)) return -3;
            sc++;
        } else if (code == LOG_EVENT || code == LOG_EVENT_LONG) {
            int n;
            if (code == LOG_EVENT) {
                n = readByte(buf, len, pos); pos += 1;
                if (n < 0) return -4;
            } else {
                if (pos + 2 > len) return -4;
                /* htons in writer; reader reverses with ntohs. The on-disk
                 * order is data[1]=low(N), data[2]=high(N), so the decoded
                 * count is (lo << 8) | hi to mirror the reader. */
                uint8_t hi = buf[pos];
                uint8_t lo = buf[pos+1];
                n = (lo << 8) | hi;
                pos += 2;
            }
            for (int i = 0; i < n; i++) {
                int ev = readByte(buf, len, pos);
                if (ev < 0) return -5;
                if (pos + 3 > len) return -5;          /* type + u16 len */
                int plen = (buf[pos+1] << 8) | buf[pos+2];  /* u16 BE payload len */
                size_t payloadStart = pos + 3;
                if (payloadStart + (size_t)plen > len) return -7;

                /* Cross-check the framed length against the per-type table. */
                int fixed = eventFixedBytesAfterCode((uint8_t)ev);
                if (fixed == -2) {
                    if (!walkAllowUnknown) return -6;  /* unknown event type */
                    /* else trust the framed length, as the reader does */
                } else if (fixed == -3) {
                    /* log_OverrunStats: the fixed part, then the bots its
                     * count byte says. */
                    if (plen < OVERRUN_RECORD_HEAD_BYTES) return -13;
                    if (plen != OVERRUN_RECORD_HEAD_BYTES +
                                buf[payloadStart + OVERRUN_RECORD_HEAD_BYTES - 1] *
                                    OVERRUN_RECORD_BOT_BYTES) {
                        return -13;                    /* length disagrees */
                    }
                } else if (fixed >= 0) {
                    if (plen != fixed) return -7;      /* length disagrees */
                } else {
                    int pref = variableEventPrefixBytes((uint8_t)ev);
                    if (pref < 0) return -8;
                    if (payloadStart + (size_t)pref >= len) return -9;
                    int strLen = buf[payloadStart + (size_t)pref]; /* pstr len byte */
                    if (pref + 1 + strLen != plen) return -10;     /* length disagrees */
                }

                if (capture != NULL && capture->len < 0 &&
                    (uint8_t)ev == capture->code &&
                    (size_t)plen <= sizeof(capture->bytes)) {
                    memcpy(capture->bytes, buf + payloadStart, (size_t)plen);
                    capture->len = plen;
                }

                pos = payloadStart + (size_t)plen;
                if (ec < maxCodes) {
                    eventCodes[ec++] = (uint8_t)ev;
                }
            }
        } else {
            return -11;
        }
    }
    return -12;  /* hit EOF without LOG_QUIT */
}

/* -------- Test-side zip extraction (uses minizip's unz API) -------- */

/* Reads the inner "log.dat" out of `wbvPath` into `*outBuf` / `*outLen`
 * (caller frees). Returns false on any error. Shared with test_stream_load.c
 * (prototype in test_harness.h). */
bool extractLogDat(const char *wbvPath, uint8_t **outBuf, size_t *outLen) {
    unzFile uf = unzOpen(wbvPath);
    if (uf == NULL) return false;
    if (unzGoToFirstFile(uf) != UNZ_OK) { unzClose(uf); return false; }
    unz_file_info info;
    if (unzGetCurrentFileInfo(uf, &info, NULL, 0, NULL, 0, NULL, 0) != UNZ_OK) {
        unzClose(uf); return false;
    }
    if (unzOpenCurrentFile(uf) != UNZ_OK) { unzClose(uf); return false; }
    size_t total = (size_t)info.uncompressed_size;
    uint8_t *buf = (uint8_t *)malloc(total ? total : 1);
    if (buf == NULL) { unzCloseCurrentFile(uf); unzClose(uf); return false; }
    size_t got = 0;
    while (got < total) {
        int chunk = unzReadCurrentFile(uf, buf + got, (unsigned int)(total - got));
        if (chunk <= 0) break;
        got += (size_t)chunk;
    }
    unzCloseCurrentFile(uf);
    unzClose(uf);
    if (got != total) { free(buf); return false; }
    *outBuf = buf;
    *outLen = got;
    return true;
}

/* Locate the start of the outer block stream by walking the logStart
 * header layout: WBOLOMOV(8) + ver(1) + mapname(1+N) + game(8) +
 * addr(4) + port(2) + time(4) + wbnkey(32). Sets *outBlockStart and
 * *outVersion (the format version byte; v2 streams are plaintext). */
static bool locateSnapshotMarker(const uint8_t *buf, size_t len,
                                 size_t *outBlockStart, uint8_t *outVersion) {
    if (len < 8 || memcmp(buf, "WBOLOMOV", 8) != 0) return false;
    uint8_t version = buf[8];
    size_t pos = 8 + 1;            /* magic + version */
    if (pos >= len) return false;
    pos += 1 + buf[pos];           /* mapname pstr */
    pos += 8;                      /* game info */
    pos += 4;                      /* server addr */
    pos += 2;                      /* port */
    pos += 4 + 32;                 /* time + WBN key */
    if (pos >= len) return false;
    *outBlockStart = pos;
    *outVersion    = version;
    return true;
}

/* -------- The actual tests -------- */

/* Build a temp path in the current working directory. Caller is
 * responsible for unlink/remove on entry and exit so leftover files
 * from a failed previous run don't pollute the test. */
static void mkTempPath(char *out, size_t outSz, const char *tag) {
    snprintf(out, outSz, "test_log_roundtrip_%s.wbv", tag);
}

/* Common bring-up: build a sim, start a log, register at least one
 * tick so logOwnerThread pins to the current (test) thread. */
static ServerSim *startTestLog(const char *fname) {
    ServerSim *sim = ut_make_running_sim("Tester");
    if (!sim) return NULL;
    logCreate();
    if (!logStart((char *)fname, sim, /*ai*/0, /*maxPlayers*/MAX_TANKS,
                  /*usePassword*/FALSE)) {
        logDestroy();
        serverSimDestroy(sim);
        return NULL;
    }
    /* First tick pins logOwnerThread to this thread (matches production
     * — owner check is otherwise a no-op when owner is still 0). */
    logWriteTick();
    return sim;
}

int run_log_roundtrip_basic(void) {
    char fname[64];
    /* A settings blob with nothing left at a default, so a field that is
       mis-sized, reordered or byte-swapped on the way out shows up as a
       mismatch rather than matching by luck. The bytes are the wire form and
       are not checked for sense — the policy byte sets classic mode alongside
       policies the server would override, which is exactly the sort of value
       a byte-for-byte round trip has to carry unchanged. */
    static const uint8_t wantSettings[17] = {
        0xF6,               /* pill decay, base key, ally off, classic, trees */
        0x00, 0x2D,         /* pill decay 45s */
        0x01, 0x2C,         /* base decay 300s */
        0x00, 0x05,         /* ally decay 5s */
        0x03,               /* gameStrictTournament */
        0x02,               /* aiYesAdvantage */
        0x2A,               /* time limit + ranked + allow new players */
        0x00, 0x1E,         /* 30 minutes */
        0x0A, 0x41,         /* lock mask low half, bits 0/6/9/11 */
        0x80, 0x01,         /* lock mask high half, bits 16 and 31 */
        0x01                /* settings flags: smart pings banned */
    };
    char settingsBlob[18];

    settingsBlob[0] = (char)sizeof(wantSettings);
    memcpy(settingsBlob + 1, wantSettings, sizeof(wantSettings));

    mkTempPath(fname, sizeof(fname), "basic");
    remove(fname);

    ServerSim *sim = startTestLog(fname);
    UT_ASSERT_MSG(sim != NULL, "startTestLog failed");

    /* Fire a small but varied event sequence. Each logAddEvent
     * appends to logMem; logWriteTick flushes it as a LOG_EVENT block. */
    logAddEvent(log_LobbyEnter,      0, 0, 0, 0, 0, NULL);
    logAddEvent(log_CountdownStart,  0, 0, 0, 0, 0, NULL);
    logAddEvent(log_BalanceApplied,  0, 0, 0, 0, 0, NULL);
    logWriteTick();

    logAddEvent(log_LobbyExit,       0, 0, 0, 0, 0, NULL);
    logAddEvent(log_BaseSetOwner,    2, 5, 1, 0, 0, NULL);
    logAddEvent(log_PillSetHealth,   3 /* pillNum */, 10 /* armour */,
                                     0, 0, 0, NULL);
    logAddEvent(log_Shell,           42, 64, 0x42, 3, 0, NULL);
    logAddEvent(log_GameSettings,    0, 0, 0, 0, 0, settingsBlob);
    logWriteTick();

    logStop();
    logDestroy();
    serverSimDestroy(sim);

    /* Read it back and walk it. */
    uint8_t *buf = NULL;
    size_t   len = 0;
    UT_ASSERT_MSG(extractLogDat(fname, &buf, &len), "extractLogDat failed");

    size_t  blockStart = 0;
    uint8_t version    = 0;
    UT_ASSERT_MSG(locateSnapshotMarker(buf, len, &blockStart, &version),
                  "header parse failed");
    UT_ASSERT_MSG(version == LOG_VERSION, "log version = %d (want %d)",
                  version, LOG_VERSION);

    uint8_t got[64];
    int     nGot      = 0;
    int     nSnap     = 0;
    eventPayloadCapture settings;
    settings.code = log_GameSettings;
    settings.len  = -1;
    int     rc        = walkLog(buf, len, blockStart,
                                got, &nGot, (int)sizeof(got), &nSnap,
                                &settings);
    free(buf);
    remove(fname);

    UT_ASSERT_MSG(rc == 0, "walkLog rc=%d", rc);
    /* Must have seen the initial snapshot from logStart. */
    UT_ASSERT_MSG(nSnap >= 1, "expected initial snapshot, saw %d", nSnap);

    /* All eight events we fired must come back in order. */
    UT_ASSERT_MSG(nGot == 8, "expected 8 events, got %d", nGot);
    UT_ASSERT(got[0] == log_LobbyEnter);
    UT_ASSERT(got[1] == log_CountdownStart);
    UT_ASSERT(got[2] == log_BalanceApplied);
    UT_ASSERT(got[3] == log_LobbyExit);
    UT_ASSERT(got[4] == log_BaseSetOwner);
    UT_ASSERT(got[5] == log_PillSetHealth);
    UT_ASSERT(got[6] == log_Shell);
    UT_ASSERT(got[7] == log_GameSettings);

    /* The settings blob is binary and full of 0x00 bytes, so framing alone
       is not enough — all seventeen bytes have to come back untouched. */
    UT_ASSERT_MSG(settings.len == (int)sizeof(wantSettings) + 1,
                  "settings payload len = %d (want %d)",
                  settings.len, (int)sizeof(wantSettings) + 1);
    UT_ASSERT_MSG(settings.bytes[0] == (uint8_t)sizeof(wantSettings),
                  "settings length byte = %d (want %d)",
                  settings.bytes[0], (int)sizeof(wantSettings));
    for (int i = 0; i < (int)sizeof(wantSettings); i++) {
        UT_ASSERT_MSG(settings.bytes[i + 1] == wantSettings[i],
                      "settings byte %d = 0x%02X (want 0x%02X)",
                      i, (unsigned)settings.bytes[i + 1],
                      (unsigned)wantSettings[i]);
    }
    return 0;
}

/* log_Ping carries a sub-map-square position, so its two coordinates are
 * big-endian u16 rather than the map bytes most world events use. The six
 * payload bytes have to come back in that exact order, or the replay draws
 * the marker somewhere the sender never clicked. */
int run_log_roundtrip_ping(void) {
    char fname[64];
    /* Values chosen so a byte swap, a truncation to 8 bits or a swapped
       coordinate pair all show up as a mismatch rather than matching by
       luck: every byte is distinct and none is zero. */
    const uint8_t wantSender = 3;
    const uint8_t wantKind   = PING_KIND_ON_MY_WAY;
    const uint16_t wantX     = 0x1234;
    const uint16_t wantY     = 0xABCD;

    mkTempPath(fname, sizeof(fname), "ping");
    remove(fname);

    ServerSim *sim = startTestLog(fname);
    UT_ASSERT_MSG(sim != NULL, "startTestLog failed");

    /* The same call the dispatcher makes: the x halves ride opt3/opt4 and
       the whole y rides short1. */
    logAddEvent(log_Ping, wantSender, wantKind,
                (BYTE)(wantX >> 8), (BYTE)(wantX & 0xFF), wantY, NULL);
    logWriteTick();

    logStop();
    logDestroy();
    serverSimDestroy(sim);

    uint8_t *buf = NULL;
    size_t   len = 0;
    UT_ASSERT_MSG(extractLogDat(fname, &buf, &len), "extractLogDat failed");

    size_t  blockStart = 0;
    uint8_t version    = 0;
    UT_ASSERT_MSG(locateSnapshotMarker(buf, len, &blockStart, &version),
                  "header parse failed");

    uint8_t got[8];
    int     nGot  = 0;
    int     nSnap = 0;
    eventPayloadCapture ping;
    ping.code = log_Ping;
    ping.len  = -1;
    int rc = walkLog(buf, len, blockStart, got, &nGot, (int)sizeof(got),
                     &nSnap, &ping);
    free(buf);
    remove(fname);

    UT_ASSERT_MSG(rc == 0, "walkLog rc=%d", rc);
    UT_ASSERT_MSG(nGot == 1, "expected 1 event, got %d", nGot);
    UT_ASSERT(got[0] == log_Ping);
    UT_ASSERT_MSG(ping.len == 6, "ping payload len = %d (want 6)", ping.len);
    UT_ASSERT_MSG(ping.bytes[0] == wantSender, "sender = %d", ping.bytes[0]);
    UT_ASSERT_MSG(ping.bytes[1] == wantKind,   "kind = %d", ping.bytes[1]);
    UT_ASSERT_MSG(((ping.bytes[2] << 8) | ping.bytes[3]) == wantX,
                  "worldX = 0x%02X%02X", ping.bytes[2], ping.bytes[3]);
    UT_ASSERT_MSG(((ping.bytes[4] << 8) | ping.bytes[5]) == wantY,
                  "worldY = 0x%02X%02X", ping.bytes[4], ping.bytes[5]);
    return 0;
}

/* The specific scenario that triggered the original snapshot-key
 * desync bug: queue events in a tick, fire logWriteSnapshot (which
 * flushes the queued events via logWriteEvents but historically did
 * NOT re-sync logOldKey to logKey), then fire more events. If the
 * fix at log.c logWriteSnapshot ever regresses, the events after the
 * snapshot will decode as garbage and walkLog will fail. */
int run_log_roundtrip_snapshot_keeps_chain_synced(void) {
    char fname[64];
    mkTempPath(fname, sizeof(fname), "snapsync");
    remove(fname);

    ServerSim *sim = startTestLog(fname);
    UT_ASSERT_MSG(sim != NULL, "startTestLog failed");

    /* Queue several events (mixed types so logKey rotates through
     * different values before the snapshot). */
    logAddEvent(log_BaseSetOwner,   1, 3, 1, 0, 0, NULL);
    logAddEvent(log_PillSetHealth,  2, 1, 0, 0, 0, NULL);
    logAddEvent(log_Shell,          10, 20, 0x33, 1, 0, NULL);

    /* Fire a snapshot WITH the events still in logMem. This is the
     * key-sync hotspot. */
    logWriteSnapshot(sim, FALSE);

    /* More events after the snapshot — if the key chain desyncs the
     * walker will fail on one of these. */
    logAddEvent(log_LobbyEnter,     0, 0, 0, 0, 0, NULL);
    logAddEvent(log_BalanceApplied, 0, 0, 0, 0, 0, NULL);
    logAddEvent(log_CountdownStart, 0, 0, 0, 0, 0, NULL);
    logAddEvent(log_LobbyExit,      0, 0, 0, 0, 0, NULL);
    logWriteTick();

    logStop();
    logDestroy();
    serverSimDestroy(sim);

    uint8_t *buf = NULL;
    size_t   len = 0;
    UT_ASSERT_MSG(extractLogDat(fname, &buf, &len), "extractLogDat failed");

    size_t  blockStart = 0;
    uint8_t version    = 0;
    UT_ASSERT_MSG(locateSnapshotMarker(buf, len, &blockStart, &version),
                  "header parse failed");
    UT_ASSERT_MSG(version == LOG_VERSION, "log version = %d (want %d)",
                  version, LOG_VERSION);

    uint8_t got[64];
    int     nGot  = 0;
    int     nSnap = 0;
    int     rc = walkLog(buf, len, blockStart,
                         got, &nGot, (int)sizeof(got), &nSnap, NULL);
    free(buf);
    remove(fname);

    UT_ASSERT_MSG(rc == 0, "walkLog rc=%d — events after snapshot did not round-trip",
                  rc);
    /* Initial snapshot (from logStart) + mid-stream snapshot we fired. */
    UT_ASSERT_MSG(nSnap >= 2, "expected >= 2 snapshots, saw %d", nSnap);
    UT_ASSERT_MSG(nGot == 7, "expected 7 events, got %d", nGot);
    UT_ASSERT(got[0] == log_BaseSetOwner);
    UT_ASSERT(got[1] == log_PillSetHealth);
    UT_ASSERT(got[2] == log_Shell);
    UT_ASSERT(got[3] == log_LobbyEnter);
    UT_ASSERT(got[4] == log_BalanceApplied);
    UT_ASSERT(got[5] == log_CountdownStart);
    UT_ASSERT(got[6] == log_LobbyExit);
    return 0;
}

/* Lobby-mode snapshot must be the empty-world shape: a 0-pill, 0-base,
 * 0-start payload (each 1 byte long: just the count=0), a single
 * deep-sea terminator run (no body runs), and 16 2-byte not-in-use
 * player stubs. Asserting the shape catches future regressions where
 * the snapshot writer accidentally falls back to the real-world path
 * while lobby mode is on. */
int run_log_roundtrip_lobby_snapshot_is_empty_world(void) {
    char fname[64];
    mkTempPath(fname, sizeof(fname), "lobbysnap");
    remove(fname);

    ServerSim *sim = ut_make_running_sim("Tester");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim failed");

    logCreate();
    logSetLobbyMode(TRUE);
    UT_ASSERT_MSG(logStart(fname, sim, 0, MAX_TANKS, FALSE),
                  "logStart failed");
    logWriteTick();  /* pin owner thread */

    /* Fire one lobby-allowed event so we exercise the tick flush
     * after the opening snapshot — easier to spot mis-alignment. */
    logAddEvent(log_LobbyEnter, 0, 0, 0, 0, 0, NULL);
    logWriteTick();
    logStop();
    logDestroy();
    serverSimDestroy(sim);

    uint8_t *buf = NULL;
    size_t   blen = 0;
    UT_ASSERT_MSG(extractLogDat(fname, &buf, &blen), "extractLogDat failed");

    size_t  blockStart = 0;
    uint8_t version    = 0;
    UT_ASSERT_MSG(locateSnapshotMarker(buf, blen, &blockStart, &version),
                  "header parse failed");
    UT_ASSERT_MSG(version == LOG_VERSION, "log version = %d (want %d)",
                  version, LOG_VERSION);

    /* The first outer byte at blockStart is the snapshot marker
     * (LOG_EVENT_SNAPSHOT), plaintext in v2. Consume it then decode. */
    size_t pos = blockStart;
    int code = readByte(buf, blen, pos);
    pos++;
    UT_ASSERT_MSG(code == LOG_EVENT_SNAPSHOT,
                  "expected initial snapshot marker, got %d", code);

    snapshotShape shape;
    UT_ASSERT_MSG(decodeSnapshot(buf, blen, &pos, &shape),
                  "decodeSnapshot failed on lobby snapshot");
    free(buf);
    remove(fname);

    UT_ASSERT_MSG(shape.pillsPayloadLen  == 1,
                  "lobby pills payload len = %d (want 1)",
                  shape.pillsPayloadLen);
    UT_ASSERT_MSG(shape.basesPayloadLen  == 1,
                  "lobby bases payload len = %d (want 1)",
                  shape.basesPayloadLen);
    UT_ASSERT_MSG(shape.startsPayloadLen == 1,
                  "lobby starts payload len = %d (want 1)",
                  shape.startsPayloadLen);
    UT_ASSERT_MSG(shape.mapRunCount      == 0,
                  "lobby map run count = %d (want 0, terminator only)",
                  shape.mapRunCount);
    UT_ASSERT_MSG(shape.playerStubCount  == MAX_TANKS,
                  "lobby player stub count = %d (want %d)",
                  shape.playerStubCount, MAX_TANKS);
    UT_ASSERT_MSG(shape.playerFullCount  == 0,
                  "lobby player full count = %d (want 0)",
                  shape.playerFullCount);
    return 0;
}

/* In lobby mode, logAddEvent must drop world-mutation opcodes
 * (MapChange, PillSet*, BaseSet*, Shell, Sound*, etc.) so the lobby
 * segment doesn't get polluted with deltas against the deep-sea base
 * world. Lobby-flow events (LobbyEnter/Exit, joins, ready, countdown,
 * team, alliance, chat) must pass through. After clearing the flag,
 * world events resume being written. */
int run_log_roundtrip_lobby_mode_drops_world_events(void) {
    char fname[64];
    mkTempPath(fname, sizeof(fname), "lobbydrop");
    remove(fname);

    ServerSim *sim = ut_make_running_sim("Tester");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim failed");

    logCreate();
    logSetLobbyMode(TRUE);
    UT_ASSERT_MSG(logStart(fname, sim, 0, MAX_TANKS, FALSE),
                  "logStart failed");
    logWriteTick();

    /* Lobby segment — mix allowed events with world-mutation events.
     * Only the allowed events should survive the gate. */
    logAddEvent(log_LobbyEnter,     0, 0, 0, 0, 0, NULL);
    logAddEvent(log_MapChange,      10, 20, 5, 0, 0, NULL);    /* dropped */
    logAddEvent(log_TeamSet,        2, 1, 0, 0, 0, NULL);
    logAddEvent(log_PillSetHealth,  2, 1, 0, 0, 0, NULL);   /* dropped */
    logAddEvent(log_CountdownStart, 0, 0, 0, 0, 0, NULL);
    logAddEvent(log_Shell,          42, 64, 0x42, 3, 0, NULL); /* dropped */
    logAddEvent(log_AllyAccept,     1, 2, 0, 0, 0, NULL);
    logWriteTick();

    /* Game-start transition: leave lobby mode, exit lobby, run normally. */
    logSetLobbyMode(FALSE);
    logAddEvent(log_LobbyExit,      0, 0, 0, 0, 0, NULL);
    logAddEvent(log_BaseSetOwner,   3, 4, 1, 0, 0, NULL);      /* kept */
    logAddEvent(log_Shell,          50, 60, 0x33, 1, 0, NULL); /* kept */
    logWriteTick();

    logStop();
    logDestroy();
    serverSimDestroy(sim);

    uint8_t *buf = NULL;
    size_t   blen = 0;
    UT_ASSERT_MSG(extractLogDat(fname, &buf, &blen), "extractLogDat failed");

    size_t  blockStart = 0;
    uint8_t version    = 0;
    UT_ASSERT_MSG(locateSnapshotMarker(buf, blen, &blockStart, &version),
                  "header parse failed");
    UT_ASSERT_MSG(version == LOG_VERSION, "log version = %d (want %d)",
                  version, LOG_VERSION);

    uint8_t got[64];
    int     nGot      = 0;
    int     nSnap     = 0;
    int     rc        = walkLog(buf, blen, blockStart,
                                got, &nGot, (int)sizeof(got), &nSnap, NULL);
    free(buf);
    remove(fname);

    UT_ASSERT_MSG(rc == 0, "walkLog rc=%d", rc);
    UT_ASSERT_MSG(nGot == 7, "expected 7 events post-gate, got %d", nGot);
    UT_ASSERT(got[0] == log_LobbyEnter);
    UT_ASSERT(got[1] == log_TeamSet);
    UT_ASSERT(got[2] == log_CountdownStart);
    UT_ASSERT(got[3] == log_AllyAccept);
    UT_ASSERT(got[4] == log_LobbyExit);
    UT_ASSERT(got[5] == log_BaseSetOwner);
    UT_ASSERT(got[6] == log_Shell);
    return 0;
}

/* The header's map name is written when the log opens. A log that opens in
 * the lobby is started while the previous round's map is still loaded, and
 * the host chooses the round's map after that, so the header of every round
 * but a server's first named the wrong map. The round start writes
 * log_MapName with the map that is played. This drives the writer the way the
 * dedicated log does (open in lobby mode on one map, change the map, start
 * the round) and reads the file back twice: byte by byte, and through the
 * viewer. */
int run_log_roundtrip_map_name_at_round_start(void) {
    static const char kLobbyMap[] = "Oil Rigette";
    static const char kRoundMap[] = "Chew Toy 3";
    char fname[64];
    mkTempPath(fname, sizeof(fname), "mapname");
    remove(fname);

    ServerSim *sim = ut_make_running_sim("Tester");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim failed");

    /* The lobby opens with the last round's map still loaded. */
    serverSimSetMapName(sim, kLobbyMap);
    logCreate();
    logSetLobbyMode(TRUE);
    UT_ASSERT_MSG(logStart(fname, sim, 0, MAX_TANKS, FALSE),
                  "logStart failed");
    logWriteTick();
    logAddEvent(log_LobbyEnter, 0, 0, 0, 0, 0, NULL);
    logWriteTick();

    /* The host picks another map, and the countdown ends. */
    serverSimSetMapName(sim, kRoundMap);
    logSetLobbyMode(FALSE);
    logAddEvent(log_LobbyExit, 0, 0, 0, 0, 0, NULL);
    logAddMapName(sim);
    logWriteSnapshot(sim, TRUE);
    logWriteTick();

    logStop();
    logDestroy();
    serverSimDestroy(sim);

    uint8_t *buf = NULL;
    size_t   blen = 0;
    UT_ASSERT_MSG(extractLogDat(fname, &buf, &blen), "extractLogDat failed");

    /* The header is untouched: it still names the map the log opened on.
     * That is what an old reader shows, and why the record exists. */
    UT_ASSERT_MSG(blen > 10 && buf[9] == sizeof(kLobbyMap) - 1 &&
                  memcmp(buf + 10, kLobbyMap, sizeof(kLobbyMap) - 1) == 0,
                  "the header does not name the map the log opened on");

    size_t  blockStart = 0;
    uint8_t version    = 0;
    UT_ASSERT_MSG(locateSnapshotMarker(buf, blen, &blockStart, &version),
                  "header parse failed");

    uint8_t got[16];
    int     nGot  = 0;
    int     nSnap = 0;
    eventPayloadCapture name;
    name.code = log_MapName;
    name.len  = -1;
    int rc = walkLog(buf, blen, blockStart, got, &nGot, (int)sizeof(got),
                     &nSnap, &name);
    free(buf);

    UT_ASSERT_MSG(rc == 0, "walkLog rc=%d", rc);
    UT_ASSERT_MSG(nGot == 3, "expected 3 events, got %d", nGot);
    UT_ASSERT(got[0] == log_LobbyEnter);
    UT_ASSERT(got[1] == log_LobbyExit);
    UT_ASSERT(got[2] == log_MapName);
    UT_ASSERT_MSG(name.len == (int)sizeof(kRoundMap),
                  "log_MapName payload is %d bytes (want %d)", name.len,
                  (int)sizeof(kRoundMap));
    UT_ASSERT(name.bytes[0] == sizeof(kRoundMap) - 1);
    UT_ASSERT_MSG(memcmp(name.bytes + 1, kRoundMap, sizeof(kRoundMap) - 1) == 0,
                  "log_MapName does not carry the map the round started on");

    /* The viewer takes the record over the header. */
    ReplayWorld    *world = (ReplayWorld *)malloc(sizeof(*world));
    ReplayFileInfo  info;
    UT_ASSERT_MSG(world != NULL, "out of memory");
    bool decoded = replayHarnessDecodeFile(fname, world, &info);
    free(world);
    remove(fname);
    UT_ASSERT_MSG(decoded, "the viewer could not decode the file");
    UT_ASSERT_MSG(strcmp(info.mapName, kRoundMap) == 0,
                  "the viewer reports map '%s' (want '%s')", info.mapName,
                  kRoundMap);
    return 0;
}

/* log_PlayerLag is a fixed four-byte record: the player, the RTT as a
 * big-endian u16, and the rewind in 20 ms entries. This writes one through
 * logAddEvent the way serverSimLogTick does, reads the bytes back, and
 * checks the viewer still decodes the file. */
int run_log_roundtrip_player_lag(void) {
    char fname[64];
    mkTempPath(fname, sizeof(fname), "playerlag");
    remove(fname);

    ServerSim *sim = ut_make_running_sim("Tester");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim failed");

    logCreate();
    UT_ASSERT_MSG(logStart(fname, sim, 0, MAX_TANKS, FALSE),
                  "logStart failed");
    logAddEvent(log_PlayerLag, 3, 7, 0, 0, 0x0123, NULL);
    logWriteTick();
    logStop();
    logDestroy();
    serverSimDestroy(sim);

    uint8_t *buf = NULL;
    size_t   blen = 0;
    UT_ASSERT_MSG(extractLogDat(fname, &buf, &blen), "extractLogDat failed");

    size_t  blockStart = 0;
    uint8_t version    = 0;
    UT_ASSERT_MSG(locateSnapshotMarker(buf, blen, &blockStart, &version),
                  "header parse failed");

    uint8_t got[64];
    int     nGot  = 0;
    int     nSnap = 0;
    eventPayloadCapture lag;
    lag.code = log_PlayerLag;
    lag.len  = -1;
    int rc = walkLog(buf, blen, blockStart, got, &nGot, (int)sizeof(got),
                     &nSnap, &lag);
    free(buf);

    UT_ASSERT_MSG(rc == 0, "walkLog rc=%d", rc);
    UT_ASSERT_MSG(lag.len == 4, "log_PlayerLag payload is %d bytes (want 4)",
                  lag.len);
    UT_ASSERT_MSG(lag.bytes[0] == 3, "player byte is %u (want 3)", lag.bytes[0]);
    UT_ASSERT_MSG(lag.bytes[1] == 0x01 && lag.bytes[2] == 0x23,
                  "RTT bytes are %02x %02x (want 01 23)", lag.bytes[1],
                  lag.bytes[2]);
    UT_ASSERT_MSG(lag.bytes[3] == 7, "rewind byte is %u (want 7)", lag.bytes[3]);

    ReplayWorld    *world = (ReplayWorld *)malloc(sizeof(*world));
    ReplayFileInfo  info;
    UT_ASSERT_MSG(world != NULL, "out of memory");
    bool decoded = replayHarnessDecodeFile(fname, world, &info);
    free(world);
    remove(fname);
    UT_ASSERT_MSG(decoded, "the viewer could not decode the file");
    return 0;
}

/* A recorded round writes each player's lag on its own: the first recorded
 * tick of the round has a record for the player, with the RTT and the rewind
 * the server holds for that player. */
int run_log_roundtrip_player_lag_recorded(void) {
    ReplayHarness h;
    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessStartRecording(&h, "playerLagRecorded", "Tester"),
                  "replayHarnessStartRecording failed");
    replayHarnessTick(&h, 20);
    uint16_t wantRtt    = h.sim->playerPing[0];
    uint8_t  wantRewind = h.sim->statLastRewindTicks[0];
    UT_ASSERT_MSG(replayHarnessStopRecording(&h),
                  "replayHarnessStopRecording failed");

    uint8_t *buf = NULL;
    size_t   blen = 0;
    bool extracted = extractLogDat(h.path, &buf, &blen);
    if (!extracted) {
        replayHarnessStop(&h);
    }
    UT_ASSERT_MSG(extracted, "extractLogDat failed");

    size_t  blockStart = 0;
    uint8_t version    = 0;
    bool located = locateSnapshotMarker(buf, blen, &blockStart, &version);
    uint8_t got[1024];
    int     nGot  = 0;
    int     nSnap = 0;
    int     nLag  = 0;
    int     rc    = -100;
    eventPayloadCapture lag;
    lag.code = log_PlayerLag;
    lag.len  = -1;
    if (located) {
        walkAllowUnknown = true;
        rc = walkLog(buf, blen, blockStart, got, &nGot, (int)sizeof(got),
                     &nSnap, &lag);
        walkAllowUnknown = false;
    }
    free(buf);
    replayHarnessStop(&h);

    UT_ASSERT_MSG(located, "header parse failed");
    UT_ASSERT_MSG(rc == 0, "walkLog rc=%d", rc);
    for (int i = 0; i < nGot; i++) {
        if (got[i] == log_PlayerLag) {
            nLag++;
        }
    }
    /* Twenty sim ticks are ten recorded ticks, far less than a second, and
     * nothing changes: one record. */
    UT_ASSERT_MSG(nLag == 1, "%d log_PlayerLag records (want 1)", nLag);
    UT_ASSERT_MSG(lag.len == 4, "log_PlayerLag payload is %d bytes (want 4)",
                  lag.len);
    UT_ASSERT_MSG(lag.bytes[0] == 0, "player byte is %u (want 0)", lag.bytes[0]);
    UT_ASSERT_MSG(((lag.bytes[1] << 8) | lag.bytes[2]) == wantRtt,
                  "RTT is %d (want %u)", (lag.bytes[1] << 8) | lag.bytes[2],
                  (unsigned)wantRtt);
    UT_ASSERT_MSG(lag.bytes[3] == wantRewind, "rewind is %u (want %u)",
                  lag.bytes[3], (unsigned)wantRewind);
    return 0;
}

/* log_OverrunStats: a length-prefixed record of the server's timing and each
 * bot's overruns (overrun_record.h). This writes one through logAddEvent the
 * way serverSimLogOverrunStats does, reads the bytes back, checks they are
 * the encoded bytes and decode to the same record, and checks the viewer
 * still decodes the file. */
int run_log_roundtrip_overrun_stats(void) {
    char fname[64];
    OverrunRecord rec;
    OverrunRecord back;
    uint8_t payload[OVERRUN_RECORD_MAX_BYTES];
    int len;

    mkTempPath(fname, sizeof(fname), "overrunstats");
    remove(fname);

    memset(&rec, 0, sizeof(rec));
    rec.flags = OVERRUN_RECORD_FLAG_BOTS;
    rec.roundFrames = 3000u;
    rec.tickLast = 52;
    rec.tickEwma = 41;
    rec.tickPeak = 1873;
    rec.ticksOverBudget = 7;
    rec.budgetMs = 20;
    rec.simLast = 30;
    rec.simEwma = 28;
    rec.workers = 4;
    rec.activeBots = 3;
    rec.targetPerBot = 80;
    rec.brainLast = 90;
    rec.brainEwma = 85;
    rec.brainPeak = 412;
    rec.prepLast = 6;
    rec.prepEwma = 5;
    rec.totalOverruns = 70000u;
    rec.totalThinks = 0x01020304u;
    rec.botCount = 3;
    for (int i = 0; i < 3; i++) {
        rec.bots[i].player = (uint8_t)(2 + i * 4);
        rec.bots[i].last = (uint16_t)(10 + i);
        rec.bots[i].peak = (uint16_t)(300 + i);
        rec.bots[i].overruns = (uint32_t)(i * 1000);
        rec.bots[i].thinks = 90000u + (uint32_t)i;
    }
    len = overrunRecordEncode(&rec, payload, (int)sizeof(payload));
    UT_ASSERT_MSG(len == OVERRUN_RECORD_HEAD_BYTES + 3 * OVERRUN_RECORD_BOT_BYTES,
                  "encoded %d bytes", len);
    /* Big-endian fields where the layout puts them. */
    UT_ASSERT(payload[0] == OVERRUN_RECORD_VERSION && payload[1] == OVERRUN_RECORD_FLAG_BOTS);
    UT_ASSERT(payload[2] == 0 && payload[3] == 0 && payload[4] == 0x0B && payload[5] == 0xB8);
    UT_ASSERT(payload[10] == 0x07 && payload[11] == 0x51);           /* tick peak 1873 */
    UT_ASSERT(payload[39] == 1 && payload[40] == 2 && payload[41] == 3 && payload[42] == 4);
    UT_ASSERT(payload[43] == 3 && payload[44] == 2);                 /* count, player */
    /* A buffer too small gets nothing; a short record does not decode. */
    UT_ASSERT(overrunRecordEncode(&rec, payload, len - 1) == 0);
    UT_ASSERT(!overrunRecordDecode(payload, OVERRUN_RECORD_HEAD_BYTES - 1, &back));
    /* A count past the bytes there keeps the bots that are there. */
    UT_ASSERT(overrunRecordDecode(payload, len - 1, &back));
    UT_ASSERT(back.botCount == 2);

    ServerSim *sim = ut_make_running_sim("Tester");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim failed");

    logSetStopHook(NULL);
    logCreate();
    UT_ASSERT_MSG(logStart(fname, sim, 0, MAX_TANKS, FALSE),
                  "logStart failed");
    logAddEvent(log_OverrunStats, 0, 0, 0, 0, (unsigned short)len, (char *)payload);
    logWriteTick();
    logStop();
    logDestroy();
    serverSimDestroy(sim);

    uint8_t *buf = NULL;
    size_t   blen = 0;
    UT_ASSERT_MSG(extractLogDat(fname, &buf, &blen), "extractLogDat failed");

    size_t  blockStart = 0;
    uint8_t version    = 0;
    UT_ASSERT_MSG(locateSnapshotMarker(buf, blen, &blockStart, &version),
                  "header parse failed");

    uint8_t got[64];
    int     nGot  = 0;
    int     nSnap = 0;
    eventPayloadCapture cap;
    cap.code = log_OverrunStats;
    cap.len  = -1;
    int rc = walkLog(buf, blen, blockStart, got, &nGot, (int)sizeof(got),
                     &nSnap, &cap);
    free(buf);

    UT_ASSERT_MSG(rc == 0, "walkLog rc=%d", rc);
    UT_ASSERT_MSG(cap.len == len, "log_OverrunStats payload is %d bytes (want %d)",
                  cap.len, len);
    UT_ASSERT_MSG(memcmp(cap.bytes, payload, (size_t)len) == 0,
                  "the written bytes differ from the encoded ones");
    UT_ASSERT(overrunRecordDecode(cap.bytes, cap.len, &back));
    rec.version = OVERRUN_RECORD_VERSION;
    UT_ASSERT_MSG(memcmp(&back, &rec, sizeof(rec)) == 0,
                  "the decoded record differs from the one written");

    ReplayWorld    *world = (ReplayWorld *)malloc(sizeof(*world));
    ReplayFileInfo  info;
    UT_ASSERT_MSG(world != NULL, "out of memory");
    bool decoded = replayHarnessDecodeFile(fname, world, &info);
    free(world);
    remove(fname);
    UT_ASSERT_MSG(decoded, "the viewer could not decode the file");
    return 0;
}

/* Every log_OverrunStats record in a log, in order, and whether one is the
 * log's last event before LOG_QUIT. Returns the count, or -1 when the stream
 * does not walk. */
static int collectOverruns(const uint8_t *buf, size_t len, size_t pos,
                           OverrunRecord *out, int max, bool *lastIsOverrun) {
    int n = 0;
    bool lastWasOverrun = false;
    while (pos < len) {
        int code = buf[pos++];
        if (code == LOG_QUIT) {
            *lastIsOverrun = lastWasOverrun;
            return n;
        } else if (code == LOG_NOEVENTS) {
            pos += 1;
        } else if (code == LOG_NOEVENTS_LONG) {
            pos += 2;
        } else if (code == LOG_EVENT_SNAPSHOT) {
            if (!skipSnapshot(buf, len, &pos)) return -1;
            lastWasOverrun = false;
        } else if (code == LOG_EVENT || code == LOG_EVENT_LONG) {
            int count;
            if (code == LOG_EVENT) {
                if (pos + 1 > len) return -1;
                count = buf[pos];
                pos += 1;
            } else {
                if (pos + 2 > len) return -1;
                count = (buf[pos + 1] << 8) | buf[pos];
                pos += 2;
            }
            for (int i = 0; i < count; i++) {
                if (pos + 3 > len) return -1;
                int ev = buf[pos];
                int plen = (buf[pos + 1] << 8) | buf[pos + 2];
                pos += 3;
                if (pos + (size_t)plen > len) return -1;
                lastWasOverrun = (ev == log_OverrunStats);
                if (lastWasOverrun && n < max &&
                    overrunRecordDecode(buf + pos, plen, &out[n])) {
                    n++;
                }
                pos += (size_t)plen;
            }
        } else {
            return -1;
        }
    }
    return -1;
}

/* Records a 3010-frame round, with the frame count and call the server loop
 * makes, with or without the round log's stop hook, and returns its log.dat
 * (the caller frees *buf). */
static bool recordOverrunRound(const char *tag, bool hook, uint8_t **buf, size_t *blen,
                               int *reports, uint32_t *reportFrame) {
    ReplayHarness h;
    memset(&h, 0, sizeof(h));
    *buf = NULL;
    *blen = 0;
    *reports = 0;
    *reportFrame = 0;
    if (!replayHarnessPrepare(&h, tag, "Tester")) {
        replayHarnessStop(&h);
        return false;
    }
    /* The header holds the round's start time and the first log key is its
     * low byte. Two rounds made a second apart would differ in nearly every
     * byte, so both rounds use the same start time. */
    h.sim->timeCreated = 1700000000u;
    if (!replayHarnessBeginRecording(&h)) {
        replayHarnessStop(&h);
        return false;
    }
    /* What the server's round log sets, and the round start's reset. */
    logSetStopHook(hook ? serverLifecycleLogRoundEndOverrunStats : NULL);
    serverLifecycleResetTickPeak();
    for (uint32_t frame = 1; frame <= 3010u; frame++) {
        /* Two sim steps a 20 ms frame, as serverInstanceTick runs them. */
        replayHarnessTick(&h, 2);
        serverLifecycleRecordTickMs(5.0);
        if (serverLifecycleCountRoundFrame()) {
            serverLifecycleReportOverrunStats(h.sim);
            (*reports)++;
            *reportFrame = frame;
        }
    }
    bool stopped = replayHarnessStopRecording(&h);
    logSetStopHook(NULL);
    bool extracted = stopped && extractLogDat(h.path, buf, blen);
    replayHarnessStop(&h);
    remove(h.path);
    return extracted;
}

/* A recorded round gets a log_OverrunStats record at 1:00 of the round
 * (frame 3000), from the frame count and call the server loop makes, and
 * one more with the round-end flag when the round's log closes, as the log's
 * last event. The hook adds only that entry: the same round recorded without
 * the hook has the same bytes up to its LOG_QUIT. */
int run_log_roundtrip_overrun_stats_recorded(void) {
    uint8_t *offBuf = NULL;
    size_t   offLen = 0;
    int offReports = 0;
    uint32_t offFrame = 0;
    bool offOk = recordOverrunRound("overrunRecordedOff", false, &offBuf, &offLen,
                                    &offReports, &offFrame);

    uint8_t *buf = NULL;
    size_t   blen = 0;
    int reports = 0;
    uint32_t reportFrame = 0;
    bool extracted = recordOverrunRound("overrunRecorded", true, &buf, &blen,
                                        &reports, &reportFrame);
    size_t  blockStart = 0;
    uint8_t version    = 0;
    bool located = extracted && locateSnapshotMarker(buf, blen, &blockStart, &version);
    OverrunRecord recs[8];
    bool lastIsOverrun = false;
    int n = located ? collectOverruns(buf, blen, blockStart, recs, 8, &lastIsOverrun) : -2;
    OverrunRecord offRecs[8];
    bool offLastIsOverrun = false;
    size_t offStart = 0;
    uint8_t offVersion = 0;
    bool offLocated = offOk && locateSnapshotMarker(offBuf, offLen, &offStart, &offVersion);
    int offN = offLocated ? collectOverruns(offBuf, offLen, offStart, offRecs, 8,
                                            &offLastIsOverrun) : -2;
    /* Both logs end with the two LOG_QUIT bytes. */
    bool offEnds = offOk && offLen >= 2 && offBuf[offLen - 2] == LOG_QUIT &&
                   offBuf[offLen - 1] == LOG_QUIT;
    size_t offQuit = offEnds ? offLen - 2 : 0;
    bool samePrefix = offEnds && extracted && blen > offLen &&
                      memcmp(offBuf, buf, offQuit) == 0;
    free(offBuf);
    free(buf);

    UT_ASSERT_MSG(offOk, "the round without the hook did not record");
    UT_ASSERT_MSG(extracted, "the round with the hook did not record");
    UT_ASSERT_MSG(located && offLocated, "header parse failed");
    UT_ASSERT_MSG(offEnds, "the log without the hook does not end with LOG_QUIT");
    UT_ASSERT_MSG(samePrefix,
                  "the log with the hook does not start with the bytes of the log "
                  "without it (%u bytes before LOG_QUIT, %u with the hook)",
                  (unsigned)offQuit, (unsigned)blen);
    UT_ASSERT_MSG(offReports == 1 && offFrame == 3000u && offN == 1,
                  "without the hook: %d reports, last at %u, %d records (want 1, 3000, 1)",
                  offReports, (unsigned)offFrame, offN);
    UT_ASSERT_MSG(reports == 1 && reportFrame == 3000u,
                  "%d reports, the last at frame %u (want 1 at 3000)",
                  reports, (unsigned)reportFrame);
    UT_ASSERT_MSG(n == 2, "%d log_OverrunStats records (want 2)", n);
    UT_ASSERT_MSG(recs[0].roundFrames == 3000u &&
                  !(recs[0].flags & OVERRUN_RECORD_FLAG_ROUND_END),
                  "first record: frames %u flags %u (want 3000, no round end)",
                  (unsigned)recs[0].roundFrames, (unsigned)recs[0].flags);
    UT_ASSERT_MSG(recs[1].roundFrames == 3010u &&
                  (recs[1].flags & OVERRUN_RECORD_FLAG_ROUND_END),
                  "second record: frames %u flags %u (want 3010, round end)",
                  (unsigned)recs[1].roundFrames, (unsigned)recs[1].flags);
    UT_ASSERT_MSG(lastIsOverrun, "the round-end record is not the log's last event");
    /* The frame time the loop recorded, 5.0 ms, in tenths. */
    UT_ASSERT(recs[0].tickLast == 50 && recs[0].budgetMs == 20);
    UT_ASSERT(recs[0].version == OVERRUN_RECORD_VERSION);
    /* A human player and no bots. */
    UT_ASSERT(!(recs[0].flags & OVERRUN_RECORD_FLAG_BOTS) && recs[0].botCount == 0);
    return 0;
}

/* A recorded round with two bots that think, in a game whose computer tank
 * setting is "no", as a server started without -ai has: both the 1:00 record
 * and the round-end record have the bot pool and a part for each bot with
 * its thinks. The log closes before the bots are removed, as the server's
 * shutdown does it, so the round-end record keeps their parts. */
int run_log_roundtrip_overrun_stats_bots(void) {
    static const BYTE kBots[2] = { 1, 2 };
    ReplayHarness h;
    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessStartRecording(&h, "overrunBots", "Tester"),
                  "replayHarnessStartRecording failed");
    ut_brain_stub_arm(true);
    ut_brain_stub_lua(true);
    ut_brain_stub_tick_ok(true);
    bool added = true;
    for (int i = 0; i < 2; i++) {
        added = added && serverSimCreateBot(h.sim, kBots[i], "brains/GoalHunter/init.lua",
                                            i == 0 ? "BotA" : "BotB", aiNone, gameOpen,
                                            false, 0, NULL);
    }
    logSetStopHook(serverLifecycleLogRoundEndOverrunStats);
    serverLifecycleResetTickPeak();
    int reports = 0;
    if (added) {
        for (uint32_t frame = 1; frame <= 3010u; frame++) {
            /* The bots' think, then two sim steps, as serverInstanceTick
             * runs a frame. */
            serverSimBotTick(h.sim, aiNone);
            replayHarnessTick(&h, 2);
            serverLifecycleRecordTickMs(5.0);
            if (serverLifecycleCountRoundFrame()) {
                serverLifecycleReportOverrunStats(h.sim);
                reports++;
            }
        }
    }
    /* The server's shutdown order: the log closes, then the bots go. */
    bool stopped = replayHarnessStopRecording(&h);
    serverSimDestroyBots(h.sim);
    logSetStopHook(NULL);
    ut_brain_stub_tick_ok(false);
    ut_brain_stub_lua(false);
    ut_brain_stub_arm(false);

    uint8_t *buf = NULL;
    size_t   blen = 0;
    bool extracted = stopped && extractLogDat(h.path, &buf, &blen);
    size_t  blockStart = 0;
    uint8_t version    = 0;
    bool located = extracted && locateSnapshotMarker(buf, blen, &blockStart, &version);
    OverrunRecord recs[8];
    bool lastIsOverrun = false;
    int n = located ? collectOverruns(buf, blen, blockStart, recs, 8, &lastIsOverrun) : -2;
    free(buf);
    replayHarnessStop(&h);

    UT_ASSERT_MSG(added, "a bot was refused");
    UT_ASSERT_MSG(stopped, "replayHarnessStopRecording failed");
    UT_ASSERT_MSG(located, "the log did not read");
    UT_ASSERT_MSG(reports == 1, "%d reports (want 1)", reports);
    UT_ASSERT_MSG(n == 2, "%d log_OverrunStats records (want 2)", n);
    UT_ASSERT_MSG(lastIsOverrun, "the round-end record is not the log's last event");
    for (int r = 0; r < 2; r++) {
        const OverrunRecord *rec = &recs[r];
        UT_ASSERT_MSG(rec->roundFrames == (r == 0 ? 3000u : 3010u),
                      "record %d: frames %u", r, (unsigned)rec->roundFrames);
        UT_ASSERT_MSG(((rec->flags & OVERRUN_RECORD_FLAG_ROUND_END) != 0) == (r == 1),
                      "record %d: flags %u", r, (unsigned)rec->flags);
        UT_ASSERT_MSG(rec->flags & OVERRUN_RECORD_FLAG_BOTS,
                      "record %d has no bot pool part", r);
        UT_ASSERT_MSG(rec->activeBots == 2, "record %d: %u active bots (want 2)",
                      r, (unsigned)rec->activeBots);
        UT_ASSERT_MSG(rec->totalThinks > 0, "record %d: the pool has no thinks", r);
        UT_ASSERT_MSG(rec->botCount == 2, "record %d: %u bot parts (want 2)",
                      r, (unsigned)rec->botCount);
        uint32_t sum = 0;
        for (int b = 0; b < 2; b++) {
            UT_ASSERT_MSG(rec->bots[b].player == kBots[b], "record %d bot %d: player %u",
                          r, b, (unsigned)rec->bots[b].player);
            UT_ASSERT_MSG(rec->bots[b].thinks > 0,
                          "record %d: player %u has no thinks", r,
                          (unsigned)rec->bots[b].player);
            sum += rec->bots[b].thinks;
        }
        UT_ASSERT_MSG(sum == rec->totalThinks, "record %d: bot thinks %u, pool %u",
                      r, (unsigned)sum, (unsigned)rec->totalThinks);
    }
    /* The round-end record is later, so its bots have thought more. */
    UT_ASSERT(recs[1].bots[0].thinks > recs[0].bots[0].thinks);
    return 0;
}

/* When the server writes a player's lag again: when it has none yet, at the
 * round start or a snapshot, a second after the last one, or when the RTT
 * moves by 20 ms or the rewind by 2 entries. */
int run_server_lag_log_due(void) {
    /* No record yet, and an anchor, always write. */
    UT_ASSERT(serverSimLagLogDue(false, 0, 0, 0, 500, 0, 0, false));
    UT_ASSERT(serverSimLagLogDue(true, 500, 80, 4, 510, 80, 4, true));
    /* Nothing changed and under a second: no write. */
    UT_ASSERT(!serverSimLagLogDue(true, 500, 80, 4, 510, 80, 4, false));
    UT_ASSERT(!serverSimLagLogDue(true, 500, 80, 4,
                                  500 + LAG_LOG_INTERVAL_TICKS - 1, 80, 4, false));
    /* A second later. */
    UT_ASSERT(serverSimLagLogDue(true, 500, 80, 4,
                                 500 + LAG_LOG_INTERVAL_TICKS, 80, 4, false));
    /* The tick went back (a new round's count). */
    UT_ASSERT(serverSimLagLogDue(true, 500, 80, 4, 400, 80, 4, false));
    /* The RTT moves: under the step no, at the step yes, both ways. */
    UT_ASSERT(!serverSimLagLogDue(true, 500, 80, 4, 510,
                                  80 + LAG_LOG_RTT_STEP_MS - 1, 4, false));
    UT_ASSERT(serverSimLagLogDue(true, 500, 80, 4, 510,
                                 80 + LAG_LOG_RTT_STEP_MS, 4, false));
    UT_ASSERT(serverSimLagLogDue(true, 500, 80, 4, 510,
                                 80 - LAG_LOG_RTT_STEP_MS, 4, false));
    /* The rewind moves. */
    UT_ASSERT(!serverSimLagLogDue(true, 500, 80, 4, 510, 80,
                                  4 + LAG_LOG_REWIND_STEP - 1, false));
    UT_ASSERT(serverSimLagLogDue(true, 500, 80, 4, 510, 80,
                                 4 + LAG_LOG_REWIND_STEP, false));
    UT_ASSERT(serverSimLagLogDue(true, 500, 80, 4, 510, 80,
                                 4 - LAG_LOG_REWIND_STEP, false));
    return 0;
}

/* When the server prints its overrun lines during a round: at one minute of
 * game time, then at five, ten, fifteen minutes and so on, and at no other
 * frame. A frame is 20 ms, 50 a second. */
int run_server_overrun_stats_due(void) {
    uint32_t frame;
    int prints = 0;

    UT_ASSERT(SERVER_TICK_LENGTH == 20);
    UT_ASSERT(SERVER_OVERRUN_STATS_FIRST_FRAMES == 3000u);
    UT_ASSERT(SERVER_OVERRUN_STATS_INTERVAL_FRAMES == 15000u);
    UT_ASSERT(!serverOverrunStatsDue(0));
    UT_ASSERT(!serverOverrunStatsDue(1));
    UT_ASSERT(!serverOverrunStatsDue(2999));
    UT_ASSERT(serverOverrunStatsDue(3000));     /* 1:00 */
    UT_ASSERT(!serverOverrunStatsDue(3001));
    UT_ASSERT(!serverOverrunStatsDue(6000));    /* 2:00 */
    UT_ASSERT(serverOverrunStatsDue(15000));    /* 5:00 */
    UT_ASSERT(serverOverrunStatsDue(30000));    /* 10:00 */
    UT_ASSERT(serverOverrunStatsDue(45000));    /* 15:00 */
    UT_ASSERT(!serverOverrunStatsDue(45001));

    /* Twenty minutes of frames print five times: 1, 5, 10, 15, 20. */
    for (frame = 1; frame <= 20u * 60u * 50u; frame++) {
        if (serverOverrunStatsDue(frame)) {
            prints++;
        }
    }
    UT_ASSERT_MSG(prints == 5, "twenty minutes printed %d times (want 5)", prints);
    return 0;
}

/* A snapshot's player block ends with the tank's four stock values — shells,
 * mines, armour, trees, in that order — for a slot in use, and a slot that is
 * not in use stays the 2-byte stub. Walks the block field by field (the field
 * order the reader consumes) rather than trusting a fixed offset, so a change
 * to the name / location / alliance sections still lands on the stocks.
 *
 * The four values are set to distinct numbers first: they are all 40 at spawn,
 * so a writer that swapped two of them would round-trip by luck otherwise. */
int run_log_roundtrip_snapshot_tank_stocks(void) {
    char fname[64];
    const uint8_t wantShells = 7;
    const uint8_t wantMines  = 11;
    const uint8_t wantArmour = 23;
    const uint8_t wantTrees  = 5;

    mkTempPath(fname, sizeof(fname), "tankstocks");
    remove(fname);

    ServerSim *sim = ut_make_running_sim("Tester");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim failed");
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL && gs->tanks[0] != NULL, "no slot-0 tank");
    tankSetShells(gs, &gs->tanks[0], wantShells);
    tankSetMines(gs, &gs->tanks[0], wantMines);
    tankSetArmour(&gs->tanks[0], wantArmour);
    tankSetTrees(gs, &gs->tanks[0], wantTrees);

    /* logStart writes the opening snapshot, which is the one decoded below.
     * Lobby mode would make that an empty world with 16 stubs, and it is
     * process state an earlier test may have left on, so say which one. */
    logCreate();
    logSetLobbyMode(FALSE);
    UT_ASSERT_MSG(logStart(fname, sim, 0, MAX_TANKS, FALSE), "logStart failed");
    logWriteTick();
    logStop();
    logDestroy();
    serverSimDestroy(sim);

    uint8_t *buf = NULL;
    size_t   blen = 0;
    UT_ASSERT_MSG(extractLogDat(fname, &buf, &blen), "extractLogDat failed");

    size_t  blockStart = 0;
    uint8_t version    = 0;
    UT_ASSERT_MSG(locateSnapshotMarker(buf, blen, &blockStart, &version),
                  "header parse failed");

    size_t pos = blockStart;
    int code = readByte(buf, blen, pos);
    pos++;
    UT_ASSERT_MSG(code == LOG_EVENT_SNAPSHOT,
                  "expected initial snapshot marker, got %d", code);

    snapshotShape shape;
    UT_ASSERT_MSG(decodeSnapshot(buf, blen, &pos, &shape),
                  "decodeSnapshot failed");
    free(buf);
    remove(fname);

    /* One player joined, so one full block and fifteen not-in-use stubs. */
    UT_ASSERT_MSG(shape.playerFullCount == 1,
                  "full player blocks = %d (want 1)", shape.playerFullCount);
    UT_ASSERT_MSG(shape.playerStubCount == MAX_TANKS - 1,
                  "player stubs = %d (want %d)",
                  shape.playerStubCount, MAX_TANKS - 1);

    /* Walk slot 0: 2 header bytes, 9 fixed fields, name, location, alliances,
     * then the stocks. */
    const uint8_t *b = shape.slot0Block;
    int blockLen = shape.slot0BlockLen;
    UT_ASSERT_MSG(b[0] == 0, "block player number = %d (want 0)", b[0]);
    UT_ASSERT_MSG(b[1] != 0, "slot 0 not marked in use");
    int p = 11;                       /* past the 2 header bytes + 9 fixed */
    UT_ASSERT_MSG(p < blockLen, "block ends before the name");
    p += 1 + b[p];                    /* name */
    UT_ASSERT_MSG(p < blockLen, "block ends before the location");
    p += 1 + b[p];                    /* location */
    UT_ASSERT_MSG(p < blockLen, "block ends before the alliances");
    p += 1 + b[p];                    /* alliance count + entries */

    UT_ASSERT_MSG(p + 4 == blockLen,
                  "%d stock bytes on the end of the block (want 4)",
                  blockLen - p);
    UT_ASSERT_MSG(b[p]     == wantShells, "shells = %d (want %d)", b[p], wantShells);
    UT_ASSERT_MSG(b[p + 1] == wantMines,  "mines = %d (want %d)", b[p+1], wantMines);
    UT_ASSERT_MSG(b[p + 2] == wantArmour, "armour = %d (want %d)", b[p+2], wantArmour);
    UT_ASSERT_MSG(b[p + 3] == wantTrees,  "trees = %d (want %d)", b[p+3], wantTrees);
    return 0;
}

/* log_TankSetStock carries the player and the four values, and only goes out
 * when one of them changed since the last record for that tank: the per-tick
 * pass offers one for every connected tank, so without that rule every tank
 * would write five bytes every tick. */
int run_log_roundtrip_tank_stock_record(void) {
    char fname[64];
    mkTempPath(fname, sizeof(fname), "stockrecord");
    remove(fname);

    /* log_TankSetStock is a world event, so lobby mode would drop it — it is
     * process state an earlier test may have left on, so say which one. */
    logSetLobbyMode(FALSE);
    ServerSim *sim = startTestLog(fname);
    UT_ASSERT_MSG(sim != NULL, "startTestLog failed");

    /* Distinct values so a mis-ordered payload cannot match by luck. */
    logAddEvent(log_TankSetStock, 3, 12, 5, 34, 7, NULL);  /* first: written */
    logAddEvent(log_TankSetStock, 3, 12, 5, 34, 7, NULL);  /* same: dropped */
    logAddEvent(log_TankSetStock, 3, 12, 5, 34, 6, NULL);  /* trees moved: written */
    logAddEvent(log_TankSetStock, 4, 12, 5, 34, 6, NULL);  /* other tank: written */
    logWriteTick();

    logStop();
    logDestroy();
    serverSimDestroy(sim);

    uint8_t *buf = NULL;
    size_t   blen = 0;
    UT_ASSERT_MSG(extractLogDat(fname, &buf, &blen), "extractLogDat failed");

    size_t  blockStart = 0;
    uint8_t version    = 0;
    UT_ASSERT_MSG(locateSnapshotMarker(buf, blen, &blockStart, &version),
                  "header parse failed");

    uint8_t got[8];
    int     nGot  = 0;
    int     nSnap = 0;
    eventPayloadCapture stock;
    stock.code = log_TankSetStock;
    stock.len  = -1;
    int rc = walkLog(buf, blen, blockStart, got, &nGot, (int)sizeof(got),
                     &nSnap, &stock);
    free(buf);
    remove(fname);

    UT_ASSERT_MSG(rc == 0, "walkLog rc=%d", rc);
    UT_ASSERT_MSG(nGot == 3, "expected 3 events (one dropped), got %d", nGot);
    UT_ASSERT(got[0] == log_TankSetStock);
    UT_ASSERT(got[1] == log_TankSetStock);
    UT_ASSERT(got[2] == log_TankSetStock);
    UT_ASSERT_MSG(stock.len == 5, "payload len = %d (want 5)", stock.len);
    UT_ASSERT_MSG(stock.bytes[0] == 3,  "player = %d", stock.bytes[0]);
    UT_ASSERT_MSG(stock.bytes[1] == 12, "shells = %d", stock.bytes[1]);
    UT_ASSERT_MSG(stock.bytes[2] == 5,  "mines = %d", stock.bytes[2]);
    UT_ASSERT_MSG(stock.bytes[3] == 34, "armour = %d", stock.bytes[3]);
    UT_ASSERT_MSG(stock.bytes[4] == 7,  "trees = %d", stock.bytes[4]);
    return 0;
}
