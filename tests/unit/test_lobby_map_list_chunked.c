/*
 * Coverage for the chunked PACKET_LOBBY_MAP_LIST_RSP and
 * PACKET_LOBBY_MAP_SEARCH_RSP receive path. The production dispatcher
 * (transport_udp_client.c) calls udpClientHandleLobbyMap{List,Search}Rsp
 * for each chunk; the tests feed crafted byte streams through those
 * helpers and read the client accumulator back through
 * client_sim_internal.h, mirroring what the lobby UI does at render
 * time.
 *
 * Wire formats (server-emitted, repeated per chunk for the same
 * request):
 *   MAP_LIST_RSP   [header 8] [pathLen 1] [path]            [final 1] [count 1]
 *                  per entry: [nameLen 1] [name] [isFolder 1] [modTime 8 BE]
 *                             [scripted 1]
 *   MAP_SEARCH_RSP [header 8] [pathLen 1] [path] [queryLen 1] [query]
 *                  [final 1] [count 1] + entry layout as above but with no
 *                  scripted byte — the search results do not carry the flag.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_internal.h"  /* LOBBY_MAP_LIST_MAX, accumulator fields */
#include "transport_udp.h"        /* udpClientHandleLobbyMap{List,Search}Rsp */
#include "transport_udp_internal.h" /* packHeader, PACKET_HEADER_SIZE */
#include "netpacks.h"             /* PACKET_LOBBY_MAP_LIST_RSP / SEARCH_RSP */
#include "test_harness.h"

/* Spacious upper bound for a single chunk in either RSP — header +
 * pathLen + 256-byte path + queryLen + 128-byte query + final +
 * count + 64 max-size entries. The dispatcher caps cnt at 64
 * (LOBBY_MAP_LIST_MAX), and per-entry size is bounded by
 * LOBBY_MAP_LIST_NAME_LEN + 9 bytes for the metadata. */
#define CHUNK_BUF_CAP 16384

static ClientSim *fresh_client_sim(void) {
    ClientSim *cs = clientSimAlloc();
    if (!cs) return NULL;
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 0);
    return cs;
}

/* Mimic transportUdpClientSendLobbyMapListRequest's REQ-side state
 * reset so the helper sees a coherent in-flight request. */
static void reset_list_request(ClientSim *cs, const char *path) {
    memset(cs->lobbyMapListReqPath, 0, sizeof(cs->lobbyMapListReqPath));
    if (path && *path) {
        SDL_strlcpy(cs->lobbyMapListReqPath, path,
                    sizeof(cs->lobbyMapListReqPath));
    }
    cs->lobbyMapListCount = 0;
    cs->lobbyMapListReady = false;
    cs->lobbyMapListInFlight = true;
}

static void reset_search_request(ClientSim *cs,
                                 const char *path, const char *query) {
    memset(cs->lobbyMapSearchReqPath, 0, sizeof(cs->lobbyMapSearchReqPath));
    memset(cs->lobbyMapSearchReqQuery, 0, sizeof(cs->lobbyMapSearchReqQuery));
    if (path && *path) {
        SDL_strlcpy(cs->lobbyMapSearchReqPath, path,
                    sizeof(cs->lobbyMapSearchReqPath));
    }
    if (query && *query) {
        SDL_strlcpy(cs->lobbyMapSearchReqQuery, query,
                    sizeof(cs->lobbyMapSearchReqQuery));
    }
    cs->lobbyMapSearchCount = 0;
    cs->lobbyMapSearchReady = false;
    cs->lobbyMapSearchInFlight = true;
}

/* Write the per-entry tail the search uses:
 * [nameLen 1][name][isFolder 1][modTime 8 BE]. */
static int write_entry(uint8_t *buf, int pos,
                       const char *name, uint8_t isFolder, int64_t modTime) {
    size_t nl = strlen(name);
    buf[pos++] = (uint8_t)nl;
    if (nl > 0) memcpy(buf + pos, name, nl);
    pos += (int)nl;
    buf[pos++] = isFolder;
    uint64_t mt = (uint64_t)modTime;
    int b;
    for (b = 7; b >= 0; b--) {
        buf[pos++] = (uint8_t)((mt >> (b * 8)) & 0xFFu);
    }
    return pos;
}

