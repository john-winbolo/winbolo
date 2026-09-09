/*
 * Concurrent-talker selection (src/bolo/voice_talker_select.c). Two halves:
 *
 *   - the ranking, which is the whole of the decision the server's voice
 *     fan-out makes per recipient: when more people are talking than a
 *     listener can follow, the ones kept are those who started most
 *     recently.
 *   - the bounds, since the caller's candidate count is whatever a tick of
 *     arrivals produced and the output buffer is only as long as the cap.
 *
 * Driven directly rather than over the wire: the input is (onset, seq) per
 * sender and the output is a chosen set, so a server, a socket and five
 * simultaneous clients would all be scaffolding around a pure function. The
 * jitter buffer is tested the same way and for the same reason.
 */
#include <string.h>

#include "voice_talker_select.h"
#include "global.h"
#include "test_harness.h"

#define OUT_SENTINEL 0xEE

/* Out buffer larger than any cap under test, pre-filled, so a write past
 * maxTalkers is visible rather than silently in bounds. */
typedef struct {
    uint8_t slot[VOICE_TALKER_SELECT_MAX + 4];
} OutBuf;

static void outReset(OutBuf *o) {
    memset(o->slot, OUT_SENTINEL, sizeof(o->slot));
}

/* True when the chosen slots are exactly `want`, in exactly that order. */
static bool chosenIs(const OutBuf *o, int n, const uint8_t *want, int wantN) {
    int i;
    if (n != wantN) return false;
    for (i = 0; i < n; i++) {
        if (o->slot[i] != want[i]) return false;
    }
    return true;
}

static bool chosenHas(const OutBuf *o, int n, uint8_t slot) {
    int i;
    for (i = 0; i < n; i++) {
        if (o->slot[i] == slot) return true;
    }
    return false;
}

/* Nothing past the cap was touched. */
static bool tailIntact(const OutBuf *o, int maxTalkers) {
    int i;
    for (i = maxTalkers; i < (int)sizeof(o->slot); i++) {
        if (o->slot[i] != OUT_SENTINEL) return false;
    }
    return true;
}

int run_voice_talker_select_ranks_recent(void) {
    VoiceTalkerCandidate cands[8];
    OutBuf out;
    int n;

    /* --- six talkers, cap of four: the four most recent onsets ---
     *
     * Slot order and onset order deliberately disagree — the newest onsets
     * are the highest-numbered slots — so an implementation that keeps the
     * first four by slot index fails here instead of passing by accident. */
    {
        static const uint8_t want[] = { 5, 4, 3, 2 };
        int i;
        for (i = 0; i < 6; i++) {
            cands[i].slot      = (uint8_t)i;
            cands[i].onsetTick = (uint32_t)(100 + i);
            cands[i].newestSeq = 10;
        }
        outReset(&out);
        n = voiceSelectTalkers(cands, 6, 4, out.slot);
        UT_ASSERT_MSG(n == 4, "cap of 4 over 6 talkers chose %d", n);
        UT_ASSERT_MSG(chosenIs(&out, n, want, 4),
                      "expected the newest four (5,4,3,2), got %d,%d,%d,%d",
                      out.slot[0], out.slot[1], out.slot[2], out.slot[3]);
        /* The two longest-running talkers are the ones dropped. */
        UT_ASSERT(!chosenHas(&out, n, 0));
        UT_ASSERT(!chosenHas(&out, n, 1));
        UT_ASSERT_MSG(tailIntact(&out, 4), "wrote past maxTalkers");
    }

    /* --- the same, with the candidates offered in onset order ---
     * Rank must not depend on the order they arrive in. */
    {
        static const uint8_t want[] = { 5, 4, 3, 2 };
        int i;
        for (i = 0; i < 6; i++) {
            cands[i].slot      = (uint8_t)(5 - i);
            cands[i].onsetTick = (uint32_t)(105 - i);
            cands[i].newestSeq = 10;
        }
        outReset(&out);
        n = voiceSelectTalkers(cands, 6, 4, out.slot);
        UT_ASSERT(n == 4);
        UT_ASSERT_MSG(chosenIs(&out, n, want, 4),
                      "arrival order changed the ranking: %d,%d,%d,%d",
                      out.slot[0], out.slot[1], out.slot[2], out.slot[3]);
    }

    /* --- a tie on onset breaks by the newer sequence number, across the
     * 256 wrap. All four sit inside one 21-wide window that straddles the
     * wrap, so 250 < 251 < 3 < 5 in arrival order; a raw byte compare ranks
     * the two that wrapped last instead of first and keeps the wrong pair. */
    {
        cands[0].slot = 0; cands[0].onsetTick = 500; cands[0].newestSeq = 250;
        cands[1].slot = 1; cands[1].onsetTick = 500; cands[1].newestSeq = 251;
        cands[2].slot = 2; cands[2].onsetTick = 500; cands[2].newestSeq = 3;
        cands[3].slot = 3; cands[3].onsetTick = 500; cands[3].newestSeq = 5;

        outReset(&out);
        n = voiceSelectTalkers(cands, 4, 2, out.slot);
        UT_ASSERT_MSG(n == 2, "cap of 2 chose %d", n);
        UT_ASSERT_MSG(out.slot[0] == 3,
                      "seq 5 must rank first across the wrap; got slot %d",
                      out.slot[0]);
        UT_ASSERT_MSG(out.slot[1] == 2,
                      "seq 3 must rank second across the wrap; got slot %d",
                      out.slot[1]);
        /* The pre-wrap pair is what a raw byte compare would have kept. */
        UT_ASSERT(!chosenHas(&out, n, 0));
        UT_ASSERT(!chosenHas(&out, n, 1));
        UT_ASSERT(tailIntact(&out, 2));
    }

    /* --- onset outranks sequence: an older utterance with a much newer seq
     * still loses to one that started later. --- */
    {
        cands[0].slot = 0; cands[0].onsetTick = 900; cands[0].newestSeq = 1;
        cands[1].slot = 1; cands[1].onsetTick = 400; cands[1].newestSeq = 250;

        outReset(&out);
        n = voiceSelectTalkers(cands, 2, 1, out.slot);
        UT_ASSERT(n == 1);
        UT_ASSERT_MSG(out.slot[0] == 0,
                      "onset must outrank seq; kept slot %d", out.slot[0]);
    }

    /* --- tied on both keys: the lower slot wins, so the result is stable
     * rather than dependent on where the tie landed in the input. --- */
    {
        uint8_t firstRun;
        cands[0].slot = 7; cands[0].onsetTick = 42; cands[0].newestSeq = 9;
        cands[1].slot = 3; cands[1].onsetTick = 42; cands[1].newestSeq = 9;

        outReset(&out);
        n = voiceSelectTalkers(cands, 2, 1, out.slot);
        UT_ASSERT(n == 1);
        firstRun = out.slot[0];
        UT_ASSERT_MSG(firstRun == 3, "tie must break to the lower slot, got %d",
                      firstRun);

        /* Offered the other way round, the same one is chosen. */
        cands[0].slot = 3;
        cands[1].slot = 7;
        outReset(&out);
        n = voiceSelectTalkers(cands, 2, 1, out.slot);
        UT_ASSERT(n == 1);
        UT_ASSERT_MSG(out.slot[0] == firstRun,
                      "tie broke differently on reordered input: %d vs %d",
                      out.slot[0], firstRun);
    }

    return 0;
}

