/*
 * Bot chat routing — send + receive.
 *
 * Confirms a bot ClientSim sends and receives chat through the same
 * dispatcher arm as every human ClientSim:
 *
 *   serverSimApplyCommand(CMD_CHAT)
 *     -> serverSimPublishControl(CTRL_CHAT)
 *        -> per-subscriber clientSimApplyControl(cs, evt)
 *           -> CTRL_CHAT arm: clientSimIncomingMessage()
 *              -> clientMessageAdd() pushes into MessageState's brain inbox
 *
 * Each test invokes serverSimApplyCommand directly with the desired
 * senderSlot — that is what botManagerTick Stage 3 does on the
 * producer thread after draining BotJobCtx::pendingCmds, and what the
 * UDP server does after decoding PACKET_COMMAND_TICK. The dispatcher
 * is the single funnel both transports converge on, so testing it
 * with a bot's slot as sender is the test that "bots send chat the
 * same as everyone else."
 *
 * Receive is verified by reading the recipient ClientSim's brain inbox
 * via messageInboxCount + messageInboxPeek. That is the same path the
 * bot brain's BrainInfo.messages reads each tick (brainDataMakeInfo),
 * so an entry in the inbox is exactly what the brain would see.
 *
 * The dispatcher's CMD_CHAT arm does not check whether senderSlot is a
 * bot or a human — sender attribution is purely the slot number. So
 * "bot" for this test means: a ClientSim flagged clientSimSetIsBot,
 * registered as a subscriber, with its player table seeded. Real bots
 * additionally pump through botManagerQueueingChatSendCallback to defer
 * across the worker→producer thread hop, but that mechanism only
 * affects WHEN serverSimApplyCommand is called, not what it does — so
 * the dispatcher-level test pins the behavior the bot path relies on.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "allience.h"
#include "client_command.h"
#include "client_sim.h"
#include "client_sim_control.h"
#include "client_sim_internal.h"   /* clientSimSetBoundServerSim — a hosted
                                   * bot is bound, which is what makes its
                                   * inbox read the server's alliances */
#include "control_event.h"
#include "game_sim.h"
#include "messages.h"
#include "players.h"
#include "server_sim.h"
#include "threads.h"
#include "test_harness.h"

/* Builds a ClientSim for `slot` (slot 0 = human, 1+ = bot), seeds its
 * local player table with three named slots so playersMakeMessageName
 * in clientSimIncomingMessage has names to format, and registers it
 * as a subscriber so CTRL_CHAT publishes reach it. Caller destroys
 * via clientSimDestroy. */
static ClientSim *make_subscriber_clientsim(ServerSim *sim, BYTE slot,
                                            bool isBot,
                                            SubscriberHandle *outHandle) {
    ClientSim *cs = clientSimAlloc();
    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, slot);
    if (isBot) clientSimSetIsBot(cs, true);

    /* Seed all three slots in this cs's local player table. The
     * CTRL_CHAT arm's clientSimIncomingMessage calls
     * playersMakeMessageName(cs, plrs, myPN, fromPN, topLine) which
     * reads ->item[fromPN].playerName — without these entries it
     * formats "" / crashes. NEUTRAL=255 means "not allied to me." */
    GameSim *cgs = clientSimGetGameSim(cs);
    playersSetPlayer(cs, &cgs->plyrs, NEUTRAL, 0,
                     (char *)"Human", "??",
                     0, 0, 0, 0, 0, FALSE, 0, NULL, TRUE);
    playersSetPlayer(cs, &cgs->plyrs, NEUTRAL, 1,
                     (char *)"Bot1", "??",
                     0, 0, 0, 0, 0, FALSE, 0, NULL, TRUE);
    playersSetPlayer(cs, &cgs->plyrs, NEUTRAL, 2,
                     (char *)"Bot2", "??",
                     0, 0, 0, 0, 0, FALSE, 0, NULL, TRUE);

    SubscriberHandle h = serverSimRegisterClientSubscriber(sim, cs);
    if (h == SUBSCRIBER_HANDLE_INVALID) {
        clientSimDestroy(cs);
        return NULL;
    }
    if (outHandle) *outHandle = h;
    return cs;
}

/* Make slots `a` and `b` allies in THIS ClientSim's own player table — the
 * table its inbox filter reads. allienceAdd is one-directional, so both ways
 * are added. */