/* The list's entry: the same tail with the scripted byte after it. */
static int write_list_entry(uint8_t *buf, int pos,
                            const char *name, uint8_t isFolder,
                            int64_t modTime, uint8_t scripted) {
    pos = write_entry(buf, pos, name, isFolder, modTime);
    buf[pos++] = scripted;
    return pos;
}

/* Build a single MAP_LIST_RSP chunk. Names are generated as
 * "%s%02d.map" using `namePrefix` and the index offset, so a 3-chunk
 * sequence keeps unique names across chunks for spot-check assertions.
 * `finalFlag` and `count` are stamped into the wire bytes verbatim. */
static int build_list_chunk(uint8_t *buf, const char *path,
                            uint8_t finalFlag, uint8_t count,
                            const char *namePrefix, int firstIdx) {
    packHeader(buf, PACKET_LOBBY_MAP_LIST_RSP, 0);
    int pos = PACKET_HEADER_SIZE;
    size_t pl = strlen(path);
    buf[pos++] = (uint8_t)pl;
    if (pl > 0) memcpy(buf + pos, path, pl);
    pos += (int)pl;
    buf[pos++] = finalFlag;
    buf[pos++] = count;
    int i;
    for (i = 0; i < count; i++) {
        char name[32];
        snprintf(name, sizeof(name), "%s%02d.map", namePrefix, firstIdx + i);
        /* Every third map carries a script, so a case can tell the flag
           apart from a constant. */
        pos = write_list_entry(buf, pos, name, /*isFolder*/ 0,
                               /*modTime*/ 1000000 + firstIdx + i,
                               (uint8_t)(((firstIdx + i) % 3 == 0) ? 1 : 0));
    }
    return pos;
}

static int build_search_chunk(uint8_t *buf,
                              const char *path, const char *query,
                              uint8_t finalFlag, uint8_t count,
                              const char *namePrefix, int firstIdx) {
    packHeader(buf, PACKET_LOBBY_MAP_SEARCH_RSP, 0);
    int pos = PACKET_HEADER_SIZE;
    size_t pl = strlen(path);
    buf[pos++] = (uint8_t)pl;
    if (pl > 0) memcpy(buf + pos, path, pl);
    pos += (int)pl;
    size_t ql = strlen(query);
    buf[pos++] = (uint8_t)ql;
    if (ql > 0) memcpy(buf + pos, query, ql);
    pos += (int)ql;
    buf[pos++] = finalFlag;
    buf[pos++] = count;
    int i;
    for (i = 0; i < count; i++) {
        char name[32];
        snprintf(name, sizeof(name), "%s%02d.map", namePrefix, firstIdx + i);
        pos = write_entry(buf, pos, name, /*isFolder*/ 0,
                          /*modTime*/ 2000000 + firstIdx + i);
    }
    return pos;
}

/* ================================================================
 * MAP_LIST_RSP
 * ================================================================ */
