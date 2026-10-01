/*
 * Script upload over PACKET_LOBBY_MAP_UPLOAD_BEGIN with UPLOAD_KIND_SCRIPT.
 *
 *   upload_begin_golden          the client's BEGIN body for both kinds, held
 *                                to hand-written bytes.
 *   script_upload_begin_refusals the server's BEGIN checks for a script and
 *                                the map ones the kind byte must not move,
 *                                driven by hand-built BEGIN packets handed
 *                                straight to the server's packet handler from
 *                                a connected client's address. The ACK goes
 *                                back over the real socket to that client,
 *                                whose ACK handler records the code.
 *   loopback_script_upload_at_cap
 *                                a 4 MiB package (the cap) sent through
 *                                clientSimNetSendLobbyScriptUpload arrives
 *                                whole and in order at the registered accept
 *                                callback, and DONE carries the session path.
 *   loopback_script_upload_refused
 *                                the accept callback says no; the client ends
 *                                rejected with the callback's reason as its
 *                                final path.
 *   script_upload_client_refusals
 *                                transportUdpClientStartLobbyScriptUpload
 *                                refuses an over-cap file, a .txt name and a
 *                                missing file, and sends nothing.
 *
 * Landing, with the real host callbacks registered by
 * scenarioHostRegisterScenarioLister. Every directory is a scratch one: the
 * host's own, the player's (WB_MOD_DIR_USER), an empty Workshop one
 * (WB_MOD_DIR_WORKSHOP), the shipped one (WB_MOD_DIR_SHIPPED) and the
 * landing directory, set on the sim.
 *
 *   loopback_script_upload_lands_listed
 *                                a mod .lua lands in the landing directory
 *                                and the merged listing offers it.
 *   loopback_script_upload_package_listed
 *                                a .scenario package lands and is listed
 *                                under its manifest's name.
 *   loopback_script_upload_bound_refused
 *                                a bound package is refused with the reason
 *                                in DONE and nothing left in the directory.
 *   loopback_script_upload_syntax_line
 *                                a .lua that will not load is refused with
 *                                its line number in DONE.
 *   script_upload_name_taken     a name the shipped directory holds is
 *                                refused at BEGIN with NAME_TAKEN and nothing
 *                                is armed; the uploader's own earlier file
 *                                is not taken.
 *   script_upload_persist_caps   under PERSIST the 33rd file is refused at
 *                                BEGIN with UPLOAD_LIMIT_HIT, and a file
 *                                replacing one of the same name is not.
 *   script_upload_accept_refusals
 *                                the host's accept callback refuses with
 *                                scripts off, on a name a higher directory
 *                                holds, and for an api above the server's.
 *   script_upload_session_emptied
 *                                under ALLOW the file is gone once the
 *                                session directory is emptied, and a
 *                                directory below it is left alone; under
 *                                PERSIST it is still listed.
 *   script_upload_listing_sees_own_removal
 *                                a file the emptying removed is not listed
 *                                even with the directory's stamp set back to
 *                                the listing's (POSIX only).
 *   script_upload_reset_drops_session_picks
 *                                the lobby reset drops the pick naming a
 *                                session file, keeps the other, and empties
 *                                the directory.
 *   loopback_script_upload_plays_next_round
 *                                an uploaded mod on the pick list is read by
 *                                the next decision and composes.
 *   script_upload_list_source    a row read from the landing directory says
 *                                SCN_DIR_SOURCE_UPLOAD through the listing,
 *                                the frontend enumeration, the pick list and
 *                                the script-list event; a shipped row and a
 *                                name both directories hold say SERVER; and
 *                                a landing directory that is also a higher
 *                                one marks nothing.
 *   loopback_script_upload_source_on_wire
 *                                a remote client reads the uploaded row as
 *                                UPLOAD off the scenario-list packet, and
 *                                again off the script-list event once the
 *                                host has put it on the list.
 *   loopback_script_upload_list_after_done
 *                                a list request sent on the pump DONE lands
 *                                is answered within a few pumps, though the
 *                                upload's BEGIN started the request cooldown,
 *                                and holds the file as UPLOAD.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#ifndef _WIN32
#include <fcntl.h>     /* AT_FDCWD */
#include <sys/stat.h>  /* utimensat, UTIME_OMIT — setting a stamp back */
#include <time.h>      /* struct timespec, time_t */
#endif

#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "client_sim_internal.h"   /* lobbyMapUploadStatus, to arm the ACK */
#include "transport_udp.h"         /* transportUdpClientBuildUploadBeginBody,
                                    * transportUdpClientStartLobbyScriptUpload */
#include "server_sim.h"
#include "server_sim_lifecycle.h"  /* serverSimSetOpenHost, serverSimSetServerLocks */
#include "server_sim_scenario.h"   /* serverSimSetScriptUploadAccept */
#include "upload_policy.h"         /* UPLOAD_KIND_*, SCRIPT_UPLOAD_* */
#include "wire_limits.h"           /* LOBBY_*_UPLOAD_MAX_BYTES, LOBBY_LOCK_MAP */
#include "netpacks.h"              /* PACKET_LOBBY_MAP_UPLOAD_BEGIN, LOBBY_REJECT_* */
#include "threads.h"
#include "test_harness.h"
#include "loopback_harness.h"
#include "transport_udp_server_internal.h" /* udpServer, serverProcessPacket */
#include "server_sim_internal.h"   /* serverSimSetScriptList */
#include "scenario_defs.h"         /* ScnDirEntry */
#include "scenario_host.h"         /* scenarioHostRegisterScenarioLister,
                                    * scenarioHostFollowMap, scenarioHostName */
#include "scenario_validate.h"     /* scenarioHostScriptCount */
#include "scenario_package.h"      /* scnPackageWrite */
#include "server/sim/server_sim_shared.h" /* serverSimSetActive,
                                           * serverSimResetLobbyToDefaults */

#define CONNECT_MAX      2000
#define ACK_MAX           400
#define SMALL_UPLOAD_MAX 4000
/* A 4 MiB package is about sixteen thousand 256-byte bulk segments, and the
 * client's one standalone channel frame a tick carries about five of them. */
#define BIG_UPLOAD_MAX  30000

/* ── golden BEGIN bodies ─────────────────────────────────────────────── */

static int check_bytes(const char *label, const uint8_t *got, size_t gotLen,
                       const uint8_t *want, size_t wantLen) {
    size_t i;
    if (gotLen != wantLen) {
        fprintf(stderr, "FAIL %s: length %zu, expected %zu\n",
                label, gotLen, wantLen);
        return 1;
    }
    for (i = 0; i < wantLen; i++) {
        if (got[i] != want[i]) {
            fprintf(stderr, "FAIL %s: byte %zu is 0x%02x, expected 0x%02x\n",
                    label, i, got[i], want[i]);
            return 1;
        }
    }
    return 0;
}

int run_upload_begin_golden(void) {
    /* kind, totalLen (big-endian), nameLen, name, bulkStartSeq (big-endian). */
    static const uint8_t wantMap[] = {
        0x00,
        0x00, 0x00, 0x12, 0x34,
        0x05,
        0x61, 0x2e, 0x6d, 0x61, 0x70,           /* "a.map" */
        0x01, 0x02, 0x03, 0x04,
    };
    static const uint8_t wantScript[] = {
        0x01,
        0x00, 0x40, 0x00, 0x00,
        0x05,
        0x6d, 0x2e, 0x6c, 0x75, 0x61,           /* "m.lua" */
        0x00, 0x00, 0x00, 0x05,
    };
    uint8_t out[64];
    size_t n;
    int fail = 0;

    UT_ASSERT(UPLOAD_KIND_MAP == 0);
    UT_ASSERT(UPLOAD_KIND_SCRIPT == 1);

    n = transportUdpClientBuildUploadBeginBody(out, sizeof(out),
                                               UPLOAD_KIND_MAP, 0x00001234u,
                                               "a.map", 0x01020304u);
    fail |= check_bytes("mapBegin", out, n, wantMap, sizeof(wantMap));

    n = transportUdpClientBuildUploadBeginBody(out, sizeof(out),
                                               UPLOAD_KIND_SCRIPT, 0x00400000u,
                                               "m.lua", 5u);
    fail |= check_bytes("scriptBegin", out, n, wantScript, sizeof(wantScript));

    /* One byte short of the body refuses rather than writing past cap. */
    n = transportUdpClientBuildUploadBeginBody(out, sizeof(wantScript) - 1,
                                               UPLOAD_KIND_SCRIPT, 0x00400000u,
                                               "m.lua", 5u);
    UT_ASSERT_MSG(n == 0, "short buffer must refuse, wrote %zu", n);

    UT_ASSERT(fail == 0);
    return 0;
}

/* ── shared loopback helpers ─────────────────────────────────────────── */

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static bool pred_upload_settled(LoopbackHarness *h, void *user) {
    uint8_t st;
    (void)user;
    st = clientSimGetLobbyMapUploadStatus(h->cs);
    return st == 3 || st == 4;
}

static bool pred_ack_seen(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetLobbyMapUploadStatus(h->cs) != 1;
}

/* What the test accept callback was handed, and what it answers. */
typedef struct {
    int      calls;
    bool     answer;
    char     dir[FILENAME_MAX];
    char     name[128];
    uint8_t *bytes;
    uint32_t len;
} AcceptLog;