static void chat_ally(ClientSim *cs, BYTE a, BYTE b) {
    players *plrs = &clientSimGetGameSim(cs)->plyrs;
    allienceAdd(&(*plrs)->item[a].allie, b);
    allienceAdd(&(*plrs)->item[b].allie, a);
}

/* Build a CMD_CHAT ClientCommand carrying `body` to `destPlayer`. */
static ClientCommand make_chat_cmd(BYTE destPlayer, const char *body) {
    ClientCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_CHAT;
    cmd.u.chat.destPlayer = destPlayer;
    size_t n = strlen(body);
    if (n > PACKET_MAX_CHAT_MESSAGE) n = PACKET_MAX_CHAT_MESSAGE;
    cmd.u.chat.bodyLen = (uint16_t)n;
    memcpy(cmd.u.chat.body, body, n);
    return cmd;
}

/* Read the body text the recipient stored at inbox index `i`. The inbox
 * stores Pascal strings (byte 0 = length, bytes 1+ = body without NUL).
 * Returns the sender slot, or 0xFF on no entry, and writes a
 * NUL-terminated copy of the body into outBody. */
static BYTE inbox_peek_body(ClientSim *cs, int i, char *outBody,
                            size_t outCap) {
    char pbuf[BRAIN_INBOX_MSG_LEN];
    BYTE from = messageInboxPeek(clientSimGetMessages(cs), i, pbuf);
    if (outBody == NULL || outCap == 0) return from;
    size_t plen = (size_t)(unsigned char)pbuf[0];
    if (plen >= outCap) plen = outCap - 1;
    memcpy(outBody, pbuf + 1, plen);
    outBody[plen] = '\0';
    return from;
}

/* Common scaffolding: bring up threads, build the running sim, add
 * slots 1 (Bot1) and 2 (Bot2) on top of the slot-0 human the
 * ut_make_running_sim helper installs. Caller owns sim and the cs
 * pointers. */
typedef struct {
    ServerSim *sim;
    ClientSim *human; SubscriberHandle hHuman;
    ClientSim *bot1;  SubscriberHandle hBot1;
    ClientSim *bot2;  SubscriberHandle hBot2;
} ChatFixture;

static int chat_fixture_setup(ChatFixture *f) {
    memset(f, 0, sizeof(*f));
    if (!threadsCreate(TRUE)) {
        fprintf(stderr, "threadsCreate failed\n");
        return 1;
    }
    f->sim = ut_make_running_sim("Human");
    if (f->sim == NULL) {
        fprintf(stderr, "ut_make_running_sim returned NULL\n");
        return 1;
    }
    /* Slot 0 already exists as "Human". Bring up two bot slots so
     * the dispatcher has somewhere to attribute senderSlot=1/2. */
    serverSimAddPlayer(f->sim, 1, "Bot1", false);
    serverSimAddPlayer(f->sim, 2, "Bot2", false);

    f->human = make_subscriber_clientsim(f->sim, 0, false, &f->hHuman);
    f->bot1  = make_subscriber_clientsim(f->sim, 1, true,  &f->hBot1);
    f->bot2  = make_subscriber_clientsim(f->sim, 2, true,  &f->hBot2);
    if (!f->human || !f->bot1 || !f->bot2) {
        fprintf(stderr, "make_subscriber_clientsim failed\n");
        return 1;
    }
    /* The two bots are on one team; the human is on the other.
     *
     * The alliance matters now: a hosted bot keeps a BROADCAST only from an
     * ally, because an enemy's "everyone fall back" is chatter and not an
     * order (client_sim.c clientSimChatReachesInbox). Unicast and team chat
     * are unaffected — anything aimed at the seat still lands.
     *
     * Each ClientSim's own player table is what its own filter reads, so the
     * pair is set in bot1's and bot2's copies. */
    chat_ally(f->bot1, 1, 2);
    chat_ally(f->bot2, 1, 2);
    return 0;
}

static void chat_fixture_teardown(ChatFixture *f) {
    if (f->human) {
        serverSimUnregisterSubscriber(f->sim, f->hHuman);
        clientSimDestroy(f->human);
    }
    if (f->bot1) {
        serverSimUnregisterSubscriber(f->sim, f->hBot1);
        clientSimDestroy(f->bot1);
    }
    if (f->bot2) {
        serverSimUnregisterSubscriber(f->sim, f->hBot2);
        clientSimDestroy(f->bot2);
    }
    if (f->sim) serverSimDestroy(f->sim);
    threadsDestroy();
}