int run_lobby_map_list_chunked(void) {
    uint8_t buf[CHUNK_BUF_CAP];
    int len;

    /* ── Three-chunk reassembly ─────────────────────────────────
     * 8 + 8 + 8 = 24 entries delivered with finalFlags = 0, 0, 1. */
    {
        ClientSim *cs = fresh_client_sim();
        UT_ASSERT(cs != NULL);
        reset_list_request(cs, "Maps/Public");

        len = build_list_chunk(buf, "Maps/Public", 0, 8, "Map", 0);
        udpClientHandleLobbyMapListRsp(cs, buf, len);
        UT_ASSERT_MSG(cs->lobbyMapListCount == 8,
                      "after chunk 1 count=%d want 8", cs->lobbyMapListCount);
        UT_ASSERT(cs->lobbyMapListReady == false);
        UT_ASSERT(cs->lobbyMapListInFlight == true);

        len = build_list_chunk(buf, "Maps/Public", 0, 8, "Map", 8);
        udpClientHandleLobbyMapListRsp(cs, buf, len);
        UT_ASSERT_MSG(cs->lobbyMapListCount == 16,
                      "after chunk 2 count=%d want 16", cs->lobbyMapListCount);
        UT_ASSERT(cs->lobbyMapListReady == false);
        UT_ASSERT(cs->lobbyMapListInFlight == true);

        len = build_list_chunk(buf, "Maps/Public", 1, 8, "Map", 16);
        udpClientHandleLobbyMapListRsp(cs, buf, len);
        UT_ASSERT_MSG(cs->lobbyMapListCount == 24,
                      "after final chunk count=%d want 24",
                      cs->lobbyMapListCount);
        UT_ASSERT(cs->lobbyMapListReady == true);
        UT_ASSERT(cs->lobbyMapListInFlight == false);
        UT_ASSERT(strcmp(cs->lobbyMapListPath, "Maps/Public") == 0);

        /* Spot-check names landed in order across chunk boundaries. */
        UT_ASSERT(strcmp(cs->lobbyMapListNames[0],  "Map00.map") == 0);
        UT_ASSERT(strcmp(cs->lobbyMapListNames[7],  "Map07.map") == 0);
        UT_ASSERT(strcmp(cs->lobbyMapListNames[8],  "Map08.map") == 0);
        UT_ASSERT(strcmp(cs->lobbyMapListNames[15], "Map15.map") == 0);
        UT_ASSERT(strcmp(cs->lobbyMapListNames[16], "Map16.map") == 0);
        UT_ASSERT(strcmp(cs->lobbyMapListNames[23], "Map23.map") == 0);
        UT_ASSERT(cs->lobbyMapListModTime[0]  == 1000000);
        UT_ASSERT(cs->lobbyMapListModTime[23] == 1000023);
        clientSimDestroy(cs);
    }

    /* ── Empty result: single chunk, count=0, final=1 ─────────── */
    {
        ClientSim *cs = fresh_client_sim();
        UT_ASSERT(cs != NULL);
        reset_list_request(cs, "Empty");
        len = build_list_chunk(buf, "Empty", 1, 0, "X", 0);
        udpClientHandleLobbyMapListRsp(cs, buf, len);
        UT_ASSERT(cs->lobbyMapListCount == 0);
        UT_ASSERT(cs->lobbyMapListReady == true);
        UT_ASSERT(cs->lobbyMapListInFlight == false);
        clientSimDestroy(cs);
    }

    /* ── Stale-path drop ────────────────────────────────────────
     * REQ for "A" is in flight; a chunk addressed to "B" must not
     * mutate the accumulator at all. */
    {
        ClientSim *cs = fresh_client_sim();
        UT_ASSERT(cs != NULL);
        reset_list_request(cs, "A");
        len = build_list_chunk(buf, "B", 1, 4, "Stale", 0);
        udpClientHandleLobbyMapListRsp(cs, buf, len);
        UT_ASSERT_MSG(cs->lobbyMapListCount == 0,
                      "stale path stamped count=%d want 0",
                      cs->lobbyMapListCount);
        UT_ASSERT(cs->lobbyMapListReady == false);
        UT_ASSERT(cs->lobbyMapListInFlight == true);
        UT_ASSERT_MSG(cs->lobbyMapListPath[0] == '\0',
                      "stale path stamped lobbyMapListPath='%s'",
                      cs->lobbyMapListPath);
        clientSimDestroy(cs);
    }

    /* ── Single chunk with final=1 ──────────────────────────────
     * Small-directory case: 24 entries fit in one packet, final flag
     * flips Ready/InFlight on the same chunk that delivers data. */
    {
        ClientSim *cs = fresh_client_sim();
        UT_ASSERT(cs != NULL);
        reset_list_request(cs, "Small");
        len = build_list_chunk(buf, "Small", 1, 24, "S", 0);
        udpClientHandleLobbyMapListRsp(cs, buf, len);
        UT_ASSERT(cs->lobbyMapListCount == 24);
        UT_ASSERT(cs->lobbyMapListReady == true);
        UT_ASSERT(cs->lobbyMapListInFlight == false);
        UT_ASSERT(strcmp(cs->lobbyMapListNames[0],  "S00.map") == 0);
        UT_ASSERT(strcmp(cs->lobbyMapListNames[23], "S23.map") == 0);
        clientSimDestroy(cs);
    }

    return 0;
}

