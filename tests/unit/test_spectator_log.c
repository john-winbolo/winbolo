/*
 * Spectator replay-log coverage.
 *
 * Two gates over the additive spectator log events (log_SpectatorJoined,
 * log_SpectatorLeft, log_SpectatorChat) on the v2 length-framed .wbv stream:
 *
 *   (1) Serialize round-trip: fire each of the three kinds through the real
 *       logAddEvent → logWriteTick → .wbv writer, read the inner log.dat back
 *       out of the zip, and walk the framed byte stream to assert every field
 *       (type, slot, country, wbnFlags, reserved, and the pascal-string name /
 *       message) comes back exactly as written. This proves the on-disk shape
 *       of all three kinds — including the chat kind that has no emitter yet.
 *
 *   (2) Loopback emission: a real spectator joining a recording loopback server
 *       writes a log_SpectatorJoined; aging it out (timeout) writes a
 *       log_SpectatorLeft. Proves serverAcceptSpectator / serverDisconnect-
 *       Spectator emit the right kinds into a live log.
 *
 * The stream walker here is length-driven (each event is [type][u16 BE payload
 * length][payload]) and deliberately independent of src/logviewer/'s reader —
 * it is the writer-side wire shape encoded as C, so a writer bug fails here with
 * the actual byte position, not a generic "log corrupt" later.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"            /* MAX_TANKS */
#include "log.h"
#include "server_sim.h"
#include "transport_udp.h"     /* transportUdpServerGetSpectatorCount */
#include "test_harness.h"
#include "loopback_harness.h"

/* -------- Captured-event model -------- */

#define SL_MAX_EVENTS   512
#define SL_MAX_PAYLOAD  264

typedef struct {
    uint8_t type;
    int     payloadLen;                 /* bytes after the u16 length field */
    uint8_t payload[SL_MAX_PAYLOAD];
} SlEvent;

typedef struct {
    SlEvent ev[SL_MAX_EVENTS];
    int     count;
} SlEvents;

/* -------- v2 stream walker (length-driven, self-contained) -------- */

static int slReadByte(const uint8_t *buf, size_t len, size_t pos) {
    if (pos >= len) return -1;
    return buf[pos];
}

/* Skip a snapshot body (post-marker): startDelay+timeLimit, then the
 * pills/bases/starts count-prefixed sections, the map runs up to the
 * deep-sea terminator, then MAX_TANKS player blocks. Plaintext, not
 * length-framed. Mirrors the v2 snapshot layout. */
