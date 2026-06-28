/*
 * Spectator-chat routing.
 *
 * A spectator's lobby chat is published as CTRL_SPECTATOR_CHAT, which must fan
 * to BOTH audiences: players (every in-process / UDP bus subscriber) and other
 * spectators (the serverSpectatorDeliverControl allowlist, which 8a left
 * dropping it by default until this event existed). This test pins that split:
 *
 *   - a CTRL_SPECTATOR_CHAT publish reaches a player bus subscriber AND advances
 *     a live spectator's control channel (allowlist passes it);
 *   - a broadcast (0xFF) CTRL_CHAT still reaches the spectator;
 *   - team (0x81..) and unicast CTRL_CHAT do NOT reach the spectator.
 *
 * The live spectator is the harness's real lobby-spectator client; what it
 * received is read white-box via transportUdpServerGetSpectatorControlSeq.
 * After the spectator is subscribed, the test stops pumping — serverSimPublish-
 * Control delivers synchronously, so each seq delta is solely the publish under
 * test, with no ticks racing it.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "control_event.h"
#include "transport_udp.h"   /* transportUdpServerGetSpectator* */
#include "wire_limits.h"     /* PACKET_MAX_CHAT_MESSAGE */
#include "test_harness.h"
#include "loopback_harness.h"

#define CR_SUBSCRIBE_PUMPS 1200

/* Player-audience sink: records whether a CTRL_SPECTATOR_CHAT was delivered. */
typedef struct {
    bool    sawSpectatorChat;
    uint8_t lastSpecIdx;
    char    lastMsg[PACKET_MAX_CHAT_MESSAGE + 1];
} PlayerSink;

static void crPlayerDeliver(void *ctx, const struct ControlEvent *evt) {
    PlayerSink *s = (PlayerSink *)ctx;
    if (evt->type == CTRL_SPECTATOR_CHAT) {
        uint16_t n = evt->u.spectatorChat.bodyLen;
        if (n > PACKET_MAX_CHAT_MESSAGE) n = PACKET_MAX_CHAT_MESSAGE;
        s->sawSpectatorChat = true;
        s->lastSpecIdx = evt->u.spectatorChat.specIdx;
        memcpy(s->lastMsg, evt->u.spectatorChat.body, n);
        s->lastMsg[n] = '\0';
    }
}

static bool crSpectatorSubscribed(LoopbackHarness *h, void *user) {
    (void)h; (void)user;
    return transportUdpServerGetSpectatorCount() >= 1 &&
           transportUdpServerGetSpectatorControlSeq(0) > 0;
}

int run_spectator_chat_routing(void) {
    LoopbackHarness h;
    PlayerSink sink;
    SubscriberHandle player;
    ControlEvent evt;
    uint32_t before;

    UT_ASSERT_MSG(loopbackHarnessStartSpectatorLobby(&h, "Watcher", /*seed*/ 1u),
                  "lobby-spectator harness start failed");
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, CR_SUBSCRIBE_PUMPS,
                                           crSpectatorSubscribed, NULL) > 0,
                  "spectator never registered on the control bus");

    /* A player-audience bus subscriber. Registration triggers a sync replay
     * (which carries no CTRL_SPECTATOR_CHAT), so the flag starts clean. */
    memset(&sink, 0, sizeof(sink));
    player = serverSimRegisterSubscriber(h.sim, crPlayerDeliver, &sink);
    UT_ASSERT_MSG(player != SUBSCRIBER_HANDLE_INVALID,
                  "player subscriber register failed");
    sink.sawSpectatorChat = false;
    /* No pump from here on: each seq delta is solely the publish under test. */

    /* (1) CTRL_SPECTATOR_CHAT reaches the player (bus) AND the spectator
     * (allowlist). */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SPECTATOR_CHAT;
    evt.u.spectatorChat.specIdx = 0;
    evt.u.spectatorChat.bodyLen = 5;
    memcpy(evt.u.spectatorChat.body, "hello", 5);
    before = transportUdpServerGetSpectatorControlSeq(0);
    serverSimPublishControl(h.sim, &evt);
    UT_ASSERT_MSG(sink.sawSpectatorChat,
                  "CTRL_SPECTATOR_CHAT did not reach the player bus subscriber");
    UT_ASSERT_MSG(sink.lastSpecIdx == 0 &&
                  strcmp(sink.lastMsg, "hello") == 0,
                  "player received the wrong spectator-chat payload");
    UT_ASSERT_MSG(transportUdpServerGetSpectatorControlSeq(0) == before + 1,
                  "CTRL_SPECTATOR_CHAT did not reach the spectator (seq %u -> %u)",
                  before, transportUdpServerGetSpectatorControlSeq(0));

    /* (2) A broadcast (0xFF) CTRL_CHAT still reaches the spectator. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_CHAT;
    evt.u.chat.fromPlayer = 0;
    evt.u.chat.destPlayer = 0xFF;
    evt.u.chat.bodyLen    = 0;
    before = transportUdpServerGetSpectatorControlSeq(0);
    serverSimPublishControl(h.sim, &evt);
    UT_ASSERT_MSG(transportUdpServerGetSpectatorControlSeq(0) == before + 1,
                  "broadcast CTRL_CHAT (0xFF) did not reach the spectator");

    /* (3) Team-addressed CTRL_CHAT must NOT reach the spectator. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_CHAT;
    evt.u.chat.fromPlayer = 0;
    evt.u.chat.destPlayer = 0x81;     /* team 1 */
    evt.u.chat.bodyLen    = 0;
    before = transportUdpServerGetSpectatorControlSeq(0);
    serverSimPublishControl(h.sim, &evt);
    UT_ASSERT_MSG(transportUdpServerGetSpectatorControlSeq(0) == before,
                  "team CTRL_CHAT leaked to the spectator");

    /* (4) Unicast CTRL_CHAT must NOT reach the spectator. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_CHAT;
    evt.u.chat.fromPlayer = 0;
    evt.u.chat.destPlayer = 1;        /* unicast to slot 1 */
    evt.u.chat.bodyLen    = 0;
    before = transportUdpServerGetSpectatorControlSeq(0);
    serverSimPublishControl(h.sim, &evt);
    UT_ASSERT_MSG(transportUdpServerGetSpectatorControlSeq(0) == before,
                  "unicast CTRL_CHAT leaked to the spectator");

    serverSimUnregisterSubscriber(h.sim, player);
    loopbackHarnessStop(&h);
    return 0;
}
