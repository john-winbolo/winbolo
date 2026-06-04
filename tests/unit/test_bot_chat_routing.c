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
#include "client_command.h"
#include "client_sim.h"
#include "client_sim_control.h"
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

    /* Human also receives the broadcast — same flow, different slot. */
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
