/*
 * A re-authentication's client/verify is off the tick thread
 * (test_reauth_result.c).
 *
 * transportUdpServerHandleWbnReauth used to call client/verify inline: one
 * HTTPS round trip, up to 30 s of it, inside serverInstanceTick and under the
 * tick lock. After a rekey every WBN player re-auths at once, so a slow
 * WinBolo.net stopped the whole game.
 *
 * It now captures the slot, queues the verify for the WinBolo.net worker and
 * returns. The reply comes back through winbolonetThreadDrainResults at the
 * top of a later tick, and udpServerApplyReauthResult does the stamping the
 * reauth used to do: the flags, the deferred PLAYER_JOIN, the lobby publish
 * and the provisional-claim resolve, plus the write of the slot's WinBolo.net
 * key that the verify call itself used to do.
 *
 * What these two cases pin:
 *
 *   reauth_result_after_slot_reuse_discarded — the slot can be freed and
 *     handed to somebody else while the verify is out. A result whose connId
 *     no longer matches the slot's is dropped, with one log line, before
 *     anything reads or writes the slot. Stamping a new player with the
 *     previous occupant's identity is the failure this check exists to
 *     prevent, and it is not observable any other way: the wire looks the
 *     same either way.
 *
 *   reauth_result_stamps_slot — the happy path end to end over the loopback
 *     transport. The tick the REAUTH lands on queues the verify and stamps
 *     nothing; delivering a success result stamps the slot, fires the
 *     deferred join exactly once, and settles the provisional name claim.
 *
 * The other three are the anonymous fallback's side of it. A slot with a
 * deferred PLAYER_JOIN owed announces it un-keyed once the grace window
 * elapses, and the verify result announces it keyed. Those are different
 * events, not duplicates: winbolonetAddEvent resolves the slot's key when it
 * is called and winbolonetServerUpdate leaves player_a out when that key is
 * empty, so one names the account and one names nobody. Exactly one may go
 * out, and while a verify can still answer it is the verify's to make —
 * publishing the fallback's would leave the account with keyed kills, wins
 * and a leave against no join.
 *
 *   reauth_result_outruns_grace_stamps_once — the grace elapses with a verify
 *     still out. Nothing is announced while it can still answer; the success
 *     then announces once, keyed.
 *
 *   reauth_result_failure_releases_anonymous_join — same, but the verify
 *     refuses. The slot is not stamped and the fallback announces un-keyed,
 *     which is what it would have done with no verify at all.
 *
 *   reauth_result_lost_still_announces — the result never arrives. The hold
 *     has an absolute deadline (WBN_REAUTH_HOLD_TICKS) so a lost reply cannot
 *     take the announcement with it: past it the fallback announces un-keyed,
 *     once. This is the case that stops the fix turning a duplicate join into
 *     a missing one.
 *
 * Keyed versus un-keyed is read off wbnStubLastJoinKey, which the stub's
 * event spy fills from the slot's key at publish time the way the real
 * winbolonetAddEvent does.
 *
 * The WinBolo.net stub (test_stubs.c) records the enqueue, holds the reply
 * until the test releases it, and decides what it says. Its verify answers
 * FALSE by default, which is what the old synchronous stub always answered,
 * so the success body here is the first test to reach it.
 *
 * Log capture is SDL_SetLogOutputFunction, as test_hitch_logged.c does it.
 * test_main.c has already opened every category to WARN, so the discard line
 * reaches the sink.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"            /* clientSimGetConnectState */
#include "client_connect_state.h"  /* CLIENT_CONNECT_CONNECTED */
#include "client_command.h"
#include "game_sim.h"
#include "players.h"
#include "server_sim.h"
#include "server_sim_lifecycle.h"
#include "transport_udp.h"
#include "transport_udp_server_internal.h"  /* udpServer,
                                             * WBN_JOIN_REGISTER_GRACE_TICKS */
#include "threads.h"
#include "everard_map.h"
#include "../../src/winbolonet/winbolonet_core.h"   /* WINBOLONET_KEY_LEN */
#include "../../src/common/wb_log.h"
#include "test_harness.h"
#include "loopback_harness.h"

