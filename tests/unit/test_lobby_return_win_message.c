/*
 * The win message a round leaves for the returning lobby
 * (test_lobby_return_win_message.c).
 *
 * serverSimLocalOnReturnToLobby is the game-over -> lobby step that both the
 * server's tick and the browser client's local tick run. Its last act sends
 * sim->pendingWinMessage to every subscriber as one CTRL_SERVER_TEXT and
 * clears it, after republishing the lobby settings and every slot.
 *
 * The send goes through publishServerEnglishBroadcast, which a winners list
 * can overflow: CTRL_SERVER_TEXT carries PACKET_MAX_CHAT_MESSAGE bytes, and
 * a longer message is cut on a UTF-8 character boundary and ends in "...".
 *
 * What these cases pin:
 *   - a pending message goes out once, to everyone, and is cleared
 *   - the lobby settings and all MAX_TANKS slots are republished with it
 *   - with no pending message nothing is said
 *   - an over-long message is cut whole characters short of the cap, with
 *     the ellipsis, never through the middle of a character
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* pendingWinMessage */
#include "server_sim_lifecycle.h"  /* serverSimLocalOnReturnToLobby,
                                    * serverSimSetLobbyEnabled */
#include "control_event.h"
#include "wire_limits.h"           /* PACKET_MAX_CHAT_MESSAGE */
#include "everard_map.h"           /* E_MAP */
#include "test_harness.h"

/* ----------------------------------------------------------------
 * Counting subscriber.
 * ---------------------------------------------------------------- */

typedef struct {
    int     serverTextCount;   /* CTRL_SERVER_TEXT seen */
    int     settingsCount;     /* CTRL_LOBBY_SETTINGS seen */
    int     slotCount;         /* CTRL_LOBBY_SLOT seen */
    char    lastText[PACKET_MAX_CHAT_MESSAGE + 1];
    uint8_t lastDestPlayer;
} LwCounter;

static void lw_count_events(void *ctx, const ControlEvent *evt) {
    LwCounter *c = (LwCounter *)ctx;
    if (evt->type == CTRL_SERVER_TEXT) {
        c->serverTextCount++;
        memcpy(c->lastText, evt->u.serverText.text, sizeof(c->lastText));
        c->lastDestPlayer = evt->u.serverText.destPlayer;
    } else if (evt->type == CTRL_LOBBY_SETTINGS) {
        c->settingsCount++;
    } else if (evt->type == CTRL_LOBBY_SLOT) {
        c->slotCount++;
    }
}

/* A lobby-enabled server sitting in its lobby, as it is on the tick that
 * returns from a round. */
static ServerSim *lw_make_lobby_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

/* Registration replays current server state to the new subscriber, so the
 * counters are cleared afterwards and measure only what happens next. */
static SubscriberHandle lw_subscribe(ServerSim *sim, LwCounter *c) {
    SubscriberHandle h;
    memset(c, 0, sizeof(*c));
    h = serverSimRegisterSubscriber(sim, lw_count_events, c);
    memset(c, 0, sizeof(*c));
    return h;
}

/* ================================================================
 * 1. A pending win message goes out once, to everyone, and is cleared,
 *    with the lobby republished alongside it.
 * ================================================================ */
int run_lobby_return_sends_win_message(void) {
    static const char msg[] = "*** Game Won! Sweeper captured every base. ***";
    ServerSim *sim = lw_make_lobby_sim();
    LwCounter c;
    SubscriberHandle h;

    UT_ASSERT(sim != NULL);
    h = lw_subscribe(sim, &c);
    SDL_strlcpy(sim->pendingWinMessage, msg, sizeof(sim->pendingWinMessage));

    serverSimLocalOnReturnToLobby(sim);

    UT_ASSERT_MSG(c.serverTextCount == 1,
                  "the win message must go out exactly once, saw %d "
                  "CTRL_SERVER_TEXT", c.serverTextCount);
    UT_ASSERT_MSG(strcmp(c.lastText, msg) == 0,
                  "a message under the cap must arrive whole, got \"%s\"",
                  c.lastText);
    UT_ASSERT_MSG(c.lastDestPlayer == 0xFF,
                  "the win message is for everyone (0xFF), got dest %u",
                  (unsigned)c.lastDestPlayer);
    UT_ASSERT_MSG(sim->pendingWinMessage[0] == '\0',
                  "the sent message must be cleared, still \"%s\"",
                  sim->pendingWinMessage);
    UT_ASSERT_MSG(c.settingsCount >= 1,
                  "the lobby settings must be republished, saw %d",
                  c.settingsCount);
    UT_ASSERT_MSG(c.slotCount >= MAX_TANKS,
                  "every one of the %d slots must be republished, saw %d",
                  MAX_TANKS, c.slotCount);

    /* Nothing pending: the lobby is republished again, but nothing said. */
    memset(&c, 0, sizeof(c));
    serverSimLocalOnReturnToLobby(sim);
    UT_ASSERT_MSG(c.serverTextCount == 0,
                  "with no pending message nothing may be said, saw %d",
                  c.serverTextCount);

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 2. A win message over the wire's cap is cut on a character boundary and
 *    ends in "...".
 *
 * Every character is the two-byte "é", so a lead byte sits at each even
 * offset. The first cut point (cap - 3) is odd, a continuation byte, so a
 * correct cut backs up one byte to the even offset before it.
 * ================================================================ */
int run_lobby_return_cuts_long_win_message(void) {
    ServerSim *sim = lw_make_lobby_sim();
    LwCounter c;
    SubscriberHandle h;
    char longMsg[2 * PACKET_MAX_CHAT_MESSAGE + 1];
    size_t i, len, keep;

    UT_ASSERT(sim != NULL);
    for (i = 0; i + 1 < sizeof(longMsg) - 1; i += 2) {
        longMsg[i]     = (char)0xC3;
        longMsg[i + 1] = (char)0xA9;
    }
    longMsg[i] = '\0';

    h = lw_subscribe(sim, &c);
    SDL_strlcpy(sim->pendingWinMessage, longMsg, sizeof(sim->pendingWinMessage));

    serverSimLocalOnReturnToLobby(sim);

    UT_ASSERT_MSG(c.serverTextCount == 1,
                  "the win message must go out exactly once, saw %d",
                  c.serverTextCount);
    len = strlen(c.lastText);
    UT_ASSERT_MSG(len <= PACKET_MAX_CHAT_MESSAGE,
                  "the sent text must fit the %d-byte cap, is %u bytes",
                  PACKET_MAX_CHAT_MESSAGE, (unsigned)len);
    UT_ASSERT_MSG(len >= 3 && strcmp(c.lastText + len - 3, "...") == 0,
                  "an over-long message must end in \"...\", got \"%s\"",
                  c.lastText);
    keep = len - 3;
    UT_ASSERT_MSG(keep % 2 == 0,
                  "the cut must fall between characters, kept %u bytes of "
                  "two-byte characters", (unsigned)keep);
    UT_ASSERT_MSG(keep == ((PACKET_MAX_CHAT_MESSAGE - 3) & ~(size_t)1),
                  "the cut must back up to the last whole character before "
                  "the cap, kept %u bytes", (unsigned)keep);
    UT_ASSERT_MSG(memcmp(c.lastText, longMsg, keep) == 0,
                  "the kept part must be the message's own start");

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}
