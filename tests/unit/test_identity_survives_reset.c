/*
 * Identity-survives-reset regressions (test_identity_survives_reset.c).
 *
 * Connection identity — name, country, clientType, and clientFlags
 * (including PLAYER_FLAG_BOT and the WBN/Steam/supporter bits) — is
 * connection-scoped and must survive the per-round world reset. It was
 * twice silently dropped on the networked game-start path:
 * serverSimStartGame -> serverSimResetGameWorld used to destroy and
 * recreate the Players struct, zeroing clientFlags, and only restored
 * names afterward. Bots lost their brain badge and humans lost their
 * WBN/Steam icons over the network, while single-player (which skips the
 * reset via StartGameInPlace) looked correct — the classic asymmetric-
 * runtime bug.
 *
 * These pin the invariant at three levels: the playersResetRoundState
 * primitive in isolation, the game-start path, and the return-to-lobby
 * path (which deliberately drops the WBN-session bits but keeps the rest).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "allience.h"
#include "players.h"               /* player struct + PLAYER_FLAG_* + accessors */
#include "game_sim.h"
#include "everard_map.h"           /* E_MAP */
#include "server_sim.h"
#include "server_sim_internal.h"   /* serverSimGetGameSim -> GameSim */
#include "server_sim_lifecycle.h"  /* serverSimStartGame / ReturnToLobby / SetLobbyEnabled */
#include "test_harness.h"

/* Lobby-enabled ServerSim from the embedded Everard map with one human in
 * slot 0. Caller owns the sim. Returns NULL on failure. */
static ServerSim *make_lobby_sim_with_player(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Human", false);
    return sim;
}

/* ---- 1. The primitive: round state cleared, identity preserved. ---- */
int run_reset_round_state_preserves_identity(void) {
    players plrs = NULL;
    playersCreate(&plrs, TRUE);
    UT_ASSERT(plrs != NULL);

    /* Identity bits. */
    plrs->item[0].inUse = TRUE;
    strcpy(plrs->item[0].playerName, "Tester");
    plrs->item[0].location[0] = 'U';
    plrs->item[0].location[1] = 'S';
    plrs->item[0].location[2] = '\0';
    playersSetClientType(&plrs, 0, CLIENT_TYPE_LINUX);
    playersSetClientFlags(&plrs, 0, PLAYER_FLAG_BOT | PLAYER_FLAG_SUPPORTER);

    /* Round/world state. */
    plrs->item[0].mapX  = 50;
    plrs->item[0].mapY  = 60;
    plrs->item[0].frame = 7;
    plrs->item[0].onBoat = TRUE;
    plrs->item[0].speed = 12;
    allienceAdd(&plrs->item[0].allie, 3);

    playersResetRoundState(&plrs, 0);

    /* Identity survives. */
    UT_ASSERT_MSG(plrs->item[0].inUse == TRUE, "inUse must survive");
    UT_ASSERT_MSG(strcmp(plrs->item[0].playerName, "Tester") == 0,
                  "playerName must survive");
    UT_ASSERT_MSG(plrs->item[0].location[0] == 'U' &&
                  plrs->item[0].location[1] == 'S',
                  "country must survive");
    UT_ASSERT_MSG(playersGetClientType(&plrs, 0) == CLIENT_TYPE_LINUX,
                  "clientType must survive");
    UT_ASSERT_MSG(playersGetClientFlags(&plrs, 0) ==
                  (PLAYER_FLAG_BOT | PLAYER_FLAG_SUPPORTER),
                  "clientFlags must survive");

    /* Round state is cleared. */
    UT_ASSERT_MSG(plrs->item[0].mapX == 0 && plrs->item[0].mapY == 0,
                  "position must reset");
    UT_ASSERT_MSG(plrs->item[0].frame == 0, "frame must reset");
    UT_ASSERT_MSG(plrs->item[0].onBoat == FALSE, "onBoat must reset");
    UT_ASSERT_MSG(plrs->item[0].speed == 0, "speed must reset");
    UT_ASSERT_MSG(allienceExist(&plrs->item[0].allie, 3) == FALSE,
                  "alliance must reset");

    playersDestroy(&plrs);
    return 0;
}

/* ---- 2. serverSimStartGame preserves clientFlags (the bug). ---- */
int run_start_game_preserves_client_flags(void) {
    ServerSim *sim = make_lobby_sim_with_player();
    UT_ASSERT(sim != NULL);

    /* Stamp the bits a bot (PLAYER_FLAG_BOT) and a supporter would carry. */
    playersSetClientFlags(&serverSimGetGameSim(sim)->plyrs, 0,
                          PLAYER_FLAG_BOT | PLAYER_FLAG_SUPPORTER);

    serverSimStartGame(sim);

    uint8_t flags = playersGetClientFlags(&serverSimGetGameSim(sim)->plyrs, 0);
    UT_ASSERT_MSG((flags & PLAYER_FLAG_BOT) != 0,
                  "PLAYER_FLAG_BOT must survive serverSimStartGame (got 0x%02x)",
                  flags);
    UT_ASSERT_MSG((flags & PLAYER_FLAG_SUPPORTER) != 0,
                  "PLAYER_FLAG_SUPPORTER must survive serverSimStartGame (got 0x%02x)",
                  flags);

    serverSimDestroy(sim);
    return 0;
}

/* ---- 3. serverSimReturnToLobby keeps identity, drops WBN-session bits. ---- */
int run_return_to_lobby_drops_wbn_keeps_identity(void) {
    ServerSim *sim = make_lobby_sim_with_player();
    UT_ASSERT(sim != NULL);

    playersSetClientFlags(&serverSimGetGameSim(sim)->plyrs, 0,
                          PLAYER_FLAG_WBN_VERIFIED |
                          PLAYER_FLAG_SUPPORTER |
                          PLAYER_FLAG_BOT);

    serverSimReturnToLobby(sim);

    uint8_t flags = playersGetClientFlags(&serverSimGetGameSim(sim)->plyrs, 0);
    UT_ASSERT_MSG((flags & PLAYER_FLAG_WBN_VERIFIED) == 0,
                  "WBN_VERIFIED must drop on return-to-lobby (got 0x%02x)", flags);
    UT_ASSERT_MSG((flags & PLAYER_FLAG_SUPPORTER) != 0,
                  "SUPPORTER must survive return-to-lobby (got 0x%02x)", flags);
    UT_ASSERT_MSG((flags & PLAYER_FLAG_BOT) != 0,
                  "PLAYER_FLAG_BOT must survive return-to-lobby (got 0x%02x)", flags);

    serverSimDestroy(sim);
    return 0;
}
