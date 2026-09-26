/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * A copy of one of the server's scripts, asked for from the lobby.
 *
 * The client sends PACKET_LOBBY_SCRIPT_FETCH_REQ naming a file from the
 * scenario listing; the server answers on CHANNEL_BULK with a
 * BULK_KIND_SCRIPT_PACKAGE blob, a status byte and then, when the file is
 * found, its raw bytes. The server finds the name among the rows its
 * directory reads produced and never opens it as a path. The client holds
 * the answer until the lobby takes it.
 *
 * run_script_fetch_req_golden      — the request body against committed hex
 * run_script_fetch_found_lua       — a loose .lua, under loss, arrives byte
 *                                    for byte under its own name
 * run_script_fetch_found_package   — a .scenario arrives as its ZIP bytes
 * run_script_fetch_found_shipped   — a file only the shipped mods directory
 *                                    holds arrives whole
 * run_script_fetch_not_found       — an unknown name, a name reaching a real
 *                                    file outside the directories with ../,
 *                                    and the committed map's own script
 * run_script_fetch_disabled        — sharing off: a status byte and nothing
 *                                    more
 * run_script_fetch_too_large       — a listed file grown past the cap is
 *                                    refused, and one exactly at it is read
 * run_script_fetch_busy_retry      — a request inside the server's cooldown
 *                                    is answered BUSY, asked again and served
 * run_script_fetch_give_up         — a server that answers nothing: the
 *                                    client stops after its sends
 * run_script_fetch_sink_bound      — the client's bulk sink refuses a header
 *                                    whose gen, path or size is not the one
 *                                    it asked for
 * run_script_fetch_stall_fails     — an answer that stops arriving fails
 *                                    with no answer once it has been silent
 *                                    long enough, and the lobby can ask again
 * run_script_fetch_round_start_abort — a round started while a copy is
 *                                    arriving stops the server sending it,
 *                                    and the client reports it cut off
 *
 * The scenarios directory, the player's own directory, the Workshop one and
 * the shipped one are all scratch directories of the case's own
 * (WB_MOD_DIR_USER, WB_MOD_DIR_WORKSHOP and WB_MOD_DIR_SHIPPED name the last
 * three), so nothing on the machine running
 * the test is read. Reads the ServerSim and the server transport's globals
 * directly; the unittests profile permits it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"                 /* the script-fetch API */
#include "client_connect_state.h"
#include "server_sim.h"                 /* serverSimScriptFileRead */
#include "server_sim_internal.h"        /* sim->scenarioMapScript */
#include "server_sim_scenario.h"
#include "transport_udp_internal.h"     /* packHeader, PACKET_HEADER_SIZE */
#include "transport_udp.h"              /* the body builder, SCRIPT_FETCH_* */
#include "transport_udp_server_internal.h" /* udpServer, serverProcessPacket,
                                              LOBBY_REQ_COOLDOWN_TICKS */
#include "bulk_transfer.h"
#include "netpacks.h"
#include "wire_limits.h"                /* LOBBY_PACKAGE_UPLOAD_MAX_BYTES */
#include "scenario_defs.h"              /* ScnDirEntry */
#include "scenario_host.h"              /* scenarioHostRegisterScenarioLister */
#include "scenario_package.h"           /* scnPackageWrite */
#include "threads.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define SF_CONNECT_MAX 2000
#define SF_FETCH_MAX   3000   /* re-sends and bulk resends under loss */

/* ── The directories and the environment ──────────────────────────────── */

static char sfScn[512];       /* the server's scenarios directory */
static char sfUser[512];      /* the player's own mods directory  */
static char sfWorkshop[512];  /* the Workshop directory, left empty */
static char sfShipped[512];   /* the shipped mods directory       */
static char sfLanding[512];   /* where uploads would land          */
static char sfOld[3][1024];   /* the three seams as the case found them */
static bool sfHadOld[3];

static const char *const kSfSeams[3] = { "WB_MOD_DIR_USER",
                                         "WB_MOD_DIR_SHIPPED",
                                         "WB_MOD_DIR_WORKSHOP" };

static void sfSetEnv(const char *key, const char *val) {
#ifdef _WIN32
    _putenv_s(key, val != NULL ? val : "");
#else
    if (val != NULL) setenv(key, val, 1);
    else unsetenv(key);
#endif
}

static bool sfDirs(void) {
    if (!utScratchPath(sfScn, sizeof(sfScn), "scenarios") ||
        !utScratchPath(sfUser, sizeof(sfUser), "Mods") ||
        !utScratchPath(sfWorkshop, sizeof(sfWorkshop), "Workshop") ||
        !utScratchPath(sfShipped, sizeof(sfShipped), "shipped") ||
        !utScratchPath(sfLanding, sizeof(sfLanding), "landing")) {
        return false;
    }
    return SDL_CreateDirectory(sfScn) && SDL_CreateDirectory(sfUser) &&
           SDL_CreateDirectory(sfWorkshop) &&
           SDL_CreateDirectory(sfShipped) && SDL_CreateDirectory(sfLanding);
}

static void sfEnvUp(void) {
    int i;

    for (i = 0; i < 3; i++) {
        const char *was = getenv(kSfSeams[i]);

        sfHadOld[i] = (was != NULL);
        if (was != NULL) SDL_strlcpy(sfOld[i], was, sizeof(sfOld[i]));
    }
    sfSetEnv(kSfSeams[0], sfUser);
    sfSetEnv(kSfSeams[1], sfShipped);
    sfSetEnv(kSfSeams[2], sfWorkshop);
}

