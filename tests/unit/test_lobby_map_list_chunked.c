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
 *   MAP_SEARCH_RSP [header 8] [pathLen 1] [path] [queryLen 1] [query]
 *                  [final 1] [count 1] + entry layout as above.
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

/* Write the per-entry tail: [nameLen 1][name][isFolder 1][modTime 8 BE]. */
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
        pos = write_entry(buf, pos, name, /*isFolder*/ 0,
                          /*modTime*/ 1000000 + firstIdx + i);
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