static bool test_accept(void *ctx, const char *dir, const char *name,
                        const uint8_t *bytes, uint32_t len,
                        ScriptUploadRefusal *why) {
    AcceptLog *log = (AcceptLog *)ctx;
    log->calls++;
    SDL_strlcpy(log->dir, dir != NULL ? dir : "", sizeof(log->dir));
    SDL_strlcpy(log->name, name != NULL ? name : "", sizeof(log->name));
    free(log->bytes);
    log->bytes = (uint8_t *)malloc(len > 0 ? len : 1);
    if (log->bytes != NULL && len > 0) memcpy(log->bytes, bytes, len);
    log->len = len;
    if (!log->answer) {
        why->reason = SCRIPT_REFUSE_SYNTAX;
        why->a      = 3;
        SDL_strlcpy(why->text, "bad manifest: line 3", sizeof(why->text));
        return false;
    }
    return true;
}

static bool start_lobby(LoopbackHarness *h, const char *name) {
    if (!loopbackHarnessStart(h, name, /*lobbyMode*/ true, NULL, 0xC0FFEEu)) {
        return false;
    }
    if (loopbackHarnessPumpUntil(h, CONNECT_MAX, pred_connected, NULL) < 0) {
        return false;
    }
    threadsWaitForMutex();
    serverSimSetOpenHost(h->sim, true);
    threadsReleaseMutex();
    return true;
}

static bool write_scratch(const char *path, size_t len,
                          uint8_t (*byteAt)(size_t)) {
    FILE *fp = fopen(path, "wb");
    uint8_t chunk[4096];
    size_t done = 0;
    if (fp == NULL) return false;
    while (done < len) {
        size_t n = len - done, i;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        for (i = 0; i < n; i++) chunk[i] = byteAt(done + i);
        if (fwrite(chunk, 1, n, fp) != n) { fclose(fp); return false; }
        done += n;
    }
    fclose(fp);
    return true;
}

/* Changes every byte and does not repeat on a 256-byte segment, so a
 * segment delivered out of place, twice or not at all shows. */
static uint8_t pattern_byte(size_t i) {
    return (uint8_t)((i * 31u) ^ (i >> 8) ^ (i >> 16));
}

static uint8_t text_byte(size_t i) {
    static const char text[] = "return { name = \"mod\" }\n";
    return (uint8_t)text[i % (sizeof(text) - 1)];
}

/* ── server BEGIN refusals ───────────────────────────────────────────── */

/* BEGIN built here by hand, not through the client's builder, so the server
 * is held to the layout on its own: [header 8][kind 1][totalLen 4]
 * [nameLen 1][name N][bulkStartSeq 4]. The callers pass the server's own
 * expected bulk sequence for the slot, so taking it changes nothing. */
static int hand_begin(uint8_t *buf, uint8_t kind, uint32_t totalLen,
                      const char *name, uint32_t bulkStartSeq) {
    size_t nameLen = strlen(name);
    int pos = PACKET_HEADER_SIZE;
    packHeader(buf, PACKET_LOBBY_MAP_UPLOAD_BEGIN, 0);
    buf[pos++] = kind;
    buf[pos++] = (uint8_t)(totalLen >> 24);
    buf[pos++] = (uint8_t)(totalLen >> 16);
    buf[pos++] = (uint8_t)(totalLen >> 8);
    buf[pos++] = (uint8_t)totalLen;
    buf[pos++] = (uint8_t)nameLen;
    memcpy(buf + pos, name, nameLen);
    pos += (int)nameLen;
    packU32(buf + pos, bulkStartSeq);
    return pos + 4;
}

/* Whether the last send_begin left the server holding an upload for the slot,
 * read straight after the BEGIN: a refused one arms nothing, so no bytes are
 * asked for. */
static bool lastBeginArmed;

/* Hand one BEGIN to the server as if from the client's slot, pump until the
 * client has taken the ACK, and answer the ACK's code (-1 for no ACK). An
 * approved BEGIN is cleared again so the next one starts from nothing. */
static int send_begin(LoopbackHarness *h, int slot, uint8_t kind,
                      uint32_t totalLen, const char *name) {
    uint8_t pkt[PACKET_HEADER_SIZE + 6 + 128 + 4];
    int len = hand_begin(pkt, kind, totalLen, name,
        udpServer.channelMux[slot].ch[CHANNEL_BULK].expectedSeq);
    struct sockaddr_in from;
    int code;

    threadsWaitForMutex();
    udpServer.clientReqCooldownTicks[slot] = 0;
    from = udpServer.clients[slot].addr;
    /* Arm the client's ACK handler, which only listens after a BEGIN. */
    h->cs->lobbyMapUploadStatus = 1;
    h->cs->lobbyMapUploadRejectCode = 0;
    serverProcessPacket(h->sim, pkt, len, &from);
    lastBeginArmed = udpServer.clientUploadActive[slot] ||
                     udpServer.clientScriptUploadBuf[slot] != NULL;
    threadsReleaseMutex();

    if (loopbackHarnessPumpUntil(h, ACK_MAX, pred_ack_seen, NULL) < 0) {
        code = -1;
    } else {
        code = (clientSimGetLobbyMapUploadStatus(h->cs) == 2)
                   ? 0 : clientSimGetLobbyMapUploadRejectCode(h->cs);
    }
    threadsWaitForMutex();
    udpServerClearClientUploadState(slot);
    h->cs->lobbyMapUploadStatus = 0;
    threadsReleaseMutex();
    return code;
}

int run_script_upload_begin_refusals(void) {
    LoopbackHarness h;
    AcceptLog log;
    int slot, code;

    memset(&log, 0, sizeof(log));
    log.answer = true;
    if (!start_lobby(&h, "Refusals")) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness did not connect");
    }
    slot = clientSimGetMyPlayerNum(h.cs);
    UT_ASSERT(slot >= 0 && slot < MAX_TANKS);

    threadsWaitForMutex();
    serverSimSetScriptUploadAccept(h.sim, test_accept, &log);
    udpServer.scriptUploadPolicy = SCRIPT_UPLOAD_ALLOW;
    threadsReleaseMutex();

    /* Accepted at the cap: the case the refusals below are measured from.
     * A heap buffer of the announced size stands behind the approval. */
    {
        uint8_t pkt[PACKET_HEADER_SIZE + 6 + 128 + 4];
        int len = hand_begin(pkt, UPLOAD_KIND_SCRIPT,
                             LOBBY_PACKAGE_UPLOAD_MAX_BYTES, "cap.scenario",
                             udpServer.channelMux[slot].ch[CHANNEL_BULK].expectedSeq);
        struct sockaddr_in from;
        bool active, isScript, haveBuf, freed, kindBack, cleared;
        threadsWaitForMutex();
        udpServer.clientReqCooldownTicks[slot] = 0;
        from = udpServer.clients[slot].addr;
        h.cs->lobbyMapUploadStatus = 1;
        h.cs->lobbyMapUploadRejectCode = 0;
        serverProcessPacket(h.sim, pkt, len, &from);
        active   = udpServer.clientUploadActive[slot];
        isScript = (udpServer.clientUploadKind[slot] == UPLOAD_KIND_SCRIPT);
        haveBuf  = (udpServer.clientScriptUploadBuf[slot] != NULL);
        threadsReleaseMutex();
        /* Take the ACK so it cannot answer for the next BEGIN. */
        code = (loopbackHarnessPumpUntil(&h, ACK_MAX, pred_ack_seen, NULL) < 0)
                   ? -1 : (int)clientSimGetLobbyMapUploadStatus(h.cs);
        threadsWaitForMutex();
        /* Clearing the slot frees the buffer and puts the kind back. */
        udpServerClearClientUploadState(slot);
        freed    = (udpServer.clientScriptUploadBuf[slot] == NULL);
        kindBack = (udpServer.clientUploadKind[slot] == UPLOAD_KIND_MAP);
        cleared  = !udpServer.clientUploadActive[slot];
        h.cs->lobbyMapUploadStatus = 0;
        threadsReleaseMutex();
        UT_ASSERT_MSG(code == 2, "script at the cap not approved (status %d, "
                      "reject %d)", code,
                      (int)clientSimGetLobbyMapUploadRejectCode(h.cs));
        UT_ASSERT(active && isScript && haveBuf);
        UT_ASSERT(freed && kindBack && cleared);
    }

    /* One byte over the package cap. */
    code = send_begin(&h, slot, UPLOAD_KIND_SCRIPT,
                      LOBBY_PACKAGE_UPLOAD_MAX_BYTES + 1, "big.scenario");
    UT_ASSERT_MSG(code == LOBBY_REJECT_INVALID,
                  "script over the cap: code %d", code);

    /* A script name that is not one. */
    code = send_begin(&h, slot, UPLOAD_KIND_SCRIPT, 100, "mod.map");
    UT_ASSERT_MSG(code == LOBBY_REJECT_INVALID,
                  "script named .map: code %d", code);

    /* Scripts off. */
    threadsWaitForMutex();
    udpServer.scriptUploadPolicy = SCRIPT_UPLOAD_OFF;
    threadsReleaseMutex();
    code = send_begin(&h, slot, UPLOAD_KIND_SCRIPT, 100, "mod.lua");
    UT_ASSERT_MSG(code == LOBBY_REJECT_UPLOAD_DISABLED,
                  "script under OFF: code %d", code);
    threadsWaitForMutex();
    udpServer.scriptUploadPolicy = SCRIPT_UPLOAD_ALLOW;
    threadsReleaseMutex();

    /* Nothing registered to take it. */
    threadsWaitForMutex();
    serverSimSetScriptUploadAccept(h.sim, NULL, NULL);
    threadsReleaseMutex();
    code = send_begin(&h, slot, UPLOAD_KIND_SCRIPT, 100, "mod.lua");
    UT_ASSERT_MSG(code == LOBBY_REJECT_UPLOAD_DISABLED,
                  "script with no accept callback: code %d", code);
    threadsWaitForMutex();
    serverSimSetScriptUploadAccept(h.sim, test_accept, &log);
    threadsReleaseMutex();

    /* The map lock leaves a script alone, and still holds a map. */
    threadsWaitForMutex();
    serverSimSetServerLocks(h.sim, LOBBY_LOCK_MAP);
    threadsReleaseMutex();
    code = send_begin(&h, slot, UPLOAD_KIND_SCRIPT, 100, "mod.lua");
    UT_ASSERT_MSG(code == 0, "script under the map lock: code %d", code);
    code = send_begin(&h, slot, UPLOAD_KIND_MAP, 100, "locked.map");
    UT_ASSERT_MSG(code == LOBBY_REJECT_LOCKED,
                  "map under the map lock: code %d", code);
    threadsWaitForMutex();
    serverSimSetServerLocks(h.sim, 0);
    threadsReleaseMutex();

    /* A kind the server does not know. */
    code = send_begin(&h, slot, 2, 100, "mod.lua");
    UT_ASSERT_MSG(code == LOBBY_REJECT_INVALID, "unknown kind: code %d", code);

    /* The map cap is unchanged by the kind byte: 64 KiB + 1 refused,
     * 64 KiB approved. */
    code = send_begin(&h, slot, UPLOAD_KIND_MAP,
                      LOBBY_MAP_UPLOAD_MAX_BYTES + 1, "big.map");
    UT_ASSERT_MSG(code == LOBBY_REJECT_INVALID, "map over the cap: code %d", code);
    code = send_begin(&h, slot, UPLOAD_KIND_MAP,
                      LOBBY_MAP_UPLOAD_MAX_BYTES, "ok.map");
    UT_ASSERT_MSG(code == 0, "map at the cap: code %d", code);

    UT_ASSERT_MSG(log.calls == 0, "no BEGIN here finishes an upload");

    threadsWaitForMutex();
    serverSimSetScriptUploadAccept(h.sim, NULL, NULL);
    threadsReleaseMutex();
    loopbackHarnessStop(&h);
    free(log.bytes);
    return 0;
}

