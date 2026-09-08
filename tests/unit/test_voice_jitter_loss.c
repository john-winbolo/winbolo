/*
 * Jitter buffer (src/client_frontend/voice_core.c) under adverse arrival.
 *
 * test_voice_jitter.c pins the ordering rules against hand-placed frames.
 * This one drives the same buffer with a generated arrival pattern — loss,
 * reorder, and a burst outage — and measures what comes out.
 *
 * Arrival is expressed in ticks rather than in milliseconds: jitter shows up
 * as how many pushes happen between pops. So a tick here is "push whatever
 * landed, then pop once", which is what the playback caller does. A tick is
 * one frame time, and it waits the playout deadline out before accepting
 * that there is nothing to play — nothing can land inside that wait, because
 * arrivals only happen on tick boundaries. The deadline itself is covered by
 * the hand-placed cases at the end of this file.
 *
 * Real Opus frames are used, cycled from a small pool, so decode genuinely
 * succeeds and a concealed frame is distinguishable from a played one. The
 * sequence numbers are the point, not the audio.
 *
 * The buffer reports only a bool from pop, so which sequence number each pop
 * produced is derived from the documented contract: playback starts at the
 * oldest frame held, advances one per pop, and a run of VOICE_JITTER_MAX_PLC
 * concealed frames ends the utterance and drops whatever was buffered. Every
 * derivation is cross-checked against the counters — if the buffer plays a
 * frame the model did not think it held, or conceals one it did, the test
 * fails rather than reporting a wrong number.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bolo_rand.h"
#include "voice_core.h"
#include "test_harness.h"

/* Fixed so every run is the same run. Each scenario draws from JL_SEED + row
 * so one scenario's draws cannot shift another's. */
#define JL_SEED        20260801u

#define JL_TONE_HZ        440.0
#define JL_TONE_AMPLITUDE 0.3
#define JL_POOL_FRAMES    8
#define JL_MAX_FRAMES     800
/* Ticks run on past the last arrival so the buffer plays out what it holds. */
#define JL_DRAIN_TICKS    24
/* A tick is one frame of playout. */
#define JL_TICK_MS        20

typedef struct {
    uint8_t data[VOICE_MAX_PACKET];
    int     len;
} JlEncoded;

typedef struct {
    bool arrives;       /* the schedule delivers it at all */
    int  arrivalTick;   /* tick it lands, if it arrives    */
    int  playTick;      /* tick it was decoded, else -1    */
    bool played;
} JlFrame;

typedef struct {
    /* what was asked for */
    int frames;
    int lossPct;
    int reorderDepth;
    /* what the schedule did */
    int lostScheduled;      /* frames never delivered                     */
    int arrivedAccepted;    /* pushed and taken by the buffer             */
    /* what came out */
    int pops;               /* pops that returned true                    */
    int falsePops;          /* pops with nothing to play                  */
    int concealedInRange;   /* concealed slots of frames that were sent   */
    int playedFrames;       /* frames the model saw decoded               */
    int firstPlayedSeq;     /* where playback started, -1 if it never did */
    int maxAddedLatency;    /* max playTick - arrivalTick over played     */
    int primeRuns;          /* times playback started or restarted        */
    int firstPrimeTick;
    int lastPrimeTick;
    int discardedOnUnprime; /* buffered frames thrown away when playback
                             * ended — never played, never counted late   */
    int heldAtEnd;          /* still buffered when the ticks ran out      */
    int shadowMismatch;     /* buffer disagreed with the model            */
    VoiceSpeakerStats st;
} JlResult;

/* Scratch, file-scope to keep the per-scenario stack small. */
static JlFrame jlFrames[JL_MAX_FRAMES];
static bool    jlHeld[JL_MAX_FRAMES];