/* WinBolo.net stub surface, defined in test_stubs.c. */
extern bool     wbnStubRunning;
extern char     wbnStubServerKey[];
extern char     wbnStubPlayerKey[][WINBOLONET_KEY_LEN];
extern int      wbnStubJoinEventCalls;
extern uint16_t wbnStubJoinEventMask;
extern char     wbnStubLastJoinKey[];
extern int      wbnStubVerifyQueueCalls;
extern uint32_t wbnStubVerifyJobId;
extern char     wbnStubVerifyLastKey[];
extern char     wbnStubVerifyLastName[];
extern bool     wbnStubVerifyResultReady;
extern int      wbnStubApplyVerifyCalls;
extern bool     wbnStubVerifyOk;

/* A session key for the stub to hold. The queue path checks nothing here, but
 * a server with no key is not a server anyone re-auths against. */
#define RR_SERVER_KEY "0123456789abcdef0123456789abcdef"

/* The token the client presents. Its only job is to be non-empty and to come
 * back out of the stub unchanged, which is how the capture is observed. */
#define RR_TOKEN "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

/* The bare name a provisional claim is waiting to be promoted to. */
#define RR_DESIRED_NAME "Reauther"

/* The name the client joins under. It must be one the server will accept:
 * playerNameValidate reserves the -unverified suffix (rule 8,
 * PLAYER_NAME_ERR_RESERVED_SUFFIX) because that form is the server's own, and
 * serverHandleJoinRequest turns a name carrying it away with a JOIN_REJECT
 * before any other check. The temp name a provisional claim really wears is
 * built server-side by playerNameMakeUnverifiedSuffix and never comes off the
 * wire, so a joiner cannot ask for one. */
#define RR_JOIN_NAME "Provisional"

/* Pumps allowed for the join to converge. The map download dominates it, which
 * is why the clean-path loopback cases budget this much (loopback_quit_keeps_
 * peer 2000, loopback_hitch_recovers 4000) rather than a couple of hundred. */
#define RR_CONNECT_PUMPS 2000

/* Clear everything the stub carries between cases so neither case can read
 * the other's leavings. */
static void rrResetStub(void) {
    int i;
    wbnStubVerifyQueueCalls  = 0;
    wbnStubVerifyJobId       = 0;
    wbnStubVerifyResultReady = FALSE;
    wbnStubApplyVerifyCalls  = 0;
    wbnStubVerifyOk          = FALSE;
    wbnStubJoinEventCalls    = 0;
    wbnStubJoinEventMask     = 0;
    wbnStubLastJoinKey[0]    = '\0';
    wbnStubVerifyLastKey[0]  = '\0';
    wbnStubVerifyLastName[0] = '\0';
    for (i = 0; i < MAX_TANKS; i++) {
        wbnStubPlayerKey[i][0] = '\0';
    }
}

/* ---------------------------------------------------------------- */
/* Log capture                                                       */
/* ---------------------------------------------------------------- */

static int                   s_netWarnings;
static char                  s_lastWarning[512];
static SDL_LogOutputFunction s_prevSink;
static void                 *s_prevSinkData;

static void SDLCALL rrSink(void *userdata, int category,
                           SDL_LogPriority priority, const char *message) {
    (void)userdata;
    if (category == WB_LOG_CAT_NET && priority == SDL_LOG_PRIORITY_WARN) {
        s_netWarnings++;
        if (message != NULL) {
            SDL_strlcpy(s_lastWarning, message, sizeof(s_lastWarning));
        }
    }
}

static void rrCaptureBegin(void) {
    s_netWarnings = 0;
    s_lastWarning[0] = '\0';
    SDL_GetLogOutputFunction(&s_prevSink, &s_prevSinkData);
    SDL_SetLogOutputFunction(rrSink, NULL);
}

static void rrCaptureEnd(void) {
    SDL_SetLogOutputFunction(s_prevSink, s_prevSinkData);
}

/* ================================================================ */
/* The discard path                                                  */
/* ================================================================ */

/* Drive the reauth against a bare sim with the transport's client table
 * seeded by hand: the case is about what the handler does with a result, and
 * a real connection cannot be recycled to order. */