/* ================================================================
 * MAP_SEARCH_RSP — 3-chunk reassembly and (path, query) stale drop.
 * ================================================================ */
int run_lobby_map_search_chunked(void) {
    uint8_t buf[CHUNK_BUF_CAP];
    int len;

    /* ── Three-chunk reassembly: 4 + 4 + 1 = 9 entries ─────────── */
    {
        ClientSim *cs = fresh_client_sim();
        UT_ASSERT(cs != NULL);
        reset_search_request(cs, "Maps", "tank");

        len = build_search_chunk(buf, "Maps", "tank", 0, 4, "Hit", 0);
        udpClientHandleLobbyMapSearchRsp(cs, buf, len);
        UT_ASSERT_MSG(cs->lobbyMapSearchCount == 4,
                      "after chunk 1 count=%d want 4",
                      cs->lobbyMapSearchCount);
        UT_ASSERT(cs->lobbyMapSearchReady == false);
        UT_ASSERT(cs->lobbyMapSearchInFlight == true);

        len = build_search_chunk(buf, "Maps", "tank", 0, 4, "Hit", 4);
        udpClientHandleLobbyMapSearchRsp(cs, buf, len);
        UT_ASSERT_MSG(cs->lobbyMapSearchCount == 8,
                      "after chunk 2 count=%d want 8",
                      cs->lobbyMapSearchCount);
        UT_ASSERT(cs->lobbyMapSearchReady == false);
        UT_ASSERT(cs->lobbyMapSearchInFlight == true);

        len = build_search_chunk(buf, "Maps", "tank", 1, 1, "Hit", 8);
        udpClientHandleLobbyMapSearchRsp(cs, buf, len);
        UT_ASSERT_MSG(cs->lobbyMapSearchCount == 9,
                      "after final chunk count=%d want 9",
                      cs->lobbyMapSearchCount);
        UT_ASSERT(cs->lobbyMapSearchReady == true);
        UT_ASSERT(cs->lobbyMapSearchInFlight == false);
        UT_ASSERT(strcmp(cs->lobbyMapSearchPath, "Maps") == 0);
        UT_ASSERT(strcmp(cs->lobbyMapSearchQuery, "tank") == 0);
        UT_ASSERT(strcmp(cs->lobbyMapSearchNames[0], "Hit00.map") == 0);
        UT_ASSERT(strcmp(cs->lobbyMapSearchNames[4], "Hit04.map") == 0);
        UT_ASSERT(strcmp(cs->lobbyMapSearchNames[8], "Hit08.map") == 0);
        clientSimDestroy(cs);
    }

    /* ── Stale (path, query) drop: REQ for (Maps,tank); chunk for
     * (Maps,plane) must be ignored (query mismatch). ───────────── */
    {
        ClientSim *cs = fresh_client_sim();
        UT_ASSERT(cs != NULL);
        reset_search_request(cs, "Maps", "tank");
        len = build_search_chunk(buf, "Maps", "plane", 1, 3, "Stale", 0);
        udpClientHandleLobbyMapSearchRsp(cs, buf, len);
        UT_ASSERT_MSG(cs->lobbyMapSearchCount == 0,
                      "stale query stamped count=%d want 0",
                      cs->lobbyMapSearchCount);
        UT_ASSERT(cs->lobbyMapSearchReady == false);
        UT_ASSERT(cs->lobbyMapSearchInFlight == true);
        UT_ASSERT(cs->lobbyMapSearchPath[0] == '\0');
        UT_ASSERT(cs->lobbyMapSearchQuery[0] == '\0');
        clientSimDestroy(cs);
    }

    return 0;
}

