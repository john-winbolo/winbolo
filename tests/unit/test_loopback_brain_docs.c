/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * A brain's commands.txt, fetched over the real UDP transport.
 *
 * A joiner is sent each brain's announce line and the length and generation
 * of its docs (CTRL_LOBBY_BRAIN_ANNOUNCE), and nothing more. The docs are
 * asked for with PACKET_LOBBY_BRAIN_DOCS_REQ, a bare datagram, and come back
 * zlib-compressed on CHANNEL_BULK. Two cases:
 *
 *   loopback_brain_docs_fetch — a player, under loss. The request can be
 *       lost and the answer's segments can be lost; the client's re-ask and
 *       the bulk channel's resends must still land the text whole. Then the
 *       file changes on the server and the round-return path re-announces
 *       it: the client must drop what it holds and fetch the new text rather
 *       than go on showing the old one.
 *
 *   loopback_brain_docs_spectator — a spectator watching the live lobby
 *       reads the same bot announce lines, so the server answers its request
 *       on the spectator's own bulk stream.
 *
 *   loopback_brain_docs_survives_bulk_rebase — the server re-bases the
 *       player's bulk channel (a lobby map change does this) while the docs
 *       answer is part way through arriving. The client drops the partial
 *       answer, so it must ask for the docs again rather than wait for the
 *       rest of an answer that will never come.
 *
 * The brain is written into the test's own scratch directory and put in the
 * server's catalogue by hand, so the case does not depend on which brains are
 * installed beside the binary.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "brain_list.h"
#include "client_sim.h"
#include "client_net.h"             /* clientSimGetConnectState */
#include "client_connect_state.h"
#include "control_event.h"
#include "server_sim.h"
#include "server_sim_internal.h"    /* brainList / brainPaths */
#include "client_sim_internal.h"    /* lobbyBrainTexts->rxIdx */
#include "transport_udp_server_internal.h" /* serverRebaseBulkAndRearmDownload */
#include "threads.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define CONNECT_MAX   2000
#define ANNOUNCE_MAX  2000
#define DOCS_MAX      4000   /* re-asks + bulk resends under loss */

/* Large enough to span many bulk segments once compressed, and made of words
 * rather than one repeated letter so it does not collapse to a few bytes. */
#define DOCS_BYTES    24000

static char s_dir[512];

static bool writeFile(const char *path, const char *bytes, size_t n) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) return false;
    if (n > 0 && fwrite(bytes, 1, n, f) != n) { fclose(f); return false; }
    fclose(f);
    return true;
}

/* Fill `out` with DOCS_BYTES of word salad from `seed`. */
static void makeDocs(char *out, uint32_t seed) {
    static const char *const words[] = {
        "hold", "north", "fire", "pillbox", "base", "mine", "tree", "boat",
        "follow", "me", "retreat", "cover", "attack", "the", "at", "once",
    };
    size_t pos = 0;
    while (pos < DOCS_BYTES) {
        const char *w;
        size_t n;
        seed = seed * 1103515245u + 12345u;
        w = words[(seed >> 16) % 16u];
        n = strlen(w);
        if (pos + n + 1 > DOCS_BYTES) break;
        memcpy(out + pos, w, n);
        pos += n;
        out[pos++] = ((seed >> 8) % 9u == 0) ? '\n' : ' ';
    }
    while (pos < DOCS_BYTES) out[pos++] = '.';
    out[DOCS_BYTES] = '\0';
}

/* Write the brain's two files. */
static bool writeBrain(const char *announce, const char *docs) {
    char path[600];
    SDL_snprintf(path, sizeof(path), "%s/announce.txt", s_dir);
    if (!writeFile(path, announce, strlen(announce))) return false;
    SDL_snprintf(path, sizeof(path), "%s/commands.txt", s_dir);
    return writeFile(path, docs, strlen(docs));
}

static void publishCb(void *ctx, const ControlEvent *evt) {
    serverSimPublishControl((ServerSim *)ctx, evt);
}

/* Make the scratch brain the server's only brain, re-read it, and announce it
 * to every client — the round-return path's refresh and publish. */