/* ============================================================
 * Test 1 — bot SENDS chat to a human.
 *
 * Bot at slot 1 directs a CMD_CHAT at the human in slot 0. After the
 * dispatcher runs the CMD_CHAT arm, the human's MessageState inbox
 * contains one entry tagged with the bot's slot.
 * ============================================================ */
int run_bot_chat_send_to_human_lands_in_human_inbox(void) {
    ChatFixture f;
    if (chat_fixture_setup(&f) != 0) return 1;

    ClientCommand cmd = make_chat_cmd(/*dest=*/0, "hello human");

    threadsWaitForMutex();
    CmdResult r = serverSimApplyCommand(f.sim, /*senderSlot=bot1*/1, &cmd);
    threadsReleaseMutex();

    UT_ASSERT_MSG(r == CMD_OK, "dispatcher rejected CMD_CHAT, got %d", (int)r);

    /* Human (slot 0) is the recipient. */
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.human)) == 1,
                  "human inbox count: got %d want 1",
                  messageInboxCount(clientSimGetMessages(f.human)));
    char body[BRAIN_INBOX_MSG_LEN];
    BYTE from = inbox_peek_body(f.human, 0, body, sizeof(body));
    UT_ASSERT_MSG(strcmp(body, "hello human") == 0,
                  "human inbox body: got '%s' want 'hello human'", body);
    UT_ASSERT_MSG((int)from == 1,
                  "human inbox sender: got %u want 1 (bot1)", (unsigned)from);

    /* Sender (bot1) does NOT self-deliver — CTRL_CHAT arm filters
     * fromPlayer == myPN. The Lua side captures self-sends separately. */
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot1)) == 0,
                  "bot1 (sender) inbox must be empty, got %d",
                  messageInboxCount(clientSimGetMessages(f.bot1)));

    /* Bot2 was not the directed recipient. */
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot2)) == 0,
                  "bot2 (non-recipient) inbox must be empty, got %d",
                  messageInboxCount(clientSimGetMessages(f.bot2)));

    chat_fixture_teardown(&f);
    return 0;
}

/* ============================================================
 * Test 2 — bot SENDS chat to another bot.
 *
 * Bot 1 directs a CMD_CHAT at bot 2. Bot 2's inbox gets the entry;
 * sender (bot 1) and the unrelated human (slot 0) see nothing.
 * ============================================================ */
int run_bot_chat_send_to_other_bot_lands_in_recipient_inbox(void) {
    ChatFixture f;
    if (chat_fixture_setup(&f) != 0) return 1;

    ClientCommand cmd = make_chat_cmd(/*dest=*/2, "ack ally");

    threadsWaitForMutex();
    CmdResult r = serverSimApplyCommand(f.sim, /*senderSlot=bot1*/1, &cmd);
    threadsReleaseMutex();

    UT_ASSERT_MSG(r == CMD_OK, "dispatcher rejected CMD_CHAT, got %d", (int)r);

    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot2)) == 1,
                  "bot2 inbox count: got %d want 1",
                  messageInboxCount(clientSimGetMessages(f.bot2)));
    char body[BRAIN_INBOX_MSG_LEN];
    BYTE from = inbox_peek_body(f.bot2, 0, body, sizeof(body));
    UT_ASSERT_MSG(strcmp(body, "ack ally") == 0,
                  "bot2 inbox body: got '%s' want 'ack ally'", body);
    UT_ASSERT_MSG((int)from == 1,
                  "bot2 inbox sender: got %u want 1 (bot1)", (unsigned)from);

    /* Self and unrelated slot both empty. */
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot1)) == 0,
                  "bot1 (sender) inbox must be empty, got %d",
                  messageInboxCount(clientSimGetMessages(f.bot1)));
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.human)) == 0,
                  "human (non-recipient) inbox must be empty, got %d",
                  messageInboxCount(clientSimGetMessages(f.human)));

    chat_fixture_teardown(&f);
    return 0;
}

/* ============================================================
 * Test 3 — bot RECEIVES chat from a human.
 *
 * Human at slot 0 directs a CMD_CHAT at bot 1. Bot 1's inbox gets
 * the entry tagged from the human's slot.
 * ============================================================ */