static bool slSkipSnapshot(const uint8_t *buf, size_t len, size_t *pos) {
    size_t p = *pos;
    int n;
    if (p + 8 > len) return false;
    p += 8;
    if ((n = slReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n; /* pills */
    if ((n = slReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n; /* bases */
    if ((n = slReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n; /* starts */
    while (1) {
        if (p + 4 > len) return false;
        int dlen = slReadByte(buf, len, p);
        int y    = slReadByte(buf, len, p + 1);
        int sx   = slReadByte(buf, len, p + 2);
        int ex   = slReadByte(buf, len, p + 3);
        p += 4;
        if (dlen == 4 && y == 255 && sx == 255 && ex == 255) break;
        if (dlen < 4) return false;
        p += (size_t)(dlen - 4);
    }
    for (int i = 0; i < MAX_TANKS; i++) {
        if ((n = slReadByte(buf, len, p)) < 0) return false;
        p += 1 + (size_t)n;
    }
    *pos = p;
    return true;
}

/* Locate the first outer block byte after the logStart header:
 * WBOLOMOV(8) + ver(1) + mapname(1+N) + game(8) + addr(4) + port(2) +
 * time(4) + wbnkey(32). */
static bool slLocateBlockStart(const uint8_t *buf, size_t len,
                               size_t *outBlockStart, uint8_t *outVersion) {
    if (len < 8 || memcmp(buf, "WBOLOMOV", 8) != 0) return false;
    uint8_t version = buf[8];
    size_t pos = 8 + 1;
    if (pos >= len) return false;
    pos += 1 + buf[pos];   /* mapname pstr */
    pos += 8;              /* game info */
    pos += 4;              /* server addr */
    pos += 2;              /* port */
    pos += 4 + 32;         /* time + WBN key */
    if (pos >= len) return false;
    *outBlockStart = pos;
    *outVersion    = version;
    return true;
}

/* Walk the outer block stream from blockStart, capturing every framed event
 * (type + payload). Snapshots are skipped. Returns true on a clean LOG_QUIT. */
static bool slWalk(const uint8_t *buf, size_t len, size_t blockStart,
                   SlEvents *out) {
    size_t pos = blockStart;
    out->count = 0;
    while (pos < len) {
        int code = slReadByte(buf, len, pos); pos++;
        if (code < 0) return false;
        if (code == LOG_QUIT) {
            return true;
        } else if (code == LOG_NOEVENTS) {
            if (slReadByte(buf, len, pos) < 0) return false;
            pos += 1;
        } else if (code == LOG_NOEVENTS_LONG) {
            if (pos + 2 > len) return false;
            pos += 2;
        } else if (code == LOG_EVENT_SNAPSHOT) {
            if (!slSkipSnapshot(buf, len, &pos)) return false;
        } else if (code == LOG_EVENT || code == LOG_EVENT_LONG) {
            int n;
            if (code == LOG_EVENT) {
                n = slReadByte(buf, len, pos); pos += 1;
                if (n < 0) return false;
            } else {
                if (pos + 2 > len) return false;
                /* writer stores data[1]=low, data[2]=high; reader rebuilds
                 * the count as (lo << 8) | hi. */
                uint8_t hi = buf[pos];
                uint8_t lo = buf[pos + 1];
                n = (lo << 8) | hi;
                pos += 2;
            }
            for (int i = 0; i < n; i++) {
                if (pos + 3 > len) return false;       /* type + u16 len */
                int ev   = buf[pos];
                int plen = (buf[pos + 1] << 8) | buf[pos + 2];
                size_t payloadStart = pos + 3;
                if (payloadStart + (size_t)plen > len) return false;
                /* Capture only the spectator kinds. A long-running world can
                 * emit far more than SL_MAX_EVENTS unrelated events before the
                 * timeout-driven log_SpectatorLeft; filtering keeps that late
                 * event reachable regardless of world chatter. */
                if ((ev == log_SpectatorJoined || ev == log_SpectatorLeft ||
                     ev == log_SpectatorChat) &&
                    out->count < SL_MAX_EVENTS && plen <= SL_MAX_PAYLOAD) {
                    SlEvent *e = &out->ev[out->count++];
                    e->type       = (uint8_t)ev;
                    e->payloadLen = plen;
                    memcpy(e->payload, buf + payloadStart, (size_t)plen);
                }
                pos = payloadStart + (size_t)plen;
            }
        } else {
            return false;
        }
    }
    return false;  /* hit EOF without LOG_QUIT */
}

/* Return the first captured event of the given type, or NULL. */
static const SlEvent *slFind(const SlEvents *evs, uint8_t type) {
    int i;
    for (i = 0; i < evs->count; i++) {
        if (evs->ev[i].type == type) return &evs->ev[i];
    }
    return NULL;
}

/* Read the framed stream out of `fname` and walk it into `out`. */
static bool slReadBack(const char *fname, SlEvents *out) {
    uint8_t *buf = NULL;
    size_t   len = 0;
    size_t   blockStart = 0;
    uint8_t  version = 0;
    bool     ok;
    if (!extractLogDat(fname, &buf, &len)) return false;
    if (!slLocateBlockStart(buf, len, &blockStart, &version) ||
        version != LOG_VERSION) {
        free(buf);
        return false;
    }
    ok = slWalk(buf, len, blockStart, out);
    free(buf);
    return ok;
}

/* Build a pascal string (len byte + bytes) into dst from a C string. */
static void slMakePstr(char *dst, const char *s) {
    int n = (int)strlen(s);
    if (n > 255) n = 255;
    dst[0] = (char)n;
    memcpy(dst + 1, s, (size_t)n);
}

/* -------- (1) serialize round-trip -------- */

static int slRoundtrip(void) {
    const char *fname = "test_spectator_log_roundtrip.wbv";
    char        pstr[256];
    ServerSim  *sim;
    SlEvents    evs;
    const SlEvent *e;

    remove(fname);
    sim = ut_make_running_sim("Tester");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim failed");
    logCreate();
    UT_ASSERT_MSG(logStart((char *)fname, sim, 0, MAX_TANKS, FALSE) == TRUE,
                  "logStart failed");
    logWriteTick();  /* pin owner thread to this thread */

    /* JOIN: slot 7, country "GB", wbnFlags 0x01, reserved 0, name "Watcher". */
    slMakePstr(pstr, "Watcher");
    logAddEvent(log_SpectatorJoined, 7, 'G', 'B', 0x01, 0, pstr);
    /* LEFT: slot 7, name "Watcher". */
    slMakePstr(pstr, "Watcher");
    logAddEvent(log_SpectatorLeft, 7, 0, 0, 0, 0, pstr);
    /* CHAT (format-reserved): sender slot 3, message "hi all". */
    slMakePstr(pstr, "hi all");
    logAddEvent(log_SpectatorChat, 3, 0, 0, 0, 0, pstr);
    logWriteTick();

    logStop();
    logDestroy();
    serverSimDestroy(sim);

    UT_ASSERT_MSG(slReadBack(fname, &evs), "read-back/walk failed");
    remove(fname);

    /* JOIN fields: slot, country[0], country[1], wbnFlags, reserved, name. */
    e = slFind(&evs, log_SpectatorJoined);
    UT_ASSERT_MSG(e != NULL, "log_SpectatorJoined missing from stream");
    UT_ASSERT_MSG(e->payloadLen == 5 + 1 + 7,
                  "joined payload len = %d (want %d)", e->payloadLen, 5 + 1 + 7);
    UT_ASSERT_MSG(e->payload[0] == 7, "joined slot = %d", e->payload[0]);
    UT_ASSERT_MSG(e->payload[1] == 'G', "joined country[0] = %d", e->payload[1]);
    UT_ASSERT_MSG(e->payload[2] == 'B', "joined country[1] = %d", e->payload[2]);
    UT_ASSERT_MSG(e->payload[3] == 0x01, "joined wbnFlags = %d", e->payload[3]);
    UT_ASSERT_MSG(e->payload[4] == 0, "joined reserved = %d", e->payload[4]);
    UT_ASSERT_MSG(e->payload[5] == 7, "joined name len = %d", e->payload[5]);
    UT_ASSERT_MSG(memcmp(e->payload + 6, "Watcher", 7) == 0,
                  "joined name mismatch");

    /* LEFT fields: slot + name. */
    e = slFind(&evs, log_SpectatorLeft);
    UT_ASSERT_MSG(e != NULL, "log_SpectatorLeft missing from stream");
    UT_ASSERT_MSG(e->payloadLen == 1 + 1 + 7,
                  "left payload len = %d (want %d)", e->payloadLen, 1 + 1 + 7);
    UT_ASSERT_MSG(e->payload[0] == 7, "left slot = %d", e->payload[0]);
    UT_ASSERT_MSG(e->payload[1] == 7, "left name len = %d", e->payload[1]);
    UT_ASSERT_MSG(memcmp(e->payload + 2, "Watcher", 7) == 0,
                  "left name mismatch");

    /* CHAT fields: sender slot + message. */
    e = slFind(&evs, log_SpectatorChat);
    UT_ASSERT_MSG(e != NULL, "log_SpectatorChat missing from stream");
    UT_ASSERT_MSG(e->payloadLen == 1 + 1 + 6,
                  "chat payload len = %d (want %d)", e->payloadLen, 1 + 1 + 6);
    UT_ASSERT_MSG(e->payload[0] == 3, "chat slot = %d", e->payload[0]);
    UT_ASSERT_MSG(e->payload[1] == 6, "chat msg len = %d", e->payload[1]);
    UT_ASSERT_MSG(memcmp(e->payload + 2, "hi all", 6) == 0,
                  "chat message mismatch");
    return 0;
}

/* -------- (2) loopback emission -------- */

static bool slSpectatorSeated(LoopbackHarness *h, void *user) {
    (void)h; (void)user;
    return transportUdpServerGetSpectatorCount() >= 1;
}

static bool slNoSpectators(LoopbackHarness *h, void *user) {
    (void)h; (void)user;
    return transportUdpServerGetSpectatorCount() == 0;
}

static int slEmission(void) {
    const char *fname = "test_spectator_log_emit.wbv";
    LoopbackHarness h;
    SlEvents evs;
    int seatedAt, goneAt;

    /* Server up + a real tankless spectator connect initiated (not yet pumped,
     * so the accept has not run). Start recording BEFORE pumping so the join is
     * captured. */
    UT_ASSERT_MSG(loopbackHarnessStartSpectator(&h, "SpecLog", /*seed*/ 1u),
                  "spectator harness start failed");

    remove(fname);
    logCreate();
    if (logStart((char *)fname, h.sim, 0, MAX_TANKS, FALSE) != TRUE) {
        loopbackHarnessStop(&h);
        UT_FAIL("logStart failed");
    }

    /* Pump until the viewer is accepted — serverAcceptSpectator emits
     * log_SpectatorJoined into the recording log. */
    seatedAt = loopbackHarnessPumpUntil(&h, 800, slSpectatorSeated, NULL);
    if (seatedAt < 0) {
        logStop(); logDestroy(); loopbackHarnessStop(&h); remove(fname);
        UT_FAIL("spectator never seated within pump budget");
    }

    /* Stop ticking the viewer so it ages out; pump the server until the
     * timeout sweep disconnects it (emitting log_SpectatorLeft), then a few
     * extra ticks so the event flushes to the .wbv buffer. */
    h.clientUp = false;
    goneAt = loopbackHarnessPumpUntil(&h, 3000, slNoSpectators, NULL);
    if (goneAt < 0) {
        h.clientUp = true;
        logStop(); logDestroy(); loopbackHarnessStop(&h); remove(fname);
        UT_FAIL("idle spectator never aged out");
    }
    loopbackHarnessPumpUntil(&h, 5, NULL, NULL);  /* flush trailing tick */
    h.clientUp = true;

    logStop();
    logDestroy();
    loopbackHarnessStop(&h);

    UT_ASSERT_MSG(slReadBack(fname, &evs), "read-back/walk failed");
    remove(fname);

    UT_ASSERT_MSG(slFind(&evs, log_SpectatorJoined) != NULL,
                  "loopback join produced no log_SpectatorJoined");
    UT_ASSERT_MSG(slFind(&evs, log_SpectatorLeft) != NULL,
                  "loopback timeout produced no log_SpectatorLeft");
    return 0;
}

int run_spectator_log(void) {
    int rc;
    if ((rc = slRoundtrip()) != 0) return rc;
    if ((rc = slEmission()) != 0) return rc;
    return 0;
}