/* ── a package at the cap, end to end ────────────────────────────────── */

int run_loopback_script_upload_at_cap(void) {
    char path[1024];
    const uint32_t size = LOBBY_PACKAGE_UPLOAD_MAX_BYTES;
    LoopbackHarness h;
    AcceptLog log;
    int settledAt;
    uint32_t i;
    char finalPath[256];
    uint8_t status, reject, kind;
    bool bufFreed;
    size_t dirLen;
    int slot;

    memset(&log, 0, sizeof(log));
    log.answer = true;
    /* The scratch directory is this process's own and goes at exit. */
    UT_ASSERT(utScratchPath(path, sizeof(path), "big.scenario"));
    UT_ASSERT(write_scratch(path, size, pattern_byte));

    if (!start_lobby(&h, "BigUpload")) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness did not connect");
    }
    slot = clientSimGetMyPlayerNum(h.cs);
    threadsWaitForMutex();
    serverSimSetScriptUploadAccept(h.sim, test_accept, &log);
    udpServer.scriptUploadPolicy = SCRIPT_UPLOAD_ALLOW;
    threadsReleaseMutex();

    if (!clientSimNetSendLobbyScriptUpload(h.cs, path)) {
        loopbackHarnessStop(&h);
        UT_FAIL("script upload kick refused");
    }
    UT_ASSERT(clientSimGetLobbyUploadKind(h.cs) == UPLOAD_KIND_SCRIPT);

    settledAt = loopbackHarnessPumpUntil(&h, BIG_UPLOAD_MAX,
                                         pred_upload_settled, NULL);
    /* Read the client before the harness frees it. */
    status = clientSimGetLobbyMapUploadStatus(h.cs);
    reject = clientSimGetLobbyMapUploadRejectCode(h.cs);
    kind   = clientSimGetLobbyUploadKind(h.cs);
    SDL_strlcpy(finalPath, clientSimGetLobbyMapUploadFinalPath(h.cs),
                sizeof(finalPath));
    fprintf(stderr, "  script upload at cap: settled@%d status=%d reject=%d "
                    "path='%s'\n",
            settledAt, (int)status, (int)reject, finalPath);

    threadsWaitForMutex();
    serverSimSetScriptUploadAccept(h.sim, NULL, NULL);
    /* The finished upload let go of its buffer. */
    bufFreed = (slot >= 0 && slot < MAX_TANKS &&
                udpServer.clientScriptUploadBuf[slot] == NULL);
    threadsReleaseMutex();
    loopbackHarnessStop(&h);

    UT_ASSERT_MSG(settledAt >= 0, "upload never settled within %d pumps",
                  BIG_UPLOAD_MAX);
    UT_ASSERT(status == 3);
    UT_ASSERT(reject == 0);
    UT_ASSERT(kind == UPLOAD_KIND_SCRIPT);
    UT_ASSERT(bufFreed);
    /* The name as the listing will show it, and not where the server keeps
     * it. */
    UT_ASSERT_MSG(strcmp(finalPath, "big.scenario") == 0,
                  "final path '%s'", finalPath);

    UT_ASSERT_MSG(log.calls == 1, "accept called %d times", log.calls);
    UT_ASSERT_MSG(strcmp(log.name, "big.scenario") == 0, "name '%s'", log.name);
    /* The session directory under the map root, named for the server's
     * port so two servers on one root do not empty each other's. */
    dirLen = strlen(log.dir);
    UT_ASSERT_MSG(dirLen > 0 && strstr(log.dir, "/Uploads/Session-") != NULL,
                  "dir '%s'", log.dir);
    UT_ASSERT_MSG(log.len == size, "callback got %u bytes, expected %u",
                  (unsigned)log.len, (unsigned)size);
    UT_ASSERT(log.bytes != NULL);
    for (i = 0; i < size; i++) {
        if (log.bytes[i] != pattern_byte(i)) {
            uint8_t got = log.bytes[i];
            free(log.bytes);
            UT_FAIL("byte %u is 0x%02x, expected 0x%02x",
                    (unsigned)i, got, pattern_byte(i));
        }
    }
    free(log.bytes);
    return 0;
}

/* ── the callback says no ────────────────────────────────────────────── */

int run_loopback_script_upload_refused(void) {
    char path[1024];
    char persistDir[1024];
    LoopbackHarness h;
    AcceptLog log;
    int settledAt;
    char finalPath[256];
    uint8_t status, reject, kind;

    memset(&log, 0, sizeof(log));
    log.answer = false;
    /* The scratch directory is this process's own and goes at exit. */
    UT_ASSERT(utScratchPath(path, sizeof(path), "mod.lua"));
    UT_ASSERT(write_scratch(path, 200, text_byte));
    UT_ASSERT(utScratchPath(persistDir, sizeof(persistDir), "Uploads/Scripts"));

    if (!start_lobby(&h, "Refused")) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness did not connect");
    }
    threadsWaitForMutex();
    serverSimSetScriptUploadAccept(h.sim, test_accept, &log);
    udpServer.scriptUploadPolicy = SCRIPT_UPLOAD_PERSIST;
    /* The directory is the sim's, as serverInstanceStartup resolves it for
     * PERSIST; the transport only hands it over. */
    serverSimSetScriptUploadDir(h.sim, persistDir);
    serverSimSetScriptSessionDir(h.sim, "");
    threadsReleaseMutex();

    UT_ASSERT(clientSimNetSendLobbyScriptUpload(h.cs, path));
    settledAt = loopbackHarnessPumpUntil(&h, SMALL_UPLOAD_MAX,
                                         pred_upload_settled, NULL);
    /* Read the client before the harness frees it. */
    status = clientSimGetLobbyMapUploadStatus(h.cs);
    reject = clientSimGetLobbyMapUploadRejectCode(h.cs);
    kind   = clientSimGetLobbyUploadKind(h.cs);
    SDL_strlcpy(finalPath, clientSimGetLobbyMapUploadFinalPath(h.cs),
                sizeof(finalPath));

    threadsWaitForMutex();
    serverSimSetScriptUploadAccept(h.sim, NULL, NULL);
    threadsReleaseMutex();
    loopbackHarnessStop(&h);

    UT_ASSERT_MSG(settledAt >= 0, "upload never settled");
    UT_ASSERT(status == 4);
    UT_ASSERT(reject == LOBBY_REJECT_INVALID);
    UT_ASSERT(kind == UPLOAD_KIND_SCRIPT);
    UT_ASSERT_MSG(strcmp(finalPath, "bad manifest: line 3") == 0,
                  "final path '%s'", finalPath);
    UT_ASSERT(log.calls == 1);
    UT_ASSERT(log.len == 200);
    /* The callback is handed the directory the sim holds. */
    UT_ASSERT_MSG(strcmp(log.dir, persistDir) == 0, "dir '%s'", log.dir);
    free(log.bytes);
    return 0;
}

/* ── the client refuses before sending ───────────────────────────────── */

