/*
 * Stall-advance lockout: a stale-but-newest input rebases the slot.
 *
 * Once a slot has been dry for more than STALL_ADVANCE_DRY_TICKS half-steps
 * the server stall-advances it — every half-step it synthesises
 * lastProcessedInput + 1 from the held buttons and applies it. Only a fresh
 * apply resets inputDryTicks, so a client that has fallen behind never stops
 * that run on its own: its inputs are renumbered (clientBuildInputPacket) off
 * a lastProcessedInput that is half a round trip old, land at or below the
 * current one, and are dropped as stale forever. The player cannot move.
 *
 * Two things break the lockout. serverSimInputWouldRebase lets an input
 * strictly newer than anything the slot has ever received past the intake
 * check although it is stale, so it reaches the queue at all. The dequeue
 * loop then moves lastProcessedInput back under the newest queued tick while
 * the slot is in a stall-advance run, so that one entry is taken as fresh —
 * and only that one: everything below it still drops as stale, so a tick a
 * substitute already moved the tank through is not run a second time.
 *
 * What these cases pin: a real input is applied again after a hitch at every
 * one-way delay and keeps being applied for the rest of the run; a tick is
 * rebased onto once however many redundant copies of it arrive; and a
 * one-shot commanded on the rebased tick executes exactly once.
 *
 * What they deliberately do not pin: statDroppedStaleInputs reaching zero or
 * ceasing to climb. A rebase takes one entry and drops the backlog under it,
 * and a producer whose counter is still behind keeps numbering its inputs off
 * a stale lastProcessedInput, so stale drops continue. These cases pin that
 * inputs are applied again, not that none are dropped.
 *
 * The modelled client re-implements the renumber rule from
 * clientBuildInputPacket rather than calling it: the real one needs a
 * ClientSim with a tank, which this test does not build. Delivery mirrors the
 * server's own intake check (transport_udp_server's input loop), so the test
 * exercises the path that was broken rather than a shortcut into the sim.
 *
 * Drives serverSimApplyInput + serverSimTick directly on ut_make_running_sim
 * (slot 0) and reads T2 state off the ServerSim struct (the unittests profile
 * permits internal access). serverSimTick runs two half-steps per call, so
 * the modelled client's two inputs for a frame are delivered at its boundary;
 * every delay used here is an even number of half-steps, so no packet lands
 * on the wrong side of a frame.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* direct field access: lastProcessedInput etc. */
#include "game_sim.h"              /* GameSim.tanks[] */
#include "tank.h"                  /* tankSetMines / tankGetMines */
#include "client_sim_internal.h"   /* CLIENT_INPUT_JUMP_MARGIN_HALFSTEPS */
#include "input_packet.h"
#include "test_harness.h"

#define SLR_SLOT 0

/* Buttons held through establish and the hitch. */
#define SLR_HOLD_BUTTONS   INPUT_BTN_LEFT
/* Buttons on every input the modelled client produces. A substitute is
 * synthesised from lastInputButtons and so can never introduce a new value,
 * which keeps the two streams distinguishable in the sim's own state. */
#define SLR_CLIENT_BUTTONS INPUT_BTN_RIGHT

#define SLR_HITCH_FRAMES     10
#define SLR_WINDOW_HALFSTEPS 300
#define SLR_WINDOW_FRAMES    (SLR_WINDOW_HALFSTEPS / 2)
/* Slack on top of the round trip and the dry-run threshold before a real
 * input must be applied again. The jitter buffer re-enters filling mode on
 * every drain and grows toward JITTER_BUFFER_MAX, so each refill costs up to
 * that many half-steps during which nothing is dequeued. */
#define SLR_SLACK_HALFSTEPS  24
#define SLR_MAX_DELAY_HALFSTEPS 10
/* Ticks stay well under this: establish ends at 12 and nothing advances
 * lastProcessedInput by more than one per half-step thereafter. */
#define SLR_TICK_MAX 2048

/* Hand one input to the sim the way an arriving packet would. */
static void slr_feed(ServerSim *sim, uint32_t tick, uint8_t buttons,
                     uint8_t actions) {
    InputPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.tick      = tick;
    pkt.playerNum = SLR_SLOT;
    pkt.buttons   = buttons;
    pkt.actions   = actions;
    serverSimApplyInput(sim, &pkt);
}