int run_bot_chat_receive_from_human_lands_in_bot_inbox(void) {
    ChatFixture f;
    if (chat_fixture_setup(&f) != 0) return 1;

    ClientCommand cmd = make_chat_cmd(/*dest=*/1, "regroup at base 3");

    threadsWaitForMutex();
    CmdResult r = serverSimApplyCommand(f.sim, /*senderSlot=human*/0, &cmd);
    threadsReleaseMutex();

    UT_ASSERT_MSG(r == CMD_OK, "dispatcher rejected CMD_CHAT, got %d", (int)r);

    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot1)) == 1,
                  "bot1 inbox count: got %d want 1",
                  messageInboxCount(clientSimGetMessages(f.bot1)));
    char body[BRAIN_INBOX_MSG_LEN];
    BYTE from = inbox_peek_body(f.bot1, 0, body, sizeof(body));
    UT_ASSERT_MSG(strcmp(body, "regroup at base 3") == 0,
                  "bot1 inbox body: got '%s' want 'regroup at base 3'", body);
    UT_ASSERT_MSG((int)from == 0,
                  "bot1 inbox sender: got %u want 0 (human)", (unsigned)from);

    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.human)) == 0,
                  "human (sender) inbox must be empty, got %d",
                  messageInboxCount(clientSimGetMessages(f.human)));
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot2)) == 0,
                  "bot2 (non-recipient) inbox must be empty, got %d",
                  messageInboxCount(clientSimGetMessages(f.bot2)));

    chat_fixture_teardown(&f);
    return 0;
}

/* ============================================================
 * Test 4 — bot RECEIVES chat from another bot (broadcast variant).
 *
 * Bot 1 broadcasts (destPlayer = 0xFF). The CTRL_CHAT arm of every
 * non-sender ClientSim's apply path inserts the entry; the sender
 * (bot 1) is filtered out per the fromPlayer == myPN guard.
 * ============================================================ */
int run_bot_chat_receive_from_other_bot_via_broadcast(void) {
    ChatFixture f;
    if (chat_fixture_setup(&f) != 0) return 1;

    ClientCommand cmd = make_chat_cmd(/*dest=*/0xFF, "/info state");

    threadsWaitForMutex();
    CmdResult r = serverSimApplyCommand(f.sim, /*senderSlot=bot1*/1, &cmd);
    threadsReleaseMutex();

    UT_ASSERT_MSG(r == CMD_OK, "dispatcher rejected CMD_CHAT, got %d", (int)r);

    /* Bot2 — the receiver we are pinning. */
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot2)) == 1,
                  "bot2 inbox count: got %d want 1",
                  messageInboxCount(clientSimGetMessages(f.bot2)));
    char body[BRAIN_INBOX_MSG_LEN];
    BYTE from = inbox_peek_body(f.bot2, 0, body, sizeof(body));
    UT_ASSERT_MSG(strcmp(body, "/info state") == 0,
                  "bot2 inbox body: got '%s' want '/info state'", body);
    UT_ASSERT_MSG((int)from == 1,
                  "bot2 inbox sender: got %u want 1 (bot1)", (unsigned)from);

    /* The human also receives the broadcast — same flow, different slot.
       It is NOT allied to bot1 (the fixture allies only the two bots), and
       it still gets the line: the ally rule is asked of a hosted BOT alone.
       A person is entitled to read the room, and this inbox is also what
       the HUD's "there is a new message" flag reads. */
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.human)) == 1,
                  "human inbox count on broadcast: got %d want 1",
                  messageInboxCount(clientSimGetMessages(f.human)));

    /* Sender is filtered. */
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot1)) == 0,
                  "bot1 (sender) inbox on broadcast must be empty, got %d",
                  messageInboxCount(clientSimGetMessages(f.bot1)));

    chat_fixture_teardown(&f);
    return 0;
}

/* ============================================================
 * Test 5 — an ENEMY's broadcast does not reach a bot's inbox.
 *
 * The hole this closes: on a dedicated server every player's broadcast
 * landed in every hosted bot's inbox, whichever side the player was on.
 * GoalHunter survives that because it re-checks the ally mask in Lua. A
 * brain nobody here wrote does not, so "everyone fall back to base 3"
 * shouted by the other team was an order any third-party brain would obey.
 *
 * The human in slot 0 is on the other side from bot1 and bot2 (the fixture
 * allies only the two bots). Its broadcast must reach the two bots' HUDs —
 * which they do not have — and neither of their inboxes. bot1's own
 * broadcast, from an ally, must still reach bot2, because that is the
 * channel /info coordination runs on.
 * ============================================================ */