int run_reauth_result_after_slot_reuse_discarded(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim;
    const BYTE slot = 1;
    const uint64_t joinedConnId = 0x00000000deadbeefULL;
    const uint64_t reusedConnId = 0x0000000012345678ULL;
    uint32_t jobId;
    uint8_t flagsBefore;
    uint8_t flagsAfter;

    sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                    gameOpen, false, 0, -1);
    UT_ASSERT(sim != NULL);
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, slot, "Occupant", false);

    rrResetStub();
    wbnStubRunning = TRUE;
    SDL_strlcpy(wbnStubServerKey, RR_SERVER_KEY, WINBOLONET_KEY_LEN);

    memset(&udpServer.clients[slot], 0, sizeof(udpServer.clients[slot]));
    udpServer.clients[slot].connected = true;
    udpServer.clients[slot].connId    = joinedConnId;
    SDL_strlcpy(udpServer.clients[slot].playerName, "Occupant",
                PACKET_MAX_PLAYER_NAME);

    flagsBefore = playersGetClientFlags(&serverSimGetGameSim(sim)->plyrs, slot);

    /* The reauth queues and returns. */
    transportUdpServerHandleWbnReauth(sim, slot, RR_TOKEN);
    UT_ASSERT_MSG(wbnStubVerifyQueueCalls == 1,
                  "the reauth must queue exactly one client/verify (queued %d)",
                  wbnStubVerifyQueueCalls);
    jobId = wbnStubVerifyJobId;
    UT_ASSERT_MSG(jobId != 0, "the queued verify must have a job id");

    /* The slot is freed and handed to somebody else while the verify is out.
     * A fresh occupant gets a fresh connId. */
    udpServer.clients[slot].connId = reusedConnId;

    wbnStubVerifyOk = TRUE;   /* a result that WOULD stamp, if it were applied */
    rrCaptureBegin();
    udpServerApplyReauthResult(sim, jobId, 200, "{}");
    rrCaptureEnd();

    UT_ASSERT_MSG(wbnStubApplyVerifyCalls == 0,
                  "the connId check must run before the reply is even read "
                  "(the reply was read %d times)", wbnStubApplyVerifyCalls);

    flagsAfter = playersGetClientFlags(&serverSimGetGameSim(sim)->plyrs, slot);
    UT_ASSERT_MSG(flagsAfter == flagsBefore,
                  "slot %d flags changed on a discarded result: 0x%02x -> 0x%02x",
                  (int)slot, (unsigned)flagsBefore, (unsigned)flagsAfter);
    UT_ASSERT_MSG((flagsAfter & PLAYER_FLAG_WBN_VERIFIED) == 0,
                  "slot %d must not be stamped verified by a discarded result "
                  "(flags 0x%02x)", (int)slot, (unsigned)flagsAfter);
    UT_ASSERT_MSG(wbnStubPlayerKey[slot][0] == '\0',
                  "slot %d must keep no WinBolo.net key on a discarded result "
                  "(got '%s')", (int)slot, wbnStubPlayerKey[slot]);
    UT_ASSERT_MSG(udpServer.clients[slot].wbnWasVerified == false,
                  "slot %d must not gain the durable rekey bit on a discarded "
                  "result", (int)slot);
    UT_ASSERT_MSG(wbnStubJoinEventCalls == 0,
                  "a discarded result must emit no PLAYER_JOIN (emitted %d)",
                  wbnStubJoinEventCalls);

    UT_ASSERT_MSG(s_netWarnings == 1,
                  "the discard must write exactly one log line (wrote %d, last "
                  "'%s')", s_netWarnings, s_lastWarning);

    /* A second delivery of the same job finds no entry and stays quiet, so a
     * repeated drain cannot multiply the line. */
    rrCaptureBegin();
    udpServerApplyReauthResult(sim, jobId, 200, "{}");
    rrCaptureEnd();
    UT_ASSERT_MSG(s_netWarnings == 0,
                  "re-delivering job %u wrote %d log lines, expected none - "
                  "the pending entry was not consumed",
                  (unsigned)jobId, s_netWarnings);

    memset(&udpServer.clients[slot], 0, sizeof(udpServer.clients[slot]));
    wbnStubRunning = FALSE;
    wbnStubServerKey[0] = '\0';
    rrResetStub();
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================ */
/* The stamping path                                                 */
/* ================================================================ */

