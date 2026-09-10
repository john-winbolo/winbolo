/*
 * Per-recipient mute over the real loopback transport.
 *
 * CMD_PLAYER_MUTE is a reliable command, so the client-side call must land
 * as a bit in the server's per-client voiceMuteMask — the single piece of
 * state both the voice fan-out and the CTRL_CHAT delivery filter read.
 * This drives the whole path (client send wrapper -> command codec ->
 * command tick -> dispatcher -> transport) and observes the mask.
 *
 *   1. Mute sets the target's bit; unmute clears it.
 *   2. A mute of the sender's own slot is rejected — the bit never appears.
 *   3. A mute is released when its TARGET leaves: the muter keeps its mask,
 *      but the departed player's bit is swept out of it, so the next
 *      occupant of that recycled slot is not silenced (voice and chat both,
 *      since they read the one bit) by a mute it never earned. A bit for an
 *      unrelated slot in the same mask must survive the sweep.
 *   4. The whole mask goes with the muter's own slot when the muter leaves.
 *
 * Case 3 needs two connected clients — the mask is per-muting-client state
 * and only a connected slot has one — so it joins a second client to the
 * running harness. Cases 1, 2 and 4 are single-client: the target slot need
 * not be occupied for the server to record a bit for it.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "transport_udp.h"         /* transportUdpServerGetVoiceMuteMask */
#include "test_harness.h"
#include "loopback_harness.h"

#define CONNECT_MAX     2000   /* join + map download */
#define COMMAND_MAX      600   /* command tick -> dispatcher -> mask */
#define REJECT_STAB      300   /* window a rejected command must not act in */

static const char kPlayerName[] = "MuteTester";
static const char kOtherName[]  = "MuteTarget";

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static bool pred_second_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return h->cs2 != NULL &&
           clientSimGetConnectState(h->cs2) == CLIENT_CONNECT_CONNECTED;
}

/* Pump until `slot`'s mute bit for `target` reaches `want`. Returns the
 * 1-based pump count it happened on, or -1 if it never did. */
static int pump_until_bit(LoopbackHarness *h, int slot, int target, bool want,
                          int maxIters) {
    const PlayerBitMap bit = (PlayerBitMap)1u << target;
    int i;
    for (i = 1; i <= maxIters; i++) {
        loopbackHarnessPump(h);
        if (((transportUdpServerGetVoiceMuteMask((BYTE)slot) & bit) != 0)
            == want) {
            return i;
        }
    }
    return -1;
}