static int jlEncodePool(JlEncoded *pool, int count) {
    VoiceEncoder *enc;
    int16_t pcm[VOICE_FRAME_SAMPLES];
    double phase = 0.0;
    const double phaseStep = 2.0 * 3.14159265358979323846 * JL_TONE_HZ /
                             (double)VOICE_SAMPLE_RATE;
    int f, i;

    enc = voiceEncoderCreate(VOICE_DEFAULT_BITRATE, VOICE_DEFAULT_COMPLEXITY);
    if (enc == NULL) {
        return 0;
    }
    for (f = 0; f < count; f++) {
        for (i = 0; i < VOICE_FRAME_SAMPLES; i++) {
            pcm[i] = (int16_t)(sin(phase) * JL_TONE_AMPLITUDE * 32767.0);
            phase += phaseStep;
        }
        pool[f].len = voiceEncoderEncode(enc, pcm, pool[f].data,
                                         (int)sizeof(pool[f].data));
        if (pool[f].len <= 0) {
            voiceEncoderDestroy(enc);
            return 0;
        }
    }
    voiceEncoderDestroy(enc);
    return 1;
}

/* The oldest frame the model believes is buffered, or -1. */
static int jlLowestHeld(int frames) {
    int s;
    for (s = 0; s < frames; s++) {
        if (jlHeld[s]) {
            return s;
        }
    }
    return -1;
}

/* Run one arrival pattern end to end.
 *
 * frames        how many 20 ms frames the talker sends (one per tick)
 * lossPct       each frame is dropped with this probability
 * reorderDepth  a frame lands 0..reorderDepth ticks after it was sent, so
 *               frames can overtake each other
 * blackoutAt    first frame of a burst outage (< 0 for none)
 * blackoutLen   how many consecutive frames the outage swallows
 *
 * Sequence numbers are the frame index modulo 256, so a run longer than 256
 * frames also crosses the wrap.
 */