int run_script_upload_client_refusals(void) {
    char overCap[1024];
    char txt[1024];
    char missing[1024];
    LoopbackHarness h;
    int slot, i;
    bool okOver, okTxt, okMissing, serverIdle;

    /* The scratch directory is this process's own and goes at exit. */
    UT_ASSERT(utScratchPath(overCap, sizeof(overCap), "over.lua"));
    UT_ASSERT(utScratchPath(txt, sizeof(txt), "mod.txt"));
    UT_ASSERT(utScratchPath(missing, sizeof(missing), "missing.lua"));
    UT_ASSERT(write_scratch(overCap, LOBBY_PACKAGE_UPLOAD_MAX_BYTES + 1,
                            text_byte));
    UT_ASSERT(write_scratch(txt, 64, text_byte));

    if (!start_lobby(&h, "ClientRefuse")) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness did not connect");
    }
    slot = clientSimGetMyPlayerNum(h.cs);

    okOver    = transportUdpClientStartLobbyScriptUpload(&h.cs->transport,
                                                         overCap);
    okTxt     = transportUdpClientStartLobbyScriptUpload(&h.cs->transport, txt);
    okMissing = transportUdpClientStartLobbyScriptUpload(&h.cs->transport,
                                                         missing);
    for (i = 0; i < 50; i++) loopbackHarnessPump(&h);

    UT_ASSERT_MSG(!okOver, "an over-cap script must be refused");
    UT_ASSERT_MSG(!okTxt, "a .txt must be refused");
    UT_ASSERT_MSG(!okMissing, "a missing file must be refused");
    /* Nothing went out: the client never announced, the server holds nothing. */
    UT_ASSERT(clientSimGetLobbyMapUploadStatus(h.cs) == 0);
    UT_ASSERT(clientSimGetLobbyUploadKind(h.cs) == UPLOAD_KIND_MAP);
    threadsWaitForMutex();
    serverIdle = (slot >= 0 && slot < MAX_TANKS &&
                  !udpServer.clientUploadActive[slot]);
    threadsReleaseMutex();
    UT_ASSERT(serverIdle);

    loopbackHarnessStop(&h);
    return 0;
}

/* ── landing, with the host's own callbacks ──────────────────────────── */

/* The five directories a landing case reads, each under the case's scratch
 * directory. The landing one is not made here: the accept callback makes it,
 * and a case that needs it first makes it itself. The Workshop one is made
 * and left empty. */
typedef struct {
    char configured[1024];
    char user[1024];
    char workshop[1024];
    char shipped[1024];
    char landing[1024];
} UpDirs;

static bool up_dirs(UpDirs *d, const char *landingLeaf) {
    if (!utScratchPath(d->configured, sizeof(d->configured), "configured") ||
        !utScratchPath(d->user, sizeof(d->user), "Mods") ||
        !utScratchPath(d->workshop, sizeof(d->workshop), "Workshop") ||
        !utScratchPath(d->shipped, sizeof(d->shipped), "shipped") ||
        !utScratchPath(d->landing, sizeof(d->landing), landingLeaf)) {
        return false;
    }
    return SDL_CreateDirectory(d->configured) &&
           SDL_CreateDirectory(d->user) &&
           SDL_CreateDirectory(d->workshop) &&
           SDL_CreateDirectory(d->shipped);
}

static void up_set_env(const char *key, const char *val) {
#ifdef _WIN32
    _putenv_s(key, val != NULL ? val : "");
#else
    if (val != NULL) setenv(key, val, 1);
    else unsetenv(key);
#endif
}

/* The player's directory, the Workshop one and the shipped one, which SDL
 * would otherwise name in the home directory and beside the executable. Up
 * for the whole case and taken down by the runner before it returns, pass or
 * fail. */
static void up_env(const UpDirs *d) {
    up_set_env("WB_MOD_DIR_USER", d->user);
    up_set_env("WB_MOD_DIR_WORKSHOP", d->workshop);
    up_set_env("WB_MOD_DIR_SHIPPED", d->shipped);
}

static void up_env_clear(void) {
    up_set_env("WB_MOD_DIR_USER", NULL);
    up_set_env("WB_MOD_DIR_WORKSHOP", NULL);
    up_set_env("WB_MOD_DIR_SHIPPED", NULL);
}