static void sfEnvDown(void) {
    int i;

    for (i = 0; i < 3; i++) {
        sfSetEnv(kSfSeams[i], sfHadOld[i] ? sfOld[i] : NULL);
    }
}

static bool sfWrite(const char *dir, const char *name, const void *bytes,
                    size_t len) {
    char  path[1200];
    FILE *f;
    bool  ok;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (f == NULL) return false;
    ok = (len == 0) || (fwrite(bytes, 1, len, f) == len);
    if (fclose(f) != 0) ok = false;
    return ok;
}

static bool sfWriteText(const char *dir, const char *name, const char *text) {
    return sfWrite(dir, name, text, strlen(text));
}

/* A loose mod the listing offers. */
static const char kSfMod[] =
    "scenario = {\n"
    "  name = \"Copy Me\", api = 1, kind = \"mod\", bound = false,\n"
    "  rules = { tank_full_shells = 60 },\n"
    "}\n"
    "-- the rest of the file comes along too\n"
    "function on_start() end\n";

static const char kSfManifest[] =
    "{\n"
    "  \"manifest\": 1, \"api\": 1, \"name\": \"Packed Copy\",\n"
    "  \"bound\": false,\n"
    "  \"script\": \"main.lua\"\n"
    "}\n";

/* ── The loopback ─────────────────────────────────────────────────────── */

static LoopbackHarness sfH;
static bool            sfUp;
static int             sfSlot;

static bool sfPredConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* The fetch has ended, one way or the other. */
static bool sfPredSettled(LoopbackHarness *h, void *user) {
    int st = clientSimGetScriptFetchState(h->cs);

    (void)user;
    return st == CLIENT_SCRIPT_FETCH_DONE || st == CLIENT_SCRIPT_FETCH_FAILED;
}

/* A BUSY answer has come back, or the fetch ended without one. */
static bool sfPredBusySeen(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetScriptFetchStatus(h->cs) ==
               CLIENT_SCRIPT_FETCH_STATUS_BUSY ||
           sfPredSettled(h, NULL);
}

/* The fetch has left WAITING. */
static bool sfPredLeftWaiting(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetScriptFetchState(h->cs) != CLIENT_SCRIPT_FETCH_WAITING;
}

/* This client's bulk sender on the server has nothing staged. */
static bool sfPredSenderIdle(LoopbackHarness *h, void *user) {
    bool idle;

    (void)h;
    (void)user;
    threadsWaitForMutex();
    idle = !bulkSenderBusy(&udpServer.bulkSend[sfSlot]);
    threadsReleaseMutex();
    return idle;
}

/* A lobby server and a client joined to it, the server reading the case's
 * scratch directories through the host's own lister and reader. */
static bool sfNetStart(const char *impair, uint64_t seed) {
    sfUp = true;
    if (!loopbackHarnessStart(&sfH, "Copier", /*lobbyMode*/ true, impair,
                              seed)) {
        return false;
    }
    if (loopbackHarnessPumpUntil(&sfH, SF_CONNECT_MAX, sfPredConnected,
                                 NULL) < 0) {
        return false;
    }
    sfSlot = clientSimGetMyPlayerNum(sfH.cs);
    threadsWaitForMutex();
    serverSimSetScenarioDir(sfH.sim, sfScn);
    serverSimSetScriptUploadDir(sfH.sim, sfLanding);
    scenarioHostRegisterScenarioLister(sfH.sim);
    threadsReleaseMutex();
    return true;
}

/* Ask for file and pump until the fetch ends. The pump count, or -1 when it
 * was not sent or did not end within max pumps. */
static int sfFetch(const char *file, int max) {
    if (!clientSimNetSendLobbyScriptFetch(sfH.cs, file)) return -1;
    return loopbackHarnessPumpUntil(&sfH, max, sfPredSettled, NULL);
}

/* Let the server's per-client request cooldown run out, so the next request
 * is answered on its merits. */
static void sfCooldown(void) {
    loopbackHarnessPumpUntil(&sfH, LOBBY_REQ_COOLDOWN_TICKS + 5, NULL, NULL);
}

/* Take the finished copy and compare it with want. 0 when it matches. */
static int sfTakeMatches(const char *file, const void *want, size_t wantLen) {
    uint8_t *bytes = NULL;
    size_t   len   = 0;
    char     name[BULK_PATH_MAX + 1];
    int      same;

    UT_ASSERT_MSG(clientSimGetScriptFetchState(sfH.cs) ==
                      CLIENT_SCRIPT_FETCH_DONE,
                  "%s ended in state %d, status %d", file,
                  clientSimGetScriptFetchState(sfH.cs),
                  clientSimGetScriptFetchStatus(sfH.cs));
    UT_ASSERT(clientSimGetScriptFetchStatus(sfH.cs) ==
              CLIENT_SCRIPT_FETCH_STATUS_FOUND);
    UT_ASSERT(clientSimTakeScriptFetch(sfH.cs, &bytes, &len, name,
                                       sizeof(name)));
    same = (len == wantLen) &&
           (wantLen == 0 || memcmp(bytes, want, wantLen) == 0);
    free(bytes);
    UT_ASSERT_MSG(same, "%s arrived as %u bytes that are not the file's %u",
                  file, (unsigned)len, (unsigned)wantLen);
    UT_ASSERT_MSG(strcmp(name, file) == 0, "taken under the name %s", name);
    UT_ASSERT(clientSimGetScriptFetchState(sfH.cs) == CLIENT_SCRIPT_FETCH_IDLE);
    return 0;
}