static void installAndAnnounce(LoopbackHarness *h) {
    threadsWaitForMutex();
    SDL_snprintf(h->sim->brainPaths[0], BRAIN_LIST_PATH_LEN, "%s/init.lua",
                 s_dir);
    SDL_snprintf(h->sim->brainList.entries[0].name, BRAIN_LIST_NAME_LEN,
                 "%s", "DocsBrain");
    h->sim->brainList.count = 1;
    serverSimRefreshBrainDocs(h->sim);
    serverSimEmitBrainAnnounces(h->sim, publishCb, h->sim);
    threadsReleaseMutex();
}

typedef struct {
    const char *announce;
} AnnounceWant;

static bool pred_connected(LoopbackHarness *h, void *u) {
    (void)u;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static bool pred_spectating(LoopbackHarness *h, void *u) {
    (void)u;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_SPECTATING;
}

static bool pred_announced(LoopbackHarness *h, void *u) {
    const AnnounceWant *w = (const AnnounceWant *)u;
    return clientSimLobbyBrainHasDocs(h->cs, 0) &&
           strcmp(clientSimGetLobbyBrainAnnounce(h->cs, 0), w->announce) == 0;
}

/* The docs are in, or the client has given up on them. Asks every pump, the
 * way the dialog does every frame, so a new generation is asked for too. */
static bool pred_docs_settled(LoopbackHarness *h, void *u) {
    ClientBrainDocsState st;
    (void)u;
    clientSimLobbyBrainDocsWant(h->cs, 0, false);
    st = clientSimGetLobbyBrainDocsState(h->cs, 0);
    return st == CLIENT_BRAIN_DOCS_READY || st == CLIENT_BRAIN_DOCS_FAILED;
}

static bool setupDir(const char *leaf) {
    if (!utScratchPath(s_dir, sizeof(s_dir), leaf)) return false;
    SDL_CreateDirectory(s_dir);
    return true;
}

int run_loopback_brain_docs_fetch(void) {
    LoopbackHarness h;
    AnnounceWant    want;
    char           *docs  = NULL, *docs2 = NULL;
    ClientBrainDocsState st;
    int             at;

    docs  = (char *)malloc(DOCS_BYTES + 1);
    docs2 = (char *)malloc(DOCS_BYTES + 1);
    UT_ASSERT(docs != NULL && docs2 != NULL);
    makeDocs(docs, 1u);
    makeDocs(docs2, 2u);
    UT_ASSERT(strcmp(docs, docs2) != 0);
    UT_ASSERT(setupDir("docsbrain"));
    UT_ASSERT(writeBrain("First words.", docs));

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "DocsReader", /*lobbyMode*/ true,
                                       "loss=5,burst=2", /*seed*/ 0xD0C5u),
                  "harness start failed");
    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    installAndAnnounce(&h);
    want.announce = "First words.";
    at = loopbackHarnessPumpUntil(&h, ANNOUNCE_MAX, pred_announced, &want);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the announce never reached the client");
    }
    UT_ASSERT_MSG(clientSimGetLobbyBrainDocs(h.cs, 0)[0] == '\0',
                  "docs arrived without being asked for");

    at = loopbackHarnessPumpUntil(&h, DOCS_MAX, pred_docs_settled, NULL);
    st = clientSimGetLobbyBrainDocsState(h.cs, 0);
    fprintf(stderr, "  brain docs (loss=5,burst=2): settled@%d state=%d\n", at,
            (int)st);
    if (at < 0 || st != CLIENT_BRAIN_DOCS_READY) {
        loopbackHarnessStop(&h);
        UT_FAIL("the docs did not arrive within %d pumps (state %d)", DOCS_MAX,
                (int)st);
    }
    UT_ASSERT_MSG(strcmp(clientSimGetLobbyBrainDocs(h.cs, 0), docs) == 0,
                  "the docs arrived but are not the file on the server "
                  "(%u bytes, want %u)",
                  (unsigned)strlen(clientSimGetLobbyBrainDocs(h.cs, 0)),
                  (unsigned)DOCS_BYTES);

    /* The operator edits the brain between rounds. Write until the file's
       time moves, since the server's cache keys on it. */
    {
        char    path[600];
        int64_t before;
        int     tries;
        threadsWaitForMutex();
        before = brainListTextsMtimeForPath(h.sim->brainPaths[0]);
        threadsReleaseMutex();
        SDL_snprintf(path, sizeof(path), "%s/init.lua", s_dir);
        for (tries = 0; tries < 30; tries++) {
            UT_ASSERT(writeBrain("Second words.", docs2));
            if (brainListTextsMtimeForPath(path) != before) break;
            SDL_Delay(100);
        }
    }
    installAndAnnounce(&h);
    want.announce = "Second words.";
    at = loopbackHarnessPumpUntil(&h, ANNOUNCE_MAX, pred_announced, &want);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the second announce never reached the client");
    }
    UT_ASSERT_MSG(strcmp(clientSimGetLobbyBrainDocs(h.cs, 0), docs) != 0,
                  "the client is still showing the docs the server replaced");

    at = loopbackHarnessPumpUntil(&h, DOCS_MAX, pred_docs_settled, NULL);
    st = clientSimGetLobbyBrainDocsState(h.cs, 0);
    if (at < 0 || st != CLIENT_BRAIN_DOCS_READY) {
        loopbackHarnessStop(&h);
        UT_FAIL("the new docs did not arrive within %d pumps (state %d)",
                DOCS_MAX, (int)st);
    }
    UT_ASSERT_MSG(strcmp(clientSimGetLobbyBrainDocs(h.cs, 0), docs2) == 0,
                  "the client fetched something other than the new docs");

    loopbackHarnessStop(&h);
    free(docs);
    free(docs2);
    return 0;
}

