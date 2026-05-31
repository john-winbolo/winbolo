/*
 * Internal brain-message routing (messagedest=0 dispatch).
 *
 * Pins behavior of botManagerDeliverInternalMessage: the dispatcher
 * arm brain_data.c picks when a bot brain emits a message with
 * messagedest == 0 while running under the bot manager. The intent
 * is bot-coordination chatter (NewAutopilot's /info state slate)
 * that must reach every allied bot's brain inbox WITHOUT touching
 * the chat wire — so no human ever sees it in their newswire and
 * no UDP packet leaves the host.
 *
 * The helper is exercised directly rather than via brainDataExtractInfo
 * because that path needs a full BrainInfo + tank state + view buffer.
 * Calling the helper with hand-wired BotContext slots gives the same
 * coverage with a fraction of the scaffolding, and the dispatcher
 * branch in brain_data.c is one line that calls this helper, so a
 * direct test pins the only place behavior could regress.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "allience.h"
#include "bot_manager.h"        /* botManagerDeliverInternalMessage, BotContext */
#include "client_sim.h"
#include "client_sim_internal.h"
#include "game_sim.h"
#include "internal/messages.h"  /* messageInboxCount/Peek, BRAIN_INBOX_MSG_LEN */
#include "players.h"            /* PLAYER_FLAG_BOT (via player_flags.h) */
#include "server_sim.h"
#include "server_sim_internal.h"   /* full struct ServerSim — fixture pokes botMgr.bots[] */
#include "test_harness.h"

/* Bring up a sim with four slots wired exactly the way the bot manager
 * sees them at runtime:
 *   slot 0 — human (alliance bit set on slots 1 + 2)
 *   slot 1 — bot, allied with 0 + 2
 *   slot 2 — bot, allied with 0 + 1
 *   slot 3 — bot, NOT allied with anybody (enemy team)
 *
 * The helper iterates sim->botMgr.bots[i] looking for active entries
 * whose cs is non-NULL; that's the only handle it needs into the bot
 * world. We construct three ClientSims (one per bot slot) and patch
 * them straight into botMgr.bots[] — that bypasses botManagerAddBot
 * (which spins a real Lua VM, loads a brain script and registers a
 * subscriber) and keeps the test under a tenth of a second.
 *
 * Caller frees via fixture_teardown. */
typedef struct {
    ServerSim *sim;
    ClientSim *bot1cs;
    ClientSim *bot2cs;
    ClientSim *bot3cs;
} Fixture;

static ClientSim *make_bot_cs(BYTE slot) {
    ClientSim *cs = clientSimAlloc();
    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, slot);
    return cs;
}

static int fixture_setup(Fixture *f) {
    memset(f, 0, sizeof(*f));

    f->sim = ut_make_running_sim("Human");
    if (f->sim == NULL) {
        fprintf(stderr, "ut_make_running_sim returned NULL\n");
        return 1;
    }
    /* ut_make_running_sim adds slot 0 (Human). Add the three bot slots. */
    serverSimAddPlayer(f->sim, 1, "Bot1", false);
    serverSimAddPlayer(f->sim, 2, "Bot2", false);
    serverSimAddPlayer(f->sim, 3, "Enemy", false);

    GameSim *gs = serverSimGetGameSim(f->sim);
    players *plrs = &gs->plyrs;

    /* Mark slots 1, 2, 3 as bots in the server's player table. The
     * helper does not read this directly (it gates on
     * sim->botMgr.bots[i].active), but the flag is part of the
     * production setup so we keep the fixture honest. */
    (*plrs)->item[1].clientFlags |= PLAYER_FLAG_BOT;
    (*plrs)->item[2].clientFlags |= PLAYER_FLAG_BOT;
    (*plrs)->item[3].clientFlags |= PLAYER_FLAG_BOT;

    /* Alliance triangle 0—1—2 (mutual). Slot 3 left isolated so
     * playersGetAlliesBitMap from anyone in {0,1,2} omits bit 3. */
    allienceAdd(&(*plrs)->item[0].allie, 1);
    allienceAdd(&(*plrs)->item[0].allie, 2);
    allienceAdd(&(*plrs)->item[1].allie, 0);
    allienceAdd(&(*plrs)->item[1].allie, 2);
    allienceAdd(&(*plrs)->item[2].allie, 0);
    allienceAdd(&(*plrs)->item[2].allie, 1);

    /* Build bot ClientSims and patch them into botMgr.bots[]. The
     * helper reads .active and .cs only — no other BotContext fields
     * are touched on the read path. */
    f->bot1cs = make_bot_cs(1);
    f->bot2cs = make_bot_cs(2);
    f->bot3cs = make_bot_cs(3);
    if (!f->bot1cs || !f->bot2cs || !f->bot3cs) {
        fprintf(stderr, "make_bot_cs failed\n");
        return 1;
    }
    f->sim->botMgr.bots[1].active = true;
    f->sim->botMgr.bots[1].cs     = f->bot1cs;
    f->sim->botMgr.bots[2].active = true;
    f->sim->botMgr.bots[2].cs     = f->bot2cs;
    f->sim->botMgr.bots[3].active = true;
    f->sim->botMgr.bots[3].cs     = f->bot3cs;
    return 0;
}