int run_player_mute_mask_roundtrip(void) {
    LoopbackHarness h;
    int connectedAt;
    int mySlot;
    int target;
    int otherSlot;
    int keeper;
    int at;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, kPlayerName, /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 0x4D07Eu),
                  "harness start (player mute) failed");

    connectedAt = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    mySlot = (int)clientSimGetMyPlayerNum(h.cs);
    target = (mySlot == 0) ? 1 : 0;

    if (transportUdpServerGetVoiceMuteMask((BYTE)mySlot) != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("slot %d joined with a non-zero mute mask (0x%08x)",
                mySlot,
                (unsigned)transportUdpServerGetVoiceMuteMask((BYTE)mySlot));
    }

    /* 1a. Mute sets the bit. */
    clientSimNetSendPlayerMute(h.cs, (BYTE)target, true);
    at = pump_until_bit(&h, mySlot, target, /*want*/ true, COMMAND_MAX);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("mute of player %d never reached slot %d's mask within %d pumps "
                "(mask=0x%08x)", target, mySlot, COMMAND_MAX,
                (unsigned)transportUdpServerGetVoiceMuteMask((BYTE)mySlot));
    }
    fprintf(stderr, "  player mute: slot %d muted %d after %d pump(s)\n",
            mySlot, target, at);

    /* 1b. Unmute clears it again. */
    clientSimNetSendPlayerMute(h.cs, (BYTE)target, false);
    at = pump_until_bit(&h, mySlot, target, /*want*/ false, COMMAND_MAX);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("unmute of player %d never cleared slot %d's bit within %d pumps "
                "(mask=0x%08x)", target, mySlot, COMMAND_MAX,
                (unsigned)transportUdpServerGetVoiceMuteMask((BYTE)mySlot));
    }

    /* 2. Muting yourself is rejected — the sender's own bit must never set.
     * Checked every pump, so a bit that sets and is later cleared still
     * fails rather than slipping through the end-of-loop read. */
    clientSimNetSendPlayerMute(h.cs, (BYTE)mySlot, true);
    {
        const PlayerBitMap selfBit = (PlayerBitMap)1u << mySlot;
        int i;
        for (i = 1; i <= REJECT_STAB; i++) {
            loopbackHarnessPump(&h);
            if ((transportUdpServerGetVoiceMuteMask((BYTE)mySlot) & selfBit)
                != 0) {
                loopbackHarnessStop(&h);
                UT_FAIL("self-mute was applied to slot %d on pump %d "
                        "(mask=0x%08x)", mySlot, i,
                        (unsigned)transportUdpServerGetVoiceMuteMask(
                            (BYTE)mySlot));
            }
        }
    }

    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("client dropped while exchanging mute commands");
    }

    /* 3. A mute is released when its target leaves. Join a second client,
     * mute it, kick it, and watch its bit go from the FIRST client's mask —
     * the direction a recycled slot would otherwise hand a stranger someone
     * else's mute, silently and on both voice and chat. */
    if (!loopbackHarnessAddClient(&h, kOtherName)) {
        loopbackHarnessStop(&h);
        UT_FAIL("second client (the mute target) failed to connect");
    }
    at = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_second_connected, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("second client never reached CONNECTED within %d pumps",
                CONNECT_MAX);
    }
    otherSlot = (int)clientSimGetMyPlayerNum(h.cs2);
    if (otherSlot >= MAX_TANKS || otherSlot == mySlot) {
        loopbackHarnessStop(&h);
        UT_FAIL("second client landed on slot %d (first client is on %d)",
                otherSlot, mySlot);
    }

    /* A bit for a slot nobody is leaving, held in the same mask: the sweep
     * has to take out the departing player's bit and only that one. */
    keeper = -1;
    {
        int i;
        for (i = 0; i < MAX_TANKS; i++) {
            if (i != mySlot && i != otherSlot) { keeper = i; break; }
        }
    }
    if (keeper < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("no third slot to hold a bystander mute (MAX_TANKS=%d)",
                (int)MAX_TANKS);
    }
    clientSimNetSendPlayerMute(h.cs, (BYTE)keeper, true);
    at = pump_until_bit(&h, mySlot, keeper, /*want*/ true, COMMAND_MAX);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("mute of bystander slot %d never reached slot %d's mask "
                "within %d pumps", keeper, mySlot, COMMAND_MAX);
    }

    clientSimNetSendPlayerMute(h.cs, (BYTE)otherSlot, true);
    at = pump_until_bit(&h, mySlot, otherSlot, /*want*/ true, COMMAND_MAX);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("mute of player %d never reached slot %d's mask within %d "
                "pumps (mask=0x%08x)", otherSlot, mySlot, COMMAND_MAX,
                (unsigned)transportUdpServerGetVoiceMuteMask((BYTE)mySlot));
    }

    transportUdpServerKickPlayer(h.sim, kOtherName);
    {
        const PlayerBitMap otherBit  = (PlayerBitMap)1u << otherSlot;
        const PlayerBitMap keeperBit = (PlayerBitMap)1u << keeper;
        const PlayerBitMap mask = transportUdpServerGetVoiceMuteMask((BYTE)mySlot);
        if ((mask & otherBit) != 0) {
            loopbackHarnessStop(&h);
            UT_FAIL("slot %d kept its mute of departed player %d "
                    "(mask=0x%08x) — the next occupant of slot %d inherits it",
                    mySlot, otherSlot, (unsigned)mask, otherSlot);
        }
        if ((mask & keeperBit) == 0) {
            loopbackHarnessStop(&h);
            UT_FAIL("slot %d lost its unrelated mute of %d when %d left "
                    "(mask=0x%08x)", mySlot, keeper, otherSlot,
                    (unsigned)mask);
        }
    }
    fprintf(stderr, "  player mute: slot %d's mute of departed %d cleared, "
            "mute of %d kept\n", mySlot, otherSlot, keeper);

    /* 4. The whole mask goes with the muter's own slot. Re-mute so there is
     * something to lose, then disconnect this client through the server's own
     * kick path (which runs serverDisconnectClient) and read the mask back.
     *
     * Note what this can and cannot see: the getter returns 0 for any slot
     * that is not connected, so it cannot distinguish a cleared mask from a
     * stale one behind a closed slot. It holds the API contract — no caller
     * can observe a departed slot's mutes — and case 3 above is what actually
     * proves a recycled slot comes up clean. */
    clientSimNetSendPlayerMute(h.cs, (BYTE)target, true);
    at = pump_until_bit(&h, mySlot, target, /*want*/ true, COMMAND_MAX);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("re-mute of player %d never reached slot %d's mask within "
                "%d pumps", target, mySlot, COMMAND_MAX);
    }

    transportUdpServerKickPlayer(h.sim, kPlayerName);
    if (transportUdpServerGetVoiceMuteMask((BYTE)mySlot) != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("slot %d kept a mute mask (0x%08x) after disconnect", mySlot,
                (unsigned)transportUdpServerGetVoiceMuteMask((BYTE)mySlot));
    }

    loopbackHarnessStop(&h);
    return 0;
}