int run_loopback_brain_docs_spectator(void) {
    LoopbackHarness h;
    AnnounceWant    want;
    char           *docs = NULL;
    ClientBrainDocsState st;
    int             at;

    docs = (char *)malloc(DOCS_BYTES + 1);
    UT_ASSERT(docs != NULL);
    makeDocs(docs, 3u);
    UT_ASSERT(setupDir("specdocsbrain"));
    UT_ASSERT(writeBrain("Spectators too.", docs));

    UT_ASSERT_MSG(loopbackHarnessStartSpectatorLobby(&h, "DocsViewer",
                                                     /*seed*/ 0x5BECu),
                  "spectator lobby harness start failed");
    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_spectating, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("spectator never reached SPECTATING within %d pumps",
                CONNECT_MAX);
    }

    installAndAnnounce(&h);
    want.announce = "Spectators too.";
    at = loopbackHarnessPumpUntil(&h, ANNOUNCE_MAX, pred_announced, &want);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the announce never reached the spectator");
    }

    at = loopbackHarnessPumpUntil(&h, DOCS_MAX, pred_docs_settled, NULL);
    st = clientSimGetLobbyBrainDocsState(h.cs, 0);
    if (at < 0 || st != CLIENT_BRAIN_DOCS_READY) {
        loopbackHarnessStop(&h);
        UT_FAIL("a live-lobby spectator's docs did not arrive within %d pumps "
                "(state %d)", DOCS_MAX, (int)st);
    }
    UT_ASSERT(strcmp(clientSimGetLobbyBrainDocs(h.cs, 0), docs) == 0);

    loopbackHarnessStop(&h);
    free(docs);
    return 0;
}

/* Fill `out` with DOCS_BYTES of letters, digits, spaces and newlines drawn at
 * random from `seed`. This compresses far less than makeDocs's words, so the
 * answer spans more bulk segments and a lost one leaves it part way in. */