/* ================================================================
 * The scripted byte on a MAP_LIST_RSP entry.
 *
 * The encoder lives inside the UDP dispatcher's request handler and
 * needs a socket, so there is nothing to encode-and-decode against.
 * The golden case below writes the chunk out byte by byte at literal
 * offsets instead, which is what pins the layout: a field that moves
 * moves here or the case fails.
 * ================================================================ */

/* Where each byte of a one-entry chunk sits, counted from the start of
 * the buffer. The path is empty, so the header is followed straight by
 * the zero pathLen. */
#define GL_POS_PATHLEN   (PACKET_HEADER_SIZE + 0)
#define GL_POS_FINAL     (PACKET_HEADER_SIZE + 1)
#define GL_POS_COUNT     (PACKET_HEADER_SIZE + 2)
#define GL_POS_NAMELEN   (PACKET_HEADER_SIZE + 3)
#define GL_POS_NAME      (PACKET_HEADER_SIZE + 4)
#define GL_NAME_LEN      8                      /* "Wave.map" */
#define GL_POS_ISFOLDER  (GL_POS_NAME + GL_NAME_LEN)
#define GL_POS_MODTIME   (GL_POS_ISFOLDER + 1)
#define GL_POS_SCRIPTED  (GL_POS_MODTIME + 8)
#define GL_CHUNK_LEN     (GL_POS_SCRIPTED + 1)

int run_lobby_map_list_scripted_golden(void) {
    uint8_t    buf[CHUNK_BUF_CAP];
    ClientSim *cs;

    memset(buf, 0, sizeof(buf));
    packHeader(buf, PACKET_LOBBY_MAP_LIST_RSP, 0);
    buf[GL_POS_PATHLEN]  = 0;      /* the root, so no path bytes follow */
    buf[GL_POS_FINAL]    = 1;      /* the only chunk */
    buf[GL_POS_COUNT]    = 1;      /* one entry */
    buf[GL_POS_NAMELEN]  = GL_NAME_LEN;
    memcpy(buf + GL_POS_NAME, "Wave.map", GL_NAME_LEN);
    buf[GL_POS_ISFOLDER] = 0;
    /* modTime, big-endian, with every byte different so a swapped pair
       would show. */
    buf[GL_POS_MODTIME + 0] = 0x01;
    buf[GL_POS_MODTIME + 1] = 0x02;
    buf[GL_POS_MODTIME + 2] = 0x03;
    buf[GL_POS_MODTIME + 3] = 0x04;
    buf[GL_POS_MODTIME + 4] = 0x05;
    buf[GL_POS_MODTIME + 5] = 0x06;
    buf[GL_POS_MODTIME + 6] = 0x07;
    buf[GL_POS_MODTIME + 7] = 0x08;
    buf[GL_POS_SCRIPTED] = 1;

    cs = fresh_client_sim();
    UT_ASSERT(cs != NULL);
    reset_list_request(cs, "");
    udpClientHandleLobbyMapListRsp(cs, buf, GL_CHUNK_LEN);

    UT_ASSERT_MSG(cs->lobbyMapListCount == 1,
                  "the one-entry chunk read as %d entries",
                  cs->lobbyMapListCount);
    UT_ASSERT_MSG(strcmp(cs->lobbyMapListNames[0], "Wave.map") == 0,
                  "entry 0's name read as \"%s\"", cs->lobbyMapListNames[0]);
    UT_ASSERT(cs->lobbyMapListIsFolder[0] == 0);
    UT_ASSERT_MSG(cs->lobbyMapListModTime[0] == (int64_t)0x0102030405060708ll,
                  "entry 0's modTime read as %lld",
                  (long long)cs->lobbyMapListModTime[0]);
    UT_ASSERT_MSG(cs->lobbyMapListScripted[0],
                  "the scripted byte at offset %d did not reach the entry",
                  GL_POS_SCRIPTED);
    clientSimDestroy(cs);

    /* The same bytes with a zero there read as plain, so the assertion
       above is the byte and not a constant. */
    buf[GL_POS_SCRIPTED] = 0;
    cs = fresh_client_sim();
    UT_ASSERT(cs != NULL);
    reset_list_request(cs, "");
    udpClientHandleLobbyMapListRsp(cs, buf, GL_CHUNK_LEN);
    UT_ASSERT(cs->lobbyMapListCount == 1);
    UT_ASSERT_MSG(!cs->lobbyMapListScripted[0],
                  "a zero scripted byte read as scripted");
    clientSimDestroy(cs);

    /* A chunk one byte short of the entry's tail keeps the entry out
       rather than reading past it. */
    buf[GL_POS_SCRIPTED] = 1;
    cs = fresh_client_sim();
    UT_ASSERT(cs != NULL);
    reset_list_request(cs, "");
    udpClientHandleLobbyMapListRsp(cs, buf, GL_CHUNK_LEN - 1);
    UT_ASSERT_MSG(cs->lobbyMapListCount == 0,
                  "a truncated entry was accepted (%d entries)",
                  cs->lobbyMapListCount);
    clientSimDestroy(cs);
    return 0;
}