/* The client has joined when its OWN state says so. Watching only the server
 * side is not enough: clientSimGetMyPlayerNum returns 0 for a client that has
 * not joined at all (ClientSim starts myPlayerNum at 0, and the accessor
 * returns BYTE, so there is no out-of-range value to test), which would read
 * as slot 0 and go true the moment the server marked slot 0 connected —
 * before the accept had been read back. CLIENT_CONNECT_CONNECTED is also
 * exactly what transportUdpClientSubmitCommand requires before it will put a
 * command on the wire. The server-side check stays because the setup below
 * reaches into udpServer.clients[slot] straight after. */
static bool rrClientJoined(LoopbackHarness *h, void *user) {
    BYTE slot;
    (void)user;
    if (clientSimGetConnectState(h->cs) != CLIENT_CONNECT_CONNECTED) {
        return false;
    }
    slot = clientSimGetMyPlayerNum(h->cs);
    return slot < MAX_TANKS && udpServer.clients[slot].connected;
}

static bool rrVerifyQueued(LoopbackHarness *h, void *user) {
    (void)h; (void)user;
    return wbnStubVerifyQueueCalls > 0;
}

int run_reauth_result_stamps_slot(void) {
    LoopbackHarness h;
    ClientCommand cmd;
    BYTE slot;
    uint8_t flags;
    char nameAfter[PACKET_MAX_PLAYER_NAME];

    memset(&h, 0, sizeof(h));
    UT_ASSERT(loopbackHarnessStart(&h, RR_JOIN_NAME, /*lobbyMode*/ true,
                                   NULL, /*seed*/ 9119));

    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, RR_CONNECT_PUMPS,
                                           rrClientJoined, NULL) > 0,
                  "the client never finished joining: connect state %d",
                  (int)clientSimGetConnectState(h.cs));
    slot = clientSimGetMyPlayerNum(h.cs);
    UT_ASSERT_MSG(slot < MAX_TANKS,
                  "the client has no server slot (got %d)", (int)slot);

    rrResetStub();
    wbnStubRunning = TRUE;
    SDL_strlcpy(wbnStubServerKey, RR_SERVER_KEY, WINBOLONET_KEY_LEN);

    /* Put the slot where a fresh WBN joiner sits: a provisional claim on the
     * bare name it will be promoted to once WBN vouches for it, and a
     * deferred join owed. Re-arming rather than trusting the one the join
     * left keeps the case independent of how many pumps the join took. */
    threadsWaitForMutex();
    udpServer.clients[slot].claimPending = true;
    SDL_strlcpy(udpServer.clients[slot].claimDesiredName, RR_DESIRED_NAME,
                PACKET_MAX_PLAYER_NAME);
    wbnJoinArm(&udpServer.clients[slot].wbnJoin, udpServer.tickCount,
               WBN_JOIN_REGISTER_GRACE_TICKS);
    threadsReleaseMutex();
    wbnStubJoinEventCalls = 0;
    wbnStubJoinEventMask  = 0;

    /* The client re-authenticates. */
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_WBN_REAUTH;
    SDL_strlcpy(cmd.u.wbnReauth.token, RR_TOKEN, sizeof(cmd.u.wbnReauth.token));
    clientSimSubmitCommand(h.cs, &cmd);

    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, 200, rrVerifyQueued, NULL) > 0,
                  "the REAUTH never reached the server: %d verifies queued",
                  wbnStubVerifyQueueCalls);

    /* Queued with what the reauth captured: the presented key, and the
     * claim's desired bare name rather than the temp display name. */
    UT_ASSERT_MSG(strcmp(wbnStubVerifyLastKey, RR_TOKEN) == 0,
                  "client/verify was queued with key '%s', expected '%s'",
                  wbnStubVerifyLastKey, RR_TOKEN);
    UT_ASSERT_MSG(strcmp(wbnStubVerifyLastName, RR_DESIRED_NAME) == 0,
                  "client/verify was queued under name '%s', expected '%s'",
                  wbnStubVerifyLastName, RR_DESIRED_NAME);

    /* Nothing was stamped on the tick that queued it. */
    threadsWaitForMutex();
    flags = playersGetClientFlags(&serverSimGetGameSim(h.sim)->plyrs, slot);
    threadsReleaseMutex();
    UT_ASSERT_MSG((flags & PLAYER_FLAG_WBN_VERIFIED) == 0,
                  "slot %d was stamped verified on the tick that queued the "
                  "verify (flags 0x%02x)", (int)slot, (unsigned)flags);
    UT_ASSERT_MSG(wbnStubPlayerKey[slot][0] == '\0',
                  "slot %d holds key '%s' before the verify came back",
                  (int)slot, wbnStubPlayerKey[slot]);
    UT_ASSERT_MSG(wbnStubJoinEventCalls == 0,
                  "the deferred join fired %d times before the verify came "
                  "back", wbnStubJoinEventCalls);
    UT_ASSERT_MSG(udpServer.clients[slot].claimPending == true,
                  "slot %d's provisional claim resolved before the verify came "
                  "back", (int)slot);

    /* Deliver a success for the slot's current connId. */
    wbnStubVerifyOk          = TRUE;
    wbnStubVerifyResultReady = TRUE;
    loopbackHarnessPump(&h);
    loopbackHarnessPump(&h);

    UT_ASSERT_MSG(wbnStubApplyVerifyCalls == 1,
                  "the verify reply was read %d times, expected once",
                  wbnStubApplyVerifyCalls);

    threadsWaitForMutex();
    flags = playersGetClientFlags(&serverSimGetGameSim(h.sim)->plyrs, slot);
    SDL_strlcpy(nameAfter, udpServer.clients[slot].playerName,
                sizeof(nameAfter));
    threadsReleaseMutex();

    UT_ASSERT_MSG((flags & PLAYER_FLAG_WBN_VERIFIED) != 0,
                  "slot %d did not gain PLAYER_FLAG_WBN_VERIFIED within two "
                  "pumps of the result (flags 0x%02x)",
                  (int)slot, (unsigned)flags);
    UT_ASSERT_MSG(udpServer.clients[slot].wbnWasVerified == true,
                  "slot %d did not gain the durable rekey bit", (int)slot);
    UT_ASSERT_MSG(strcmp(wbnStubPlayerKey[slot], RR_TOKEN) == 0,
                  "slot %d holds key '%s', expected '%s' - the key must be "
                  "written when the result is applied",
                  (int)slot, wbnStubPlayerKey[slot], RR_TOKEN);

    UT_ASSERT_MSG(wbnStubJoinEventCalls == 1,
                  "the deferred PLAYER_JOIN fired %d times, expected once",
                  wbnStubJoinEventCalls);
    UT_ASSERT_MSG((wbnStubJoinEventMask & (uint16_t)(1u << slot)) != 0,
                  "the PLAYER_JOIN named no slot %d (mask 0x%04x)",
                  (int)slot, (unsigned)wbnStubJoinEventMask);
    UT_ASSERT_MSG(strcmp(wbnStubLastJoinKey, RR_TOKEN) == 0,
                  "the PLAYER_JOIN went out under key '%s', expected the "
                  "account's '%s' - a join published before the key is "
                  "written carries no player_a",
                  wbnStubLastJoinKey, RR_TOKEN);

    /* The claim settles: nobody else holds the bare name, so the slot is
     * promoted straight to it. */
    UT_ASSERT_MSG(udpServer.clients[slot].claimPending == false,
                  "slot %d's provisional claim is still pending", (int)slot);
    UT_ASSERT_MSG(strcmp(nameAfter, RR_DESIRED_NAME) == 0,
                  "slot %d is named '%s', expected the promoted '%s'",
                  (int)slot, nameAfter, RR_DESIRED_NAME);

    wbnStubRunning = FALSE;
    wbnStubServerKey[0] = '\0';
    rrResetStub();
    loopbackHarnessStop(&h);
    return 0;
}