/* The server's intake check, mirrored from the input loop in
 * transport_udp_server: an input reaches the sim only when it is strictly
 * newer than the last processed tick, or when it is the one that breaks a
 * lockout. Returns TRUE when the input was handed to serverSimApplyInput. */
static bool slr_deliver(ServerSim *sim, const InputPacket *pkt) {
    if (pkt->tick > serverSimGetLastProcessedInput(sim, SLR_SLOT) ||
        serverSimInputWouldRebase(sim, SLR_SLOT, pkt->tick)) {
        serverSimApplyInput(sim, pkt);
        return TRUE;
    }
    return FALSE;
}

/* The producer-side renumber from clientBuildInputPacket: an input at or
 * behind the server's last-processed tick is renumbered to the smallest value
 * past it whose parity matches the input's intent — a game tick wants an even
 * number, a keys tick an odd one — with the intent taken from the producer's
 * own alternation and never derived from the stale tick. The producer's
 * counter keeps running on its own clock either way. */
static uint32_t slr_renumber(uint32_t tick, bool isGameTick, uint32_t lpiSeen) {
    if (tick <= lpiSeen) {
        uint32_t renum = lpiSeen + 1;
        if (((renum % 2) == 0) != isGameTick) {
            renum++;
        }
        return renum;
    }
    return tick;
}

/* Establish the stream: two fresh inputs per serverSimTick over six frames,
 * feeding ticks 1..12 so the jitter buffer fills without ever stalling.
 * Leaves lastProcessedInput == 12; returns the next unused tick (13). */
static uint32_t slr_establish(ServerSim *sim) {
    uint32_t t = 1;
    int frame;
    for (frame = 0; frame < 6; frame++) {
        slr_feed(sim, t++, SLR_HOLD_BUTTONS, 0);
        slr_feed(sim, t++, SLR_HOLD_BUTTONS, 0);
        serverSimTick(sim);
    }
    return t;
}

/* One delay sweep: establish, hitch, then model a client at `delayMs` of
 * one-way delay for SLR_WINDOW_HALFSTEPS half-steps. */
