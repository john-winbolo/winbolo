/*
 * Round-trip coverage for the dedicated-server replay log writer.
 *
 * Drives log.c's public API the same way the dedicated server does
 * (logStart → logAddEvent → logWriteTick → logStop), reads the
 * resulting .wbv back out of the zip, and walks the inner log.dat byte
 * stream by hand to assert every event we wrote comes back in order
 * with a clean LOG_QUIT terminator.
 *
 * Specifically asserts the snapshot/key-sync invariant:
 *   - When logWriteSnapshot fires WITH events queued, the events
 *     branch must re-sync logOldKey to logKey before writing the
 *     snapshot marker, otherwise the XOR-key chain desyncs and every
 *     byte after the snapshot becomes unparseable. (The bug fixed in
 *     commit 18ad9be.)
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
#include "log.h"
#include "server_sim.h"
#include "unzip.h"
#include "test_harness.h"

/* -------- Wire-format walker (self-contained on purpose) ---------- */

/* Per-event "bytes-after-code-byte" table. For variable-length
 * events (pstr suffix) the entry is -1 and the walker handles them
 * inline. Indexed by logitem code (1..46). */
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
        case log_PillSetHealth:    return 1;
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
        default:                  return -1;  /* not variable */
    }
}

/* Pull a single byte from buf[pos] XOR'd with key. Returns -1 on EOF. */
static int readByte(const uint8_t *buf, size_t len, size_t pos, uint8_t key) {
    if (pos >= len) return -1;
    return buf[pos] ^ key;
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
} snapshotShape;

/* Decode the snapshot at *pos, advancing *pos past it. Fills *shape with
 * the per-section sizes so the caller can assert on the lobby shape. */
static bool decodeSnapshot(const uint8_t *buf, size_t len, size_t *pos, uint8_t key,
                           snapshotShape *shape) {
    size_t p = *pos;
    int n;
    memset(shape, 0, sizeof(*shape));
    if (p + 8 > len) return false;
    p += 8;                                      /* startDelay + timeLimit */
    if ((n = readByte(buf, len, p, key)) < 0) return false;
    shape->pillsPayloadLen = n;
    p += 1 + (size_t)n;
    if ((n = readByte(buf, len, p, key)) < 0) return false;
    shape->basesPayloadLen = n;
    p += 1 + (size_t)n;
    if ((n = readByte(buf, len, p, key)) < 0) return false;
    shape->startsPayloadLen = n;
    p += 1 + (size_t)n;
    while (1) {
        if (p + 4 > len) return false;
        int dlen = readByte(buf, len, p,     key);
        int y    = readByte(buf, len, p + 1, key);
        int sx   = readByte(buf, len, p + 2, key);
        int ex   = readByte(buf, len, p + 3, key);
        p += 4;
        if (dlen == 4 && y == 255 && sx == 255 && ex == 255) break;
        if (dlen < 4) return false;
        p += (size_t)(dlen - 4);
        shape->mapRunCount++;
    }
    for (int i = 0; i < MAX_TANKS; i++) {
        if ((n = readByte(buf, len, p, key)) < 0) return false;
        if (n == 2) shape->playerStubCount++;
        else        shape->playerFullCount++;
        p += 1 + (size_t)n;
    }
    *pos = p;
    return true;
}

/* Skip a full snapshot body (post-marker), updating pos in place.
 * Returns false on short read or malformed map-run terminator. */
static bool skipSnapshot(const uint8_t *buf, size_t len, size_t *pos, uint8_t key) {
    size_t p = *pos;
    if (p + 8 > len) return false;
    p += 8;                                      /* startDelay + timeLimit */
    int n;
    /* pills */
    if ((n = readByte(buf, len, p, key)) < 0) return false;
    p += 1 + (size_t)n;
    /* bases */
    if ((n = readByte(buf, len, p, key)) < 0) return false;
    p += 1 + (size_t)n;
    /* starts */
    if ((n = readByte(buf, len, p, key)) < 0) return false;
    p += 1 + (size_t)n;
    /* map runs until terminator (datalen=4, y=255, sx=255, ex=255) */
    while (1) {
        if (p + 4 > len) return false;
        int dlen = readByte(buf, len, p,     key);
        int y    = readByte(buf, len, p + 1, key);
        int sx   = readByte(buf, len, p + 2, key);
        int ex   = readByte(buf, len, p + 3, key);
        p += 4;
        if (dlen == 4 && y == 255 && sx == 255 && ex == 255) break;
        if (dlen < 4) return false;
        p += (size_t)(dlen - 4);
    }
    /* 16 players */
    for (int i = 0; i < MAX_TANKS; i++) {
        if ((n = readByte(buf, len, p, key)) < 0) return false;
        p += 1 + (size_t)n;
    }
    *pos = p;
    return true;
}