/* ================================================================ */
/* The fallback's side: what happens when the grace runs out first   */
/* ================================================================ */

/* Bring a client up, put its slot where a fresh WBN joiner sits, send a REAUTH
 * and pump until the verify is queued — then expire the grace.
 *
 * The order matters. The join is armed with its real grace first, so the sweep
 * stays quiet through the pumps that carry the REAUTH to the server; only once
 * the verify is outstanding is the arm re-made with a zero grace, which puts
 * its deadline on the current tick. From there the only thing keeping the
 * anonymous join from firing is the hold. That is how these cases reach the
 * expired-grace path without running five seconds of clock.
 *
 * Returns 0 with *outSlot set, or 1 with the failure already reported. */
static int rrSetupHeldReauth(LoopbackHarness *h, BYTE *outSlot) {
    ClientCommand cmd;
    BYTE slot;

    UT_ASSERT_MSG(loopbackHarnessPumpUntil(h, RR_CONNECT_PUMPS,
                                           rrClientJoined, NULL) > 0,
                  "the client never finished joining: connect state %d",
                  (int)clientSimGetConnectState(h->cs));
    slot = clientSimGetMyPlayerNum(h->cs);
    UT_ASSERT_MSG(slot < MAX_TANKS,
                  "the client has no server slot (got %d)", (int)slot);

    rrResetStub();
    wbnStubRunning = TRUE;
    SDL_strlcpy(wbnStubServerKey, RR_SERVER_KEY, WINBOLONET_KEY_LEN);

    threadsWaitForMutex();
    udpServer.clients[slot].claimPending = true;
    SDL_strlcpy(udpServer.clients[slot].claimDesiredName, RR_DESIRED_NAME,
                PACKET_MAX_PLAYER_NAME);
    wbnJoinArm(&udpServer.clients[slot].wbnJoin, udpServer.tickCount,
               WBN_JOIN_REGISTER_GRACE_TICKS);
    threadsReleaseMutex();
    wbnStubJoinEventCalls = 0;
    wbnStubJoinEventMask  = 0;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_WBN_REAUTH;
    SDL_strlcpy(cmd.u.wbnReauth.token, RR_TOKEN, sizeof(cmd.u.wbnReauth.token));
    clientSimSubmitCommand(h->cs, &cmd);

    UT_ASSERT_MSG(loopbackHarnessPumpUntil(h, 200, rrVerifyQueued, NULL) > 0,
                  "the REAUTH never reached the server: %d verifies queued",
                  wbnStubVerifyQueueCalls);
    UT_ASSERT_MSG(wbnStubJoinEventCalls == 0,
                  "the fallback fired %d times before the grace was expired",
                  wbnStubJoinEventCalls);

    /* A zero grace puts the anonymous deadline on this tick: the sweep would
     * announce on its next pass if it were not deferring to the verify. */
    threadsWaitForMutex();
    wbnJoinArm(&udpServer.clients[slot].wbnJoin, udpServer.tickCount, 0);
    threadsReleaseMutex();

    *outSlot = slot;
    return 0;
}