static int slr_run_delay(uint32_t delayMs) {
    ServerSim *sim;
    uint32_t delayHalf = delayMs / 10;
    uint32_t clientTick;
    uint32_t lpiAtBurstEnd;
    bool     rebaseSeen = FALSE;
    int      frame, k, firstApplyFrame = -1;

    /* The delay line, indexed by the half-step a packet lands on. */
    static InputPacket line[SLR_WINDOW_HALFSTEPS + SLR_MAX_DELAY_HALFSTEPS + 2];
    static bool    lineValid[SLR_WINDOW_HALFSTEPS + SLR_MAX_DELAY_HALFSTEPS + 2];
    /* What the client can see of lastProcessedInput, per half-step: a
     * snapshot value is delayHalf half-steps old by the time it is read. */
    static uint32_t lpiSeen[SLR_WINDOW_HALFSTEPS + 2];
    /* Ticks the test itself produced, so a real apply is told apart from a
     * stall-advance substitute landing on the same number. */
    static uint8_t fedTick[SLR_TICK_MAX];
    /* Per-frame accounting, read back once the recovery point is known. */
    static uint16_t staleByFrame[SLR_WINDOW_FRAMES];
    static uint8_t  deliveredByFrame[SLR_WINDOW_FRAMES];
    static uint8_t  appliedByFrame[SLR_WINDOW_FRAMES];

    memset(lineValid, 0, sizeof(lineValid));
    memset(fedTick, 0, sizeof(fedTick));
    memset(staleByFrame, 0, sizeof(staleByFrame));
    memset(deliveredByFrame, 0, sizeof(deliveredByFrame));
    memset(appliedByFrame, 0, sizeof(appliedByFrame));

    sim = ut_make_running_sim("Lockout");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    clientTick = slr_establish(sim);  /* 13 */
    UT_ASSERT_MSG(sim->inputBufferFilled[SLR_SLOT],
                  "delay %ums: stream not established", delayMs);
    UT_ASSERT_MSG(sim->lastProcessedInput[SLR_SLOT] == 12,
                  "delay %ums: establish left lastProcessedInput at %u, "
                  "expected 12", delayMs, sim->lastProcessedInput[SLR_SLOT]);

    /* The hitch: ten frames back to back with nothing fed. That is twenty
     * half-steps, past STALL_ADVANCE_DRY_TICKS, so the slot stall-advances
     * and lastProcessedInput runs ahead of anything the client has produced.
     * The client's own counter keeps running through it. */
    for (frame = 0; frame < SLR_HITCH_FRAMES; frame++) {
        serverSimTick(sim);
        clientTick += 2;
    }
    lpiAtBurstEnd = sim->lastProcessedInput[SLR_SLOT];
    UT_ASSERT_MSG(lpiAtBurstEnd > 12,
                  "delay %ums: the hitch did not stall-advance the slot "
                  "(lastProcessedInput %u)", delayMs, lpiAtBurstEnd);
    UT_ASSERT_MSG(sim->inputDryTicks[SLR_SLOT] > STALL_ADVANCE_DRY_TICKS,
                  "delay %ums: the hitch left the slot out of a stall-advance "
                  "run (inputDryTicks %u)", delayMs,
                  sim->inputDryTicks[SLR_SLOT]);

    for (frame = 0; frame < SLR_WINDOW_FRAMES; frame++) {
        int      hs0 = frame * 2;
        uint32_t lpiNow = sim->lastProcessedInput[SLR_SLOT];
        uint32_t lpiPre, staleBefore, staleAfter, lpiPost;

        /* Both half-steps of a frame read the same server state. */
        lpiSeen[hs0] = lpiNow;
        lpiSeen[hs0 + 1] = lpiNow;

        /* The client produces one input per half-step off its own counter,
         * renumbered against the last-processed tick it can see. */
        for (k = 0; k < 2; k++) {
            int      hs = hs0 + k;
            bool     isGameTick = ((clientTick % 2) == 0);
            uint32_t seen = (hs >= (int)delayHalf) ? lpiSeen[hs - delayHalf]
                                                   : lpiAtBurstEnd;
            uint32_t tick = slr_renumber(clientTick, isGameTick, seen);
            int      arrival = hs + (int)delayHalf;

            clientTick++;
            memset(&line[arrival], 0, sizeof(line[arrival]));
            line[arrival].tick      = tick;
            line[arrival].playerNum = SLR_SLOT;
            line[arrival].buttons   = SLR_CLIENT_BUTTONS;
            lineValid[arrival] = TRUE;
        }

        /* Deliver whatever lands in this frame, through the real intake
         * check. An input the check admits only on its second arm is one the
         * tick boundary will rebase onto. */
        for (k = 0; k < 2; k++) {
            int hs = hs0 + k;
            if (!lineValid[hs]) {
                continue;
            }
            if (serverSimInputWouldRebase(sim, SLR_SLOT, line[hs].tick)) {
                rebaseSeen = TRUE;
            }
            if (slr_deliver(sim, &line[hs])) {
                deliveredByFrame[frame]++;
                if (line[hs].tick < SLR_TICK_MAX) {
                    fedTick[line[hs].tick] = 1;
                }
            }
        }

        lpiPre = sim->lastProcessedInput[SLR_SLOT];
        staleBefore = sim->statDroppedStaleInputs[SLR_SLOT];
        serverSimTick(sim);
        lpiPost = sim->lastProcessedInput[SLR_SLOT];
        staleAfter = sim->statDroppedStaleInputs[SLR_SLOT];
        /* statDroppedStaleInputs is a window counter zeroed every 100
         * half-steps by the [netstat] line, so accumulate the per-frame
         * delta; a frame that spans a reset contributes what it shows. */
        staleByFrame[frame] = (uint16_t)(staleAfter >= staleBefore
                                             ? staleAfter - staleBefore
                                             : staleAfter);

        /* The rebase feeds the backlog catch-up, which is capped at two
         * applies per half-step. A frame that rebases can end below where it
         * started, which is not an advance at all. */
        if (rebaseSeen) {
            UT_ASSERT_MSG(lpiPost <= lpiPre + 4,
                          "delay %ums frame %d: lastProcessedInput went %u -> "
                          "%u in one tick — past the 2-applies-per-half-step "
                          "cap of 4", delayMs, frame, lpiPre, lpiPost);
        }

        /* A real apply is the only thing that zeroes inputDryTicks, so a
         * value of 0 or 1 after two half-steps means one of them applied a
         * real input rather than a substitute. Pair that with lastProcessedInput
         * sitting on a tick the test actually fed — stall-advance walks it
         * through those same numbers, so watching it move is not on its own
         * evidence of anything. Nothing can carry it off a fed tick within the
         * frame: a substitute needs a dry run past the threshold, which the
         * apply has just cleared. */
        if (sim->inputDryTicks[SLR_SLOT] <= 1) {
            UT_ASSERT_MSG(lpiPost < SLR_TICK_MAX && fedTick[lpiPost],
                          "delay %ums frame %d: a fresh apply left "
                          "lastProcessedInput at %u, which the test never fed",
                          delayMs, frame, lpiPost);
            appliedByFrame[frame] = 1;
            if (firstApplyFrame < 0) {
                firstApplyFrame = frame;
            }
        }
    }

    /* Recovery: a real input is applied again within one round trip plus the
     * dry-run threshold and the refill slack. Against the unrebased server
     * there is no such frame at all at 60ms and up — the slot never leaves
     * the lockout — so this is a margin, not a near miss. */
    {
        uint32_t limit = 2 * delayHalf + STALL_ADVANCE_DRY_TICKS +
                         SLR_SLACK_HALFSTEPS;
        uint32_t firstApplyHalfStep;
        UT_ASSERT_MSG(firstApplyFrame >= 0,
                      "delay %ums: no fed input was ever applied again in %d "
                      "half-steps after the hitch — the slot is locked out",
                      delayMs, SLR_WINDOW_HALFSTEPS);
        firstApplyHalfStep = (uint32_t)(firstApplyFrame * 2 + 1);
        UT_ASSERT_MSG(firstApplyHalfStep <= limit,
                      "delay %ums: first real apply at half-step %u, past the "
                      "limit of %u", delayMs, firstApplyHalfStep, limit);
    }

    /* The stream keeps running: at least one real apply in each quarter of
     * what is left of the window. A single lucky apply is not a repair. */
    {
        int startFrame = firstApplyFrame + 1;
        int span = SLR_WINDOW_FRAMES - startFrame;
        int q;
        UT_ASSERT_MSG(span >= 8,
                      "delay %ums: only %d frames left after the first apply "
                      "— too few to measure the stream", delayMs, span);
        for (q = 0; q < 4; q++) {
            int qStart = startFrame + (span * q) / 4;
            int qEnd   = startFrame + (span * (q + 1)) / 4;
            int f, applies = 0;
            for (f = qStart; f < qEnd; f++) {
                applies += appliedByFrame[f];
            }
            UT_ASSERT_MSG(applies > 0,
                          "delay %ums: no real apply in quarter %d of the "
                          "post-recovery window (frames %d..%d)", delayMs, q,
                          qStart, qEnd - 1);
        }
    }

    /* Not every arrival dies stale any more. While the slot is locked out
     * every input that reaches the sim is dropped as stale; once the rebase
     * lets one through, a share of them is applied instead, so over the back
     * half of the post-recovery window the stale drops are strictly fewer
     * than the inputs delivered. The rate itself does not fall over the run:
     * each recovery is the same refill cycle as the one before it. */
    {
        int startFrame = firstApplyFrame + 1;
        int span = SLR_WINDOW_FRAMES - startFrame;
        int f, halfStart = startFrame + span / 2;
        uint32_t stale = 0, delivered = 0;
        for (f = halfStart; f < SLR_WINDOW_FRAMES; f++) {
            stale += staleByFrame[f];
            delivered += deliveredByFrame[f];
        }
        UT_ASSERT_MSG(delivered > 0,
                      "delay %ums: no input reached the sim in the back half "
                      "of the window", delayMs);
        UT_ASSERT_MSG(stale < delivered,
                      "delay %ums: %u of %u delivered inputs died stale in "
                      "the back half of the window — every arrival is still "
                      "being dropped", delayMs, stale, delivered);
    }

    serverSimDestroy(sim);
    return 0;
}