static void jlRun(const JlEncoded *pool, JlResult *r, uint64_t seed,
                  int frames, int lossPct, int reorderDepth,
                  int blackoutAt, int blackoutLen) {
    VoiceSpeaker *sp;
    int16_t pcm[VOICE_FRAME_SAMPLES];
    VoiceSpeakerStats prev, now;
    bool primed = false;
    int  nextSeq = -1;       /* frame index due next, while primed */
    int  consecConceal = 0;
    int  lastTick;
    int  t, s;
    uint32_t nowMs = 0;

    memset(r, 0, sizeof(*r));
    r->frames = frames;
    r->lossPct = lossPct;
    r->reorderDepth = reorderDepth;
    r->maxAddedLatency = -1;
    r->firstPlayedSeq = -1;
    r->firstPrimeTick = -1;
    r->lastPrimeTick = -1;
    memset(jlHeld, 0, sizeof(jlHeld));

    bolo_srand(seed);
    for (s = 0; s < frames; s++) {
        jlFrames[s].playTick = -1;
        jlFrames[s].played = false;
        if (blackoutLen > 0 && blackoutAt >= 0 &&
            s >= blackoutAt && s < blackoutAt + blackoutLen) {
            jlFrames[s].arrives = false;
        } else if (lossPct > 0 && (int)bolo_rand_below(100) < lossPct) {
            jlFrames[s].arrives = false;
        } else {
            jlFrames[s].arrives = true;
        }
        jlFrames[s].arrivalTick =
            s + (reorderDepth > 0
                     ? (int)bolo_rand_below((uint32_t)reorderDepth + 1)
                     : 0);
        if (!jlFrames[s].arrives) {
            r->lostScheduled++;
        }
    }

    sp = voiceSpeakerCreate();
    if (sp == NULL) {
        r->shadowMismatch = -1;   /* the caller asserts on this */
        return;
    }
    voiceSpeakerGetStats(sp, &prev);

    lastTick = frames + reorderDepth + JL_DRAIN_TICKS;
    for (t = 0; t <= lastTick; t++) {
        /* Everything landing this tick, oldest sequence number first. */
        for (s = 0; s < frames; s++) {
            if (!jlFrames[s].arrives || jlFrames[s].arrivalTick != t) {
                continue;
            }
            voiceSpeakerPush(sp, (uint8_t)s, 0, pool[s % JL_POOL_FRAMES].data,
                             pool[s % JL_POOL_FRAMES].len);
            voiceSpeakerGetStats(sp, &now);
            if (now.lateDropped == prev.lateDropped) {
                /* Taken. An eviction would put the model out of step with
                 * the buffer, which is why every scenario asserts
                 * evicted == 0 rather than trying to model which frame
                 * an eviction chose. */
                jlHeld[s] = true;
                r->arrivedAccepted++;
            }
            prev = now;
        }

        /* The tick's own frame time, then the wait for a frame that is not
         * there: a missing one is only concealed once it is overdue, and
         * nothing more lands until the next tick, so waiting the deadline
         * out here is what the buffer would see. */
        nowMs += JL_TICK_MS;
        if (!voiceSpeakerPop(sp, pcm, nowMs)) {
            nowMs += VOICE_JITTER_LATE_MS;
            if (!voiceSpeakerPop(sp, pcm, nowMs)) {
                r->falsePops++;
                continue;
            }
        }
        voiceSpeakerGetStats(sp, &now);
        r->pops++;

        if (!primed) {
            /* Playback (re)starts at the oldest frame held. */
            nextSeq = jlLowestHeld(frames);
            primed = true;
            consecConceal = 0;
            r->primeRuns++;
            if (r->firstPrimeTick < 0) {
                r->firstPrimeTick = t;
            }
            r->lastPrimeTick = t;
        }

        if (now.played != prev.played) {
            if (nextSeq >= 0 && nextSeq < frames && jlHeld[nextSeq]) {
                int added = t - jlFrames[nextSeq].arrivalTick;
                jlHeld[nextSeq] = false;
                jlFrames[nextSeq].played = true;
                jlFrames[nextSeq].playTick = t;
                r->playedFrames++;
                if (r->firstPlayedSeq < 0) {
                    r->firstPlayedSeq = nextSeq;
                }
                if (added > r->maxAddedLatency) {
                    r->maxAddedLatency = added;
                }
            } else {
                /* Decoded a frame the model did not think was buffered. */
                r->shadowMismatch++;
            }
            consecConceal = 0;
        } else {
            if (nextSeq >= 0 && nextSeq < frames) {
                r->concealedInRange++;
                if (jlHeld[nextSeq]) {
                    /* Concealed a frame it was holding. */
                    r->shadowMismatch++;
                }
            }
            consecConceal++;
            if (consecConceal >= VOICE_JITTER_MAX_PLC) {
                /* The utterance ends here and everything buffered goes with
                 * it — see voiceSpeakerUnprime. Those frames are never
                 * played, never concealed, and never counted late: playback
                 * simply resumes past them. */
                for (s = 0; s < frames; s++) {
                    if (jlHeld[s]) {
                        jlHeld[s] = false;
                        r->discardedOnUnprime++;
                    }
                }
                primed = false;
            }
        }

        nextSeq++;
        prev = now;
    }

    for (s = 0; s < frames; s++) {
        if (jlHeld[s]) {
            r->heldAtEnd++;
        }
    }
    voiceSpeakerGetStats(sp, &r->st);
    voiceSpeakerDestroy(sp);
}

/* Frames the schedule never delivered, from `from` onwards. */
static int jlLostFrom(int from, int frames) {
    int s, n = 0;
    for (s = (from < 0 ? 0 : from); s < frames; s++) {
        if (!jlFrames[s].arrives) {
            n++;
        }
    }
    return n;
}

/* Property 4, applied to every scenario: the model and the buffer agree, the
 * buffer never overflows, no frame is played twice, and every frame is
 * accounted for on both sides of the buffer. `who` names the scenario. */