/* The grace runs out with the verify still outstanding — reachable only since
 * the verify came off the tick thread. Nothing is announced while it can still
 * answer, and the success then announces once, carrying the account's key. */
int run_reauth_result_outruns_grace_stamps_once(void) {
    LoopbackHarness h;
    BYTE slot = MAX_TANKS;
    uint8_t flags;
    int i;

    memset(&h, 0, sizeof(h));
    UT_ASSERT(loopbackHarnessStart(&h, RR_JOIN_NAME, /*lobbyMode*/ true,
                                   NULL, /*seed*/ 5150));
    UT_ASSERT(rrSetupHeldReauth(&h, &slot) == 0);

    /* Well past the anonymous deadline, and still nothing: the sweep is
     * deferring to the outstanding verify. */
    for (i = 0; i < 10; i++) loopbackHarnessPump(&h);
    UT_ASSERT_MSG(wbnStubJoinEventCalls == 0,
                  "the anonymous fallback published %d joins for slot %d with "
                  "the verify still outstanding, expected none (last key '%s')",
                  wbnStubJoinEventCalls, (int)slot, wbnStubLastJoinKey);
    UT_ASSERT_MSG(udpServer.clients[slot].wbnJoin.pending == true,
                  "slot %d's deferred join was consumed while the verify was "
                  "still outstanding", (int)slot);

    /* The verify answers. Its announcement is the keyed one. */
    wbnStubVerifyOk          = TRUE;
    wbnStubVerifyResultReady = TRUE;
    loopbackHarnessPump(&h);
    loopbackHarnessPump(&h);

    UT_ASSERT_MSG(wbnStubJoinEventCalls == 1,
                  "the join was published %d times, expected once",
                  wbnStubJoinEventCalls);
    UT_ASSERT_MSG(strcmp(wbnStubLastJoinKey, RR_TOKEN) == 0,
                  "the join went out under key '%s', expected the account's "
                  "'%s'", wbnStubLastJoinKey, RR_TOKEN);
    UT_ASSERT_MSG((wbnStubJoinEventMask & (uint16_t)(1u << slot)) != 0,
                  "the join named no slot %d (mask 0x%04x)",
                  (int)slot, (unsigned)wbnStubJoinEventMask);

    threadsWaitForMutex();
    flags = playersGetClientFlags(&serverSimGetGameSim(h.sim)->plyrs, slot);
    threadsReleaseMutex();
    UT_ASSERT_MSG((flags & PLAYER_FLAG_WBN_VERIFIED) != 0,
                  "slot %d was not stamped verified (flags 0x%02x)",
                  (int)slot, (unsigned)flags);
    UT_ASSERT_MSG(strcmp(wbnStubPlayerKey[slot], RR_TOKEN) == 0,
                  "slot %d holds key '%s', expected '%s'",
                  (int)slot, wbnStubPlayerKey[slot], RR_TOKEN);
    UT_ASSERT_MSG(udpServer.clients[slot].claimPending == false,
                  "slot %d's provisional claim is still pending", (int)slot);
    UT_ASSERT_MSG(strcmp(udpServer.clients[slot].playerName,
                         RR_DESIRED_NAME) == 0,
                  "slot %d is named '%s', expected the promoted '%s'",
                  (int)slot, udpServer.clients[slot].playerName,
                  RR_DESIRED_NAME);

    /* And the fallback stays quiet behind it. */
    for (i = 0; i < 10; i++) loopbackHarnessPump(&h);
    UT_ASSERT_MSG(wbnStubJoinEventCalls == 1,
                  "the join was published %d times in total, expected once",
                  wbnStubJoinEventCalls);

    wbnStubRunning = FALSE;
    wbnStubServerKey[0] = '\0';
    rrResetStub();
    loopbackHarnessStop(&h);
    return 0;
}