/* Walk the buffer from `startPos` (which must point at the first
 * outer byte AFTER the initial logStart header) and fill `eventCodes`
 * with every event code decoded in order. Returns 0 on a clean
 * LOG_QUIT terminator; -N on the first failure (N indicating which
 * stage broke, useful for human eyeballing on test failure). */
static int walkLog(const uint8_t *buf, size_t len, size_t startPos, uint8_t initKey,
                   uint8_t *eventCodes, int *outEventCount, int maxCodes,
                   int *outSnapshotCount) {
    size_t pos = startPos;
    uint8_t key = initKey;
    int ec = 0;
    int sc = 0;
    while (pos < len) {
        int code = readByte(buf, len, pos, key); pos++;
        if (code < 0) return -1;
        if (code == LOG_QUIT) {
            *outEventCount    = ec;
            *outSnapshotCount = sc;
            return 0;
        } else if (code == LOG_NOEVENTS) {
            if (readByte(buf, len, pos, key) < 0) return -2;
            pos += 1;
        } else if (code == LOG_NOEVENTS_LONG) {
            if (pos + 2 > len) return -2;
            pos += 2;
        } else if (code == LOG_EVENT_SNAPSHOT) {
            if (!skipSnapshot(buf, len, &pos, key)) return -3;
            sc++;
            /* Snapshot body uses same key throughout; reader's blockKey
             * unchanged across the snapshot. */
        } else if (code == LOG_EVENT || code == LOG_EVENT_LONG) {
            int n;
            if (code == LOG_EVENT) {
                n = readByte(buf, len, pos, key); pos += 1;
                if (n < 0) return -4;
            } else {
                if (pos + 2 > len) return -4;
                uint8_t hi = buf[pos]   ^ key;
                uint8_t lo = buf[pos+1] ^ key;
                /* htons in writer; reader reverses with ntohs. On any
                 * little-endian (or big-endian) host the on-disk order
                 * is hi-then-lo, so the count is hi<<8 | lo... but
                 * ntohs in the actual reader then swaps it back. The
                 * writer/reader pair is consistent; for our walker
                 * decoded count is (lo << 8) | hi to mirror reader. */
                n = (lo << 8) | hi;
                pos += 2;
            }
            uint8_t blockKey = key;  /* key for the block stays at the
                                      * pre-block value for the first
                                      * event's code byte; advances per
                                      * event below mirroring the
                                      * writer's logKey rotation. */
            for (int i = 0; i < n; i++) {
                int ev = readByte(buf, len, pos, blockKey); pos += 1;
                if (ev < 0) return -5;
                int fixed = eventFixedBytesAfterCode((uint8_t)ev);
                if (fixed == -2) return -6;
                if (fixed >= 0) {
                    if (pos + (size_t)fixed > len) return -7;
                    pos += (size_t)fixed;
                } else {
                    int pref = variableEventPrefixBytes((uint8_t)ev);
                    if (pref < 0) return -8;
                    pos += (size_t)pref;
                    int plen = readByte(buf, len, pos, blockKey); pos += 1;
                    if (plen < 0) return -9;
                    if (pos + (size_t)plen > len) return -10;
                    pos += (size_t)plen;
                }
                if (ec < maxCodes) {
                    eventCodes[ec++] = (uint8_t)ev;
                }
                blockKey = (uint8_t)ev;
            }
            key = blockKey;  /* outer key advances to last event's code */
        } else {
            return -11;
        }
    }
    return -12;  /* hit EOF without LOG_QUIT */
}

/* -------- Test-side zip extraction (uses minizip's unz API) -------- */

/* Reads the inner "log.dat" out of `wbvPath` into `*outBuf` / `*outLen`
 * (caller frees). Returns false on any error. */