static void fixture_teardown(Fixture *f) {
    /* Detach the cs pointers before destroying the sim so
     * botManagerDestroy (called from serverSimDestroy) doesn't try to
     * tear down brains we never created. The bots[] entries we wrote
     * to have no .brain / .controlSub state. */
    if (f->sim != NULL) {
        for (BYTE i = 1; i <= 3; i++) {
            f->sim->botMgr.bots[i].active = false;
            f->sim->botMgr.bots[i].cs     = NULL;
        }
    }
    if (f->bot1cs) clientSimDestroy(f->bot1cs);
    if (f->bot2cs) clientSimDestroy(f->bot2cs);
    if (f->bot3cs) clientSimDestroy(f->bot3cs);
    if (f->sim)    serverSimDestroy(f->sim);
}

/* Read the Pascal-string body the inbox stores at index `i`. Mirrors
 * the inbox_peek_body helper in test_bot_chat_routing.c; duplicated
 * here to keep this test file self-contained. */
static BYTE inbox_body(ClientSim *cs, int i, char *outBody, size_t outCap) {
    char pbuf[BRAIN_INBOX_MSG_LEN];
    BYTE from = messageInboxPeek(clientSimGetMessages(cs), i, pbuf);
    if (outBody == NULL || outCap == 0) return from;
    size_t plen = (size_t)(unsigned char)pbuf[0];
    if (plen >= outCap) plen = outCap - 1;
    memcpy(outBody, pbuf + 1, plen);
    outBody[plen] = '\0';
    return from;
}

/* ============================================================
 * Test 1 — internal message reaches every allied bot.
 *
 * Bot1 broadcasts. Bot2 (allied) gets one inbox entry tagged with
 * bot1's slot. Bot3 (enemy) and bot1 itself stay empty.
 * ============================================================ */
int run_internal_msg_reaches_allied_bot(void) {
    Fixture f;
    if (fixture_setup(&f) != 0) return 1;

    botManagerDeliverInternalMessage(f.sim, /*from=*/1, "/info state goal=defend");

    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot2cs)) == 1,
                  "bot2 inbox: got %d want 1",
                  messageInboxCount(clientSimGetMessages(f.bot2cs)));
    char body[BRAIN_INBOX_MSG_LEN];
    BYTE from = inbox_body(f.bot2cs, 0, body, sizeof(body));
    UT_ASSERT_MSG(strcmp(body, "/info state goal=defend") == 0,
                  "bot2 inbox body: got '%s'", body);
    UT_ASSERT_MSG((int)from == 1,
                  "bot2 inbox sender: got %u want 1", (unsigned)from);

    fixture_teardown(&f);
    return 0;
}

/* ============================================================
 * Test 2 — sender never self-delivers.
 *
 * Confirms the explicit `if (i == fromPlayer) continue` guard in the
 * helper; without it, a bot's own brain would receive every slate it
 * broadcasts and ally_state's self-slot would be perpetually fresh
 * from its own emission instead of from real teammates.
 * ============================================================ */
int run_internal_msg_skips_sender_self(void) {
    Fixture f;
    if (fixture_setup(&f) != 0) return 1;

    botManagerDeliverInternalMessage(f.sim, /*from=*/1, "echo me?");

    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot1cs)) == 0,
                  "bot1 (sender) inbox must be empty, got %d",
                  messageInboxCount(clientSimGetMessages(f.bot1cs)));

    fixture_teardown(&f);
    return 0;
}