static bool up_write(const char *dir, const char *name, const void *bytes,
                     size_t len) {
    char  path[1200];
    FILE *f;
    bool  ok;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (f == NULL) return false;
    ok = fwrite(bytes, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    return ok;
}

static bool up_write_text(const char *dir, const char *name,
                          const char *text) {
    return up_write(dir, name, text, strlen(text));
}

/* A loose mod, named by its manifest so a case can tell which copy it got. */
static bool up_write_mod(const char *dir, const char *file, const char *name) {
    char text[256];

    snprintf(text, sizeof(text),
             "scenario = {\n"
             "  name = \"%s\",\n"
             "  api = 1,\n"
             "  kind = \"mod\",\n"
             "  bound = false,\n"
             "}\n", name);
    return up_write_text(dir, file, text);
}

static const char kUpPackageManifest[] =
    "{\n"
    "  \"manifest\": 1,\n"
    "  \"api\": 1,\n"
    "  \"name\": \"Packed Upload\",\n"
    "  \"description\": \"Sent by a player\",\n"
    "  \"bound\": false,\n"
    "  \"script\": \"main.lua\"\n"
    "}\n";

static const char kUpBoundManifest[] =
    "{\n"
    "  \"manifest\": 1,\n"
    "  \"api\": 1,\n"
    "  \"name\": \"Bound Upload\",\n"
    "  \"bound\": true,\n"
    "  \"script\": \"main.lua\"\n"
    "}\n";

/* A .scenario package, written the way -pack writes a container. */
static bool up_write_package(const char *dir, const char *file,
                             const char *manifest) {
    static const char script[] = "function on_setup() end\n";
    ScnPackageEntry   entries[2];
    uint8_t          *bytes = NULL;
    size_t            len   = 0;
    char              err[256];
    bool              ok;

    memset(entries, 0, sizeof(entries));
    entries[0].name  = SCN_PACKAGE_MANIFEST_ENTRY;
    entries[0].bytes = (const uint8_t *)manifest;
    entries[0].len   = strlen(manifest);
    entries[1].name  = SCN_PACKAGE_SCRIPT_ENTRY;
    entries[1].bytes = (const uint8_t *)script;
    entries[1].len   = strlen(script);
    err[0] = '\0';
    if (!scnPackageWrite(entries, 2, &bytes, &len, err, sizeof(err))) {
        return false;
    }
    ok = up_write(dir, file, bytes, len);
    free(bytes);
    return ok;
}

static bool up_file_exists(const char *dir, const char *name) {
    char         path[1200];
    SDL_PathInfo info;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

/* How many entries dir holds, dot files included, so a temporary file left
 * behind counts; 0 for a directory that is not there. */
static int up_entries(const char *dir) {
    char **names;
    int    count = 0;

    names = SDL_GlobDirectory(dir, "*", 0, &count);
    if (names == NULL) return 0;
    SDL_free(names);
    return count;
}

/* Whether the merged listing offers file, and under what name. */
static bool up_listed(ServerSim *sim, const char *file, char *nameOut,
                      size_t nameLen) {
    ScnDirEntry *rows;
    int          n;
    int          i;
    bool         found = false;

    if (nameOut != NULL && nameLen > 0) nameOut[0] = '\0';
    rows = (ScnDirEntry *)calloc(64, sizeof(*rows));
    if (rows == NULL) return false;
    n = serverSimScenarioListDir(sim, rows, 64);
    for (i = 0; i < n; i++) {
        if (strcmp(rows[i].file, file) == 0) {
            found = true;
            if (nameOut != NULL) SDL_strlcpy(nameOut, rows[i].name, nameLen);
            break;
        }
    }
    free(rows);
    return found;
}

/* One scratch file sent through the client, pumped until DONE. reason and
 * its two numbers are the DONE reply's SCRIPT_REFUSE_* bytes as the client
 * read them. */
typedef struct {
    int     settledAt;
    uint8_t status;
    uint8_t reject;
    uint8_t reason;
    int32_t a;
    int32_t b;
    char    finalPath[256];
} UpResult;

static void up_send(LoopbackHarness *h, const char *path, UpResult *r) {
    memset(r, 0, sizeof(*r));
    r->settledAt = -1;
    if (!clientSimNetSendLobbyScriptUpload(h->cs, path)) return;
    r->settledAt = loopbackHarnessPumpUntil(h, SMALL_UPLOAD_MAX,
                                            pred_upload_settled, NULL);
    r->status = clientSimGetLobbyMapUploadStatus(h->cs);
    r->reject = clientSimGetLobbyMapUploadRejectCode(h->cs);
    r->reason = clientSimGetLobbyScriptRefuseReason(h->cs);
    r->a      = clientSimGetLobbyScriptRefuseNumber(h->cs, 0);
    r->b      = clientSimGetLobbyScriptRefuseNumber(h->cs, 1);
    SDL_strlcpy(r->finalPath, clientSimGetLobbyMapUploadFinalPath(h->cs),
                sizeof(r->finalPath));
}

/* A file in the case's scratch directory for the client to send, and its
 * path. */
static bool up_client_file(char *path, size_t pathLen, const char *name) {
    char dir[1024];

    if (!utScratchPath(dir, sizeof(dir), NULL)) return false;
    snprintf(path, pathLen, "%s/%s", dir, name);
    return true;
}

typedef int (*UpLoopbackBody)(LoopbackHarness *h, int slot, const UpDirs *d);

/* A connected lobby whose sim carries the host's own lister, reader and
 * accept callback, reading the scratch directories, and landing uploads
 * where the policy puts them. The body reads the client before returning;
 * the harness and the environment are taken down here either way. */
static int up_run_loopback(const char *label, ScriptUploadPolicy policy,
                           UpLoopbackBody body) {
    LoopbackHarness h;
    UpDirs          d;
    int             slot;
    int             r;

    if (!up_dirs(&d, policy == SCRIPT_UPLOAD_PERSIST ? "Scripts"
                                                     : "Session")) {
        UT_FAIL("scratch directories could not be made");
    }
    up_env(&d);
    if (!start_lobby(&h, label)) {
        loopbackHarnessStop(&h);
        up_env_clear();
        UT_FAIL("harness did not connect");
    }
    slot = clientSimGetMyPlayerNum(h.cs);
    threadsWaitForMutex();
    serverSimSetScenarioDir(h.sim, d.configured);
    scenarioHostRegisterScenarioLister(h.sim);
    serverSimSetScriptUploadDir(h.sim, d.landing);
    serverSimSetScriptSessionDir(h.sim, policy == SCRIPT_UPLOAD_ALLOW
                                            ? d.landing : "");
    udpServer.scriptUploadPolicy = policy;
    threadsReleaseMutex();

    r = body(&h, slot, &d);

    loopbackHarnessStop(&h);
    up_env_clear();
    return r;
}

typedef int (*UpSimBody)(ServerSim *sim, const UpDirs *d);

/* The same callbacks on a sim with no transport, for the cases that call the
 * accept callback and the emptying directly. */
static int up_run_sim(const char *landingLeaf, UpSimBody body) {
    UpDirs     d;
    ServerSim *sim;
    int        r;

    if (!up_dirs(&d, landingLeaf)) {
        UT_FAIL("scratch directories could not be made");
    }
    sim = ut_make_running_sim("Host");
    if (sim == NULL) UT_FAIL("no sim");
    serverSimSetActive(sim);
    serverSimSetScenarioDir(sim, d.configured);
    scenarioHostRegisterScenarioLister(sim);
    serverSimSetScriptUploadDir(sim, d.landing);
    up_env(&d);

    r = body(sim, &d);

    up_env_clear();
    serverSimDestroy(sim);
    return r;
}

/* ── a mod lands and is listed ───────────────────────────────────────── */

static int body_lands_listed(LoopbackHarness *h, int slot, const UpDirs *d) {
    char     path[1200];
    char     name[64];
    UpResult r;
    bool     listed;
    (void)slot;

    UT_ASSERT(up_client_file(path, sizeof(path), "fast.lua"));
    {
        char dir[1024];
        UT_ASSERT(utScratchPath(dir, sizeof(dir), NULL));
        UT_ASSERT(up_write_mod(dir, "fast.lua", "Upload Mod"));
    }
    up_send(h, path, &r);
    threadsWaitForMutex();
    listed = up_listed(h->sim, "fast.lua", name, sizeof(name));
    threadsReleaseMutex();

    UT_ASSERT_MSG(r.settledAt >= 0, "upload never settled");
    UT_ASSERT_MSG(r.status == 3, "status %d reject %d path '%s'",
                  (int)r.status, (int)r.reject, r.finalPath);
    UT_ASSERT_MSG(strcmp(r.finalPath, "fast.lua") == 0,
                  "final path '%s'", r.finalPath);
    UT_ASSERT_MSG(r.reason == SCRIPT_REFUSE_NONE, "reason %d", (int)r.reason);
    UT_ASSERT(up_file_exists(d->landing, "fast.lua"));
    /* The file and nothing else: the temporary it was written through went
     * with the rename. */
    UT_ASSERT_MSG(up_entries(d->landing) == 1, "the landing directory holds "
                  "%d entries", up_entries(d->landing));
    UT_ASSERT_MSG(listed, "the uploaded mod is not listed");
    UT_ASSERT_MSG(strcmp(name, "Upload Mod") == 0, "listed as '%s'", name);
    return 0;
}

int run_loopback_script_upload_lands_listed(void) {
    return up_run_loopback("LandsListed", SCRIPT_UPLOAD_ALLOW,
                           body_lands_listed);
}

/* ── a package lands and is listed ───────────────────────────────────── */

static int body_package_listed(LoopbackHarness *h, int slot,
                               const UpDirs *d) {
    char     path[1200];
    char     dir[1024];
    char     name[64];
    UpResult r;
    bool     listed;
    (void)slot;

    UT_ASSERT(utScratchPath(dir, sizeof(dir), NULL));
    UT_ASSERT(up_write_package(dir, "raid.scenario", kUpPackageManifest));
    UT_ASSERT(up_client_file(path, sizeof(path), "raid.scenario"));
    up_send(h, path, &r);
    threadsWaitForMutex();
    listed = up_listed(h->sim, "raid.scenario", name, sizeof(name));
    threadsReleaseMutex();

    UT_ASSERT_MSG(r.settledAt >= 0, "upload never settled");
    UT_ASSERT_MSG(r.status == 3, "status %d reject %d path '%s'",
                  (int)r.status, (int)r.reject, r.finalPath);
    UT_ASSERT_MSG(strcmp(r.finalPath, "raid.scenario") == 0,
                  "final path '%s'", r.finalPath);
    UT_ASSERT(up_file_exists(d->landing, "raid.scenario"));
    UT_ASSERT_MSG(listed, "the uploaded package is not listed");
    UT_ASSERT_MSG(strcmp(name, "Packed Upload") == 0, "listed as '%s'", name);
    return 0;
}

int run_loopback_script_upload_package_listed(void) {
    return up_run_loopback("PackageListed", SCRIPT_UPLOAD_ALLOW,
                           body_package_listed);
}

/* ── a bound package is refused ──────────────────────────────────────── */

static int body_bound_refused(LoopbackHarness *h, int slot,
                              const UpDirs *d) {
    char     path[1200];
    char     dir[1024];
    UpResult r;
    (void)slot;

    UT_ASSERT(utScratchPath(dir, sizeof(dir), NULL));
    UT_ASSERT(up_write_package(dir, "bound.scenario", kUpBoundManifest));
    UT_ASSERT(up_client_file(path, sizeof(path), "bound.scenario"));
    up_send(h, path, &r);

    UT_ASSERT_MSG(r.settledAt >= 0, "upload never settled");
    UT_ASSERT_MSG(r.status == 4, "status %d", (int)r.status);
    UT_ASSERT_MSG(r.reject == LOBBY_REJECT_INVALID, "reject %d",
                  (int)r.reject);
    UT_ASSERT_MSG(r.reason == SCRIPT_REFUSE_BOUND, "reason %d", (int)r.reason);
    UT_ASSERT_MSG(strstr(r.finalPath, "bound") != NULL,
                  "the operator's line does not say bound: '%s'", r.finalPath);
    /* Nothing landed, and nothing was left on the way. */
    UT_ASSERT_MSG(up_entries(d->landing) == 0, "the landing directory holds "
                  "%d entries", up_entries(d->landing));
    return 0;
}

int run_loopback_script_upload_bound_refused(void) {
    return up_run_loopback("BoundRefused", SCRIPT_UPLOAD_ALLOW,
                           body_bound_refused);
}

/* ── a script that will not load, with its line ──────────────────────── */

static int body_syntax_line(LoopbackHarness *h, int slot, const UpDirs *d) {
    char     path[1200];
    char     dir[1024];
    UpResult r;
    (void)slot;

    UT_ASSERT(utScratchPath(dir, sizeof(dir), NULL));
    UT_ASSERT(up_write_text(dir, "broken.lua",
                            "-- one\n"
                            "-- two\n"
                            "local x = = 1\n"));
    UT_ASSERT(up_client_file(path, sizeof(path), "broken.lua"));
    up_send(h, path, &r);

    UT_ASSERT_MSG(r.settledAt >= 0, "upload never settled");
    UT_ASSERT_MSG(r.status == 4, "status %d", (int)r.status);
    UT_ASSERT_MSG(r.reject == LOBBY_REJECT_INVALID, "reject %d",
                  (int)r.reject);
    /* The line rides the reply as a number, for the client to put into its
     * own line; the operator's text carries it too. */
    UT_ASSERT_MSG(r.reason == SCRIPT_REFUSE_SYNTAX, "reason %d", (int)r.reason);
    UT_ASSERT_MSG(r.a == 3, "line %d", (int)r.a);
    UT_ASSERT_MSG(strstr(r.finalPath, "line 3") != NULL,
                  "the operator's line does not give line 3: '%s'",
                  r.finalPath);
    UT_ASSERT_MSG(up_entries(d->landing) == 0, "the landing directory holds "
                  "%d entries", up_entries(d->landing));
    return 0;
}

int run_loopback_script_upload_syntax_line(void) {
    return up_run_loopback("SyntaxLine", SCRIPT_UPLOAD_ALLOW,
                           body_syntax_line);
}

/* ── a name a higher directory holds, refused at BEGIN ───────────────── */

static int body_name_taken(LoopbackHarness *h, int slot, const UpDirs *d) {
    int  code;
    bool armed;

    UT_ASSERT(up_write_mod(d->shipped, "shipped.lua", "Shipped"));
    code  = send_begin(h, slot, UPLOAD_KIND_SCRIPT, 100, "shipped.lua");
    armed = lastBeginArmed;
    UT_ASSERT_MSG(code == LOBBY_REJECT_NAME_TAKEN,
                  "a shipped name: code %d", code);
    UT_ASSERT_MSG(!armed, "a refused BEGIN left the slot armed");

    /* The uploader's own earlier file is theirs to send again. */
    UT_ASSERT(SDL_CreateDirectory(d->landing));
    UT_ASSERT(up_write_mod(d->landing, "mine.lua", "Mine"));
    code = send_begin(h, slot, UPLOAD_KIND_SCRIPT, 100, "mine.lua");
    UT_ASSERT_MSG(code == 0, "the landing directory's own name: code %d",
                  code);
    /* In another case as well: the listing folds the two spellings, so the
     * landing directory is asked for the one it holds. */
    code = send_begin(h, slot, UPLOAD_KIND_SCRIPT, 100, "MINE.lua");
    UT_ASSERT_MSG(code == 0, "the landing directory's own name in another "
                  "case: code %d", code);
    return 0;
}

int run_script_upload_name_taken(void) {
    return up_run_loopback("NameTaken", SCRIPT_UPLOAD_ALLOW, body_name_taken);
}

/* ── the persist caps ────────────────────────────────────────────────── */

static int body_persist_caps(LoopbackHarness *h, int slot, const UpDirs *d) {
    char file[32];
    char path[1200];
    int  code;
    int  i;
    bool armed;

    threadsWaitForMutex();
    udpServer.scriptUploadMaxFiles        = 32;
    udpServer.scriptUploadMaxStorageBytes = 64u * 1024u * 1024u;
    threadsReleaseMutex();
    UT_ASSERT(SDL_CreateDirectory(d->landing));
    for (i = 0; i < 32; i++) {
        snprintf(file, sizeof(file), "f%02d.lua", i);
        UT_ASSERT(up_write_mod(d->landing, file, file));
    }

    /* A thirty-third file. */
    code  = send_begin(h, slot, UPLOAD_KIND_SCRIPT, 100, "new.lua");
    armed = lastBeginArmed;
    UT_ASSERT_MSG(code == LOBBY_REJECT_UPLOAD_LIMIT_HIT,
                  "the 33rd file: code %d", code);
    UT_ASSERT_MSG(!armed, "a refused BEGIN left the slot armed");

    /* A file replacing one of the 32 is not one more. */
    code = send_begin(h, slot, UPLOAD_KIND_SCRIPT, 100, "f05.lua");
    UT_ASSERT_MSG(code == 0, "a replacement at the cap: code %d", code);

    /* One gone, and the new name fits. */
    snprintf(path, sizeof(path), "%s/f31.lua", d->landing);
    UT_ASSERT(SDL_RemovePath(path));
    code = send_begin(h, slot, UPLOAD_KIND_SCRIPT, 100, "new.lua");
    UT_ASSERT_MSG(code == 0, "the 32nd file: code %d", code);
    return 0;
}

int run_script_upload_persist_caps(void) {
    return up_run_loopback("PersistCaps", SCRIPT_UPLOAD_PERSIST,
                           body_persist_caps);
}

/* ── the accept callback's refusals ──────────────────────────────────── */

static int body_accept_refusals(ServerSim *sim, const UpDirs *d) {
    static const char kMod[] =
        "scenario = {\n"
        "  name = \"Good\",\n"
        "  api = 1,\n"
        "  kind = \"mod\",\n"
        "  bound = false,\n"
        "}\n";
    static const char kFuture[] =
        "scenario = {\n"
        "  name = \"Future\",\n"
        "  api = 99,\n"
        "  kind = \"mod\",\n"
        "  bound = false,\n"
        "}\n";
    ScriptUploadRefusal why;
    bool ok;

    /* Scripts off on this host. Put back before anything is asserted. */
    scenarioHostSetEnabled(false);
    ok = serverSimScriptUploadAccept(sim, d->landing, "off.lua",
                                     (const uint8_t *)kMod, sizeof(kMod) - 1,
                                     &why);
    scenarioHostSetEnabled(true);
    UT_ASSERT_MSG(!ok, "taken with scripts off");
    UT_ASSERT_MSG(why.reason == SCRIPT_REFUSE_SCRIPTS_OFF, "reason %d '%s'",
                  (int)why.reason, why.text);

    /* A name the player's own directory holds, above the landing one. */
    UT_ASSERT(up_write_mod(d->user, "clash.lua", "Clash"));
    ok = serverSimScriptUploadAccept(sim, d->landing, "clash.lua",
                                     (const uint8_t *)kMod, sizeof(kMod) - 1,
                                     &why);
    UT_ASSERT_MSG(!ok, "a name a higher directory holds was taken");
    UT_ASSERT_MSG(why.reason == SCRIPT_REFUSE_NAME_TAKEN, "reason %d '%s'",
                  (int)why.reason, why.text);

    /* An api above the server's. */
    ok = serverSimScriptUploadAccept(sim, d->landing, "future.lua",
                                     (const uint8_t *)kFuture,
                                     sizeof(kFuture) - 1, &why);
    UT_ASSERT_MSG(!ok, "an api above the server's was taken");
    UT_ASSERT_MSG(why.reason == SCRIPT_REFUSE_API && why.a == 99 &&
                  why.b == SCENARIO_API_VERSION,
                  "reason %d (%d, %d) '%s'", (int)why.reason, (int)why.a,
                  (int)why.b, why.text);

    /* None of the three left anything behind. */
    UT_ASSERT_MSG(up_entries(d->landing) == 0, "the landing directory holds "
                  "%d entries", up_entries(d->landing));

    /* And a good one lands. */
    ok = serverSimScriptUploadAccept(sim, d->landing, "good.lua",
                                     (const uint8_t *)kMod, sizeof(kMod) - 1,
                                     &why);
    UT_ASSERT_MSG(ok, "a good mod was refused: %s", why.text);
    UT_ASSERT(up_file_exists(d->landing, "good.lua"));
    UT_ASSERT(up_entries(d->landing) == 1);

    /* The same name in another case replaces it rather than landing beside
     * it as a second file the listing would fold away: one entry, under the
     * spelling that was there. */
    ok = serverSimScriptUploadAccept(sim, d->landing, "GOOD.lua",
                                     (const uint8_t *)kMod, sizeof(kMod) - 1,
                                     &why);
    UT_ASSERT_MSG(ok, "the same name in another case was refused: %s",
                  why.text);
    UT_ASSERT(up_file_exists(d->landing, "good.lua"));
    UT_ASSERT_MSG(up_entries(d->landing) == 1, "the landing directory holds "
                  "%d entries", up_entries(d->landing));
    return 0;
}

int run_script_upload_accept_refusals(void) {
    return up_run_sim("Session", body_accept_refusals);
}

/* ── the session directory emptied, the persist one kept ─────────────── */

static int body_session_emptied(ServerSim *sim, const UpDirs *d) {
    static const char kMod[] =
        "scenario = {\n"
        "  name = \"Session Mod\",\n"
        "  api = 1,\n"
        "  kind = \"mod\",\n"
        "  bound = false,\n"
        "}\n";
    char persistDir[1024];
    char sub[1200];
    ScriptUploadRefusal why;
    bool ok, listedBefore, listedAfter, deepKept;
    int  removed;

    /* ALLOW: the landing directory is the session one. */
    serverSimSetScriptSessionDir(sim, d->landing);
    ok = serverSimScriptUploadAccept(sim, d->landing, "sess.lua",
                                     (const uint8_t *)kMod, sizeof(kMod) - 1,
                                     &why);
    UT_ASSERT_MSG(ok, "the session upload was refused: %s", why.text);
    /* A directory below it is not the emptying's to touch. Made before the
       listing, so the one change between the two listings is the emptying:
       the second listing is right because the emptying told the cache, not
       because making this directory happened to move the stamp. */
    snprintf(sub, sizeof(sub), "%s/sub", d->landing);
    UT_ASSERT(SDL_CreateDirectory(sub));
    UT_ASSERT(up_write_mod(sub, "deep.lua", "Deep"));
    listedBefore = up_listed(sim, "sess.lua", NULL, 0);

    /* What startup does as the server comes back. */
    removed      = serverSimEmptyScriptSessionDir(sim);
    listedAfter  = up_listed(sim, "sess.lua", NULL, 0);
    deepKept     = up_file_exists(sub, "deep.lua");
    UT_ASSERT(listedBefore);
    UT_ASSERT_MSG(removed == 1, "removed %d files", removed);
    UT_ASSERT(!up_file_exists(d->landing, "sess.lua"));
    UT_ASSERT_MSG(!listedAfter, "an emptied session file is still listed");
    UT_ASSERT_MSG(deepKept, "a file below the session directory was removed");

    /* PERSIST: a directory of its own, and no session directory at all. */
    UT_ASSERT(utScratchPath(persistDir, sizeof(persistDir), "Scripts"));
    serverSimSetScriptUploadDir(sim, persistDir);
    serverSimSetScriptSessionDir(sim, "");
    ok = serverSimScriptUploadAccept(sim, persistDir, "kept.lua",
                                     (const uint8_t *)kMod, sizeof(kMod) - 1,
                                     &why);
    UT_ASSERT_MSG(ok, "the persist upload was refused: %s", why.text);
    removed = serverSimEmptyScriptSessionDir(sim);
    UT_ASSERT_MSG(removed == 0, "removed %d files under PERSIST", removed);
    UT_ASSERT(up_file_exists(persistDir, "kept.lua"));
    UT_ASSERT_MSG(up_listed(sim, "kept.lua", NULL, 0),
                  "a persisted upload is not listed");
    return 0;
}

int run_script_upload_session_emptied(void) {
    return up_run_sim("Session", body_session_emptied);
}

/* ── a listing after the server's own removal, stamp unchanged ───────── */

#ifndef _WIN32
/* The worst case the coarse stamp allows, made on purpose: the directory's
   modify time put back to what it was when the listing was kept, so the
   stamp says nothing changed. Only the change count can tell the cache. */
static int body_listing_sees_own_removal(ServerSim *sim, const UpDirs *d) {
    static const char kMod[] =
        "scenario = {\n"
        "  name = \"Gone Mod\",\n"
        "  api = 1,\n"
        "  kind = \"mod\",\n"
        "  bound = false,\n"
        "}\n";
    SDL_PathInfo    before;
    SDL_PathInfo    after;
    struct timespec times[2];
    ScriptUploadRefusal why;
    bool            ok, listedBefore, listedAfter;
    int             removed;

    serverSimSetScriptSessionDir(sim, d->landing);
    ok = serverSimScriptUploadAccept(sim, d->landing, "gone.lua",
                                     (const uint8_t *)kMod, sizeof(kMod) - 1,
                                     &why);
    UT_ASSERT_MSG(ok, "the session upload was refused: %s", why.text);
    listedBefore = up_listed(sim, "gone.lua", NULL, 0);
    UT_ASSERT(listedBefore);
    UT_ASSERT(SDL_GetPathInfo(d->landing, &before));

    removed = serverSimEmptyScriptSessionDir(sim);
    UT_ASSERT_MSG(removed == 1, "removed %d files", removed);

    /* SDL_Time is nanoseconds since the Unix epoch, as the stamp is. */
    times[0].tv_sec  = 0;
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec  = (time_t)(before.modify_time / 1000000000);
    times[1].tv_nsec = (long)(before.modify_time % 1000000000);
    UT_ASSERT_MSG(utimensat(AT_FDCWD, d->landing, times, 0) == 0,
                  "the directory's stamp could not be set back");
    UT_ASSERT(SDL_GetPathInfo(d->landing, &after));
    UT_ASSERT_MSG(after.modify_time == before.modify_time,
                  "the stamp did not go back, so this case tests nothing");

    listedAfter = up_listed(sim, "gone.lua", NULL, 0);
    UT_ASSERT_MSG(!listedAfter, "a file the server removed is still listed "
                                "under an unchanged stamp");
    return 0;
}
#endif

int run_script_upload_listing_sees_own_removal(void) {
#ifdef _WIN32
    /* No utimensat to set a directory's stamp back with; the case is POSIX
       only. */
    fprintf(stderr, "  skipped: needs utimensat\n");
    return 0;
#else
    return up_run_sim("Session", body_listing_sees_own_removal);
#endif
}

/* ── the lobby reset drops the session's picks ───────────────────────── */

static int body_reset_drops(ServerSim *sim, const UpDirs *d) {
    static const char kMod[] =
        "scenario = {\n"
        "  name = \"Session Mod\",\n"
        "  api = 1,\n"
        "  kind = \"mod\",\n"
        "  bound = false,\n"
        "}\n";
    ScnDirEntry        rows[2];
    const ScnDirEntry *row0;
    ScriptUploadRefusal why;
    char               keptFile[SCN_DIR_FILE_LEN];
    bool               ok;
    int                count;

    serverSimSetScriptSessionDir(sim, d->landing);
    ok = serverSimScriptUploadAccept(sim, d->landing, "sess.lua",
                                     (const uint8_t *)kMod, sizeof(kMod) - 1,
                                     &why);
    UT_ASSERT_MSG(ok, "the session upload was refused: %s", why.text);
    UT_ASSERT(up_write_mod(d->shipped, "keep.lua", "Keep"));

    memset(rows, 0, sizeof(rows));
    SDL_strlcpy(rows[0].file, "sess.lua", sizeof(rows[0].file));
    rows[0].keepsWinCondition = true;
    SDL_strlcpy(rows[1].file, "keep.lua", sizeof(rows[1].file));
    rows[1].keepsWinCondition = true;
    serverSimSetScriptList(sim, rows, 2);

    serverSimResetLobbyToDefaults(sim);

    count = serverSimGetScriptCount(sim);
    row0  = serverSimGetScript(sim, 0);
    keptFile[0] = '\0';
    if (row0 != NULL) SDL_strlcpy(keptFile, row0->file, sizeof(keptFile));
    UT_ASSERT_MSG(count == 1, "the list holds %d rows after the reset", count);
    UT_ASSERT_MSG(strcmp(keptFile, "keep.lua") == 0, "row 0 is '%s'",
                  keptFile);
    UT_ASSERT_MSG(!up_file_exists(d->landing, "sess.lua"),
                  "the session directory was not emptied");
    return 0;
}

int run_script_upload_reset_drops_session_picks(void) {
    return up_run_sim("Session", body_reset_drops);
}

/* ── an uploaded mod plays the next round ────────────────────────────── */

static int body_plays_next_round(LoopbackHarness *h, int slot,
                                 const UpDirs *d) {
    char          path[1200];
    char          dir[1024];
    char          name[64];
    UpResult      r;
    ScnDirEntry   row;
    ScenarioHost *host = NULL;
    int           scripts = 0;
    (void)slot;
    (void)d;

    UT_ASSERT(utScratchPath(dir, sizeof(dir), NULL));
    UT_ASSERT(up_write_mod(dir, "fast.lua", "Upload Mod"));
    UT_ASSERT(up_client_file(path, sizeof(path), "fast.lua"));
    up_send(h, path, &r);
    UT_ASSERT_MSG(r.status == 3, "status %d reject %d path '%s'",
                  (int)r.status, (int)r.reject, r.finalPath);

    /* The pick, then the decision a pick asks for. */
    name[0] = '\0';
    threadsWaitForMutex();
    memset(&row, 0, sizeof(row));
    SDL_strlcpy(row.file, "fast.lua", sizeof(row.file));
    SDL_strlcpy(row.name, "Upload Mod", sizeof(row.name));
    row.keepsWinCondition = true;
    serverSimSetScriptList(h->sim, &row, 1);
    scenarioHostFollowMap(h->sim, &host);
    serverSimScenarioOnMapChanged(h->sim, "");
    if (host != NULL) {
        scripts = scenarioHostScriptCount(host);
        SDL_strlcpy(name, scenarioHostName(host), sizeof(name));
    }
    scenarioHostFollowMap(h->sim, NULL);
    scenarioHostDetach(host);
    threadsReleaseMutex();

    UT_ASSERT_MSG(scripts == 1, "the round composed %d scripts", scripts);
    UT_ASSERT_MSG(strcmp(name, "Upload Mod") == 0, "the round runs '%s'",
                  name);
    return 0;
}

int run_loopback_script_upload_plays_next_round(void) {
    return up_run_loopback("PlaysNextRound", SCRIPT_UPLOAD_ALLOW,
                           body_plays_next_round);
}

/* ── where each row came from ────────────────────────────────────────── */

/* The listing's row for file, copied into *out; false when it is not
 * listed. */
static bool up_row(ServerSim *sim, const char *file, ScnDirEntry *out) {
    ScnDirEntry *rows;
    int          n;
    int          i;
    bool         found = false;

    memset(out, 0, sizeof(*out));
    rows = (ScnDirEntry *)calloc(64, sizeof(*rows));
    if (rows == NULL) return false;
    n = serverSimScenarioListDir(sim, rows, 64);
    for (i = 0; i < n; i++) {
        if (strcmp(rows[i].file, file) == 0) {
            *out  = rows[i];
            found = true;
            break;
        }
    }
    free(rows);
    return found;
}

static int body_list_source(ServerSim *sim, const UpDirs *d) {
    ScnDirEntry          row;
    ServerScenarioEntry *pub;
    ControlEvent         evt;
    const ScnDirEntry   *lobby;
    int                  n;
    int                  i;
    bool                 pubFound = false;

    UT_ASSERT(SDL_CreateDirectory(d->landing));
    UT_ASSERT(up_write_mod(d->landing, "up.lua", "Uploaded"));
    UT_ASSERT(up_write_mod(d->shipped, "ship.lua", "Shipped"));
    /* The same name in both: the shipped directory is above the landing
       one, so its copy is the row, and that row is the server's. */
    UT_ASSERT(up_write_mod(d->shipped, "both.lua", "Both Shipped"));
    UT_ASSERT(up_write_mod(d->landing, "both.lua", "Both Uploaded"));

    UT_ASSERT_MSG(up_row(sim, "up.lua", &row), "the uploaded mod is not listed");
    UT_ASSERT_MSG(row.source == SCN_DIR_SOURCE_UPLOAD,
                  "a row from the landing directory says source %u",
                  (unsigned)row.source);
    UT_ASSERT(row.workshopId == 0);

    UT_ASSERT_MSG(up_row(sim, "ship.lua", &row), "the shipped mod is not listed");
    UT_ASSERT_MSG(row.source == SCN_DIR_SOURCE_SERVER,
                  "a shipped row says source %u", (unsigned)row.source);
    UT_ASSERT(row.workshopId == 0);

    UT_ASSERT(up_row(sim, "both.lua", &row));
    UT_ASSERT_MSG(strcmp(row.name, "Both Shipped") == 0,
                  "a name both directories hold listed the copy named '%s'",
                  row.name);
    UT_ASSERT_MSG(row.source == SCN_DIR_SOURCE_SERVER,
                  "the higher directory's copy says source %u",
                  (unsigned)row.source);

    /* The shape a frontend reads carries the same byte. */
    pub = (ServerScenarioEntry *)calloc(64, sizeof(*pub));
    UT_ASSERT(pub != NULL);
    n = serverSimEnumerateScenarioDir(sim, pub, 64);
    for (i = 0; i < n; i++) {
        if (strcmp(pub[i].file, "up.lua") == 0) {
            pubFound = true;
            UT_ASSERT_MSG(pub[i].source == SERVER_SCENARIO_SOURCE_UPLOAD,
                          "the enumeration says source %u",
                          (unsigned)pub[i].source);
            UT_ASSERT(pub[i].workshopId == 0);
        } else if (strcmp(pub[i].file, "ship.lua") == 0) {
            UT_ASSERT(pub[i].source == SERVER_SCENARIO_SOURCE_SERVER);
        }
    }
    free(pub);
    UT_ASSERT_MSG(pubFound, "the enumeration left out the uploaded mod");

    /* Picked, the row is kept whole, and the event the lobby is sent says
       the same. */
    UT_ASSERT(up_row(sim, "up.lua", &row));
    serverSimSetScriptList(sim, &row, 1);
    lobby = serverSimGetLobbyScript(sim, 0);
    UT_ASSERT(lobby != NULL);
    UT_ASSERT_MSG(lobby->source == SCN_DIR_SOURCE_UPLOAD,
                  "the pick list says source %u", (unsigned)lobby->source);
    serverSimFillScriptListEvent(sim, 0, &evt);
    UT_ASSERT(evt.u.lobbyScriptList.count == 1);
    UT_ASSERT_MSG(evt.u.lobbyScriptList.entries[0].source ==
                      SCN_DIR_SOURCE_UPLOAD,
                  "the script-list event says source %u",
                  (unsigned)evt.u.lobbyScriptList.entries[0].source);
    UT_ASSERT(evt.u.lobbyScriptList.entries[0].workshopId == 0);

    /* A landing directory that is also one above it is not listed twice,
       so it has no rows of its own: what it holds is that directory's, and
       says SERVER. */
    serverSimSetScriptUploadDir(sim, d->shipped);
    UT_ASSERT(up_row(sim, "ship.lua", &row));
    UT_ASSERT_MSG(row.source == SCN_DIR_SOURCE_SERVER,
                  "a landing directory equal to the shipped one marked its "
                  "rows %u", (unsigned)row.source);
    return 0;
}

int run_script_upload_list_source(void) {
    return up_run_sim("Session", body_list_source);
}

/* ── a remote client reads which rows were uploaded ──────────────────── */

static bool pred_scenario_list_ready(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetLobbyScenarioListReady(h->cs) &&
           !clientSimGetLobbyScenarioListInFlight(h->cs);
}

/* Where the client's script list holds file, or -1. */
static int up_script_at(const ClientSim *cs, const char *file) {
    int n = clientSimGetLobbyScriptCount(cs);
    int i;

    for (i = 0; i < n; i++) {
        if (strcmp(clientSimGetLobbyScriptFile(cs, i), file) == 0) return i;
    }
    return -1;
}

static bool pred_script_listed(LoopbackHarness *h, void *user) {
    return up_script_at(h->cs, (const char *)user) >= 0;
}

static int body_source_on_wire(LoopbackHarness *h, int slot, const UpDirs *d) {
    char        path[1200];
    char        dir[1024];
    char        fastName[] = "fast.lua";
    const char *files[1]   = { fastName };
    UpResult    r;
    int         upAt   = -1;
    int         shipAt = -1;
    int         n;
    int         i;
    int         at;
    (void)slot;

    UT_ASSERT(up_write_mod(d->shipped, "ship.lua", "Shipped"));
    UT_ASSERT(utScratchPath(dir, sizeof(dir), NULL));
    UT_ASSERT(up_write_mod(dir, "fast.lua", "Upload Mod"));
    UT_ASSERT(up_client_file(path, sizeof(path), "fast.lua"));
    up_send(h, path, &r);
    UT_ASSERT_MSG(r.status == 3, "status %d reject %d path '%s'",
                  (int)r.status, (int)r.reject, r.finalPath);

    /* The list packet, asked for the way the chooser asks. */
    clientSimNetSendLobbyScenarioListRequest(h->cs);
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(h, ACK_MAX,
                                           pred_scenario_list_ready,
                                           NULL) >= 0,
                  "the scenario list never arrived");
    n = clientSimGetLobbyScenarioListCount(h->cs);
    for (i = 0; i < n; i++) {
        const char *f = clientSimGetLobbyScenarioListFile(h->cs, i);
        if (strcmp(f, "fast.lua") == 0) upAt = i;
        if (strcmp(f, "ship.lua") == 0) shipAt = i;
    }
    UT_ASSERT_MSG(upAt >= 0, "the uploaded mod is not in the client's list");
    UT_ASSERT_MSG(shipAt >= 0, "the shipped mod is not in the client's list");
    UT_ASSERT_MSG(clientSimGetLobbyScenarioListSource(h->cs, upAt) ==
                      SERVER_SCENARIO_SOURCE_UPLOAD,
                  "the client reads the uploaded row as source %u",
                  (unsigned)clientSimGetLobbyScenarioListSource(h->cs, upAt));
    UT_ASSERT(clientSimGetLobbyScenarioListWorkshopId(h->cs, upAt) == 0);
    UT_ASSERT_MSG(clientSimGetLobbyScenarioListSource(h->cs, shipAt) ==
                      SERVER_SCENARIO_SOURCE_SERVER,
                  "the client reads the shipped row as source %u",
                  (unsigned)clientSimGetLobbyScenarioListSource(h->cs, shipAt));

    /* The host puts it on the list, through the command a chooser sends. */
    clientSimNetSendSetScriptList(h->cs, files, 1);
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(h, ACK_MAX, pred_script_listed,
                                           fastName) >= 0,
                  "the script list never named the uploaded mod");
    at = up_script_at(h->cs, "fast.lua");
    UT_ASSERT(at >= 0);
    UT_ASSERT_MSG(clientSimGetLobbyScriptSource(h->cs, at) ==
                      SERVER_SCENARIO_SOURCE_UPLOAD,
                  "the script list reads the uploaded row as source %u",
                  (unsigned)clientSimGetLobbyScriptSource(h->cs, at));
    UT_ASSERT(clientSimGetLobbyScriptWorkshopId(h->cs, at) == 0);
    return 0;
}