/* Fetch file and expect it to fail with status. 0 when it does. */
static int sfExpectRefused(const char *file, int status) {
    uint8_t *bytes = NULL;
    size_t   len   = 0;
    int      at;

    at = sfFetch(file, SF_FETCH_MAX);
    UT_ASSERT_MSG(at >= 0, "no end to the fetch of %s within %d pumps", file,
                  SF_FETCH_MAX);
    UT_ASSERT_MSG(clientSimGetScriptFetchState(sfH.cs) ==
                          CLIENT_SCRIPT_FETCH_FAILED &&
                      clientSimGetScriptFetchStatus(sfH.cs) == status,
                  "%s ended in state %d, status %d; wanted FAILED, status %d",
                  file, clientSimGetScriptFetchState(sfH.cs),
                  clientSimGetScriptFetchStatus(sfH.cs), status);
    UT_ASSERT(!clientSimTakeScriptFetch(sfH.cs, &bytes, &len, NULL, 0));
    UT_ASSERT(bytes == NULL && len == 0);
    clientSimClearScriptFetch(sfH.cs);
    UT_ASSERT(clientSimGetScriptFetchState(sfH.cs) == CLIENT_SCRIPT_FETCH_IDLE);
    sfCooldown();
    return 0;
}

/* Runs one case with the directories made and the seams up, and takes the
 * harness and the seams down afterwards, pass or fail. */
static int sfRun(int (*body)(void)) {
    int r;

    if (!sfDirs()) UT_FAIL("scratch directories could not be made");
    sfEnvUp();
    sfUp = false;
    memset(&sfH, 0, sizeof(sfH));
    r = body();
    if (sfUp) loopbackHarnessStop(&sfH);
    sfUp = false;
    sfEnvDown();
    return r;
}

/* ── 1. The request body ──────────────────────────────────────────────── */

int run_script_fetch_req_golden(void) {
    /* reqSeq 0x01020304 big-endian, fileLen 5, "m.lua". */
    static const uint8_t want[] = { 0x01, 0x02, 0x03, 0x04, 0x05,
                                    0x6D, 0x2E, 0x6C, 0x75, 0x61 };
    uint8_t out[64];
    char    longName[BULK_PATH_MAX + 2];
    size_t  n;

    memset(out, 0xEE, sizeof(out));
    n = transportUdpClientBuildScriptFetchReqBody(out, sizeof(out),
                                                  0x01020304u, "m.lua");
    UT_ASSERT_MSG(n == sizeof(want), "the body is %u bytes", (unsigned)n);
    UT_ASSERT_MSG(memcmp(out, want, sizeof(want)) == 0,
                  "the body's bytes are not the committed ones");

    /* One byte short writes nothing. */
    UT_ASSERT(transportUdpClientBuildScriptFetchReqBody(
                  out, sizeof(want) - 1, 0x01020304u, "m.lua") == 0);
    UT_ASSERT(transportUdpClientBuildScriptFetchReqBody(
                  out, sizeof(want), 0x01020304u, "m.lua") == sizeof(want));

    /* No name, and a name past the bulk path's length, are not names. */
    UT_ASSERT(transportUdpClientBuildScriptFetchReqBody(out, sizeof(out), 1,
                                                        "") == 0);
    memset(longName, 'a', sizeof(longName) - 1);
    longName[sizeof(longName) - 1] = '\0';
    UT_ASSERT(transportUdpClientBuildScriptFetchReqBody(out, sizeof(out), 1,
                                                        longName) == 0);
    return 0;
}

/* ── 2. A loose .lua ──────────────────────────────────────────────────── */

static int sfBodyFoundLua(void) {
    int at;

    UT_ASSERT(sfWriteText(sfScn, "copy_me.lua", kSfMod));
    UT_ASSERT_MSG(sfNetStart("loss=5,burst=2", 0x5C1F7001u),
                  "the harness did not come up");
    at = sfFetch("copy_me.lua", SF_FETCH_MAX);
    fprintf(stderr, "  script fetch (.lua, loss=5): settled@%d\n", at);
    UT_ASSERT_MSG(at >= 0, "no end to the fetch within %d pumps",
                  SF_FETCH_MAX);
    if (sfTakeMatches("copy_me.lua", kSfMod, strlen(kSfMod)) != 0) return 1;
    UT_ASSERT(clientSimGetConnectState(sfH.cs) == CLIENT_CONNECT_CONNECTED);
    return 0;
}

int run_script_fetch_found_lua(void) {
    return sfRun(sfBodyFoundLua);
}

/* ── 3. A .scenario package ───────────────────────────────────────────── */

