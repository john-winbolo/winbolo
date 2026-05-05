/* Trailer/screenshot hack — see trailer_hack.h. */

#include <SDL3/SDL.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

extern "C" {
#include "../../../bolo/global.h"
#include "../../../bolo/platform_net.h"
#include "../../../bolo/client_sim.h"
#include "../../../bolo/bolo_map.h"
#include "../../../bolo/pillbox.h"
#include "../../../bolo/bases.h"
#include "../../../bolo/starts.h"
#include "../../../bolo/player_flags.h"
#include "../../../bolo/netpacks.h"
#include "trailer_hack.h"
}

/* Master switch — flip to false (or git revert this file) to disable. */
extern "C" {
bool g_trailerHack = true;
}

#define TRAILER_MAX_MAPS 4
#define TRAILER_COMPRESSED_BUFSZ MAP_DOWNLOAD_MAX_SIZE

struct TrailerMap {
    char  name[64];
    BYTE *compressed;
    int   compressedLen;
    int   pillCount;
    int   baseCount;
    int   startCount;
};

static TrailerMap s_maps[TRAILER_MAX_MAPS];
static int  s_mapCount    = 0;
static int  s_currentMap  = 0;
static bool s_mapsLoaded  = false;
static Uint64 s_countdownStartMs = 0;

static bool loadOneMap(const char *path, const char *displayName, TrailerMap *out) {
    map mp = NULL;
    pillboxes pb = NULL;
    bases bs = NULL;
    starts ss = NULL;
    mapCreate(&mp);
    pillsCreate(&pb);
    basesCreate(&bs);
    startsCreate(&ss);

    bool ok = mapRead((char *)path, &mp, &pb, &bs, &ss);
    if (ok) {
        BYTE *buf = (BYTE *)SDL_malloc(TRAILER_COMPRESSED_BUFSZ);
        int len = mapSaveCompressedMap(&mp, &pb, &bs, &ss, buf);
        if (len > 0) {
            out->compressed    = buf;
            out->compressedLen = len;
            out->pillCount     = pillsGetNumPills(&pb);
            out->baseCount     = basesGetNumBases(&bs);
            out->startCount    = startsGetNumStarts(&ss);
            SDL_strlcpy(out->name, displayName, sizeof(out->name));
        } else {
            SDL_free(buf);
            ok = false;
        }
    }

    mapDestroy(&mp);
    pillsDestroy(&pb);
    basesDestroy(&bs);
    startsDestroy(&ss);
    return ok;
}

static void ensureMapsLoaded(void) {
    if (s_mapsLoaded) return;
    s_mapsLoaded = true;

    static const struct { const char *path; const char *display; } candidates[] = {
        { "data/maps/Everard Island.map", "Everard Island" },
        { "data/maps/Hexagon.map",        "Hexagon" },
        { "data/maps/Pueblo.map",         "Pueblo" },
        { "data/maps/Bad Lands.map",      "Bad Lands" },
        { "data/maps/Big island.map",     "Big Island" },
        { "data/maps/French_Land.map",    "French Land" },
        { "data/maps/Zet_Land.map",       "Zet Land" },
        { "data/maps/Chew Toy 96.map",    "Chew Toy 96" },
    };
    int n = (int)(sizeof(candidates) / sizeof(candidates[0]));
    for (int i = 0; i < n && s_mapCount < TRAILER_MAX_MAPS; i++) {
        if (loadOneMap(candidates[i].path, candidates[i].display, &s_maps[s_mapCount])) {
            s_mapCount++;
        }
    }
}

extern "C" bool trailerHackGetCurrentMapData(const unsigned char **outData, int *outLen) {
    ensureMapsLoaded();
    if (s_mapCount == 0) return false;
    if (s_currentMap >= s_mapCount) s_currentMap = 0;
    *outData = s_maps[s_currentMap].compressed;
    *outLen  = s_maps[s_currentMap].compressedLen;
    return true;
}

static void applyMapToCs(ClientSim *cs) {
    if (s_mapCount == 0) return;
    SDL_strlcpy(cs->mapName, s_maps[s_currentMap].name, MAP_STR_SIZE);
    cs->lobbyPillCount  = (uint8_t)s_maps[s_currentMap].pillCount;
    cs->lobbyBaseCount  = (uint8_t)s_maps[s_currentMap].baseCount;
    cs->lobbyStartCount = (uint8_t)s_maps[s_currentMap].startCount;
}

extern "C" void trailerHackAdvanceMap(ClientSim *cs) {
    ensureMapsLoaded();
    if (s_mapCount == 0) return;
    s_currentMap = (s_currentMap + 1) % s_mapCount;
    applyMapToCs(cs);
    cs->mapSkipMyVote = false;
    memset(cs->mapSkipVotes, 0, sizeof(cs->mapSkipVotes));
    cs->mapSkipVotes[3] = true;
    cs->mapSkipVotes[7] = true;
    cs->mapSkipVotes[11] = true;
}