/* ============================================================
 * Test 3 — enemy bot does not receive the broadcast.
 *
 * Bot3 is intentionally not in the 0—1—2 alliance triangle. A leak
 * here would mean the helper's alliance-bit check fired wrong, and
 * strategy info would flow to opponents — the exact bug the old
 * chat-wire path's `info.allies` mask existed to prevent.
 * ============================================================ */
int run_internal_msg_skips_non_allied_bot(void) {
    Fixture f;
    if (fixture_setup(&f) != 0) return 1;

    botManagerDeliverInternalMessage(f.sim, /*from=*/1, "/info state goal=attack");

    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot3cs)) == 0,
                  "enemy bot3 inbox must stay empty, got %d",
                  messageInboxCount(clientSimGetMessages(f.bot3cs)));

    fixture_teardown(&f);
    return 0;
}

/* ============================================================
 * Test 4 — inactive bot slots are skipped (no crash).
 *
 * Flip bot2's slot to inactive after fixture setup. Helper must skip
 * the slot rather than dereferencing it; the existing cs pointer is
 * still there but the active flag is the gate the production
 * lifecycle uses (botManagerRemoveBot clears active before tearing
 * down the cs).
 * ============================================================ */
int run_internal_msg_skips_inactive_bot_slot(void) {
    Fixture f;
    if (fixture_setup(&f) != 0) return 1;

    f.sim->botMgr.bots[2].active = false;

    botManagerDeliverInternalMessage(f.sim, /*from=*/1, "anyone home?");

    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot2cs)) == 0,
                  "inactive bot2 inbox must stay empty, got %d",
                  messageInboxCount(clientSimGetMessages(f.bot2cs)));

    /* Re-arm so fixture_teardown's cleanup loop matches the original
     * state. */
    f.sim->botMgr.bots[2].active = true;
    fixture_teardown(&f);
    return 0;
}

/* ============================================================
 * Test 5 — oversize body clamps cleanly at the inbox buffer.
 *
 * BRAIN_INBOX_MSG_LEN is PACKET_MAX_CHAT_MESSAGE+2 (length byte +
 * body + NUL guard). The chat wire path caps payloads before they
 * ever reach a brain; the internal path skips that cap entirely, so
 * the helper's local clamp is the only thing keeping a runaway
 * brain from scribbling past the ring slot.
 * ============================================================ */
int run_internal_msg_handles_oversized_body(void) {
    Fixture f;
    if (fixture_setup(&f) != 0) return 1;

    char big[BRAIN_INBOX_MSG_LEN + 64];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    botManagerDeliverInternalMessage(f.sim, /*from=*/1, big);

    char pbuf[BRAIN_INBOX_MSG_LEN];
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot2cs)) == 1,
                  "bot2 inbox count: got %d want 1",
                  messageInboxCount(clientSimGetMessages(f.bot2cs)));
    (void)messageInboxPeek(clientSimGetMessages(f.bot2cs), 0, pbuf);
    size_t plen = (size_t)(unsigned char)pbuf[0];
    UT_ASSERT_MSG(plen == (size_t)(BRAIN_INBOX_MSG_LEN - 2),
                  "clamped length: got %zu want %d",
                  plen, BRAIN_INBOX_MSG_LEN - 2);

    fixture_teardown(&f);
    return 0;
}

/* ============================================================
 * Test 6 — defensive: NULL / empty inputs are no-ops.
 *
 * The dispatcher in brain_data.c only calls the helper when the
 * brain emitted a non-empty sendmessage, but the helper still has
 * to refuse NULLs because it's exposed in the bot_manager API and
 * harness code (BrainTest, future tools) may exercise it directly.
 * ============================================================ */
int run_internal_msg_null_inputs_are_noop(void) {
    Fixture f;
    if (fixture_setup(&f) != 0) return 1;

    botManagerDeliverInternalMessage(NULL,  /*from=*/1, "nope");
    botManagerDeliverInternalMessage(f.sim, /*from=*/1, NULL);
    botManagerDeliverInternalMessage(f.sim, /*from=*/1, "");
    botManagerDeliverInternalMessage(f.sim, /*from=*/255, "out-of-range slot");

    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot1cs)) == 0,
                  "bot1 inbox must be empty");
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot2cs)) == 0,
                  "bot2 inbox must be empty");
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot3cs)) == 0,
                  "bot3 inbox must be empty");

    fixture_teardown(&f);
    return 0;
}