static void makeNoisyDocs(char *out, uint32_t seed) {
    static const char chars[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 \n";
    size_t pos;
    for (pos = 0; pos < DOCS_BYTES; pos++) {
        seed = seed * 1103515245u + 12345u;
        out[pos] = chars[(seed >> 16) % (sizeof(chars) - 1)];
    }
    out[DOCS_BYTES] = '\0';
}

/* The docs answer has begun arriving: the receiver is filling brain 0 and the
 * brain is still ASKED. */
static bool docsMidBody(LoopbackHarness *h) {
    return h->cs->lobbyBrainTexts != NULL &&
           h->cs->lobbyBrainTexts->rxIdx == 1 &&
           h->cs->lobbyBrainTexts->docs[0].state == CLIENT_BRAIN_DOCS_S_ASKED &&
           clientSimGetLobbyBrainDocsState(h->cs, 0) == CLIENT_BRAIN_DOCS_WAITING;
}

int run_loopback_brain_docs_survives_bulk_rebase(void) {
    LoopbackHarness h;
    AnnounceWant    want;
    char           *docs = NULL;
    ClientBrainDocsState st;
    BYTE            slot;
    int             at, i;
    bool            caught = false;

    docs = (char *)malloc(DOCS_BYTES + 1);
    UT_ASSERT(docs != NULL);
    makeNoisyDocs(docs, 4u);
    UT_ASSERT(setupDir("rebasedocsbrain"));
    UT_ASSERT(writeBrain("Cut off.", docs));

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "DocsRebase", /*lobbyMode*/ true,
                                       "loss=20,burst=2", /*seed*/ 0xBA5Eu),
                  "harness start failed");
    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }
    /* The server's clients[] slot for this player is its player number. */
    slot = clientSimGetMyPlayerNum(h.cs);
    if (slot >= MAX_TANKS) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client holds no player slot");
    }

    installAndAnnounce(&h);
    want.announce = "Cut off.";
    at = loopbackHarnessPumpUntil(&h, ANNOUNCE_MAX, pred_announced, &want);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the announce never reached the client");
    }

    /* Ask once, then pump one step at a time until the answer is part way
     * in. The client's tick sends the request on its own from here. */
    clientSimLobbyBrainDocsWant(h.cs, 0, false);
    for (i = 0; i < DOCS_MAX; i++) {
        loopbackHarnessPump(&h);
        if (docsMidBody(&h)) {
            caught = true;
            break;
        }
        if (clientSimGetLobbyBrainDocsState(h.cs, 0) == CLIENT_BRAIN_DOCS_READY ||
            clientSimGetLobbyBrainDocsState(h.cs, 0) == CLIENT_BRAIN_DOCS_FAILED) {
            break;
        }
    }
    if (!caught) {
        st = clientSimGetLobbyBrainDocsState(h.cs, 0);
        loopbackHarnessStop(&h);
        UT_FAIL("the docs answer was never seen part way in (state %d after "
                "%d pumps)", (int)st, i);
    }

    /* The server re-bases this player's bulk channel, as a lobby map change
     * does: the rest of the docs answer is never sent. */
    threadsWaitForMutex();
    serverRebaseBulkAndRearmDownload((int)slot);
    threadsReleaseMutex();

    /* The re-armed map download can hold the bulk stream first, and a request
     * that reaches a busy sender is dropped and sent again 150 ticks later,
     * so allow for several re-asks. */
    at = loopbackHarnessPumpUntil(&h, DOCS_MAX, pred_docs_settled, NULL);
    st = clientSimGetLobbyBrainDocsState(h.cs, 0);
    fprintf(stderr, "  brain docs after bulk rebase: caught@%d settled@%d "
            "state=%d\n", i, at, (int)st);
    if (at < 0 || st != CLIENT_BRAIN_DOCS_READY) {
        loopbackHarnessStop(&h);
        UT_FAIL("the docs did not arrive within %d pumps of the bulk rebase "
                "(state %d)", DOCS_MAX, (int)st);
    }
    UT_ASSERT_MSG(strcmp(clientSimGetLobbyBrainDocs(h.cs, 0), docs) == 0,
                  "the docs arrived but are not the file on the server "
                  "(%u bytes, want %u)",
                  (unsigned)strlen(clientSimGetLobbyBrainDocs(h.cs, 0)),
                  (unsigned)DOCS_BYTES);

    loopbackHarnessStop(&h);
    free(docs);
    return 0;
}