extern "C" void trailerHackBuildFakeLobby(ClientSim *cs) {
    ensureMapsLoaded();

    memset(cs->lobbySlots, 0, sizeof(cs->lobbySlots));

    /* Slot 0 — me, Elvis, Windows, not ready */
    cs->lobbySlots[0].connected   = true;
    SDL_strlcpy(cs->lobbySlots[0].playerName, "Elvis", PACKET_MAX_PLAYER_NAME);
    cs->lobbySlots[0].teamNumber  = 1;
    cs->lobbySlots[0].ready       = false;
    cs->lobbySlots[0].isBot       = false;
    cs->lobbySlots[0].pingMs      = 24;
    SDL_strlcpy(cs->lobbySlots[0].countryCode, "US", 3);
    cs->lobbySlots[0].clientFlags = PLAYER_FLAG_WBN_VERIFIED | PLAYER_FLAG_STEAM_BUILD;
    cs->lobbySlots[0].clientType  = CLIENT_TYPE_WINDOWS;

    static const char *names[15] = {
        "BoloVet",   "ZenMaster", "RedTank",    "Snipey",   "Hex0r",
        "MapMaker",  "TankGirl",  "PillCop",    "Pellucid", "Lakeside",
        "Cobalt",    "Vesper",    "DustDevil",  "Quartz",   "Aurora"
    };
    static const char *cc[15] = {
        "GB", "DE", "JP", "FR", "BR", "AU", "CA", "NL",
        "SE", "PL", "NZ", "IE", "ES", "IT", "FI"
    };

    for (int i = 1; i < 16; i++) {
        cs->lobbySlots[i].connected   = true;
        SDL_strlcpy(cs->lobbySlots[i].playerName, names[i - 1], PACKET_MAX_PLAYER_NAME);
        cs->lobbySlots[i].teamNumber  = (uint8_t)(((i - 1) % 4) + 1);
        cs->lobbySlots[i].ready       = true;
        cs->lobbySlots[i].isBot       = false;
        cs->lobbySlots[i].pingMs      = (uint16_t)(15 + ((i * 37) % 195));
        SDL_strlcpy(cs->lobbySlots[i].countryCode, cc[(i - 1) % 15], 3);
        cs->lobbySlots[i].clientFlags = (uint8_t)(((i & 1) ? PLAYER_FLAG_WBN_VERIFIED : 0)
                                      | ((i % 3 == 0) ? PLAYER_FLAG_STEAM_BUILD : 0)
                                      | ((i == 5) ? PLAYER_FLAG_SUPPORTER : 0));
        cs->lobbySlots[i].clientType  = (uint8_t)(((i - 1) % (CLIENT_TYPE_COUNT - 1)) + 1);
    }

    cs->lobbyGameType    = gameOpen;
    cs->lobbyHiddenMines = false;
    cs->lobbyAiType      = 0;
    cs->lobbyTimeLimit   = 50 * 60 * 15;
    cs->mapSkipAvailable = (s_mapCount > 1);
    cs->mapSkipMyVote    = false;
    memset(cs->mapSkipVotes, 0, sizeof(cs->mapSkipVotes));
    cs->mapSkipVotes[3]  = true;
    cs->mapSkipVotes[7]  = true;
    cs->mapSkipVotes[11] = true;

    cs->mapDownloadComplete = (s_mapCount > 0);

    cs->serverAddress.s_addr = inet_addr("198.51.100.42");
    cs->serverPort = 27500;

    SDL_strlcpy(cs->lobbyChatHistory,
        "[Server]: Welcome to winbolo.net\n"
        "ZenMaster: gg last round\n"
        "BoloVet: ready up everyone\n"
        "RedTank: o7\n"
        "MapMaker: try the new map next?\n",
        sizeof(cs->lobbyChatHistory));

    if (s_mapCount > 0) {
        s_currentMap = 0;
        applyMapToCs(cs);
    } else {
        SDL_strlcpy(cs->mapName, "Everard Island", MAP_STR_SIZE);
        cs->lobbyPillCount = 16;
        cs->lobbyBaseCount = 16;
        cs->lobbyStartCount = 8;
    }

    s_countdownStartMs   = 0;
    cs->countdownSeconds = 0;
    cs->netStat          = netLobby;
    cs->inLobby          = true;
}

extern "C" void trailerHackOnReady(ClientSim *cs, bool ready) {
    cs->lobbySlots[0].ready = ready;
    if (ready) {
        cs->countdownSeconds = 5;
        cs->netStat          = netLobbyCountdown;
        s_countdownStartMs   = SDL_GetTicks();
    } else {
        cs->countdownSeconds = 0;
        cs->netStat          = netLobby;
        s_countdownStartMs   = 0;
    }
}

extern "C" bool trailerHackTickCountdown(ClientSim *cs) {
    if (s_countdownStartMs == 0) return false;
    Uint64 now = SDL_GetTicks();
    Uint64 elapsedSec = (now - s_countdownStartMs) / 1000;
    int remaining = 5 - (int)elapsedSec;
    if (remaining < 0) remaining = 0;
    cs->countdownSeconds = remaining;
    if (remaining == 0) {
        s_countdownStartMs = 0;
        return true;
    }
    return false;
}