/* The verify refuses. The slot is not stamped, so there is no account to
 * announce — the hold is released and the fallback announces un-keyed, which
 * is what it would have done with no verify at all. */
int run_reauth_result_failure_releases_anonymous_join(void) {
    LoopbackHarness h;
    BYTE slot = MAX_TANKS;
    uint8_t flags;
    int i;

    memset(&h, 0, sizeof(h));
    UT_ASSERT(loopbackHarnessStart(&h, RR_JOIN_NAME, /*lobbyMode*/ true,
                                   NULL, /*seed*/ 5151));
    UT_ASSERT(rrSetupHeldReauth(&h, &slot) == 0);

    for (i = 0; i < 10; i++) loopbackHarnessPump(&h);
    UT_ASSERT_MSG(wbnStubJoinEventCalls == 0,
                  "the anonymous fallback published %d joins with the verify "
                  "still outstanding, expected none", wbnStubJoinEventCalls);

    /* The verify refuses (the stub's default answer). */
    wbnStubVerifyOk          = FALSE;
    wbnStubVerifyResultReady = TRUE;
    for (i = 0; i < 3; i++) loopbackHarnessPump(&h);

    UT_ASSERT_MSG(wbnStubApplyVerifyCalls == 1,
                  "the verify reply was read %d times, expected once",
                  wbnStubApplyVerifyCalls);
    UT_ASSERT_MSG(wbnStubJoinEventCalls == 1,
                  "the fallback published %d joins after the refusal, "
                  "expected once", wbnStubJoinEventCalls);
    UT_ASSERT_MSG(wbnStubLastJoinKey[0] == '\0',
                  "the join went out under key '%s', expected none - a refused "
                  "verify leaves the slot with no account to name",
                  wbnStubLastJoinKey);

    threadsWaitForMutex();
    flags = playersGetClientFlags(&serverSimGetGameSim(h.sim)->plyrs, slot);
    threadsReleaseMutex();
    UT_ASSERT_MSG((flags & PLAYER_FLAG_WBN_VERIFIED) == 0,
                  "slot %d was stamped verified by a refused verify "
                  "(flags 0x%02x)", (int)slot, (unsigned)flags);
    UT_ASSERT_MSG(wbnStubPlayerKey[slot][0] == '\0',
                  "slot %d holds key '%s' after a refused verify",
                  (int)slot, wbnStubPlayerKey[slot]);

    for (i = 0; i < 10; i++) loopbackHarnessPump(&h);
    UT_ASSERT_MSG(wbnStubJoinEventCalls == 1,
                  "the join was published %d times in total, expected once",
                  wbnStubJoinEventCalls);

    wbnStubRunning = FALSE;
    wbnStubServerKey[0] = '\0';
    rrResetStub();
    loopbackHarnessStop(&h);
    return 0;
}