static int sfBodyFoundPackage(void) {
    static const char script[] = "function on_setup() end\n";
    ScnPackageEntry   entries[2];
    uint8_t          *pkg    = NULL;
    size_t            pkgLen = 0;
    char              err[256];
    int               at;
    int               r;

    memset(entries, 0, sizeof(entries));
    entries[0].name  = SCN_PACKAGE_MANIFEST_ENTRY;
    entries[0].bytes = (const uint8_t *)kSfManifest;
    entries[0].len   = strlen(kSfManifest);
    entries[1].name  = SCN_PACKAGE_SCRIPT_ENTRY;
    entries[1].bytes = (const uint8_t *)script;
    entries[1].len   = strlen(script);
    err[0] = '\0';
    UT_ASSERT_MSG(scnPackageWrite(entries, 2, &pkg, &pkgLen, err, sizeof(err)),
                  "the package could not be written: %s", err);
    if (!sfWrite(sfScn, "packed_copy.scenario", pkg, pkgLen)) {
        free(pkg);
        UT_FAIL("the package could not be put in the directory");
    }

    if (!sfNetStart("loss=5,burst=2", 0x5C1F7002u)) {
        free(pkg);
        UT_FAIL("the harness did not come up");
    }
    at = sfFetch("packed_copy.scenario", SF_FETCH_MAX);
    fprintf(stderr, "  script fetch (.scenario, loss=5): settled@%d\n", at);
    if (at < 0) {
        free(pkg);
        UT_FAIL("no end to the fetch within %d pumps", SF_FETCH_MAX);
    }
    /* The ZIP as it sits on disk, not the script unpacked from it. */
    r = sfTakeMatches("packed_copy.scenario", pkg, pkgLen);
    free(pkg);
    return r;
}

int run_script_fetch_found_package(void) {
    return sfRun(sfBodyFoundPackage);
}

/* ── 4. Only in the shipped mods ──────────────────────────────────────── */

static int sfBodyFoundShipped(void) {
    char path[1200];
    int  at;

    UT_ASSERT(sfWriteText(sfShipped, "shipped_only.lua", kSfMod));
    snprintf(path, sizeof(path), "%s/shipped_only.lua", sfScn);
    UT_ASSERT(!SDL_GetPathInfo(path, NULL));
    UT_ASSERT_MSG(sfNetStart("loss=5,burst=2", 0x5C1F7003u),
                  "the harness did not come up");
    at = sfFetch("shipped_only.lua", SF_FETCH_MAX);
    fprintf(stderr, "  script fetch (shipped, loss=5): settled@%d\n", at);
    UT_ASSERT_MSG(at >= 0, "no end to the fetch within %d pumps",
                  SF_FETCH_MAX);
    return sfTakeMatches("shipped_only.lua", kSfMod, strlen(kSfMod));
}

int run_script_fetch_found_shipped(void) {
    return sfRun(sfBodyFoundShipped);
}

/* ── 5. Not found ─────────────────────────────────────────────────────── */

static int sfBodyNotFound(void) {
    char                   outside[512];
    char                   saved[SCN_DIR_FILE_LEN];
    uint8_t               *bytes = NULL;
    uint32_t               len   = 0;
    ServerScriptReadResult before;
    ServerScriptReadResult during;

    /* A real, listable file one directory above the scenarios directory,
       which "../" from there names. */
    UT_ASSERT(utScratchPath(outside, sizeof(outside), NULL));
    UT_ASSERT(sfWriteText(outside, "outside.lua", kSfMod));
    UT_ASSERT(sfWriteText(sfScn, "map_own.lua", kSfMod));
    UT_ASSERT_MSG(sfNetStart(NULL, 0x5C1F7005u), "the harness did not come up");

    if (sfExpectRefused("NoSuchScript.lua",
                        CLIENT_SCRIPT_FETCH_STATUS_NOT_FOUND) != 0) {
        return 1;
    }
    if (sfExpectRefused("../outside.lua",
                        CLIENT_SCRIPT_FETCH_STATUS_NOT_FOUND) != 0) {
        return 1;
    }

    /* The committed map's own script, read straight off the sim: the file of
       that name is served until the map's script carries the name, and not
       after. */
    threadsWaitForMutex();
    before = serverSimScriptFileRead(sfH.sim, "map_own.lua", &bytes, &len);
    free(bytes);
    bytes = NULL;
    SDL_strlcpy(saved, sfH.sim->scenarioMapScript.file, sizeof(saved));
    SDL_strlcpy(sfH.sim->scenarioMapScript.file, "map_own.lua",
                sizeof(sfH.sim->scenarioMapScript.file));
    during = serverSimScriptFileRead(sfH.sim, "map_own.lua", &bytes, &len);
    SDL_strlcpy(sfH.sim->scenarioMapScript.file, saved,
                sizeof(sfH.sim->scenarioMapScript.file));
    threadsReleaseMutex();
    free(bytes);
    UT_ASSERT_MSG(before == SERVER_SCRIPT_READ_FOUND,
                  "the file was not served before it was the map's (%d)",
                  (int)before);
    UT_ASSERT_MSG(during == SERVER_SCRIPT_READ_NOT_FOUND,
                  "the map's own script was served (%d)", (int)during);
    UT_ASSERT(clientSimGetConnectState(sfH.cs) == CLIENT_CONNECT_CONNECTED);
    return 0;
}

int run_script_fetch_not_found(void) {
    return sfRun(sfBodyNotFound);
}

/* ── 6. Sharing off ───────────────────────────────────────────────────── */