static bool extractLogDat(const char *wbvPath, uint8_t **outBuf, size_t *outLen) {
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
 * *outInitKey (= time & 0xFF). */
static bool locateSnapshotMarker(const uint8_t *buf, size_t len,
                                 size_t *outBlockStart, uint8_t *outInitKey) {
    if (len < 8 || memcmp(buf, "WBOLOMOV", 8) != 0) return false;
    size_t pos = 8 + 1;            /* magic + version */
    if (pos >= len) return false;
    pos += 1 + buf[pos];           /* mapname pstr (header bytes are NOT XOR'd) */
    pos += 8;                      /* game info */
    pos += 4;                      /* server addr */
    pos += 2;                      /* port */
    if (pos + 4 > len) return false;
    uint32_t createTime = ((uint32_t)buf[pos]     << 24) |
                          ((uint32_t)buf[pos + 1] << 16) |
                          ((uint32_t)buf[pos + 2] <<  8) |
                          ((uint32_t)buf[pos + 3]);
    pos += 4 + 32;                 /* time + WBN key */
    if (pos >= len) return false;
    *outBlockStart = pos;
    *outInitKey    = (uint8_t)(createTime & 0xFF);
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
    logAddEvent(log_PillSetHealth,   0x3A /* pillNum=3,armour=10 */,
                                     0, 0, 0, 0, NULL);
    logAddEvent(log_Shell,           42, 64, 0x42, 3, 0, NULL);
    logWriteTick();

    logStop();
    logDestroy();
    serverSimDestroy(sim);

    /* Read it back and walk it. */
    uint8_t *buf = NULL;
    size_t   len = 0;
    UT_ASSERT_MSG(extractLogDat(fname, &buf, &len), "extractLogDat failed");

    size_t  blockStart = 0;
    uint8_t initKey    = 0;
    UT_ASSERT_MSG(locateSnapshotMarker(buf, len, &blockStart, &initKey),
                  "header parse failed");

    uint8_t got[64];
    int     nGot      = 0;
    int     nSnap     = 0;
    int     rc        = walkLog(buf, len, blockStart, initKey,
                                got, &nGot, (int)sizeof(got), &nSnap);
    free(buf);
    remove(fname);

    UT_ASSERT_MSG(rc == 0, "walkLog rc=%d", rc);
    /* Must have seen the initial snapshot from logStart. */
    UT_ASSERT_MSG(nSnap >= 1, "expected initial snapshot, saw %d", nSnap);

    /* All seven events we fired must come back in order. */
    UT_ASSERT_MSG(nGot == 7, "expected 7 events, got %d", nGot);
    UT_ASSERT(got[0] == log_LobbyEnter);
    UT_ASSERT(got[1] == log_CountdownStart);
    UT_ASSERT(got[2] == log_BalanceApplied);
    UT_ASSERT(got[3] == log_LobbyExit);
    UT_ASSERT(got[4] == log_BaseSetOwner);
    UT_ASSERT(got[5] == log_PillSetHealth);
    UT_ASSERT(got[6] == log_Shell);
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
    logAddEvent(log_PillSetHealth,  0x21, 0, 0, 0, 0, NULL);
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
    uint8_t initKey    = 0;
    UT_ASSERT_MSG(locateSnapshotMarker(buf, len, &blockStart, &initKey),
                  "header parse failed");

    uint8_t got[64];
    int     nGot  = 0;
    int     nSnap = 0;
    int     rc = walkLog(buf, len, blockStart, initKey,
                         got, &nGot, (int)sizeof(got), &nSnap);
    free(buf);
    remove(fname);

    UT_ASSERT_MSG(rc == 0, "walkLog rc=%d — chain likely desynced at snapshot",
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
    uint8_t initKey    = 0;
    UT_ASSERT_MSG(locateSnapshotMarker(buf, blen, &blockStart, &initKey),
                  "header parse failed");

    /* The first outer byte at blockStart is the snapshot marker
     * (LOG_EVENT_SNAPSHOT) XOR'd with initKey. Consume it then decode. */
    size_t pos = blockStart;
    int code = readByte(buf, blen, pos, initKey);
    pos++;
    UT_ASSERT_MSG(code == LOG_EVENT_SNAPSHOT,
                  "expected initial snapshot marker, got %d", code);

    snapshotShape shape;
    UT_ASSERT_MSG(decodeSnapshot(buf, blen, &pos, initKey, &shape),
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
    logAddEvent(log_PillSetHealth,  0x21, 0, 0, 0, 0, NULL);   /* dropped */
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
    uint8_t initKey    = 0;
    UT_ASSERT_MSG(locateSnapshotMarker(buf, blen, &blockStart, &initKey),
                  "header parse failed");

    uint8_t got[64];
    int     nGot      = 0;
    int     nSnap     = 0;
    int     rc        = walkLog(buf, blen, blockStart, initKey,
                                got, &nGot, (int)sizeof(got), &nSnap);
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
