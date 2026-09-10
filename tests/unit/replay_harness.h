/*
 * Record-and-decode replay fixture.
 *
 * Records a headless round to a .wbv with the same writer the dedicated
 * server uses, decodes that file with the production log-viewer reader, and
 * diffs the decoded world against the sim's. A test that wants to know
 * whether a change survives a real recording asks for that in three calls
 * instead of hand-building bytes.
 *
 * ── Why this is two translation units ───────────────────────────────────
 * The sim headers and the log-viewer headers cannot share a translation
 * unit — both worlds reach "global.h" and disagree about the world types,
 * which is why src/logviewer/spec_seed_load.c takes its input as raw bytes
 * rather than including the sim headers. So the fixture is split:
 *
 *   replay_harness.c         sim world: start / tick / stop recording,
 *                            capture the sim's world, diff two captures.
 *   replay_harness_decode.c  viewer world: decode the .wbv and capture
 *                            the decoded world.
 *
 * ReplayWorld is the seam between them: plain integers, no world types, so
 * both sides can fill one and the diff can read both. That is also why the
 * sizes below are spelled out here rather than pulled from either world's
 * headers; each .c file checks them against its own world's constants.
 *
 * ── Lifecycle ───────────────────────────────────────────────────────────
 *   replayHarnessStartRecording(&h, tag, playerName)
 *       Stands up a running headless ServerSim with one player and starts
 *       a .wbv named after tag in the working directory. The opening
 *       snapshot is written here, so it holds the world as it was BEFORE
 *       the caller changes anything.
 *   h.sim                        drive the round through this.
 *   replayHarnessTick(&h, n)     advance the sim n ticks.
 *   replayHarnessStopRecording(&h)
 *       Captures the sim's world, then closes the .wbv.
 *   replayHarnessDecode(&h)      load the .wbv through the production
 *       reader and play it to end-of-log, then capture what it decoded.
 *   replayHarnessCompare(&h)     true when the two captures agree;
 *       otherwise false with replayHarnessDiff(&h) naming what differed.
 *   replayHarnessStop(&h)        tear it all down and remove the file.
 *
 * The caller owns nothing inside the harness; Stop frees it all and is safe
 * on a zeroed or partially-started fixture.
 *
 * ── The sim ticks twice per recorded tick ───────────────────────────────
 * serverSimTick alternates a keys tick and a game tick and only the game
 * tick reaches the recorder, so replayHarnessTick(&h, 2) advances the
 * recording by one tick. Events a caller raises between ticks sit in the
 * writer's buffer until the next recorded tick flushes them, so a round
 * must tick at least twice after its last change for that change to reach
 * the file.
 *
 * The keys tick turns tanks and nothing else, so every field the diff
 * compares moves only on a game tick, and the recorder reads it after that
 * tick's world update. A capture therefore states the world as of the last
 * recorded tick however many ticks the round happens to end on.
 *
 * ── What the diff compares, and what it leaves out ──────────────────────
 * Compared: every terrain cell, each pillbox's cell/owner/armour, each
 * base's cell/owner/armour/shells/mines, each start's cell/direction, and
 * every tank slot's in-use flag and shells/mines/armour/trees.
 *
 * Left out on purpose, because a recording cannot carry them rather than
 * because they happen to differ:
 *
 *   pillbox speed, reload, coolDown, justSeen and base refuelTime,
 *   baseTime, justStopped — no event carries these, so a snapshot is the
 *   only thing that ever states them. They move every tick while a round
 *   runs, so between snapshots the reader has no way to be right about
 *   them. baseTime is also an int32 that the snapshot blob truncates to 16
 *   bits, so even a snapshot does not round-trip it.
 *
 *   pillbox inTank — the recording does carry it (log_PillSetInTank), but
 *   the viewer's lv_pillsGetPill does not copy it out and there is no
 *   getter that does. Adding one is a production change; comparing a field
 *   the getter leaves untouched would read whatever was in the caller's
 *   struct.
 *
 * ── Rounds this fixture does not prove ──────────────────────────────────
 * The fields above are compared, but three sim paths change them without
 * writing the event that would carry the change, so a round that drives one
 * will differ for a reason that is not the reader's fault. None of them can
 * fire in a round of idle ticks, which is why the bundled case avoids them
 * — a round that wants any of these needs the recording side looked at
 * first:
 *
 *   basesSetOwner / basesSetBaseOwner zero a base's armour, shells and
 *   mines when it is STOLEN from another player, and write only
 *   log_BaseSetOwner. The viewer sets the owner and keeps the old stock, so
 *   the two disagree until the next log_BaseSetStock. Handing a base over
 *   the migrate way, as the bundled case does, does not zero the stock and
 *   so does not hit this.
 *
 *   basesDamagePos changes a base's armour with no event at all. It needs a
 *   player-owned shell to hit a base.
 *
 *   pillsDropSetNeutralOwner and pillsMigrate write log_PillSetOwner only
 *   for a pill that is in a tank, and pillsMigratePlanted writes nothing, so
 *   a planted pill changes hands unrecorded. These need a player to quit or
 *   an alliance to break.
 *
 * Terrain is read on the sim side through mapGetPos, the same accessor the
 * recorder serialises through — it reports the map border as deep sea
 * whatever is stored there, so capturing through it keeps the two sides
 * describing the same map rather than disagreeing about the border.
 */