int run_bot_chat_enemy_broadcast_is_not_an_order(void) {
    ChatFixture f;
    if (chat_fixture_setup(&f) != 0) return 1;

    {
        ClientCommand cmd = make_chat_cmd(/*dest=*/0xFF, "everyone fall back");
        threadsWaitForMutex();
        CmdResult r = serverSimApplyCommand(f.sim, /*senderSlot=human*/0, &cmd);
        threadsReleaseMutex();
        UT_ASSERT_MSG(r == CMD_OK, "dispatcher rejected CMD_CHAT, got %d", (int)r);
    }

    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot1)) == 0,
                  "an enemy's broadcast landed in bot1's inbox: %d entries. A "
                  "bot's inbox is its orders, and that line was not one",
                  messageInboxCount(clientSimGetMessages(f.bot1)));
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot2)) == 0,
                  "an enemy's broadcast landed in bot2's inbox: %d entries",
                  messageInboxCount(clientSimGetMessages(f.bot2)));

    /* The same shot from an ALLY does land: this is the bots' own channel. */
    {
        ClientCommand cmd = make_chat_cmd(/*dest=*/0xFF, "/info state");
        threadsWaitForMutex();
        CmdResult r = serverSimApplyCommand(f.sim, /*senderSlot=bot1*/1, &cmd);
        threadsReleaseMutex();
        UT_ASSERT_MSG(r == CMD_OK, "dispatcher rejected CMD_CHAT, got %d", (int)r);
    }

    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot2)) == 1,
                  "an ally's broadcast did not reach bot2: %d entries, "
                  "expected 1 — the filter is meant to drop the enemy's line "
                  "and nothing else",
                  messageInboxCount(clientSimGetMessages(f.bot2)));
    {
        char body[BRAIN_INBOX_MSG_LEN];
        BYTE from = inbox_peek_body(f.bot2, 0, body, sizeof(body));
        UT_ASSERT_MSG(strcmp(body, "/info state") == 0,
                      "bot2 kept the wrong line: '%s'", body);
        UT_ASSERT_MSG((int)from == 1,
                      "bot2's line is from slot %u, expected 1 (the ally)",
                      (unsigned)from);
    }

    chat_fixture_teardown(&f);
    return 0;
}

/* ============================================================
 * Test 6 — a line AIMED at a bot lands whoever sent it.
 *
 * The rule is about broadcast only. An enemy who types at this seat has
 * addressed it, and a brain is entitled to read what was said to it — a
 * surrender offer, a taunt, an alliance request in words. Dropping unicast
 * would break every bot command a human on the other side can give.
 * ============================================================ */
int run_bot_chat_enemy_unicast_still_lands(void) {
    ChatFixture f;
    if (chat_fixture_setup(&f) != 0) return 1;

    ClientCommand cmd = make_chat_cmd(/*dest=*/1, "hold that hill");

    threadsWaitForMutex();
    CmdResult r = serverSimApplyCommand(f.sim, /*senderSlot=human*/0, &cmd);
    threadsReleaseMutex();
    UT_ASSERT_MSG(r == CMD_OK, "dispatcher rejected CMD_CHAT, got %d", (int)r);

    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot1)) == 1,
                  "a line addressed to bot1 by a non-ally did not land: %d "
                  "entries, expected 1",
                  messageInboxCount(clientSimGetMessages(f.bot1)));
    {
        char body[BRAIN_INBOX_MSG_LEN];
        BYTE from = inbox_peek_body(f.bot1, 0, body, sizeof(body));
        UT_ASSERT_MSG(strcmp(body, "hold that hill") == 0,
                      "bot1 inbox body: got '%s'", body);
        UT_ASSERT_MSG((int)from == 0,
                      "bot1's line is from slot %u, expected 0 (the human)",
                      (unsigned)from);
    }

    /* Nobody else was addressed. */
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot2)) == 0,
                  "bot2 was not the recipient but holds %d entries",
                  messageInboxCount(clientSimGetMessages(f.bot2)));

    chat_fixture_teardown(&f);
    return 0;
}