#define JL_CHECK_ACCOUNTING(r, who)                                          \
    do {                                                                     \
        UT_ASSERT_MSG((r).shadowMismatch == 0,                               \
                      "property 4 (%s): the buffer played or concealed "     \
                      "against the model %d times — a frame was played "     \
                      "twice or came from nowhere", (who),                   \
                      (r).shadowMismatch);                                   \
        UT_ASSERT_MSG((r).st.evicted == 0,                                   \
                      "property 4 (%s): %u frames evicted from a %d slot "   \
                      "buffer", (who), (r).st.evicted, VOICE_JITTER_SLOTS);  \
        UT_ASSERT_MSG((r).playedFrames == (int)(r).st.played,                \
                      "property 4 (%s): model counted %d played, buffer "    \
                      "counted %u", (who), (r).playedFrames,                 \
                      (r).st.played);                                        \
        UT_ASSERT_MSG((r).arrivedAccepted == (int)(r).st.played +            \
                          (r).discardedOnUnprime + (r).heldAtEnd,            \
                      "property 4 (%s): %d frames taken by the buffer but "  \
                      "%u played + %d dropped at end-of-utterance + %d "     \
                      "still held — the rest vanished silently", (who),      \
                      (r).arrivedAccepted, (r).st.played,                    \
                      (r).discardedOnUnprime, (r).heldAtEnd);                \
        UT_ASSERT_MSG((r).frames == (int)(r).st.played +                     \
                          (int)(r).st.lateDropped + (r).lostScheduled +      \
                          (r).discardedOnUnprime + (r).heldAtEnd,            \
                      "property 4 (%s): %d frames sent but %u played + %u "  \
                      "late + %d lost + %d dropped + %d held does not add "  \
                      "up", (who), (r).frames, (r).st.played,                \
                      (r).st.lateDropped, (r).lostScheduled,                 \
                      (r).discardedOnUnprime, (r).heldAtEnd);                \
    } while (0)

