/*********************************************************
 *NAME:          brain_record.h
 *PURPOSE:
 *  Server-side brain-decision recorder. When winbolods is run
 *  with -braindebug, brainRecordTick() appends one frame per sim
 *  tick to <DEBUG_SESSION_DIR>/brainrec.btr capturing:
 *    - the god-view world snapshot (tanks / shells / bases / pills)
 *    - per bot: think time, goal info, the brain's emitted overlay
 *      (visualizer) commands, and the pool-breakdown JSON.
 *  BrainTest loads the file and replays it through its existing
 *  timeline/scrub machinery so you can step back in time and see
 *  Pool Info + visualizers exactly as the bot emitted them.
 *
 *  Format is platform-native binary (little-endian, MSVC x64 struct
 *  packing) — writer and reader are the same Windows build, so no
 *  portable encoding is attempted. Versioned via BRAINREC_VERSION;
 *  the reader must reject a mismatch.
 *********************************************************/
#ifndef BRAIN_RECORD_H
#define BRAIN_RECORD_H

#include <stdbool.h>
#include <stdint.h>

#include "server_sim.h"    /* ServerSim, BrainGoalInfo */
#include "input_packet.h"  /* TankSnapshot / ShellSnapshot / BaseSnapshot / PillSnapshot */
#include "brain_overlay.h" /* OverlayCmd */

#ifdef __cplusplus
extern "C" {
#endif

/* 8-byte file magic (incl. trailing NUL pad) + format version. Bump the
 * version whenever the frame layout below changes; the loader checks it. */
#define BRAINREC_MAGIC        "WBNREC1"
#define BRAINREC_MAGIC_LEN    8
#define BRAINREC_VERSION      5u     /* v5: + per-frame per-player alliance bitmaps */
#define BRAINREC_FRAME_MAGIC  0xB07EC0DEu
#define BRAINREC_FILENAME     "brainrec.btr"

/* Packed overlay command (replaces the raw 160-byte OverlayCmd on disk). Each
 * frame's per-bot overlay block is: uint32 count, then `count` of these:
 *   uint8  type
 *   float  x1, y1, x2, y2
 *   float  radius
 *   uint8  r, g, b, a
 *   uint8  anchor
 *   uint8  viz_idx
 *   uint8  textLen           (0 for the ~99% of cmds with no label)
 *   char   text[textLen]
 * = 28 + textLen bytes (vs a fixed 160). Lossless — the loader rebuilds a full
 * OverlayCmd, zeroing the text buffer and copying textLen chars. */

/* After BrainRecHeader, before the first frame, a one-time legend block:
 *   uint32 legendLen; char legendJson[legendLen]
 * legendJson is {"<idx>":"<viz_id>", ...} mapping each recorded overlay
 * viz_idx to its category name, so the loader can remap to BrainTest's own
 * registry by name. legendLen may be 0 (no legend available). */

/* Full-map keyframe cadence; intermediate frames store only changed tiles.
 * Terrain bytes carry mines (values in [MINE_START,MINE_END]) and boats too,
 * so this one track replays terrain + mines + boats. */
#define BRAINREC_MAP_KEYFRAME_INTERVAL 500

/* File header, written once at the top of the file. */
typedef struct {
    char     magic[BRAINREC_MAGIC_LEN];  /* BRAINREC_MAGIC */
    uint32_t version;                    /* BRAINREC_VERSION */
    char     mapName[64];                /* NUL-padded map name */
    uint32_t reserved[4];
} BrainRecHeader;

/* Compact goal-candidate row (mirror of BrainGoalInfo.candidates[] element,
 * declared here so the loader doesn't depend on the anonymous struct type). */
typedef struct {
    char  desc[120];
    float cost;
    uint8_t winner;
    float phase_weight;
} BrainRecCandidate;

/* === Writer API =========================================================
 * Compiled into bolo_static (so every host links it) but inert unless
 * enabled — only winbolods turns it on, via -braindebug. BrainTest links
 * it too and leaves it disabled (it has its own in-memory recorder). */

/* Turn recording on/off. Off by default; winbolods sets it on -braindebug. */
void brainRecordSetEnabled(bool enabled);
bool brainRecordIsEnabled(void);

/* Selective recording parts (winbolods -bd-noviz / -bd-nopool /
 * -bd-pool-full). When a stream is off, its per-bot block is written as
 * empty (count 0) so the frame layout — and every existing loader — is
 * unchanged. poolMode: 0 = off (skips the expensive per-bot
 * get_pool_breakdown_json() Lua eval entirely), 1 = round-robin (one bot
 * per frame — the winbolods -braindebug default; ~1/botCount the cost,
 * no spike ticks), 2 = full per-bot per-tick capture. */
void brainRecordSetParts(bool viz, int poolMode);

/* Pool-JSON prefetch protocol (parallelizes the capture): a bot's worker
 * thread asks CaptureWanted right after its think and, when true, builds
 * the JSON there (each bot owns its own Lua VM, so the builds fan out
 * across the worker pool instead of running serially in brainRecordTick).
 * The producer then hands each string to StashPoolJson (ownership moves;
 * producer thread only), and brainRecordTick consumes the stash — falling
 * back to an inline eval only when no prefetch arrived (budget-killed
 * think, skipped job). CaptureWanted is the single decision point both
 * sides share, keyed off the sim tick, so they can't disagree. */
bool brainRecordPoolCaptureWanted(ServerSim *sim, BYTE slot);
void brainRecordStashPoolJson(BYTE slot, char *json);

/* Set the output directory directly (winbolods creates debug_sessions/<TS>/
 * and passes it here). When set, the recorder writes there immediately and
 * does NOT depend on a bot brain exposing DEBUG_SESSION_DIR — so a game with
 * only human players is still recorded. If left unset, the recorder falls
 * back to reading DEBUG_SESSION_DIR off the first bot's Lua state. */
void brainRecordSetSessionDir(const char *dir);

/* The resolved DEBUG_SESSION_DIR the recorder is writing into, or NULL if it
 * hasn't opened yet. Used by the host to drop the .wbv replay alongside. */
const char *brainRecordGetSessionDir(void);

/* Append one frame for the current sim tick. Call ONCE per tick from the
 * server main thread, right after botManagerTick() (workers are joined, so
 * reading overlay buffers + evaluating Lua on each bot is safe). No-op unless
 * enabled. Opens the output file lazily once the brain's DEBUG_SESSION_DIR
 * is available; de-dupes repeated calls for the same tick. */
void brainRecordTick(ServerSim *sim);

/* End the current game's recording: flush + close the .btr and reset the
 * recorder's per-game state (tick anchor, map keyframe, legend skip-set)
 * WITHOUT disarming (g_enabled stays on). Call on the running->gameOver edge.
 * After a fresh brainRecordSetSessionDir(), the next brainRecordTick opens a
 * new file — so each game is a self-contained, tick-0-anchored recording in
 * its own debug_sessions/<TS>/ dir. */
void brainRecordEndGame(void);

/* Flush + close the output file. Call at server shutdown. */
void brainRecordShutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* BRAIN_RECORD_H */