static int sfBodyDisabled(void) {
    uint8_t            pkt[PACKET_HEADER_SIZE + 4 + 1 + BULK_PATH_MAX];
    size_t             n;
    struct sockaddr_in from;
    bool               staged;
    uint32_t           total;
    uint8_t            kind;
    uint8_t            status;

    UT_ASSERT(sfWriteText(sfScn, "copy_me.lua", kSfMod));
    UT_ASSERT_MSG(sfNetStart(NULL, 0x5C1F7006u), "the harness did not come up");
    threadsWaitForMutex();
    serverSimSetScriptSharing(sfH.sim, false);
    threadsReleaseMutex();

    if (sfExpectRefused("copy_me.lua", CLIENT_SCRIPT_FETCH_STATUS_DISABLED) !=
        0) {
        return 1;
    }

    /* The answer the server stages: the header, then the status byte, and
       nothing behind it. */
    UT_ASSERT(loopbackHarnessPumpUntil(&sfH, SF_FETCH_MAX, sfPredSenderIdle,
                                       NULL) >= 0);
    packHeader(pkt, PACKET_LOBBY_SCRIPT_FETCH_REQ, 1);
    n = transportUdpClientBuildScriptFetchReqBody(
        pkt + PACKET_HEADER_SIZE, sizeof(pkt) - PACKET_HEADER_SIZE, 0x77u,
        "copy_me.lua");
    UT_ASSERT(n > 0);
    threadsWaitForMutex();
    udpServer.clientReqCooldownTicks[sfSlot] = 0;
    from = udpServer.clients[sfSlot].addr;
    serverProcessPacket(sfH.sim, pkt, (int)(PACKET_HEADER_SIZE + n), &from);
    staged = bulkSenderBusy(&udpServer.bulkSend[sfSlot]);
    total  = udpServer.bulkSend[sfSlot].total;
    kind   = udpServer.bulkSend[sfSlot].kind;
    status = (staged && total > 0) ? udpServer.bulkSend[sfSlot].buf[total - 1]
                                   : 0xFF;
    threadsReleaseMutex();
    UT_ASSERT_MSG(staged && kind == BULK_KIND_SCRIPT_PACKAGE,
                  "no answer was staged");
    UT_ASSERT_MSG(total == BULK_STREAM_HEADER_FIXED + strlen("copy_me.lua") + 1,
                  "the answer is %u bytes with its header, not a status byte",
                  (unsigned)total);
    UT_ASSERT(status == BULK_SCRIPT_DISABLED);
    return 0;
}

int run_script_fetch_disabled(void) {
    return sfRun(sfBodyDisabled);
}

/* ── 7. The size cap ──────────────────────────────────────────────────── */

/* The mod, then a comment of filler to make it exactly len bytes. */
static uint8_t *sfSized(size_t len) {
    uint8_t *buf = (uint8_t *)malloc(len);
    size_t   head = strlen(kSfMod);

    if (buf == NULL || len < head + 3) {
        free(buf);
        return NULL;
    }
    memcpy(buf, kSfMod, head);
    memcpy(buf + head, "--", 2);
    memset(buf + head + 2, 'x', len - head - 3);
    buf[len - 1] = '\n';
    return buf;
}

static int sfBodyTooLarge(void) {
    ScnDirEntry            rows[8];
    uint8_t               *atCap;
    uint8_t               *over;
    uint8_t               *bytes = NULL;
    uint32_t               len   = 0;
    ServerScriptReadResult rr;
    bool                   same;
    int                    n;
    int                    i;
    bool                   listed = false;

    /* No file this size lists: the directory read takes a loose script up to
       1 MiB and a package up to the cap. So the file is listed small, and
       then grown where it stands, which leaves the directory's stamp where
       the kept listing read it. */
    UT_ASSERT(sfWriteText(sfScn, "grown.lua", kSfMod));
    UT_ASSERT_MSG(sfNetStart(NULL, 0x5C1F7007u), "the harness did not come up");
    threadsWaitForMutex();
    n = serverSimScenarioListDir(sfH.sim, rows, 8);
    threadsReleaseMutex();
    for (i = 0; i < n; i++) {
        if (strcmp(rows[i].file, "grown.lua") == 0) listed = true;
    }
    UT_ASSERT_MSG(listed, "the small file was not listed");

    /* Exactly at the cap: read whole, straight off the sim. */
    atCap = sfSized(LOBBY_PACKAGE_UPLOAD_MAX_BYTES);
    UT_ASSERT(atCap != NULL);
    if (!sfWrite(sfScn, "grown.lua", atCap, LOBBY_PACKAGE_UPLOAD_MAX_BYTES)) {
        free(atCap);
        UT_FAIL("the file could not be grown");
    }
    threadsWaitForMutex();
    rr = serverSimScriptFileRead(sfH.sim, "grown.lua", &bytes, &len);
    threadsReleaseMutex();
    same = (rr == SERVER_SCRIPT_READ_FOUND) &&
           len == LOBBY_PACKAGE_UPLOAD_MAX_BYTES &&
           memcmp(bytes, atCap, len) == 0;
    free(bytes);
    free(atCap);
    UT_ASSERT_MSG(same, "a file at the cap read as %d, %u bytes (a NOT_FOUND "
                  "here means the listing was read again after the file "
                  "grew)", (int)rr, (unsigned)len);

    /* One byte over: refused over the wire, and nothing read. */
    over = sfSized((size_t)LOBBY_PACKAGE_UPLOAD_MAX_BYTES + 1);
    UT_ASSERT(over != NULL);
    same = sfWrite(sfScn, "grown.lua", over,
                   (size_t)LOBBY_PACKAGE_UPLOAD_MAX_BYTES + 1);
    free(over);
    UT_ASSERT_MSG(same, "the file could not be grown");
    sfCooldown();
    if (sfExpectRefused("grown.lua", CLIENT_SCRIPT_FETCH_STATUS_TOO_LARGE) !=
        0) {
        return 1;
    }
    threadsWaitForMutex();
    rr = serverSimScriptFileRead(sfH.sim, "grown.lua", &bytes, &len);
    threadsReleaseMutex();
    UT_ASSERT(rr == SERVER_SCRIPT_READ_TOO_LARGE);
    UT_ASSERT(bytes == NULL && len == 0);
    return 0;
}

