/*
 * Hitch diagnostics: a catch-up burst leaves one line in the log.
 *
 * serverGameTimer runs as many ticks as the wall clock owes, uncapped, so a
 * stall on the tick thread — a synchronous WinBolo.net post, a slow round-log
 * upload — is paid back as a burst of back-to-back ticks. That burst is what
 * pushes lastProcessedInput past everything the clients have produced and
 * locks a slot out. The rebase fixes the lockout; this line is how a hitch
 * that is still happening gets read off a field log rather than guessed at.
 *
 * serverTickCatchUp is the loop, lifted out of serverGameTimer so it can be
 * driven without a sim: it takes the wall clock, the two counters by pointer,
 * and one step callback. The callback is where the shutdown flag is answered
 * — returning false stops the burst before the step runs, which is exactly
 * what the `if (shutting down) break;` at the top of the old loop did.
 *
 * What these cases pin:
 *   - a 2 s debt runs every tick it owes and logs once, with the debt and the
 *     tick count in the line;
 *   - a two-tick debt logs nothing, so the diagnostic cannot add a line to the
 *     normal cadence;
 *   - a step that refuses stops the burst there, with neither counter moved
 *     for a tick that did not run.
 *
 * The debt arithmetic: a tick is owed while (nowMs - oldTick) is STRICTLY
 * greater than SERVER_TICK_LENGTH, so a burst always stops with one tick
 * length still on the clock. A debt of N tick lengths therefore runs N-1
 * ticks, not N. That is the production condition unchanged; the expected
 * counts below are derived from it rather than rounded off.
 *
 * The assertions on the warning look for the numbers inside the captured
 * text, not for a whole format string, so rewording the line does not break
 * the case.
 *
 * Log capture is SDL_SetLogOutputFunction, the same seam wb_log.c installs
 * its file sink through. The previous sink is saved and restored around each
 * part, so a case that fails still leaves the binary's logging as it found
 * it. test_main.c has already opened every category to WARN, so the line
 * reaches the sink.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_lifecycle.h"
#include "test_harness.h"
#include "../../src/common/wb_log.h"

/* A 2 s hitch, the shape the plan's field reports describe. */
#define HL_HITCH_DEBT_MS 2000u
/* Two ticks of debt is three tick lengths on the clock — see the header. */
#define HL_QUIET_DEBT_MS (3u * SERVER_TICK_LENGTH)

/* Ticks a debt of debtMs owes, given the strictly-greater-than condition. */
static uint32_t hlExpectedTicks(uint32_t debtMs) {
    if (debtMs <= SERVER_TICK_LENGTH) {
        return 0;
    }
    return (debtMs - 1u) / SERVER_TICK_LENGTH;
}

/* ---------------------------------------------------------------- */
/* Log capture                                                       */
/* ---------------------------------------------------------------- */

typedef struct {
    int  warnings;                /* WARN lines seen on WB_LOG_CAT_NET */
    char lastWarning[512];        /* the most recent of them */
} HlCapture;

static HlCapture           s_cap;
static SDL_LogOutputFunction s_prevSink;
static void               *s_prevSinkData;

static void SDLCALL hlSink(void *userdata, int category,
                           SDL_LogPriority priority, const char *message) {
    (void)userdata;
    if (category == WB_LOG_CAT_NET && priority == SDL_LOG_PRIORITY_WARN) {
        s_cap.warnings++;
        if (message != NULL) {
            SDL_strlcpy(s_cap.lastWarning, message, sizeof(s_cap.lastWarning));
        }
    }
}

static void hlCaptureBegin(void) {
    memset(&s_cap, 0, sizeof(s_cap));
    SDL_GetLogOutputFunction(&s_prevSink, &s_prevSinkData);
    SDL_SetLogOutputFunction(hlSink, NULL);
}

static void hlCaptureEnd(void) {
    SDL_SetLogOutputFunction(s_prevSink, s_prevSinkData);
}

/* ---------------------------------------------------------------- */
/* Steps                                                             */
/* ---------------------------------------------------------------- */

typedef struct {
    uint32_t calls;      /* how many times the step was entered */
    uint32_t refuseAt;   /* return false on this call number; 0 = never */
} HlStepCtx;