/* The result never arrives — the worker was torn down with the job queued, the
 * reply was dropped at the results cap, or WinBolo.net simply never answered.
 * The hold must not take the announcement with it, so it lapses at
 * WBN_REAUTH_HOLD_TICKS and the fallback announces un-keyed, once.
 *
 * The hold is 1750 ticks (35 s @ 50 Hz), so the case ages it through the test
 * hook rather than pumping it out. */
int run_reauth_result_lost_still_announces(void) {
    LoopbackHarness h;
    BYTE slot = MAX_TANKS;
    int i;

    memset(&h, 0, sizeof(h));
    UT_ASSERT(loopbackHarnessStart(&h, RR_JOIN_NAME, /*lobbyMode*/ true,
                                   NULL, /*seed*/ 5152));
    UT_ASSERT(rrSetupHeldReauth(&h, &slot) == 0);

    /* The hold is live and nothing is announced. */
    for (i = 0; i < 10; i++) loopbackHarnessPump(&h);
    UT_ASSERT_MSG(wbnStubJoinEventCalls == 0,
                  "the fallback published %d joins while the hold was live, "
                  "expected none", wbnStubJoinEventCalls);

    /* No result is ever delivered; the hold simply runs out. */
    threadsWaitForMutex();
    transportUdpServerExpireReauthHoldForTest(slot);
    threadsReleaseMutex();
    loopbackHarnessPump(&h);
    loopbackHarnessPump(&h);

    UT_ASSERT_MSG(wbnStubApplyVerifyCalls == 0,
                  "a verify reply was read %d times, expected none - this case "
                  "is the one where none arrives", wbnStubApplyVerifyCalls);
    UT_ASSERT_MSG(wbnStubJoinEventCalls == 1,
                  "the fallback published %d joins once the hold lapsed, "
                  "expected once", wbnStubJoinEventCalls);
    UT_ASSERT_MSG(wbnStubLastJoinKey[0] == '\0',
                  "the join went out under key '%s', expected none - no verify "
                  "ever answered", wbnStubLastJoinKey);
    UT_ASSERT_MSG((wbnStubJoinEventMask & (uint16_t)(1u << slot)) != 0,
                  "the join named no slot %d (mask 0x%04x)",
                  (int)slot, (unsigned)wbnStubJoinEventMask);

    /* Exactly once: a lapsed hold must not turn into a join every tick. */
    for (i = 0; i < 20; i++) loopbackHarnessPump(&h);
    UT_ASSERT_MSG(wbnStubJoinEventCalls == 1,
                  "the fallback published %d joins in total, expected once - a "
                  "lapsed hold must announce once, not once per tick",
                  wbnStubJoinEventCalls);

    wbnStubRunning = FALSE;
    wbnStubServerKey[0] = '\0';
    rrResetStub();
    loopbackHarnessStop(&h);
    return 0;
}