int run_script_fetch_too_large(void) {
    return sfRun(sfBodyTooLarge);
}

/* ── 8. Asked too soon ────────────────────────────────────────────────── */

static int sfBodyBusyRetry(void) {
    int at;

    UT_ASSERT(sfWriteText(sfScn, "copy_me.lua", kSfMod));
    UT_ASSERT_MSG(sfNetStart(NULL, 0x5C1F7008u), "the harness did not come up");

    /* The cooldown up, as a request a moment ago would leave it. It runs out
       long before the client asks again. */
    threadsWaitForMutex();
    udpServer.clientReqCooldownTicks[sfSlot] = LOBBY_REQ_COOLDOWN_TICKS;
    threadsReleaseMutex();
    UT_ASSERT(clientSimNetSendLobbyScriptFetch(sfH.cs, "copy_me.lua"));
    at = loopbackHarnessPumpUntil(&sfH, SF_FETCH_MAX, sfPredBusySeen, NULL);
    UT_ASSERT_MSG(at >= 0 &&
                      clientSimGetScriptFetchStatus(sfH.cs) ==
                          CLIENT_SCRIPT_FETCH_STATUS_BUSY,
                  "no BUSY answer (state %d, status %d)",
                  clientSimGetScriptFetchState(sfH.cs),
                  clientSimGetScriptFetchStatus(sfH.cs));
    UT_ASSERT_MSG(clientSimGetScriptFetchState(sfH.cs) ==
                      CLIENT_SCRIPT_FETCH_WAITING,
                  "a BUSY answer did not leave the fetch waiting to ask again");

    at = loopbackHarnessPumpUntil(&sfH, SF_FETCH_MAX, sfPredSettled, NULL);
    fprintf(stderr, "  script fetch (after BUSY): settled@%d\n", at);
    UT_ASSERT_MSG(at >= 0, "no end to the fetch within %d pumps",
                  SF_FETCH_MAX);
    return sfTakeMatches("copy_me.lua", kSfMod, strlen(kSfMod));
}

int run_script_fetch_busy_retry(void) {
    return sfRun(sfBodyBusyRetry);
}

/* ── 9. No answer at all ──────────────────────────────────────────────── */

static int sfBodyGiveUp(void) {
    const int last = SCRIPT_FETCH_SENDS * SCRIPT_FETCH_TIMEOUT_TICKS;
    int       at;

    UT_ASSERT(sfWriteText(sfScn, "copy_me.lua", kSfMod));
    UT_ASSERT_MSG(sfNetStart(NULL, 0x5C1F7009u), "the harness did not come up");

    /* The server does not tick, so nothing is answered. Each send waits
       SCRIPT_FETCH_TIMEOUT_TICKS: still waiting just before the last one runs
       out, and failed just after, inside the link's own dead-server
       timeout. */
    UT_ASSERT(clientSimNetSendLobbyScriptFetch(sfH.cs, "copy_me.lua"));
    loopbackHarnessPumpClientOnly(&sfH, last - 5);
    UT_ASSERT_MSG(clientSimGetScriptFetchState(sfH.cs) ==
                      CLIENT_SCRIPT_FETCH_WAITING,
                  "the fetch ended before its %d sends ran out (state %d)",
                  SCRIPT_FETCH_SENDS, clientSimGetScriptFetchState(sfH.cs));
    loopbackHarnessPumpClientOnly(&sfH, 10);
    UT_ASSERT_MSG(clientSimGetScriptFetchState(sfH.cs) ==
                          CLIENT_SCRIPT_FETCH_FAILED &&
                      clientSimGetScriptFetchStatus(sfH.cs) ==
                          CLIENT_SCRIPT_FETCH_NO_ANSWER,
                  "after every send timed out: state %d, status %d",
                  clientSimGetScriptFetchState(sfH.cs),
                  clientSimGetScriptFetchStatus(sfH.cs));

    /* The server catches up on the queued requests, whose answers the
       finished fetch does not take; a new fetch is then served. */
    loopbackHarnessPumpUntil(&sfH, 50, NULL, NULL);
    UT_ASSERT(clientSimGetConnectState(sfH.cs) == CLIENT_CONNECT_CONNECTED);
    UT_ASSERT(clientSimGetScriptFetchState(sfH.cs) ==
              CLIENT_SCRIPT_FETCH_FAILED);
    UT_ASSERT(loopbackHarnessPumpUntil(&sfH, SF_FETCH_MAX, sfPredSenderIdle,
                                       NULL) >= 0);
    sfCooldown();
    at = sfFetch("copy_me.lua", SF_FETCH_MAX);
    UT_ASSERT_MSG(at >= 0, "no end to the next fetch");
    return sfTakeMatches("copy_me.lua", kSfMod, strlen(kSfMod));
}

int run_script_fetch_give_up(void) {
    return sfRun(sfBodyGiveUp);
}

/* ── 10. The client's sink ────────────────────────────────────────────── */

/* Pack one stream header for a script package onto the end of blob. */
static size_t sfPutHeader(uint8_t *blob, size_t at, uint32_t gen,
                          const char *path, uint32_t totalSize) {
    BulkStreamHeader sh;

    memset(&sh, 0, sizeof(sh));
    sh.kind      = BULK_KIND_SCRIPT_PACKAGE;
    sh.gen       = gen;
    sh.totalSize = totalSize;
    sh.pathLen   = (uint8_t)strlen(path);
    memcpy(sh.path, path, (size_t)sh.pathLen + 1);
    return at + (size_t)bulkPackStreamHeader(blob + at, &sh);
}