static bool hlStep(void *ctx) {
    HlStepCtx *st = (HlStepCtx *)ctx;
    st->calls++;
    if (st->refuseAt != 0 && st->calls == st->refuseAt) {
        /* The shutdown handshake's shape: refuse before doing the work, so
         * the caller must not count this call as a tick. */
        st->calls--;
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------- */

int run_hitch_logged(void) {
    /* Somewhere well clear of zero so the unsigned subtraction the loop does
     * is an ordinary one in every part below. */
    const uint32_t nowMs = 1000000u;

    /* -------- the hitch -------- */
    {
        const uint32_t expected = hlExpectedTicks(HL_HITCH_DEBT_MS);
        uint32_t oldTick = nowMs - HL_HITCH_DEBT_MS;
        uint32_t ticks   = 0;
        uint32_t ran;
        HlStepCtx st = { 0, 0 };
        char needle[32];

        UT_ASSERT_MSG(expected > SERVER_HITCH_WARN_TICKS,
                      "a %u ms debt must be over the warn threshold",
                      (unsigned)HL_HITCH_DEBT_MS);

        hlCaptureBegin();
        ran = serverTickCatchUp(nowMs, &oldTick, &ticks, hlStep, &st);
        hlCaptureEnd();

        if (st.calls != expected) {
            UT_FAIL("hitch: step ran %u times, expected %u",
                    (unsigned)st.calls, (unsigned)expected);
        }
        if (ran != expected) {
            UT_FAIL("hitch: returned %u, expected %u",
                    (unsigned)ran, (unsigned)expected);
        }
        if (oldTick != (nowMs - HL_HITCH_DEBT_MS) + expected * SERVER_TICK_LENGTH) {
            UT_FAIL("hitch: oldTick advanced by %u ms, expected %u",
                    (unsigned)(oldTick - (nowMs - HL_HITCH_DEBT_MS)),
                    (unsigned)(expected * SERVER_TICK_LENGTH));
        }
        if (ticks != expected) {
            UT_FAIL("hitch: ticks advanced by %u, expected %u",
                    (unsigned)ticks, (unsigned)expected);
        }
        /* One tick length is still owed — the loop's condition is strict. */
        if ((nowMs - oldTick) != SERVER_TICK_LENGTH) {
            UT_FAIL("hitch: %u ms left on the clock, expected %u",
                    (unsigned)(nowMs - oldTick), (unsigned)SERVER_TICK_LENGTH);
        }

        if (s_cap.warnings != 1) {
            UT_FAIL("hitch: %d warnings captured, expected exactly 1",
                    s_cap.warnings);
        }
        snprintf(needle, sizeof(needle), "%u", (unsigned)HL_HITCH_DEBT_MS);
        if (strstr(s_cap.lastWarning, needle) == NULL) {
            UT_FAIL("hitch: warning does not carry the debt (%s): '%s'",
                    needle, s_cap.lastWarning);
        }
        snprintf(needle, sizeof(needle), "%u", (unsigned)expected);
        if (strstr(s_cap.lastWarning, needle) == NULL) {
            UT_FAIL("hitch: warning does not carry the tick count (%s): '%s'",
                    needle, s_cap.lastWarning);
        }
    }

    /* -------- the quiet case -------- */
    {
        const uint32_t expected = hlExpectedTicks(HL_QUIET_DEBT_MS);
        uint32_t oldTick = nowMs - HL_QUIET_DEBT_MS;
        uint32_t ticks   = 0;
        uint32_t ran;
        HlStepCtx st = { 0, 0 };

        UT_ASSERT_MSG(expected == 2u,
                      "the quiet case must owe two ticks, not %u",
                      (unsigned)expected);

        hlCaptureBegin();
        ran = serverTickCatchUp(nowMs, &oldTick, &ticks, hlStep, &st);
        hlCaptureEnd();

        if (st.calls != 2u || ran != 2u || ticks != 2u) {
            UT_FAIL("quiet: step ran %u, returned %u, ticks %u; expected 2/2/2",
                    (unsigned)st.calls, (unsigned)ran, (unsigned)ticks);
        }
        if (s_cap.warnings != 0) {
            UT_FAIL("quiet: %d warnings captured, expected none — a normal "
                    "cadence must not log: '%s'",
                    s_cap.warnings, s_cap.lastWarning);
        }
    }

    /* -------- the step that refuses -------- */
    {
        const uint32_t startTick = nowMs - HL_HITCH_DEBT_MS;
        uint32_t oldTick = startTick;
        uint32_t ticks   = 0;
        uint32_t ran;
        /* Two ticks run, the third is refused the way the shutdown flag
         * refuses one. */
        HlStepCtx st = { 0, 3u };

        hlCaptureBegin();
        ran = serverTickCatchUp(nowMs, &oldTick, &ticks, hlStep, &st);
        hlCaptureEnd();

        if (ran != 2u || ticks != 2u) {
            UT_FAIL("early stop: returned %u, ticks %u; expected 2/2",
                    (unsigned)ran, (unsigned)ticks);
        }
        if (oldTick != startTick + 2u * SERVER_TICK_LENGTH) {
            UT_FAIL("early stop: oldTick advanced by %u ms, expected %u",
                    (unsigned)(oldTick - startTick),
                    (unsigned)(2u * SERVER_TICK_LENGTH));
        }
        /* The burst it would have run without the refusal is well over the
         * threshold, so this also pins that the line counts ticks run and
         * not the debt it was handed. */
        if (s_cap.warnings != 0) {
            UT_FAIL("early stop: %d warnings captured, expected none: '%s'",
                    s_cap.warnings, s_cap.lastWarning);
        }
    }

    return 0;
}