int run_voice_jitter_under_loss(void) {
    JlEncoded pool[JL_POOL_FRAMES];
    JlResult r;
    JlResult table[5];
    const char *tableLoss[5]  = { "0%", "0%", "2%", "5%", "10%" };
    const char *tableOrder[5] = { "none", "depth 2", "none", "depth 1",
                                  "depth 2" };
    int i;

    UT_ASSERT(jlEncodePool(pool, JL_POOL_FRAMES));

    /* ---- Property 1: clean arrival conceals nothing ----
     * The regression net for anything that lets pops outrun arrivals. */
    jlRun(pool, &r, JL_SEED + 1, 400, 0, 0, -1, 0);
    JL_CHECK_ACCOUNTING(r, "clean");
    UT_ASSERT_MSG(r.concealedInRange == 0,
                  "property 1: clean arrival concealed %d of %d frames",
                  r.concealedInRange, r.frames);
    UT_ASSERT_MSG(r.st.lateDropped == 0,
                  "property 1: clean arrival late-dropped %u frames",
                  r.st.lateDropped);
    UT_ASSERT_MSG(r.primeRuns == 1,
                  "property 1: playback restarted %d times on a clean link",
                  r.primeRuns);

    /* ---- Property 2: reorder within the buffered depth is absorbed ----
     *
     * The steady-state cushion is VOICE_JITTER_TARGET - 1 frames, not
     * VOICE_JITTER_TARGET: priming triggers when the count reaches the
     * target, and the pop in that same tick immediately spends one. At the
     * as-built target of 3 that leaves two frames of slack, so depth 1 is
     * absorbed with a frame to spare. Depth 2 is measured and reported below
     * rather than asserted — the rate is the tuning input, not a pass/fail. */
    jlRun(pool, &r, JL_SEED + 2, 400, 0, 1, -1, 0);
    JL_CHECK_ACCOUNTING(r, "reorder depth 1");
    UT_ASSERT_MSG(r.concealedInRange == 0,
                  "property 2: reorder depth 1 concealed %d frames — a "
                  "cushion of VOICE_JITTER_TARGET-1 = %d should absorb it",
                  r.concealedInRange, VOICE_JITTER_TARGET - 1);
    UT_ASSERT_MSG(r.st.lateDropped == 0,
                  "property 2: reorder depth 1 late-dropped %u frames",
                  r.st.lateDropped);
    UT_ASSERT_MSG(r.primeRuns == 1,
                  "property 2: reorder depth 1 restarted playback %d times",
                  r.primeRuns);

    jlRun(pool, &r, JL_SEED + 3, 400, 0, 2, -1, 0);
    JL_CHECK_ACCOUNTING(r, "reorder depth 2");
    fprintf(stderr,
            "  voice_jitter_under_loss: reorder depth 2 at "
            "VOICE_JITTER_TARGET=%d cost %d concealed / %d pops, %u arrived "
            "too late, playback restarted %d times. The steady-state cushion "
            "is TARGET-1 = %d frame(s), so depth 2 sits inside it.\n",
            VOICE_JITTER_TARGET, r.concealedInRange, r.pops,
            r.st.lateDropped, r.primeRuns, VOICE_JITTER_TARGET - 1);

    /* ---- Property 3: 2% loss conceals what was lost, and no more ---- */
    jlRun(pool, &r, JL_SEED + 4, 600, 2, 0, -1, 0);
    JL_CHECK_ACCOUNTING(r, "2% loss");
    {
        int expected = jlLostFrom(r.firstPlayedSeq, r.frames);
        /* Tolerance covers only the ends of the run: a frame lost before
         * playback primed is never reached, so never concealed. */
        UT_ASSERT_MSG(r.concealedInRange >= expected - 2 &&
                          r.concealedInRange <= expected + 2,
                      "property 3: concealed %d slots for %d lost frames — "
                      "concealing materially more than were lost means the "
                      "buffer is dropping frames it holds",
                      r.concealedInRange, expected);
    }

    /* ---- Property 4: nothing played twice, nothing vanishes ----
     *
     * JL_CHECK_ACCOUNTING above runs it on every scenario. This one is the
     * hardest case for it: loss and reorder together, so playback ends and
     * restarts and frames are discarded mid-run. */
    jlRun(pool, &r, JL_SEED + 5, 600, 5, 2, -1, 0);
    JL_CHECK_ACCOUNTING(r, "5% loss + depth 2");
    UT_ASSERT_MSG(r.playedFrames > r.frames / 2,
                  "property 4: only %d of %d frames played — the run did not "
                  "produce enough playback to test the accounting",
                  r.playedFrames, r.frames);

    /* ---- Property 5: added latency stays bounded ----
     * A buffer that drifts later and later is a latency leak a lossless
     * localhost path can never show. */
    for (i = 0; i <= 2; i++) {
        int bound = VOICE_JITTER_TARGET + i + 1;
        jlRun(pool, &r, JL_SEED + 10 + (uint64_t)i, 400, 0, i, -1, 0);
        JL_CHECK_ACCOUNTING(r, "latency bound");
        UT_ASSERT_MSG(r.maxAddedLatency >= 0,
                      "property 5: depth %d never played a frame", i);
        UT_ASSERT_MSG(r.maxAddedLatency <= bound,
                      "property 5: depth %d held a frame %d ticks past its "
                      "arrival, over the bound of %d — the buffer is "
                      "drifting later", i, r.maxAddedLatency, bound);
    }

    /* ---- Property 6: recovery from a burst outage ---- */
    {
        const int blackoutAt = 150;
        const int blackoutLen = VOICE_JITTER_MAX_PLC + 5;
        int resumeTick = blackoutAt + blackoutLen;

        jlRun(pool, &r, JL_SEED + 20, 400, 0, 0, blackoutAt, blackoutLen);
        JL_CHECK_ACCOUNTING(r, "burst outage");
        UT_ASSERT_MSG(r.primeRuns >= 2,
                      "property 6: playback never restarted after the outage "
                      "(%d prime runs)", r.primeRuns);
        UT_ASSERT_MSG(r.lastPrimeTick > resumeTick &&
                          r.lastPrimeTick - resumeTick <=
                              VOICE_JITTER_TARGET + 2,
                      "property 6: frames resumed at tick %d but playback "
                      "only restarted at tick %d", resumeTick,
                      r.lastPrimeTick);
        /* Settled: the outage is the only thing concealed in the whole run,
         * capped by the concealment limit that ends the utterance. */
        UT_ASSERT_MSG(r.concealedInRange <= VOICE_JITTER_MAX_PLC + 1,
                      "property 6: %d concealed slots for one outage — "
                      "playback did not settle after it", r.concealedInRange);
        UT_ASSERT_MSG(r.playedFrames == r.frames - blackoutLen,
                      "property 6: %d of %d frames played after a %d frame "
                      "outage, expected %d", r.playedFrames, r.frames,
                      blackoutLen, r.frames - blackoutLen);
    }

    /* ---- Property 7: the playout deadline ----
     *
     * Hand-placed rather than generated: the scenarios above move in whole
     * ticks and this one turns on what the buffer does inside one. A frame
     * that has not arrived is waited for rather than concealed on the spot,
     * so a frame that is merely late still plays. Concealing it immediately
     * spent a sequence number the sender had filled, and the frame was then
     * dropped as late when it turned up, leaving the receiver one frame
     * ahead of its talker with one frame less cushion for the rest of the
     * utterance. */
    {
        VoiceSpeaker *sp;
        int16_t pcm[VOICE_FRAME_SAMPLES];
        VoiceSpeakerStats st;
        uint32_t nowMs = 0;

        sp = voiceSpeakerCreate();
        UT_ASSERT(sp != NULL);

        /* Seq 0, 1 and 2 are the VOICE_JITTER_TARGET frames priming takes. */
        voiceSpeakerPush(sp, 0, 0, pool[0].data, pool[0].len);
        voiceSpeakerPush(sp, 1, 0, pool[1].data, pool[1].len);
        voiceSpeakerPush(sp, 2, 0, pool[2].data, pool[2].len);
        for (i = 0; i < 3; i++) {
            UT_ASSERT_MSG(voiceSpeakerPop(sp, pcm, nowMs),
                          "property 7: no audio for priming frame %d", i);
        }

        /* --- a frame that has not arrived: nothing plays, nothing is
         *     concealed, and the sequence number is not spent --- */
        UT_ASSERT_MSG(!voiceSpeakerPop(sp, pcm, nowMs),
                      "property 7: seq 3 has not arrived, so there is nothing "
                      "to play yet");
        nowMs += VOICE_JITTER_LATE_MS - JL_TICK_MS;
        UT_ASSERT_MSG(!voiceSpeakerPop(sp, pcm, nowMs),
                      "property 7: seq 3 is late but not yet overdue");
        voiceSpeakerGetStats(sp, &st);
        UT_ASSERT_MSG(st.concealed == 0,
                      "property 7: concealed %u inside the deadline, expected "
                      "none", st.concealed);

        /* --- the same frame, arriving before the deadline: it plays --- */
        voiceSpeakerPush(sp, 3, 0, pool[3].data, pool[3].len);
        UT_ASSERT_MSG(voiceSpeakerPop(sp, pcm, nowMs),
                      "property 7: seq 3 has arrived, so it should play");
        voiceSpeakerGetStats(sp, &st);
        UT_ASSERT_MSG(st.played == 4 && st.concealed == 0 &&
                          st.lateDropped == 0,
                      "property 7: played %u concealed %u late %u after a "
                      "frame delivered inside the deadline, expected 4, 0 "
                      "and 0", st.played, st.concealed, st.lateDropped);

        /* --- a frame that never arrives: concealed once it is overdue --- */
        UT_ASSERT(!voiceSpeakerPop(sp, pcm, nowMs));
        nowMs += VOICE_JITTER_LATE_MS;
        UT_ASSERT_MSG(voiceSpeakerPop(sp, pcm, nowMs),
                      "property 7: seq 4 is overdue, so its slot is "
                      "concealed");
        voiceSpeakerGetStats(sp, &st);
        UT_ASSERT_MSG(st.played == 4 && st.concealed == 1,
                      "property 7: played %u concealed %u once the deadline "
                      "passed, expected 4 and 1", st.played, st.concealed);

        /* The concealed slot took its sequence number with it, so seq 5 is
         * what plays next rather than being held behind seq 4. */
        voiceSpeakerPush(sp, 5, 0, pool[5].data, pool[5].len);
        UT_ASSERT_MSG(voiceSpeakerPop(sp, pcm, nowMs),
                      "property 7: seq 5 should play once 4 was given up on");
        voiceSpeakerGetStats(sp, &st);
        UT_ASSERT_MSG(st.played == 5 && st.concealed == 1 &&
                          st.lateDropped == 0,
                      "property 7: played %u concealed %u late %u for the "
                      "frame after a concealed one, expected 5, 1 and 0",
                      st.played, st.concealed, st.lateDropped);

        /* --- the case the deadline is for: seq 6 is delivered late but
         *     inside it, so it plays instead of being concealed and then
         *     counted as a late arrival --- */
        UT_ASSERT(!voiceSpeakerPop(sp, pcm, nowMs));
        nowMs += VOICE_JITTER_LATE_MS - JL_TICK_MS;
        voiceSpeakerPush(sp, 6, 0, pool[6].data, pool[6].len);
        UT_ASSERT(voiceSpeakerPop(sp, pcm, nowMs));
        voiceSpeakerGetStats(sp, &st);
        UT_ASSERT_MSG(st.lateDropped == 0,
                      "property 7: %u frames counted late — a frame delivered "
                      "inside the deadline must not be concealed and then "
                      "dropped when it lands", st.lateDropped);
        UT_ASSERT_MSG(st.played == 6 && st.concealed == 1,
                      "property 7: played %u concealed %u, expected 6 and 1",
                      st.played, st.concealed);

        voiceSpeakerDestroy(sp);
    }

    /* ---- Characterisation at the as-built VOICE_JITTER_TARGET ---- */
    jlRun(pool, &table[0], JL_SEED + 30, 600, 0,  0, -1, 0);
    jlRun(pool, &table[1], JL_SEED + 31, 600, 0,  2, -1, 0);
    jlRun(pool, &table[2], JL_SEED + 32, 600, 2,  0, -1, 0);
    jlRun(pool, &table[3], JL_SEED + 33, 600, 5,  1, -1, 0);
    jlRun(pool, &table[4], JL_SEED + 34, 600, 10, 2, -1, 0);

    fprintf(stderr,
            "  voice_jitter_under_loss: VOICE_JITTER_SLOTS=%d "
            "VOICE_JITTER_TARGET=%d VOICE_JITTER_MAX_PLC=%d seed=%u "
            "frames=600\n",
            VOICE_JITTER_SLOTS, VOICE_JITTER_TARGET, VOICE_JITTER_MAX_PLC,
            (unsigned)JL_SEED);
    fprintf(stderr,
        "  ┌───────────┬─────────┬────────────────────────┬────────────────────────────┐\n"
        "  │ loss rate │ reorder │ concealed / total pops │ max added latency (frames) │\n");
    for (i = 0; i < 5; i++) {
        fprintf(stderr,
            "  ├───────────┼─────────┼────────────────────────┼────────────────────────────┤\n"
            "  │ %-9s │ %-7s │ %10d / %-9d │ %-26d │\n",
            tableLoss[i], tableOrder[i], table[i].concealedInRange,
            table[i].pops, table[i].maxAddedLatency);
    }
    fprintf(stderr,
        "  └───────────┴─────────┴────────────────────────┴────────────────────────────┘\n");

    /* The rates above are diagnostic, but the accounting behind them is not:
     * a mismatch in any row means those numbers cannot be trusted. */
    for (i = 0; i < 5; i++) {
        JL_CHECK_ACCOUNTING(table[i], "characterisation row");
    }

    return 0;
}