/* Stage raw stream bytes on this client's bulk sender: first as the header
 * bulkSenderBegin packs, the rest as they are, so blob can carry more
 * headers of its own. */
static bool sfStage(uint32_t gen, const char *path, uint32_t totalSize,
                    const uint8_t *blob, uint32_t blobLen) {
    BulkStreamHeader sh;
    bool             staged;

    memset(&sh, 0, sizeof(sh));
    sh.kind      = BULK_KIND_SCRIPT_PACKAGE;
    sh.gen       = gen;
    sh.totalSize = totalSize;
    sh.pathLen   = (uint8_t)strlen(path);
    memcpy(sh.path, path, (size_t)sh.pathLen + 1);
    threadsWaitForMutex();
    staged = bulkSenderBegin(&udpServer.bulkSend[sfSlot], &sh, blob, blobLen);
    threadsReleaseMutex();
    return staged;
}

static int sfBodySinkBound(void) {
    uint8_t  blob[4 * BULK_STREAM_HEADER_MAX];
    size_t   at = 0;
    uint8_t *bytes = NULL;
    size_t   len   = 0;
    char     name[BULK_PATH_MAX + 1];
    bool     staged;
    int      settled;

    UT_ASSERT_MSG(sfNetStart(NULL, 0x5C1F700Au), "the harness did not come up");
    UT_ASSERT(loopbackHarnessPumpUntil(&sfH, SF_FETCH_MAX, sfPredSenderIdle,
                                       NULL) >= 0);

    /* The client's first fetch goes out as reqSeq 1: the counter starts at 0
       and skips it. Three transfers in one stream, ahead of anything the
       server answers: the wrong gen, the wrong path, then the right one. The
       server drops the real request while this is staged, or answers it
       behind it, which the finished fetch does not take. */
    UT_ASSERT(clientSimNetSendLobbyScriptFetch(sfH.cs, "sink.lua"));
    blob[at++] = BULK_SCRIPT_FOUND;   /* the wrong gen's body */
    blob[at++] = 'X';
    at = sfPutHeader(blob, at, 1, "other.lua", 2);
    blob[at++] = BULK_SCRIPT_FOUND;
    blob[at++] = 'Y';
    at = sfPutHeader(blob, at, 1, "sink.lua", 4);
    blob[at++] = BULK_SCRIPT_FOUND;
    blob[at++] = 'a';
    blob[at++] = 'b';
    blob[at++] = 'c';
    staged = sfStage(1u + 100u, "sink.lua", 2, blob, (uint32_t)at);
    UT_ASSERT_MSG(staged, "the transfers were not staged");

    settled = loopbackHarnessPumpUntil(&sfH, SF_FETCH_MAX, sfPredSettled, NULL);
    UT_ASSERT_MSG(settled >= 0, "the fetch never ended");
    UT_ASSERT_MSG(clientSimTakeScriptFetch(sfH.cs, &bytes, &len, name,
                                           sizeof(name)),
                  "no copy to take (state %d, status %d)",
                  clientSimGetScriptFetchState(sfH.cs),
                  clientSimGetScriptFetchStatus(sfH.cs));
    staged = (len == 3 && memcmp(bytes, "abc", 3) == 0);
    free(bytes);
    UT_ASSERT_MSG(staged, "the sink took a transfer that was not its answer "
                  "(%u bytes)", (unsigned)len);
    UT_ASSERT(strcmp(name, "sink.lua") == 0);

    /* A size one past the most an answer can be, for the right gen and path:
       refused before anything is allocated, so the fetch never starts
       receiving. The body is never sent; the stream is spent after this. */
    UT_ASSERT(loopbackHarnessPumpUntil(&sfH, SF_FETCH_MAX, sfPredSenderIdle,
                                       NULL) >= 0);
    UT_ASSERT(clientSimNetSendLobbyScriptFetch(sfH.cs, "sink.lua"));
    staged = sfStage(2, "sink.lua", 2u + LOBBY_PACKAGE_UPLOAD_MAX_BYTES, NULL,
                     0);
    UT_ASSERT_MSG(staged, "the oversized header was not staged");
    settled = loopbackHarnessPumpUntil(&sfH, SCRIPT_FETCH_TIMEOUT_TICKS - 20,
                                       sfPredLeftWaiting, NULL);
    UT_ASSERT_MSG(settled < 0,
                  "the fetch left WAITING for state %d on an oversized header",
                  clientSimGetScriptFetchState(sfH.cs));
    UT_ASSERT(clientSimGetScriptFetchPercent(sfH.cs) == 0);
    return 0;
}

int run_script_fetch_sink_bound(void) {
    return sfRun(sfBodySinkBound);
}

/* ── 11. An answer that stops arriving ────────────────────────────────── */

/* Big enough to span many bulk segments and well past what the channel holds
 * at once, and still small enough for the listing to read a loose script. */
#define SF_BIG_LEN (900u * 1024u)

/* The answer has started arriving, or the fetch ended without that. */
static bool sfPredReceiving(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetScriptFetchState(h->cs) ==
               CLIENT_SCRIPT_FETCH_RECEIVING ||
           sfPredSettled(h, NULL);
}