int run_loopback_script_upload_source_on_wire(void) {
    return up_run_loopback("SourceOnWire", SCRIPT_UPLOAD_ALLOW,
                           body_source_on_wire);
}

/* ── the list asked for on DONE is answered at once ──────────────────── */

/* How many pumps the list may take after DONE: the request out, the server's
 * answer, and the client reading it, with room to spare. */
#define UP_LIST_AFTER_DONE_PUMPS 8

static bool pred_list_seq_moved(LoopbackHarness *h, void *user) {
    return clientSimGetLobbyScenarioListSeq(h->cs) != *(const uint32_t *)user;
}

static int body_list_after_done(LoopbackHarness *h, int slot, const UpDirs *d) {
    char     path[1200];
    char     dir[1024];
    UpResult r;
    uint32_t seq;
    int      at;
    int      upAt = -1;
    int      n;
    int      i;
    (void)slot;
    (void)d;

    UT_ASSERT(utScratchPath(dir, sizeof(dir), NULL));
    UT_ASSERT(up_write_mod(dir, "quick.lua", "Quick Mod"));
    UT_ASSERT(up_client_file(path, sizeof(path), "quick.lua"));
    up_send(h, path, &r);
    UT_ASSERT_MSG(r.status == 3, "status %d reject %d path '%s'",
                  (int)r.status, (int)r.reject, r.finalPath);
    /* The case only means something while the cooldown the upload's BEGIN
       started would still cover the list request and its answer. */
    UT_ASSERT_MSG(r.settledAt >= 0 &&
                      r.settledAt + UP_LIST_AFTER_DONE_PUMPS <
                          LOBBY_REQ_COOLDOWN_TICKS,
                  "the upload took %d pumps, too long to be inside the "
                  "request cooldown", r.settledAt);

    /* Asked on the pump the client first reads DONE, as the chooser asks. */
    seq = clientSimGetLobbyScenarioListSeq(h->cs);
    clientSimNetSendLobbyScenarioListRequest(h->cs);
    at = loopbackHarnessPumpUntil(h, UP_LIST_AFTER_DONE_PUMPS,
                                  pred_list_seq_moved, &seq);
    UT_ASSERT_MSG(at >= 0, "the list asked for on DONE was not answered "
                           "within %d pumps", UP_LIST_AFTER_DONE_PUMPS);

    n = clientSimGetLobbyScenarioListCount(h->cs);
    for (i = 0; i < n; i++) {
        if (strcmp(clientSimGetLobbyScenarioListFile(h->cs, i),
                   "quick.lua") == 0) {
            upAt = i;
        }
    }
    UT_ASSERT_MSG(upAt >= 0, "the uploaded mod is not in the list");
    UT_ASSERT_MSG(clientSimGetLobbyScenarioListSource(h->cs, upAt) ==
                      SERVER_SCENARIO_SOURCE_UPLOAD,
                  "the uploaded row reads source %u",
                  (unsigned)clientSimGetLobbyScenarioListSource(h->cs, upAt));
    return 0;
}

int run_loopback_script_upload_list_after_done(void) {
    return up_run_loopback("ListAfterDone", SCRIPT_UPLOAD_ALLOW,
                           body_list_after_done);
}