int run_voice_talker_select_bounds(void) {
    VoiceTalkerCandidate cands[VOICE_TALKER_SELECT_MAX];
    OutBuf out;
    int n;
    int i;

    for (i = 0; i < VOICE_TALKER_SELECT_MAX; i++) {
        cands[i].slot      = (uint8_t)i;
        cands[i].onsetTick = (uint32_t)(1000 + i);
        cands[i].newestSeq = (uint8_t)i;
    }

    /* --- at or below the cap nothing is dropped: every candidate is
     * chosen, which is the case that must not misfire in an ordinary
     * two-person conversation. --- */
    {
        int c;
        for (c = 1; c <= 4; c++) {
            int k;
            outReset(&out);
            n = voiceSelectTalkers(cands, c, 4, out.slot);
            UT_ASSERT_MSG(n == c, "%d candidates under a cap of 4 chose %d",
                          c, n);
            for (k = 0; k < c; k++) {
                UT_ASSERT_MSG(chosenHas(&out, n, cands[k].slot),
                              "candidate slot %d dropped below the cap",
                              cands[k].slot);
            }
            UT_ASSERT(tailIntact(&out, c));
        }
    }

    /* --- exactly the cap --- */
    outReset(&out);
    n = voiceSelectTalkers(cands, 4, 4, out.slot);
    UT_ASSERT_MSG(n == 4, "4 candidates under a cap of 4 chose %d", n);
    UT_ASSERT(tailIntact(&out, 4));

    /* --- one over --- */
    outReset(&out);
    n = voiceSelectTalkers(cands, 5, 4, out.slot);
    UT_ASSERT_MSG(n == 4, "5 candidates under a cap of 4 chose %d", n);
    /* Slot 0 has the oldest onset, so it is the one that falls off. */
    UT_ASSERT(!chosenHas(&out, n, 0));
    UT_ASSERT(tailIntact(&out, 4));

    /* --- a full field of candidates, capped --- */
    outReset(&out);
    n = voiceSelectTalkers(cands, VOICE_TALKER_SELECT_MAX, 4, out.slot);
    UT_ASSERT(n == 4);
    UT_ASSERT(out.slot[0] == VOICE_TALKER_SELECT_MAX - 1);
    UT_ASSERT(tailIntact(&out, 4));

    /* --- a cap wider than the module's own bound is clamped, never a write
     * past the candidates it was given. --- */
    outReset(&out);
    n = voiceSelectTalkers(cands, VOICE_TALKER_SELECT_MAX,
                           VOICE_TALKER_SELECT_MAX + 4, out.slot);
    UT_ASSERT_MSG(n == VOICE_TALKER_SELECT_MAX, "clamped cap chose %d", n);
    UT_ASSERT(tailIntact(&out, VOICE_TALKER_SELECT_MAX));

    /* --- degenerate inputs write nothing and choose nothing --- */
    outReset(&out);
    UT_ASSERT_MSG(voiceSelectTalkers(cands, 0, 4, out.slot) == 0,
                  "no candidates must choose none");
    UT_ASSERT_MSG(tailIntact(&out, 0), "zero candidates wrote output");

    outReset(&out);
    UT_ASSERT_MSG(voiceSelectTalkers(cands, 4, 0, out.slot) == 0,
                  "a cap of zero must choose none");
    UT_ASSERT_MSG(tailIntact(&out, 0), "a cap of zero wrote output");

    outReset(&out);
    UT_ASSERT(voiceSelectTalkers(cands, -1, 4, out.slot) == 0);
    UT_ASSERT(voiceSelectTalkers(cands, 4, -1, out.slot) == 0);
    UT_ASSERT(tailIntact(&out, 0));

    /* --- NULL arguments are refused, not dereferenced --- */
    outReset(&out);
    UT_ASSERT(voiceSelectTalkers(NULL, 4, 4, out.slot) == 0);
    UT_ASSERT(voiceSelectTalkers(cands, 4, 4, NULL) == 0);
    UT_ASSERT(tailIntact(&out, 0));

    return 0;
}