/* The big file in the scenarios directory, and a fetch of it arriving. */
static int sfStartBigFetch(uint64_t seed) {
    uint8_t *big = sfSized(SF_BIG_LEN);
    bool     ok;

    UT_ASSERT(big != NULL);
    ok = sfWrite(sfScn, "big.lua", big, SF_BIG_LEN);
    free(big);
    UT_ASSERT_MSG(ok, "the big file could not be written");
    UT_ASSERT_MSG(sfNetStart(NULL, seed), "the harness did not come up");
    UT_ASSERT(clientSimNetSendLobbyScriptFetch(sfH.cs, "big.lua"));
    UT_ASSERT(loopbackHarnessPumpUntil(&sfH, SF_FETCH_MAX, sfPredReceiving,
                                       NULL) >= 0);
    UT_ASSERT_MSG(clientSimGetScriptFetchState(sfH.cs) ==
                      CLIENT_SCRIPT_FETCH_RECEIVING,
                  "the answer never started arriving (state %d, status %d)",
                  clientSimGetScriptFetchState(sfH.cs),
                  clientSimGetScriptFetchStatus(sfH.cs));
    return 0;
}

static int sfBodyStallFails(void) {
    uint8_t *bytes = NULL;
    size_t   len   = 0;
    int      at;

    if (sfStartBigFetch(0x5C1F700Bu) != 0) return 1;

    /* The server drops the rest of the copy without telling the client: what
       the channel already holds still arrives, and then nothing does. */
    threadsWaitForMutex();
    bulkSenderReset(&udpServer.bulkSend[sfSlot]);
    threadsReleaseMutex();

    loopbackHarnessPumpUntil(&sfH, SCRIPT_FETCH_NO_PROGRESS_TICKS / 2, NULL,
                             NULL);
    UT_ASSERT_MSG(clientSimGetScriptFetchState(sfH.cs) ==
                      CLIENT_SCRIPT_FETCH_RECEIVING,
                  "the stalled copy left RECEIVING early (state %d)",
                  clientSimGetScriptFetchState(sfH.cs));
    UT_ASSERT(clientSimGetScriptFetchPercent(sfH.cs) < 100);

    at = loopbackHarnessPumpUntil(&sfH, SCRIPT_FETCH_NO_PROGRESS_TICKS + 200,
                                  sfPredSettled, NULL);
    UT_ASSERT_MSG(at >= 0, "the stalled copy never ended");
    UT_ASSERT_MSG(clientSimGetScriptFetchState(sfH.cs) ==
                          CLIENT_SCRIPT_FETCH_FAILED &&
                      clientSimGetScriptFetchStatus(sfH.cs) ==
                          CLIENT_SCRIPT_FETCH_NO_ANSWER,
                  "the stalled copy ended in state %d, status %d",
                  clientSimGetScriptFetchState(sfH.cs),
                  clientSimGetScriptFetchStatus(sfH.cs));
    UT_ASSERT(clientSimGetScriptFetchPercent(sfH.cs) == 0);
    UT_ASSERT(!clientSimTakeScriptFetch(sfH.cs, &bytes, &len, NULL, 0));

    /* The lobby can ask again. */
    UT_ASSERT(clientSimNetSendLobbyScriptFetch(sfH.cs, "big.lua"));
    UT_ASSERT(clientSimGetScriptFetchState(sfH.cs) ==
              CLIENT_SCRIPT_FETCH_WAITING);
    return 0;
}

int run_script_fetch_stall_fails(void) {
    return sfRun(sfBodyStallFails);
}

/* ── 12. A round starts while a copy is arriving ──────────────────────── */

static bool sfPredRunning(LoopbackHarness *h, void *user) {
    (void)user;
    return serverSimGetState(h->sim) == serverStateRunning;
}

static bool sfPredLeftReceiving(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetScriptFetchState(h->cs) != CLIENT_SCRIPT_FETCH_RECEIVING;
}

static int sfBodyRoundStartAbort(void) {
    bool copying;
    int  at;

    if (sfStartBigFetch(0x5C1F700Cu) != 0) return 1;

    /* The host starts the round with the copy mid-stream; the countdown is
       cut to one tick so the start comes long before the copy would end. */
    UT_ASSERT_MSG(loopbackHarnessTriggerGameStart(&sfH),
                  "the client's slot was never ready for game start");
    threadsWaitForMutex();
    sfH.sim->countdownTicks = 1;
    threadsReleaseMutex();
    at = loopbackHarnessPumpUntil(&sfH, 600, sfPredRunning, NULL);
    UT_ASSERT_MSG(at >= 0, "the game never reached the running state");

    threadsWaitForMutex();
    copying = bulkSenderBusy(&udpServer.bulkSend[sfSlot]) &&
              udpServer.bulkSend[sfSlot].kind == BULK_KIND_SCRIPT_PACKAGE;
    threadsReleaseMutex();
    UT_ASSERT_MSG(!copying, "the copy was still being sent into the round");

    /* The bulk channel's new baseline reaches the client, which drops its
       partial copy and reports it as cut off. */
    at = loopbackHarnessPumpUntil(&sfH, 500, sfPredLeftReceiving, NULL);
    UT_ASSERT_MSG(at >= 0, "the client was still receiving the copy");
    UT_ASSERT_MSG(clientSimGetScriptFetchState(sfH.cs) ==
                          CLIENT_SCRIPT_FETCH_FAILED &&
                      clientSimGetScriptFetchStatus(sfH.cs) ==
                          CLIENT_SCRIPT_FETCH_NO_ANSWER,
                  "the cut-off copy ended in state %d, status %d",
                  clientSimGetScriptFetchState(sfH.cs),
                  clientSimGetScriptFetchStatus(sfH.cs));
    return 0;
}

int run_script_fetch_round_start_abort(void) {
    return sfRun(sfBodyRoundStartAbort);
}
