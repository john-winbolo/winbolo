/*
 * Spectator-chat replay emission.
 *
 * test_spectator_log already pins the on-disk shape of log_SpectatorChat via a
 * direct logAddEvent round-trip. This test proves the EMITTER: a lobby chat
 * line accepted from a spectator (serverSimReceiveSpectatorChat) writes a
 * log_SpectatorChat — carrying the sender's specIdx and the message — into the
 * recording .wbv. The framed-stream walker is the same length-driven, self-
 * contained reader test_spectator_log uses (independent of the logviewer): a
 * writer bug fails here at the byte position, not as a generic "log corrupt".
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"            /* MAX_TANKS */
#include "log.h"
#include "server_sim.h"
#include "test_harness.h"

#define CL_MAX_PAYLOAD 264

typedef struct {
    bool    found;
    int     payloadLen;
    uint8_t payload[CL_MAX_PAYLOAD];
} ClChatEvent;

static int clReadByte(const uint8_t *buf, size_t len, size_t pos) {
    if (pos >= len) return -1;
    return buf[pos];
}

/* Skip a snapshot body — mirrors the v2 snapshot layout. */
static bool clSkipSnapshot(const uint8_t *buf, size_t len, size_t *pos) {
    size_t p = *pos;
    int n;
    if (p + 8 > len) return false;
    p += 8;
    if ((n = clReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n; /* pills */
    if ((n = clReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n; /* bases */
    if ((n = clReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n; /* starts */
    while (1) {
        if (p + 4 > len) return false;
        int dlen = clReadByte(buf, len, p);
        int y    = clReadByte(buf, len, p + 1);
        int sx   = clReadByte(buf, len, p + 2);
        int ex   = clReadByte(buf, len, p + 3);
        p += 4;
        if (dlen == 4 && y == 255 && sx == 255 && ex == 255) break;
        if (dlen < 4) return false;
        p += (size_t)(dlen - 4);
    }
    for (int i = 0; i < MAX_TANKS; i++) {
        if ((n = clReadByte(buf, len, p)) < 0) return false;
        p += 1 + (size_t)n;
    }
    *pos = p;
    return true;
}

/* Locate the first outer block byte after the logStart header. */
static bool clLocateBlockStart(const uint8_t *buf, size_t len,
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

/* Walk the block stream, capturing the first log_SpectatorChat event. */
static bool clWalk(const uint8_t *buf, size_t len, size_t blockStart,
                   ClChatEvent *out) {
    size_t pos = blockStart;
    out->found = false;
    while (pos < len) {
        int code = clReadByte(buf, len, pos); pos++;
        if (code < 0) return false;
        if (code == LOG_QUIT) {
            return true;
        } else if (code == LOG_NOEVENTS) {
            if (clReadByte(buf, len, pos) < 0) return false;
            pos += 1;
        } else if (code == LOG_NOEVENTS_LONG) {
            if (pos + 2 > len) return false;
            pos += 2;
        } else if (code == LOG_EVENT_SNAPSHOT) {
            if (!clSkipSnapshot(buf, len, &pos)) return false;
        } else if (code == LOG_EVENT || code == LOG_EVENT_LONG) {
            int n;
            if (code == LOG_EVENT) {
                n = clReadByte(buf, len, pos); pos += 1;
                if (n < 0) return false;
            } else {
                if (pos + 2 > len) return false;
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
                if (ev == log_SpectatorChat && !out->found &&
                    plen <= CL_MAX_PAYLOAD) {
                    out->found      = true;
                    out->payloadLen = plen;
                    memcpy(out->payload, buf + payloadStart, (size_t)plen);
                }
                pos = payloadStart + (size_t)plen;
            }
        } else {
            return false;
        }
    }
    return false;  /* hit EOF without LOG_QUIT */
}

int run_spectator_chat_log(void) {
    const char *fname = "test_spectator_chat_log.wbv";
    const char *text  = "watching!";
    ServerSim  *sim;
    uint8_t    *buf = NULL;
    size_t      len = 0;
    size_t      blockStart = 0;
    uint8_t     version = 0;
    ClChatEvent chat;

    remove(fname);
    sim = ut_make_running_sim("Tester");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim failed");
    logCreate();
    UT_ASSERT_MSG(logStart((char *)fname, sim, 0, MAX_TANKS, FALSE) == TRUE,
                  "logStart failed");
    logWriteTick();  /* pin owner thread to this thread */

    /* The emitter under test: a spectator's lobby chat from slot 4. */
    serverSimReceiveSpectatorChat(sim, 4, text, strlen(text));
    logWriteTick();

    logStop();
    logDestroy();
    serverSimDestroy(sim);

    UT_ASSERT_MSG(extractLogDat(fname, &buf, &len), "extractLogDat failed");
    UT_ASSERT_MSG(clLocateBlockStart(buf, len, &blockStart, &version) &&
                  version == LOG_VERSION, "could not locate v2 block start");
    UT_ASSERT_MSG(clWalk(buf, len, blockStart, &chat), "stream walk failed");
    free(buf);
    remove(fname);

    UT_ASSERT_MSG(chat.found,
                  "serverSimReceiveSpectatorChat emitted no log_SpectatorChat");
    UT_ASSERT_MSG(chat.payloadLen == 1 + 1 + (int)strlen(text),
                  "chat payload len = %d (want %d)", chat.payloadLen,
                  1 + 1 + (int)strlen(text));
    UT_ASSERT_MSG(chat.payload[0] == 4, "chat specIdx = %d (want 4)",
                  chat.payload[0]);
    UT_ASSERT_MSG(chat.payload[1] == (uint8_t)strlen(text),
                  "chat msg len = %d (want %d)", chat.payload[1],
                  (int)strlen(text));
    UT_ASSERT_MSG(memcmp(chat.payload + 2, text, strlen(text)) == 0,
                  "chat message mismatch");
    return 0;
}