#ifndef WINBOLO_TEST_REPLAY_HARNESS_H
#define WINBOLO_TEST_REPLAY_HARNESS_H

#include <stdbool.h>
#include <stdint.h>

struct ServerSim;

/* Spelled out rather than included: see the header comment. Each .c file
 * checks these against its own world's constants. */
#define REPLAY_MAP_SIZE   256
#define REPLAY_MAX_TANKS  16
#define REPLAY_MAX_PILLS  16
#define REPLAY_MAX_BASES  16
#define REPLAY_MAX_STARTS 16

typedef struct {
    uint8_t x;
    uint8_t y;
    uint8_t owner;
    uint8_t armour;
} ReplayPill;

typedef struct {
    uint8_t x;
    uint8_t y;
    uint8_t owner;
    uint8_t armour;
    uint8_t shells;
    uint8_t mines;
} ReplayBase;

typedef struct {
    uint8_t x;
    uint8_t y;
    uint8_t dir;
} ReplayStart;

typedef struct {
    bool    inUse;
    uint8_t shells;
    uint8_t mines;
    uint8_t armour;
    uint8_t trees;
} ReplayTank;

/* One side's view of the world, in plain integers so both worlds can fill
 * one. terrain is [x][y], matching both maps' own indexing. */
typedef struct {
    uint8_t     terrain[REPLAY_MAP_SIZE][REPLAY_MAP_SIZE];
    uint8_t     numPills;
    uint8_t     numBases;
    uint8_t     numStarts;
    ReplayPill  pills[REPLAY_MAX_PILLS];
    ReplayBase  bases[REPLAY_MAX_BASES];
    ReplayStart starts[REPLAY_MAX_STARTS];
    ReplayTank  tanks[REPLAY_MAX_TANKS];
} ReplayWorld;

typedef struct ReplayHarness {
    struct ServerSim *sim;        /* the round being recorded — drive this */
    char              path[256];  /* the .wbv on disk */
    bool              recording;
    ReplayWorld      *recorded;   /* the sim's world when recording stopped */
    ReplayWorld      *replayed;   /* the decoded world at end-of-log */
    char              diff[256];  /* what Compare found, or "" */
} ReplayHarness;

/* Stand up the round and open the .wbv. tag names the file, playerName is
 * the one player added to the sim. Returns false (harness safe to Stop) on
 * any failure. */
bool replayHarnessStartRecording(ReplayHarness *h, const char *tag,
                                 const char *playerName);

/* Advance the sim by simTicks ticks. Two sim ticks per recorded tick — see
 * the header comment. */
void replayHarnessTick(ReplayHarness *h, int simTicks);

/* Capture the sim's world, then close the .wbv. Returns false if the
 * harness is not recording or the capture could not be allocated. */
bool replayHarnessStopRecording(ReplayHarness *h);

/* Load the recorded .wbv through the production reader, play it to
 * end-of-log and capture the world it decoded. Returns false if the file
 * could not be read, the reader rejected it, or playback did not reach
 * end-of-log. Defined in replay_harness_decode.c. */
bool replayHarnessDecode(ReplayHarness *h);

/* Compare the two captures. Returns true when they agree; otherwise fills
 * h->diff with the first difference and returns false. */
bool replayHarnessCompare(ReplayHarness *h);

/* What the last Compare found, or "" when it found nothing. Never NULL. */
const char *replayHarnessDiff(const ReplayHarness *h);

/* Destroy the sim, free the captures and remove the .wbv. Safe on a zeroed
 * or partially-started harness. */
void replayHarnessStop(ReplayHarness *h);

#endif /* WINBOLO_TEST_REPLAY_HARNESS_H */