/* A slot stall-advanced past everything the client has produced recovers a
 * real input stream at every one-way delay. */
int run_stall_lockout_rebase(void) {
    static const uint32_t delaysMs[] = { 0, 40, 60, 100 };
    size_t i;
    for (i = 0; i < sizeof(delaysMs) / sizeof(delaysMs[0]); i++) {
        int rc = slr_run_delay(delaysMs[i]);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/* Input redundancy resends one tick in several packets. The slot rebases
 * onto that tick once — the copies behind it are duplicates of an applied
 * tick, not a second reason to move the server back — and the one-shot the
 * tick carries executes exactly once. */
int run_stall_lockout_rebase_once_per_tick(void) {
    ServerSim *sim = ut_make_running_sim("RebaseOnce");
    GameSim *gs;
    uint32_t lpiAfterBurst, n, staleBefore;
    BYTE minesBefore, minesAfter;
    int frame;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL && gs->tanks[SLR_SLOT] != NULL);
    tankSetMines(gs, &gs->tanks[SLR_SLOT], 40);

    slr_establish(sim);
    UT_ASSERT_MSG(sim->inputBufferFilled[SLR_SLOT], "stream not established");
    UT_ASSERT_MSG(sim->lastProcessedInput[SLR_SLOT] == 12,
                  "establish left lastProcessedInput at %u, expected 12",
                  sim->lastProcessedInput[SLR_SLOT]);
    UT_ASSERT_MSG(sim->newestInputTick[SLR_SLOT] == 12,
                  "establish left newestInputTick at %u, expected 12",
                  sim->newestInputTick[SLR_SLOT]);

    for (frame = 0; frame < SLR_HITCH_FRAMES; frame++) {
        serverSimTick(sim);
    }
    lpiAfterBurst = sim->lastProcessedInput[SLR_SLOT];
    UT_ASSERT_MSG(lpiAfterBurst > 16,
                  "the hitch did not stall-advance the slot far enough "
                  "(lastProcessedInput %u)", lpiAfterBurst);

    /* A stale-but-newest tick: past anything the slot has received, at or
     * below what it has processed. Even, so the one-shot it carries runs on
     * a game tick. */
    n = lpiAfterBurst - 4;
    if ((n % 2) != 0) {
        n--;
    }
    UT_ASSERT_MSG(n > sim->newestInputTick[SLR_SLOT] &&
                      n <= sim->lastProcessedInput[SLR_SLOT],
                  "chosen tick %u is not stale-but-newest (newest %u, "
                  "processed %u)", n, sim->newestInputTick[SLR_SLOT],
                  sim->lastProcessedInput[SLR_SLOT]);

    /* Three copies of one tick, the way input redundancy resends it. Intake
     * admits the first because it is strictly newer than anything the slot
     * has received, and refuses that arm to the copies behind it, which is
     * what keeps a resend from queueing without limit. serverSimApplyInput
     * itself filters nothing, so all three enqueue here, and none of them
     * moves lastProcessedInput: that happens at the tick boundary, on the
     * newest queued tick. */
    UT_ASSERT_MSG(serverSimInputWouldRebase(sim, SLR_SLOT, n),
                  "intake should admit stale tick %u", n);
    slr_feed(sim, n, SLR_CLIENT_BUTTONS, INPUT_ACTION_LAY_MINE);
    UT_ASSERT_MSG(sim->newestInputTick[SLR_SLOT] == n,
                  "newestInputTick %u, expected %u",
                  sim->newestInputTick[SLR_SLOT], n);
    UT_ASSERT_MSG(!serverSimInputWouldRebase(sim, SLR_SLOT, n),
                  "tick %u was admitted a second time on the stale arm", n);
    slr_feed(sim, n, SLR_CLIENT_BUTTONS, INPUT_ACTION_LAY_MINE);
    slr_feed(sim, n, SLR_CLIENT_BUTTONS, INPUT_ACTION_LAY_MINE);
    UT_ASSERT_MSG(sim->lastProcessedInput[SLR_SLOT] == lpiAfterBurst,
                  "arrival moved lastProcessedInput to %u — the rebase belongs "
                  "to the tick boundary", sim->lastProcessedInput[SLR_SLOT]);
    UT_ASSERT_MSG(sim->newestInputTick[SLR_SLOT] == n,
                  "the redundant copies moved newestInputTick to %u, expected "
                  "%u", sim->newestInputTick[SLR_SLOT], n);

    /* One tick: the boundary rebase takes lastProcessedInput to n - 1, the
     * first copy applies for real and lays its mine, and the two behind it
     * drop as stale duplicates of an executed one-shot. */
    minesBefore = tankGetMines(&gs->tanks[SLR_SLOT]);
    staleBefore = sim->statDroppedStaleInputs[SLR_SLOT];
    serverSimTick(sim);
    UT_ASSERT_MSG(sim->lastProcessedInput[SLR_SLOT] == n,
                  "lastProcessedInput ended at %u, expected the rebased tick "
                  "%u", sim->lastProcessedInput[SLR_SLOT], n);
    UT_ASSERT_MSG(sim->statDroppedStaleInputs[SLR_SLOT] - staleBefore == 2,
                  "expected the two duplicate copies to drop as stale, got %u "
                  "drops", sim->statDroppedStaleInputs[SLR_SLOT] - staleBefore);
    UT_ASSERT_MSG(sim->newestInputTick[SLR_SLOT] == n,
                  "newestInputTick moved to %u across the tick, expected %u",
                  sim->newestInputTick[SLR_SLOT], n);
    UT_ASSERT_MSG(sim->lastActionAppliedTick[SLR_SLOT] == n,
                  "the one-shot marker sits at %u, expected the rebased tick "
                  "%u", sim->lastActionAppliedTick[SLR_SLOT], n);
    UT_ASSERT_MSG(!(sim->pendingHarvestActions[SLR_SLOT] &
                    INPUT_ACTION_LAY_MINE),
                  "a duplicate of an executed one-shot must not be harvested");
    minesAfter = tankGetMines(&gs->tanks[SLR_SLOT]);
    UT_ASSERT_MSG((BYTE)(minesBefore - minesAfter) <= 1,
                  "the mine on the rebased tick was laid more than once "
                  "(mines %u -> %u)", minesBefore, minesAfter);

    serverSimDestroy(sim);
    return 0;
}

/* A client's recovery jump is not gap-filled.
 *
 * A client whose counter fell behind numbers its next input the server's
 * last-processed tick plus the round trip plus
 * CLIENT_INPUT_JUMP_MARGIN_HALFSTEPS (client_snapshot.c). The server keeps
 * substituting while that packet is in flight, so what is left of the jump
 * when it is applied is the margin. A gap inside the apply's fill window
 * (`gap < 8` in serverSimApplyOneInput) is filled a half-step at a time
 * with the buttons held before the hitch - movement the client never
 * predicted, and a position correction on every jump. The margin is held
 * above that window so the jump is applied as the jump it is.
 *
 * This feeds the shape the jump makes - the margin above the tick the
 * substitutes reached - and pins statGapFillTicks at zero across the apply,
 * then pins that a real hole in a running stream still fills. The second
 * half is what says the margin was raised rather than the fill removed:
 * every producer that supplies one input per two half-steps depends on it.
 */
int run_stall_recovery_no_gap_fill(void) {
    ServerSim *sim = ut_make_running_sim("NoGapFill");
    uint32_t lpiAfterBurst, jumpTick, gapBefore, gapAfter;
    uint32_t lossTick;
    int frame, i;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    slr_establish(sim);
    UT_ASSERT_MSG(sim->inputBufferFilled[SLR_SLOT], "stream not established");

    /* The hitch, as the other cases stage it. */
    for (frame = 0; frame < SLR_HITCH_FRAMES; frame++) {
        serverSimTick(sim);
    }
    lpiAfterBurst = sim->lastProcessedInput[SLR_SLOT];
    UT_ASSERT_MSG(sim->inputDryTicks[SLR_SLOT] > STALL_ADVANCE_DRY_TICKS,
                  "the hitch left the slot out of a stall-advance run "
                  "(inputDryTicks %u)", sim->inputDryTicks[SLR_SLOT]);

    /* The jump: the first input lands CLIENT_INPUT_JUMP_MARGIN_HALFSTEPS
     * above the tick the server has reached, which is what the margin leaves
     * once the flight time has been substituted through. The ones behind it
     * are the rest of the client's stream, and they are what fills the
     * jitter buffer so the first one is dequeued on the next tick rather
     * than after the substitutes have walked past it. */
    jumpTick = lpiAfterBurst + CLIENT_INPUT_JUMP_MARGIN_HALFSTEPS;
    for (i = 0; i < JITTER_BUFFER_MAX + 2; i++) {
        slr_feed(sim, jumpTick + (uint32_t)i, SLR_CLIENT_BUTTONS, 0);
    }

    gapBefore = sim->statGapFillTicks[SLR_SLOT];
    serverSimTick(sim);
    gapAfter = sim->statGapFillTicks[SLR_SLOT];

    UT_ASSERT_MSG(sim->lastProcessedInput[SLR_SLOT] >= jumpTick,
                  "the jumped input was not applied: lastProcessedInput %u, "
                  "jump tick %u", sim->lastProcessedInput[SLR_SLOT], jumpTick);
    UT_ASSERT_MSG(gapAfter == gapBefore,
                  "the jumped input gap-filled %u half-step(s) - "
                  "CLIENT_INPUT_JUMP_MARGIN_HALFSTEPS (%d) has to stay above "
                  "the apply's fill window, or every recovery jump runs the "
                  "margin with the pre-hitch held buttons the client never "
                  "predicted", gapAfter - gapBefore,
                  CLIENT_INPUT_JUMP_MARGIN_HALFSTEPS);

    /* And the other half: with the stream running again, a hole left by a
     * dropped packet is still filled. Drain what is left of the burst first
     * - an entry still queued is contiguous with the last applied one, so
     * the hole has to be the next thing the dequeue sees - and feed it
     * straight away, before a dry run past STALL_ADVANCE_DRY_TICKS starts
     * substituting again. */
    for (i = 0; i < 8; i++) {
        if (sim->inputQueueHead[SLR_SLOT] == sim->inputQueueTail[SLR_SLOT]) {
            break;
        }
        serverSimTick(sim);
    }
    UT_ASSERT_MSG(sim->inputQueueHead[SLR_SLOT] ==
                      sim->inputQueueTail[SLR_SLOT],
                  "the fed burst had not drained after %d ticks", i);
    UT_ASSERT_MSG(sim->inputDryTicks[SLR_SLOT] <= STALL_ADVANCE_DRY_TICKS,
                  "the drain left the slot substituting again (inputDryTicks "
                  "%u)", sim->inputDryTicks[SLR_SLOT]);
    lossTick = sim->lastProcessedInput[SLR_SLOT] + 3;
    for (i = 0; i < JITTER_BUFFER_MAX + 2; i++) {
        slr_feed(sim, lossTick + (uint32_t)i, SLR_CLIENT_BUTTONS, 0);
    }
    gapBefore = sim->statGapFillTicks[SLR_SLOT];
    serverSimTick(sim);
    gapAfter = sim->statGapFillTicks[SLR_SLOT];
    UT_ASSERT_MSG(sim->lastProcessedInput[SLR_SLOT] >= lossTick,
                  "the input past the hole was not applied: lastProcessedInput "
                  "%u, tick %u", sim->lastProcessedInput[SLR_SLOT], lossTick);
    UT_ASSERT_MSG(gapAfter > gapBefore,
                  "a real two-tick hole in a running stream was not gap-filled "
                  "- the fill is what an in-process producer's under-supply "
                  "needs, and only the jump is meant to land outside it");

    serverSimDestroy(sim);
    return 0;
}