/* ============================================================
 * Test 7 — A MID-ROUND BOT READS THE SERVER'S ALLIANCES, NOT ITS OWN COPY.
 *
 * A bot built while the round is running — a scenario wave, or Add Bot during
 * play — gets a ClientSim whose local player table has an EMPTY row for
 * ITSELF: the join replay fills in everybody else and never a client's own
 * row. Its alliances therefore read as "allied to nobody".
 *
 * Decide the inbox rule on that copy and every such bot stops hearing its own
 * team's broadcast, which is what Wave Defense and every scripted round are
 * made of. The server's matrix is the authority and is filled the moment the
 * seat is taken, so that is what a BOUND ClientSim asks — the same table
 * botManagerDeliverInternalMessage asks for the bots' own channel.
 *
 * The fixture below is that bot exactly: bound to the sim, its own row in its
 * own table wiped, and the alliance recorded only on the server.
 * ============================================================ */
int run_bot_chat_mid_round_bot_reads_server_alliances(void) {
    ChatFixture f;
    GameSim    *sgs;
    GameSim    *bot2gs;
    int         i;

    if (chat_fixture_setup(&f) != 0) return 1;

    /* What botManagerAddBot does for a hosted bot. */
    clientSimSetBoundServerSim(f.bot1, f.sim);
    clientSimSetBoundServerSim(f.bot2, f.sim);

    /* The mid-round bot's own client-side row, as it really arrives: empty.
       Both directions are cleared so nothing on this side can answer the
       alliance question at all. */
    bot2gs = clientSimGetGameSim(f.bot2);
    for (i = 0; i < MAX_TANKS; i++) {
        allienceRemove(&bot2gs->plyrs->item[2].allie, (BYTE)i);
        allienceRemove(&bot2gs->plyrs->item[i].allie, 2);
    }
    UT_ASSERT_MSG((playersGetAlliesBitMap(&bot2gs->plyrs, 2) &
                   ((PlayerBitMap)1u << 1)) == 0,
                  "the fixture failed to empty the bot's own client-side "
                  "alliance row, so this case proves nothing");

    /* And the alliance the SERVER holds, which is the only place it lives for
       a bot seated after the round started. */
    sgs = serverSimGetGameSim(f.sim);
    allienceAdd(&sgs->plyrs->item[1].allie, 2);
    allienceAdd(&sgs->plyrs->item[2].allie, 1);

    /* The ally's broadcast must land. */
    {
        ClientCommand cmd = make_chat_cmd(/*dest=*/0xFF, "/info state");
        threadsWaitForMutex();
        CmdResult r = serverSimApplyCommand(f.sim, /*senderSlot=bot1*/1, &cmd);
        threadsReleaseMutex();
        UT_ASSERT_MSG(r == CMD_OK, "dispatcher rejected CMD_CHAT, got %d", (int)r);
    }
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot2)) == 1,
                  "a mid-round bot heard nothing from its own ally: %d "
                  "entries, expected 1. The filter is reading the bot's own "
                  "client-side table, whose row for itself is empty on every "
                  "bot built after the round started",
                  messageInboxCount(clientSimGetMessages(f.bot2)));
    {
        char body[BRAIN_INBOX_MSG_LEN];
        BYTE from = inbox_peek_body(f.bot2, 0, body, sizeof(body));
        UT_ASSERT_MSG(strcmp(body, "/info state") == 0,
                      "the mid-round bot kept the wrong line: '%s'", body);
        UT_ASSERT_MSG((int)from == 1,
                      "the line is from slot %u, expected 1", (unsigned)from);
    }

    /* And the enemy's must not: the server matrix is read for the whole rule,
       not just the half that lets a line through. The human in slot 0 is
       allied to nobody there. */
    {
        ClientCommand cmd = make_chat_cmd(/*dest=*/0xFF, "everyone fall back");
        threadsWaitForMutex();
        CmdResult r = serverSimApplyCommand(f.sim, /*senderSlot=human*/0, &cmd);
        threadsReleaseMutex();
        UT_ASSERT_MSG(r == CMD_OK, "dispatcher rejected CMD_CHAT, got %d", (int)r);
    }
    UT_ASSERT_MSG(messageInboxCount(clientSimGetMessages(f.bot2)) == 1,
                  "an enemy's broadcast reached a mid-round bot: %d entries, "
                  "expected the ally's line alone",
                  messageInboxCount(clientSimGetMessages(f.bot2)));

    /* Unbind before teardown: the fixture's unregister path is the one that
       owns these subscriptions, and a bound sim is not this test's to tear
       down. */
    clientSimSetBoundServerSim(f.bot1, NULL);
    clientSimSetBoundServerSim(f.bot2, NULL);
    chat_fixture_teardown(&f);
    return 0;
}