/* A list carrying a mix, and one carrying none. build_list_chunk marks
 * every third index scripted, so the pattern is what is checked rather
 * than one entry. */
int run_lobby_map_list_scripted_mixed(void) {
    uint8_t    buf[CHUNK_BUF_CAP];
    ClientSim *cs;
    int        len;
    int        i;
    int        scriptedSeen = 0;

    cs = fresh_client_sim();
    UT_ASSERT(cs != NULL);
    reset_list_request(cs, "");
    len = build_list_chunk(buf, "", 1, 9, "Mix", 0);
    udpClientHandleLobbyMapListRsp(cs, buf, len);

    UT_ASSERT_MSG(cs->lobbyMapListCount == 9,
                  "the nine-entry chunk read as %d", cs->lobbyMapListCount);
    for (i = 0; i < 9; i++) {
        bool want = (i % 3 == 0);
        UT_ASSERT_MSG(cs->lobbyMapListScripted[i] == want,
                      "entry %d (0-based) read scripted=%d, wanted %d",
                      i, (int)cs->lobbyMapListScripted[i], (int)want);
        if (cs->lobbyMapListScripted[i]) scriptedSeen++;
    }
    UT_ASSERT_MSG(scriptedSeen == 3,
                  "counted %d scripted entries of nine, wanted 3",
                  scriptedSeen);
    clientSimDestroy(cs);

    /* A list with none of them: every entry plain, which is what a server
       running no scenario library sends. */
    cs = fresh_client_sim();
    UT_ASSERT(cs != NULL);
    reset_list_request(cs, "");
    {
        int pos;
        packHeader(buf, PACKET_LOBBY_MAP_LIST_RSP, 0);
        pos = PACKET_HEADER_SIZE;
        buf[pos++] = 0;    /* pathLen */
        buf[pos++] = 1;    /* final */
        buf[pos++] = 4;    /* count */
        for (i = 0; i < 4; i++) {
            char name[32];
            snprintf(name, sizeof(name), "Plain%02d.map", i);
            pos = write_list_entry(buf, pos, name, 0, 500 + i, 0);
        }
        len = pos;
    }
    udpClientHandleLobbyMapListRsp(cs, buf, len);
    UT_ASSERT(cs->lobbyMapListCount == 4);
    for (i = 0; i < 4; i++) {
        UT_ASSERT_MSG(!cs->lobbyMapListScripted[i],
                      "entry %d (0-based) of a plain list read as scripted",
                      i);
    }
    clientSimDestroy(cs);
    return 0;
}
