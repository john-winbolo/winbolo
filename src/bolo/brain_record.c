/* SPDX-License-Identifier: GPL-3.0-or-later */
/*********************************************************
 *NAME:          brain_record.c
 *PURPOSE:       Server-side brain-decision recorder (see brain_record.h).
 *********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>   /* SDL_GetPerformanceCounter for the perf log */
#include <zlib.h>        /* block log compression */

#include "brain_record.h"
#include "brain_record_ticks.h"  /* tick-range note written at close */
#include "brain_record_io.h"
#include "server_sim.h"
#include "input_packet.h"
#include "brain_overlay.h"
#include "types.h"       /* MAP_ARRAY_SIZE, map */
#include "game_sim.h"    /* GameSim internals: ->mp->mapItem */
#include "players.h"     /* players / struct playersObj: per-player alliance bitmap */
#include "pillbox.h"     /* pillSetInTank — the pill flags byte */
#include "bases.h"       /* basesIsActive — the base on-map mask */
#include "brain_record_dij.h"  /* v8 recorded Dijkstra box */
#include "brain_pathfinder.h"  /* DijkstraSlate, DIJKSTRA_NUM_SLATES */
#include "brain_record_host.h" /* the per-bot body encoder, the hosted marker */
#include "braincore.h"   /* brainCoreReleaseLogFiles */

#define BRAINREC_MAP_TILES (MAP_ARRAY_SIZE * MAP_ARRAY_SIZE)

/* ---- module state ---------------------------------------------------- */
/* The .btr is written through the zstd container (brain_record_io.h): each
 * frame is compressed on the fly, and a new independent zstd frame starts at
 * every periodic map keyframe. g_sinceFlush throttles the flush so a hard kill
 * loses at most ~1-2s while keeping a good ratio. */
static bool     g_enabled  = false;
static BrainRecOut *g_out  = NULL;
static uint32_t g_sinceFlush = 0;
static bool     g_failed   = false;   /* gave up opening (no dir / open error) */
static uint32_t g_lastTick = 0xFFFFFFFFu;
static int      g_openAttempts = 0;
static char     g_sessionDir[512] = {0};  /* resolved DEBUG_SESSION_DIR */
static bool     g_skipViz[256] = {0};     /* overlay viz_idx values to drop (fallback set) */
static bool     g_haveSkip = false;
/* Per-slot skip sets. Every brain self-assigns its overlay viz_idx values
 * from ITS OWN sorted id list, so two different brains (an older GoalHunter
 * and the current one in a 2v2) number the same category differently. One skip set taken
 * from bot 0 therefore dropped the WRONG categories from every bot running
 * the other brain -- the current brain's pill_portfolio vanished from recordings while
 * the older one's showed. Slots without their own set (bots created after the file
 * was opened) fall back to g_skipViz. */
static bool     g_skipVizSlot[MAX_TANKS][256];
static bool     g_haveSkipSlot[MAX_TANKS];
static uint32_t g_legendBytes = 0;

/* Map-terrain delta tracking. g_prevMap holds the last frame's terrain so a
 * non-keyframe frame writes only changed tiles. */
static BYTE     g_prevMap[BRAINREC_MAP_TILES];
static bool     g_hasPrevMap = false;
static uint32_t g_frameCount = 0;

/* First and last frame tick written to the open .btr, and how many frames.
 * At close they go into brainrec_ticks.txt, so BrainTest can show the game
 * length without reading the whole recording. */
static uint32_t g_fileFirstTick = 0;
static uint32_t g_fileLastTick  = 0;
static uint64_t g_fileFrames    = 0;

/* Close the open .btr and write its tick-range note beside it. */
static void closeFileWithTicks(void) {
    if (!g_out) return;
    if (!brainRecOutClose(g_out))
        fprintf(stderr, "brain_record: write error closing the recording\n");
    g_out = NULL;
    if (g_fileFrames > 0 && g_sessionDir[0]) {
        if (!brainRecTicksWrite(g_sessionDir, g_fileFirstTick, g_fileLastTick, g_fileFrames))
            fprintf(stderr, "brain_record: could not write %s/%s\n",
                    g_sessionDir, BRAINREC_TICKS_FILENAME);
    }
    g_fileFrames = 0;
}

/* Recorded Dijkstra box (v8): what a reader holds after the last frame, so
 * the next frame writes only what changed. Heap, because it is ~150 KB. */
static BrainRecDijState *g_dijPrev = NULL;
static BrainRecDijBuf    g_dijBuf  = {0};
static uint64_t          g_pfDijBytes = 0;
static double            g_pfDijMs = 0;

BOLO_STATIC_ASSERT(BRAINREC_DIJ_SLATES == DIJKSTRA_NUM_SLATES,
                   dij_box_covers_every_slate);
BOLO_STATIC_ASSERT(BRAINREC_DIJ_MAX_BOTS == MAX_TANKS,
                   dij_box_covers_every_player_slot);

/* Selective recording parts (winbolods -bd-noviz / -bd-nopool /
 * -bd-pool-full). When off, the per-bot overlay block / pool-breakdown
 * JSON are written as empty (count 0) so the frame layout is unchanged
 * and old loaders still work.
 *
 * poolMode: the pool-breakdown JSON is a per-bot Lua serialization that
 * measured 24-36 ms/tick with 12 bots (vs the 20 ms tick budget) — it
 * alone dragged a -braindebug server from 50 Hz to ~25 Hz. Mode 1
 * (ROUND-ROBIN, the winbolods default) serializes ONE bot per tick so
 * the cost is ~1/botCount per tick with no spike ticks; each bot's
 * panel data is at most botCount frames (~0.25 s at 12 bots) stale,
 * and BrainTest's playback panel walks back to the latest sample.
 * Mode 2 (FULL) is the old exact per-tick capture for when panel-perfect
 * history is worth a slow game. */
#define BRAINREC_POOL_OFF   0
#define BRAINREC_POOL_RR    1
#define BRAINREC_POOL_FULL  2
static bool g_recViz   = true;
static int  g_poolMode = BRAINREC_POOL_FULL;

void brainRecordSetParts(bool viz, int poolMode) {
    g_recViz   = viz;
    g_poolMode = poolMode;
}

/* Pool block format (winbolods -bd-pool-format raw|json). RAW (the default)
 * asks the brain for its cheap raw capture (brain.get_pool_breakdown_raw):
 * the panel text is built later, from the recording, by pool_render.lua.
 * JSON asks for the finished panel JSON, built in the brain on the tick, as
 * every recording did before. A brain with no get_pool_breakdown_raw gives
 * its JSON either way, and BrainTest reads both forms (a raw block starts
 * with "WBPR2:", a JSON block with '{'). */
static int g_poolFormat = BRAINREC_POOL_FORMAT_RAW;

void brainRecordSetPoolFormat(int format) {
    g_poolFormat = (format == BRAINREC_POOL_FORMAT_JSON)
                   ? BRAINREC_POOL_FORMAT_JSON : BRAINREC_POOL_FORMAT_RAW;
}

int brainRecordGetPoolFormat(void) { return g_poolFormat; }

const char *brainRecordPoolExpr(void) {
    return (g_poolFormat == BRAINREC_POOL_FORMAT_JSON)
        ? "return brain.get_pool_breakdown_json()"
        : "return (brain.get_pool_breakdown_raw"
          " or brain.get_pool_breakdown_json)()";
}
void brainRecordGetParts(bool *viz, int *poolMode) {
    if (viz) *viz = g_recViz;
    if (poolMode) *poolMode = g_poolMode;
}

/* Bot Host seats (brain_record_host.h): the server answers which bot runs on
 * a host. g_body is the per-bot body scratch, reused every entry. */
static BrainRecordHostedFn g_hostedFn = NULL;
static BrainRecBuf         g_body;

void brainRecordSetHostedInfoFn(BrainRecordHostedFn fn) { g_hostedFn = fn; }

/* Worker-prefetched pool JSON, one slot per bot. Workers build the JSON in
 * parallel right after their bot's think (bot_manager runBotThinkJob); the
 * producer moves each string here in Stage 3; brainRecordTick consumes it
 * so the serial recorder section only writes bytes. All single-threaded
 * from the recorder's perspective: stash and consume both happen on the
 * producer thread. */
static char *g_poolStash[MAX_TANKS];

/* Shared capture decision — the worker prefetch and brainRecordTick's
 * writer MUST agree on which bot gets its pool captured each tick, so both
 * call this. Round-robin keys off the sim tick (stable across the whole
 * botManagerTick) and the bot's index in ascending-slot iteration order. */
bool brainRecordPoolCaptureWanted(ServerSim *sim, BYTE slot) {
    if (!g_enabled || sim == NULL) return false;
    if (g_poolMode == BRAINREC_POOL_OFF) return false;
    if (g_poolMode == BRAINREC_POOL_FULL) return true;
    int botCount = 0, iterIdx = -1;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (!serverSimIsBot(sim, (BYTE)i)) continue;
        if (i == (int)slot) iterIdx = botCount;
        botCount++;
    }
    if (botCount == 0 || iterIdx < 0) return false;
    return iterIdx == (int)((serverSimGetTick(sim) / 2u) % (uint32_t)botCount);
}

void brainRecordStashPoolJson(BYTE slot, char *json) {
    if (slot >= MAX_TANKS) { free(json); return; }
    free(g_poolStash[slot]);
    g_poolStash[slot] = json;
}

static void perf_free_pool_stash(void) {
    for (int i = 0; i < MAX_TANKS; i++) {
        free(g_poolStash[i]);
        g_poolStash[i] = NULL;
    }
}

/* ── Perf diagnostics (braindbg_perf.log) ─────────────────────────────
 * Per-tick phase costs accumulated and dumped every PERF_DUMP_TICKS as one
 * line, so a slow many-bot debug game can be broken down: which recording
 * stream (snapshot/map/overlays/pool JSON) eats the tick, how much the
 * brains themselves think, and the achieved tick rate (wall time per
 * window — the "is the game keeping up" number). */
#define PERF_DUMP_TICKS 250
static FILE    *g_perf = NULL;
static bool     g_perfTried = false;
static uint64_t g_perfWallStart = 0;
static double   g_pfSnapMs = 0, g_pfMapMs = 0, g_pfOvlMs = 0, g_pfPoolMs = 0;
static double   g_pfThinkMs = 0;   /* sum of per-bot lastThinkMs */
static uint64_t g_pfOvlCmds = 0, g_pfPoolBytes = 0;
static uint32_t g_pfTicks = 0;
/* The brain's pool build (capture or JSON), per bot, timed where it runs:
 * on the bot's worker (prefetch) or inline in brainRecordTick. One slot per
 * bot, so the parallel workers never write the same counter. */
static double   g_pfPoolEvalMs[MAX_TANKS];
static uint32_t g_pfPoolEvals[MAX_TANKS];

void brainRecordAddPoolEvalMs(BYTE slot, double ms) {
    if (slot >= MAX_TANKS) return;
    g_pfPoolEvalMs[slot] += ms;
    g_pfPoolEvals[slot]++;
}

static double perf_ms(uint64_t t0, uint64_t t1) {
    return (double)(t1 - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

static void perf_dump(uint32_t tick) {
    if (!g_perf && !g_perfTried && g_sessionDir[0]) {
        char p[600];
        snprintf(p, sizeof(p), "%s/braindbg_perf.log", g_sessionDir);
        g_perf = fopen(p, "a");
        g_perfTried = true;
        if (g_perf) {
            fprintf(g_perf, "# tick window=%d | wall_s + eff_hz measure the whole "
                    "server loop; think=sum of bot brain ms/tick; rec_* = recorder "
                    "phase ms/tick; ovl_cmds/pool_B = recorded volume per tick\n",
                    PERF_DUMP_TICKS);
        }
    }
    if (!g_perf) return;
    double wallS = perf_ms(g_perfWallStart, SDL_GetPerformanceCounter()) / 1000.0;
    double n = (double)(g_pfTicks ? g_pfTicks : 1);
    double evalMs = 0;
    uint32_t evals = 0;
    for (int i = 0; i < MAX_TANKS; i++) {
        evalMs += g_pfPoolEvalMs[i];
        evals  += g_pfPoolEvals[i];
        g_pfPoolEvalMs[i] = 0;
        g_pfPoolEvals[i]  = 0;
    }
    /* pool_eval = mean ms of ONE bot's pool build (the raw capture, or the
     * JSON with -bd-pool-format json); pool_fmt says which. */
    fprintf(g_perf,
        "PERF t=%u wall_s=%.2f eff_hz=%.1f think_ms=%.2f rec_snap=%.3f "
        "rec_map=%.3f rec_ovl=%.3f rec_pool=%.3f ovl_cmds=%.0f pool_B=%.0f "
        "pool_eval=%.3f pool_evals=%u pool_fmt=%s rec_dij=%.3f dij_B=%.0f\n",
        tick, wallS, (double)g_pfTicks / (wallS > 0 ? wallS : 1),
        g_pfThinkMs / n, g_pfSnapMs / n, g_pfMapMs / n, g_pfOvlMs / n,
        g_pfPoolMs / n, (double)g_pfOvlCmds / n, (double)g_pfPoolBytes / n,
        evals ? evalMs / (double)evals : 0.0, evals,
        (g_poolFormat == BRAINREC_POOL_FORMAT_JSON) ? "json" : "raw",
        g_pfDijMs / n, (double)g_pfDijBytes / n);
    fflush(g_perf);
    g_pfDijMs = 0;
    g_pfDijBytes = 0;
    g_pfSnapMs = g_pfMapMs = g_pfOvlMs = g_pfPoolMs = g_pfThinkMs = 0;
    g_pfOvlCmds = g_pfPoolBytes = 0;
    g_pfTicks = 0;
    g_perfWallStart = SDL_GetPerformanceCounter();
}

/* Small write helpers — native byte order (same build both ends), streamed
 * through the zstd container. */
static void wr_u8 (uint8_t v)  { brainRecOutWrite(g_out, &v, 1); }
static void wr_u32(uint32_t v) { brainRecOutWrite(g_out, &v, sizeof v); }
static void wr_buf(const void *p, size_t n) { if (n) brainRecOutWrite(g_out, p, n); }

void brainRecordSetEnabled(bool enabled) { g_enabled = enabled; }
bool brainRecordIsEnabled(void)          { return g_enabled; }

void brainRecordSetSessionDir(const char *dir) {
    if (dir && dir[0]) {
        strncpy(g_sessionDir, dir, sizeof(g_sessionDir) - 1);
        g_sessionDir[sizeof(g_sessionDir) - 1] = '\0';
    } else {
        g_sessionDir[0] = '\0';
    }
}

const char *brainRecordGetSessionDir(void) {
    return g_sessionDir[0] ? g_sessionDir : NULL;
}

/* Find the first active bot slot, or -1. */
static int firstBotSlot(ServerSim *sim) {
    for (int i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsBot(sim, (BYTE)i)) return i;
    }
    return -1;
}

/* First bot slot whose brain exposes a non-empty viz legend. A bot running a
 * non-visualizer brain (e.g. a scripted test victim) returns nothing, so the
 * legend + skip-set are taken from the first bot that ACTUALLY has visualizers
 * rather than whichever brain happens to occupy the lowest slot. On success
 * returns the slot and hands the evaluated legend back via *outLegend (caller
 * frees); returns -1 (and *outLegend = NULL) if no bot has a legend. */
static int firstVizBotSlot(ServerSim *sim, char **outLegend) {
    for (int i = 0; i < MAX_TANKS; i++) {
        if (!serverSimIsBot(sim, (BYTE)i)) continue;
        char *legend = serverSimBotEvalLuaString(sim, (BYTE)i,
                                                 "return brain.viz_legend_json()");
        if (legend && legend[0]) { *outLegend = legend; return i; }
        if (legend) free(legend);
    }
    *outLegend = NULL;
    return -1;
}

bool brainRecWriteSessionMap(const char *dir, const uint8_t *data, int len) {
    if (!dir || !dir[0] || !data || len <= 0) return false;
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", dir, BRAINREC_MAP_FILENAME);
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    bool ok = fwrite(data, 1, (size_t)len, fp) == (size_t)len;
    if (fclose(fp) != 0) ok = false;
    return ok;
}

int brainRecReadSessionMap(const char *dir, uint8_t *out, int cap) {
    if (!dir || !dir[0] || !out || cap <= 0) return 0;
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", dir, BRAINREC_MAP_FILENAME);
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    size_t n = fread(out, 1, (size_t)cap, fp);
    /* A file longer than the buffer is not a map this build made. */
    int extra = fgetc(fp);
    fclose(fp);
    if (extra != EOF) return 0;
    return (int)n;
}

/* Save the map the recording is made on beside it (see BRAINREC_MAP_FILENAME
 * in brain_record.h for why the .btr needs it). A failure only costs the
 * replay its base positions, so it is reported and the recording goes on. */
static void saveSessionMap(ServerSim *sim) {
    uint8_t *buf = (uint8_t *)malloc(MAP_COMPRESSED_MAX_SIZE);
    int len = buf ? serverSimGetCompressedMap(sim, buf, MAP_COMPRESSED_MAX_SIZE) : 0;
    if (len <= 0 || !brainRecWriteSessionMap(g_sessionDir, buf, len)) {
        fprintf(stderr, "brain_record: could not save %s/%s; a replay of this "
                        "session will have no base positions\n",
                g_sessionDir, BRAINREC_MAP_FILENAME);
    }
    free(buf);
}

/* Lazily open <sessionDir>/brainrec.btr. The session dir normally comes
 * straight from brainRecordSetSessionDir() (winbolods creates it under
 * -braindebug). As a fallback, if it wasn't set we read DEBUG_SESSION_DIR off
 * the first bot's Lua state. Returns true once g_out is ready. */
static bool ensureOpen(ServerSim *sim) {
    if (g_out)    return true;
    if (g_failed) return false;

    /* Resolve the dir. Prefer an explicitly-set one (no bot dependency). */
    if (!g_sessionDir[0]) {
        int slot = firstBotSlot(sim);
        if (slot < 0) return false;   /* no bots yet — try again next tick */
        char *dir = serverSimBotEvalLuaString(sim, (BYTE)slot,
                                              "return DEBUG_SESSION_DIR or ''");
        bool haveDir = (dir && dir[0]);
        if (haveDir) {
            strncpy(g_sessionDir, dir, sizeof(g_sessionDir) - 1);
            g_sessionDir[sizeof(g_sessionDir) - 1] = '\0';
        }
        if (dir) free(dir);
        if (!g_sessionDir[0]) {
            if (++g_openAttempts > 250) {   /* ~5s @ 50Hz — no dir ever appeared */
                g_failed = true;
                fprintf(stderr, "brain_record: no session dir set; recording "
                                "disabled\n");
            }
            return false;
        }
    }

    char path[600];
    snprintf(path, sizeof(path), "%s/%s", g_sessionDir, BRAINREC_FILENAME);

    /* Streaming zstd (level and window in brain_record_io.h). The data
     * (zero-padded structs, pool JSON that repeats frame to frame) is highly
     * compressible, and the long window finds the previous frames. */
    g_out = brainRecOutOpen(path);
    if (!g_out) {
        g_failed = true;
        fprintf(stderr, "brain_record: could not open '%s' for writing\n", path);
        return false;
    }

    BrainRecHeader hdr;
    memset(&hdr, 0, sizeof hdr);
    memcpy(hdr.magic, BRAINREC_MAGIC, BRAINREC_MAGIC_LEN);
    hdr.version = BRAINREC_VERSION;
    const char *mn = serverSimGetMapName(sim);
    if (mn) { strncpy(hdr.mapName, mn, sizeof(hdr.mapName) - 1); }
    brainRecOutWrite(g_out, &hdr, sizeof hdr);
    saveSessionMap(sim);

    /* One-time legend: overlay viz_idx -> category name, so the loader can
     * label/filter recorded overlays. The blob is ONE length-prefixed string
     * (opaque to the walker / splitter / peek), but it now carries a legend
     * PER BOT SLOT as well as the global one:
     *   {"0":"name",...,"slots":{"<slot>":{"0":"name",...},...}}
     * Brains number their categories independently (sorted-id self-assign),
     * so a single legend only ever described the first viz bot's brain and
     * mislabelled every bot on a different brain. The global map (first viz
     * bot) stays first so a loader that only knows the old shape still reads
     * it; slots created after the file opened fall back to it. */
    char *legend = NULL;
    int lslot = firstVizBotSlot(sim, &legend);
    {
        size_t cap = (legend ? strlen(legend) : 2) + 64;
        char  *slotLegend[MAX_TANKS];
        int    nSlots = 0;
        memset(slotLegend, 0, sizeof slotLegend);
        for (int i = 0; i < MAX_TANKS; i++) {
            if (!serverSimIsBot(sim, (BYTE)i)) continue;
            char *lj = serverSimBotEvalLuaString(sim, (BYTE)i, "return brain.viz_legend_json()");
            if (lj && lj[0] && lj[0] == '{') { slotLegend[i] = lj; cap += strlen(lj) + 16; nSlots++; }
            else if (lj) free(lj);
        }
        char *combined = (char *)malloc(cap);
        size_t n = 0;
        if (combined) {
            const char *g0 = (legend && legend[0] == '{') ? legend : "{}";
            size_t gl = strlen(g0);
            /* Global legend minus its closing brace, then the slots object. */
            memcpy(combined, g0, gl - 1); n = gl - 1;
            if (nSlots) {
                n += (size_t)snprintf(combined + n, cap - n, "%s\"slots\":{", gl > 2 ? "," : "");
                int first = 1;
                for (int i = 0; i < MAX_TANKS; i++) {
                    if (!slotLegend[i]) continue;
                    n += (size_t)snprintf(combined + n, cap - n, "%s\"%d\":%s",
                                          first ? "" : ",", i, slotLegend[i]);
                    first = 0;
                }
                n += (size_t)snprintf(combined + n, cap - n, "}");
            }
            n += (size_t)snprintf(combined + n, cap - n, "}");
        }
        for (int i = 0; i < MAX_TANKS; i++) if (slotLegend[i]) free(slotLegend[i]);
        uint32_t llen = combined ? (uint32_t)n : 0u;
        brainRecOutWrite(g_out, &llen, sizeof llen);
        if (llen) brainRecOutWrite(g_out, combined, llen);
        free(combined);
        g_legendBytes = llen;
    }
    if (legend) free(legend);

    /* Recorder skip set: viz_idx values the brain flags as too-heavy/cosmetic
     * (label_overlays, attack_scan_spots_all_pills). These overlay commands
     * are dropped from disk (still drawn live in BrainTest). CSV of indices,
     * asked of EVERY bot (indices are per-brain, see g_skipVizSlot); the
     * first viz bot's set doubles as the fallback for late-created slots. */
    int nskip = 0;
    memset(g_skipVizSlot, 0, sizeof g_skipVizSlot);
    memset(g_haveSkipSlot, 0, sizeof g_haveSkipSlot);
    for (int i = 0; i < MAX_TANKS; i++) {
        if (!serverSimIsBot(sim, (BYTE)i)) continue;
        char *skipcsv = serverSimBotEvalLuaString(sim, (BYTE)i,
                                                  "return brain.viz_record_skip_csv()");
        if (!skipcsv) continue;
        const char *p = skipcsv;
        while (*p) {
            int v = atoi(p);
            if (v >= 0 && v < 256) {
                if (!g_skipVizSlot[i][v]) { g_skipVizSlot[i][v] = true; g_haveSkipSlot[i] = true; nskip++; }
                if (i == lslot && !g_skipViz[v]) { g_skipViz[v] = true; g_haveSkip = true; }
            }
            while (*p && *p != ',') p++;
            if (*p == ',') p++;
        }
        free(skipcsv);
    }

    fprintf(stderr, "brain_record: recording brain decisions to '%s' "
                    "(viz legend %u bytes, %d skipped viz across slots)\n",
            path, (unsigned)g_legendBytes, nskip);
    return true;
}

/* Whether writeMap will write a periodic keyframe this frame (the first frame
 * with a map, then every BRAINREC_MAP_KEYFRAME_INTERVAL frames). The recorder
 * asks before the frame starts so that frame can open a new zstd block: a
 * reader can then start decoding at that block and get a full map at once.
 * The forced keyframe (more than 1/4 of the tiles changed) is decided later
 * and does not start a block. */
static bool periodicKeyframeDue(ServerSim *sim) {
    GameSim *gs = serverSimGetGameSim(sim);
    if (!gs || !gs->mp) return false;
    return !g_hasPrevMap || (g_frameCount % BRAINREC_MAP_KEYFRAME_INTERVAL) == 0;
}

/* Write the terrain block: keyframe (full map) or delta (changed tiles only).
 * Keyframe on the first frame, every N frames, or when a huge fraction of the
 * map changed (reload / mass terrain event) so deltas stay small. The terrain
 * bytes carry mines + boats too, so this replays all three. */
static bool writeMap(ServerSim *sim) {
    GameSim    *gs  = serverSimGetGameSim(sim);
    const BYTE *cur = (gs && gs->mp) ? &gs->mp->mapItem[0][0] : NULL;
    if (!cur) { wr_u8(0); wr_u32(0); return false; }   /* no map this frame */

    bool keyframe = !g_hasPrevMap
                    || (g_frameCount % BRAINREC_MAP_KEYFRAME_INTERVAL) == 0;

    /* Static delta scratch — avoids per-frame allocation. */
    static uint32_t offs[BRAINREC_MAP_TILES];
    static uint8_t  vals[BRAINREC_MAP_TILES];
    int nd = 0;
    if (!keyframe) {
        for (int i = 0; i < BRAINREC_MAP_TILES; i++) {
            if (cur[i] != g_prevMap[i]) { offs[nd] = (uint32_t)i; vals[nd] = cur[i]; nd++; }
        }
        if (nd > BRAINREC_MAP_TILES / 4) keyframe = true;   /* delta not worth it */
    }

    if (keyframe) {
        wr_u8(1);
        wr_buf(cur, BRAINREC_MAP_TILES);
    } else {
        wr_u8(0);
        wr_u32((uint32_t)nd);
        for (int i = 0; i < nd; i++) { wr_u32(offs[i]); wr_u8(vals[i]); }
    }

    memcpy(g_prevMap, cur, BRAINREC_MAP_TILES);
    g_hasPrevMap = true;
    g_frameCount++;
    return keyframe;
}

/* Fill one bot's recorder input from its pathfinder and tank. */
static void fillDijBot(ServerSim *sim, BYTE slot, BrainRecDijBotIn *in) {
    memset(in, 0, sizeof *in);
    TankInfo ti;
    if (serverSimGetTankInfo(sim, slot, &ti) && ti.has_tank && ti.alive) {
        in->hasTank = true;
        in->onBoat  = ti.on_boat;
        in->tx      = ti.map_x;
        in->ty      = ti.map_y;
    }
    BrainPathfinder *pf = serverSimGetBotBrainPathfinder(sim, slot);
    if (!pf) return;
    in->danger = pf->danger_grid;
    for (int s = 0; s < DIJKSTRA_NUM_SLATES; s++) {
        const DijkstraSlate *ds = &pf->dij_slates[s];
        BrainRecDijSlateIn  *si = &in->slates[s];
        if (!ds->g_cost || !ds->dir_at) continue;
        si->active = true;
        si->g_cost = ds->g_cost;
        si->dir_at = ds->dir_at;
        si->meta.kind         = (int32_t)ds->kind;
        si->meta.src_x        = (uint8_t)ds->src_x;
        si->meta.src_y        = (uint8_t)ds->src_y;
        si->meta.in_boat      = (uint8_t)(ds->in_boat ? 1 : 0);
        si->meta.allow_boat   = (uint8_t)(ds->allow_boat ? 1 : 0);
        /* Start heading (PF_START_HEADING). has_src_heading 0 = no heading;
         * files made before this field was filled hold 0 here too. */
        si->meta.has_src_heading = (uint8_t)(ds->has_src_heading ? 1 : 0);
        si->meta.src_heading     = (uint8_t)(ds->src_heading & 0xFF);
        si->meta.danger_scale   = ds->danger_scale;
        si->meta.started_tick   = ds->started_tick;
        si->meta.done           = (uint8_t)(ds->done ? 1 : 0);
        si->meta.completed_tick = ds->completed_tick;
    }
}

/* v8 tail: the recorded Dijkstra box and danger box of every bot,
 * length-prefixed. */
static void writeDijkstra(ServerSim *sim, bool keyframe) {
    uint64_t t0 = SDL_GetPerformanceCounter();
    if (!g_dijPrev) {
        g_dijPrev = (BrainRecDijState *)malloc(sizeof *g_dijPrev);
        if (g_dijPrev) brainRecDijStateClear(g_dijPrev);
        keyframe = true;
    }
    static BrainRecDijBotIn botIn[MAX_TANKS];
    const BrainRecDijBotIn *bots[MAX_TANKS];
    for (int i = 0; i < MAX_TANKS; i++) {
        bots[i] = NULL;
        if (!serverSimIsBot(sim, (BYTE)i)) continue;
        fillDijBot(sim, (BYTE)i, &botIn[i]);
        bots[i] = &botIn[i];
    }
    if (!g_dijPrev || !brainRecDijEncodeFrame(g_dijPrev, bots, keyframe, &g_dijBuf)) {
        /* Out of memory: write an empty block, and start over with a
         * keyframe once memory is back. */
        wr_u32(0);
        free(g_dijPrev);
        g_dijPrev = NULL;
    } else {
        wr_u32((uint32_t)g_dijBuf.len);
        wr_buf(g_dijBuf.data, g_dijBuf.len);
        g_pfDijBytes += g_dijBuf.len;
    }
    g_pfDijMs += perf_ms(t0, SDL_GetPerformanceCounter());
}

/* Forget the Dijkstra memory: the next file starts on a keyframe. */
static void resetDijkstra(void) {
    free(g_dijPrev);
    g_dijPrev = NULL;
    brainRecDijBufFree(&g_dijBuf);
    g_pfDijBytes = 0;
    g_pfDijMs = 0;
}

void brainRecordTick(ServerSim *sim) {
    if (!g_enabled || !sim) return;
    if (!ensureOpen(sim))   return;

    uint32_t tick = serverSimGetTick(sim);
    if (tick == g_lastTick) return;   /* serverSimTick can run twice per frame */
    g_lastTick = tick;

    if (g_perfWallStart == 0) g_perfWallStart = SDL_GetPerformanceCounter();
    uint64_t _tp0 = SDL_GetPerformanceCounter();

    /* ── God-view world snapshot (no viewport cull) ── */
    SnapshotHeader      hdr;
    TankSnapshot        tanks[MAX_TANKS];
    ShellSnapshot       shells[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot tkexp[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot        bases[MAX_SNAPSHOT_BASES];
    PillSnapshot        pills[MAX_SNAPSHOT_PILLS];
    GameEvent           evt[MAX_SNAPSHOT_EVENTS];

    int slot0 = firstBotSlot(sim);
    serverSimBuildSnapshot(sim, (BYTE)(slot0 < 0 ? 0 : slot0), &hdr,
                           tanks, MAX_TANKS,
                           shells, MAX_SNAPSHOT_SHELLS,
                           tkexp, MAX_SNAPSHOT_TK_EXPLOSIONS,
                           bases, MAX_SNAPSHOT_BASES,
                           pills, MAX_SNAPSHOT_PILLS,
                           evt, MAX_SNAPSHOT_EVENTS,
                           true /* noCull: god view */);

    if (periodicKeyframeDue(sim)) brainRecOutNewBlock(g_out);
    wr_u32(BRAINREC_FRAME_MAGIC);
    wr_u32(tick);
    if (g_fileFrames == 0) g_fileFirstTick = tick;
    g_fileLastTick = tick;
    g_fileFrames++;

    wr_u8(hdr.tankCount);   wr_buf(tanks,  (size_t)hdr.tankCount  * sizeof(TankSnapshot));
    wr_u8(hdr.shellCount);  wr_buf(shells, (size_t)hdr.shellCount * sizeof(ShellSnapshot));

    /* Bases + pills: serverSimBuildSnapshot DELTA-encodes these (full only on a
     * sync tick, then only what changed), so the snapshot's base/pill arrays are
     * empty after tick 0. Read the FULL state straight from the sim every frame
     * instead — same as BrainTest's own recorder. (tanks/shells above are sent
     * in full each tick, so the snapshot is fine for them.) */
    {
        GameSim *gs = serverSimGetGameSim(sim);
        int nb = (gs && gs->bs) ? gs->bs->numBases : 0;
        if (nb > MAX_SNAPSHOT_BASES) nb = MAX_SNAPSHOT_BASES;
        wr_u8((uint8_t)nb);
        for (int i = 0; i < nb; i++) {
            BaseSnapshot bs2;
            bs2.owner  = gs->bs->item[i].owner;
            bs2.armour = gs->bs->item[i].armour;
            bs2.shells = gs->bs->item[i].shells;
            bs2.mines  = gs->bs->item[i].mines;
            wr_buf(&bs2, sizeof bs2);
        }
        int np = (gs && gs->pb) ? gs->pb->numPills : 0;
        if (np > MAX_SNAPSHOT_PILLS) np = MAX_SNAPSHOT_PILLS;
        wr_u8((uint8_t)np);
        for (int i = 0; i < np; i++) {
            PillSnapshot ps2;
            ps2.x     = gs->pb->item[i].x;
            ps2.y     = gs->pb->item[i].y;
            ps2.owner = gs->pb->item[i].owner;
            ps2.pillFlags = pillSetInTank(0, gs->pb->item[i].inTank);
            ps2.armour    = gs->pb->item[i].armour;
            wr_buf(&ps2, sizeof ps2);
        }
    }

    uint64_t _tp1 = SDL_GetPerformanceCounter();
    g_pfSnapMs += perf_ms(_tp0, _tp1);

    /* ── Map terrain (keyframe / delta) — also carries mines + boats ── */
    bool mapKeyframe = writeMap(sim);
    uint64_t _tp2 = SDL_GetPerformanceCounter();
    g_pfMapMs += perf_ms(_tp1, _tp2);

    /* ── Per-bot: think time, goal info, overlays, pool JSON ── */
    int botCount = 0;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsBot(sim, (BYTE)i)) botCount++;
    }
    wr_u8((uint8_t)botCount);

    for (int i = 0; i < MAX_TANKS; i++) {
        if (!serverSimIsBot(sim, (BYTE)i)) continue;
        BYTE slot = (BYTE)i;

        wr_u8(slot);

        /* A bot whose brain runs on a Bot Host: no brain here, so write the
         * hosted marker (brain_record_host.h). The host's own records
         * replace it when its files are merged after the game. */
        {
            uint16_t placeGen = 0;
            char hostName[32];
            hostName[0] = 0;
            if (g_hostedFn != NULL && g_hostedFn(slot, &placeGen, hostName)) {
                brainRecBufReset(&g_body);
                brainRecBodyEncodeHosted(&g_body, placeGen, hostName,
                                         serverSimGetLastProcessedInput(sim, slot));
                wr_buf(g_body.data, g_body.len);
                free(g_poolStash[slot]);
                g_poolStash[slot] = NULL;
                continue;
            }
        }

        double thinkMs = serverSimGetBotLastThinkMs(sim, slot);
        g_pfThinkMs += thinkMs;

        BrainGoalInfo gi;
        bool haveGoal = serverSimGetBotGoalInfo(sim, slot, &gi);

        /* Overlay (visualizer) commands the brain emitted this tick, written
         * packed (variable-length), minus the skip-listed categories
         * (label_overlays etc.) which are dropped from disk entirely.
         * -bd-noviz: write an empty block (the brain also stops emitting,
         * so ocount is 0 anyway -- this keeps the file valid regardless). */
        uint64_t _tb0 = SDL_GetPerformanceCounter();
        OverlayCmdBuffer *ovl = g_recViz ? serverSimGetBotOverlayCmds(sim, slot) : NULL;
        /* This slot's own skip set when it has one, else the fallback. */
        const bool *skip = (slot < MAX_TANKS && g_haveSkipSlot[slot]) ? g_skipVizSlot[slot]
                         : (g_haveSkip ? g_skipViz : NULL);
        uint64_t _tb1 = SDL_GetPerformanceCounter();

        /* Pool-breakdown JSON (what the BrainTest Pool Info panel renders).
         * -bd-nopool: skip the per-bot Lua JSON build entirely -- it's the
         * eval itself (string building + GC) that costs, not the bytes.
         * Round-robin mode captures ONE bot per frame. The string normally
         * arrives PREFETCHED from the bot's own worker thread (built in
         * parallel right after its think -- see runBotThinkJob), so this
         * serial section only writes; the inline eval is the fallback for
         * a budget-killed think or a bot that skipped its job. */
        char *pj = NULL;
        if (brainRecordPoolCaptureWanted(sim, slot)) {
            pj = g_poolStash[slot];
            g_poolStash[slot] = NULL;
            if (!pj) {
                uint64_t _te0 = SDL_GetPerformanceCounter();
                pj = serverSimBotEvalLuaString(sim, slot, brainRecordPoolExpr());
                brainRecordAddPoolEvalMs(slot,
                    perf_ms(_te0, SDL_GetPerformanceCounter()));
            }
            g_pfPoolBytes += pj ? strlen(pj) : 0;
        }

        /* The body is built by the encoder the Bot Host uses too, so a
         * hosted bot's merged entries are byte for byte what this writes. */
        brainRecBufReset(&g_body);
        g_pfOvlCmds += brainRecBodyEncode(&g_body, (float)thinkMs,
                                          haveGoal ? &gi : NULL, ovl, skip, pj);
        wr_buf(g_body.data, g_body.len);
        if (pj) free(pj);
        uint64_t _tb2 = SDL_GetPerformanceCounter();
        g_pfOvlMs  += perf_ms(_tb0, _tb1);
        g_pfPoolMs += perf_ms(_tb1, _tb2);
    }

    /* ── Per-player alliance bitmaps (v5) ── MAX_TANKS uint32s, written every
     * frame so playback can color pills/bases by the followed bot's REAL
     * alliances. Without these the replay only knows owner, so an ally's pill
     * fails playersIsAllie(owner, viewPlayer) and renders as enemy (red). */
    {
        GameSim *gsa = serverSimGetGameSim(sim);
        for (int i = 0; i < MAX_TANKS; i++) {
            uint32_t allie = (gsa && gsa->plyrs) ? (uint32_t)gsa->plyrs->item[i].allie : 0u;
            wr_u32(allie);
        }
    }

    /* ── On-map masks (v7) ── uint32 pills, then uint32 bases: bit n set means
     * pill (base) n, 0 based, is on the map. A scenario can remove an item for
     * good; its slot stays in the lists above with its last values, so without
     * these a replay would keep drawing it. */
    {
        GameSim *gsm = serverSimGetGameSim(sim);
        uint32_t pillMask = 0, baseMask = 0;
        int np = (gsm && gsm->pb) ? gsm->pb->numPills : 0;
        int nb = (gsm && gsm->bs) ? gsm->bs->numBases : 0;
        for (int i = 0; i < np && i < 32; i++) {
            if (pillsIsActive(&gsm->pb, (BYTE)(i + 1))) pillMask |= ((uint32_t)1u << i);
        }
        for (int i = 0; i < nb && i < 32; i++) {
            if (basesIsActive(&gsm->bs, (BYTE)(i + 1))) baseMask |= ((uint32_t)1u << i);
        }
        wr_u32(pillMask);
        wr_u32(baseMask);
    }

    /* ── Recorded Dijkstra box (v8) ── keyframe on the map's keyframes. */
    writeDijkstra(sim, mapKeyframe);

    /* Throttled flush: a zstd flush every ~64 frames puts every frame so far
     * in the file (a hard kill loses at most ~1-2s) without the ratio cost of
     * flushing every frame. A clean close also writes the seek table. */
    if (++g_sinceFlush >= 64) {
        brainRecOutFlush(g_out);
        g_sinceFlush = 0;
    }

    g_pfTicks++;
    if (g_pfTicks >= PERF_DUMP_TICKS) perf_dump(tick);
}

void brainRecordEndGame(void) {
    /* Close the finished game's file so it's complete on disk, and reset the
     * per-game state so the NEXT game (after a fresh brainRecordSetSessionDir)
     * opens a clean file anchored at its own first tick. Keeps g_enabled
     * (armed) and g_sessionDir (the host overwrites the dir per game). */
    closeFileWithTicks();
    g_sinceFlush   = 0;
    g_lastTick     = 0xFFFFFFFFu;
    g_failed       = false;
    g_openAttempts = 0;
    g_hasPrevMap   = false;
    g_frameCount   = 0;
    resetDijkstra();
    g_haveSkip     = false;
    memset(g_skipViz, 0, sizeof g_skipViz);
    memset(g_skipVizSlot, 0, sizeof g_skipVizSlot);
    memset(g_haveSkipSlot, 0, sizeof g_haveSkipSlot);
    if (g_perf) { fclose(g_perf); g_perf = NULL; }
    g_perfTried = false;
    g_perfWallStart = 0;
    g_pfSnapMs = g_pfMapMs = g_pfOvlMs = g_pfPoolMs = g_pfThinkMs = 0;
    g_pfOvlCmds = g_pfPoolBytes = 0;
    g_pfTicks = 0;
    perf_free_pool_stash();
}

void brainRecordShutdown(void) {
    closeFileWithTicks();
    g_sinceFlush = 0;
    g_enabled    = false;
    g_lastTick   = 0xFFFFFFFFu;
    g_hasPrevMap = false;
    g_frameCount = 0;
    resetDijkstra();
    g_haveSkip   = false;
    memset(g_skipViz, 0, sizeof g_skipViz);
    memset(g_skipVizSlot, 0, sizeof g_skipVizSlot);
    memset(g_haveSkipSlot, 0, sizeof g_haveSkipSlot);
    if (g_perf) { fclose(g_perf); g_perf = NULL; }
    g_perfTried = false;
    g_perfWallStart = 0;
    perf_free_pool_stash();
}

/* ── Block log compression (see brain_record.h) ─────────────────────── */

static bool logNameMatches(const char *name) {
    size_t n = strlen(name);
    if (strncmp(name, "print2_bot", 10) == 0 && n > 4 && strcmp(name + n - 4, ".log") == 0)
        return true;
    if ((strncmp(name, "player", 6) == 0 || strncmp(name, "brain_p", 7) == 0) &&
        n > 6 && strcmp(name + n - 6, ".jsonl") == 0)
        return true;
    return false;
}

/* Compress one file to <path>.gz and remove the plain file. */
static bool compressOneLog(const char *path) {
    char gz[1024], tmp[1024];
    if (snprintf(gz, sizeof gz, "%s.gz", path) >= (int)sizeof gz) return false;
    if (snprintf(tmp, sizeof tmp, "%s.gz.tmp", path) >= (int)sizeof tmp) return false;
    SDL_PathInfo pi;
    if (SDL_GetPathInfo(gz, &pi)) return false;   /* done before; leave both */

    FILE *in = fopen(path, "rb");
    if (!in) return false;
    gzFile out = gzopen(tmp, "wb6");
    if (!out) { fclose(in); return false; }
    bool ok = true;
    static const size_t CHUNK = 1u << 20;
    unsigned char *buf = (unsigned char *)malloc(CHUNK);
    if (!buf) ok = false;
    while (ok) {
        size_t got = fread(buf, 1, CHUNK, in);
        if (got == 0) break;
        if (gzwrite(out, buf, (unsigned)got) != (int)got) ok = false;
    }
    if (ferror(in)) ok = false;
    free(buf);
    fclose(in);
    if (gzclose(out) != Z_OK) ok = false;
    if (!ok || !SDL_RenamePath(tmp, gz)) {
        SDL_RemovePath(tmp);
        return false;
    }
    if (!SDL_RemovePath(path)) {
        /* Something still has the plain file open: keep it, drop the copy. */
        SDL_RemovePath(gz);
        return false;
    }
    return true;
}

/* The matching names are collected first and compressed after, so the
 * directory is not changed while it is being listed. */
typedef struct {
    char **names;
    int    n, cap;
} LogEnumCtx;

static SDL_EnumerationResult SDLCALL logEnumCb(void *ud, const char *dirname,
                                               const char *fname) {
    LogEnumCtx *c = (LogEnumCtx *)ud;
    (void)dirname;
    if (!logNameMatches(fname)) return SDL_ENUM_CONTINUE;
    if (c->n == c->cap) {
        int nc = c->cap ? c->cap * 2 : 32;
        char **nv = (char **)realloc(c->names, (size_t)nc * sizeof *nv);
        if (!nv) return SDL_ENUM_FAILURE;
        c->names = nv;
        c->cap = nc;
    }
    c->names[c->n] = SDL_strdup(fname);
    if (c->names[c->n]) c->n++;
    return SDL_ENUM_CONTINUE;
}

/* ── Block folder locks (see brain_record.h) ─────────────────────────── */

#define DIRLOCK_MAX 16
static SDL_InitState  g_dirLockState;
static SDL_Mutex     *g_dirLockMx = NULL;
static SDL_Condition *g_dirLockCv = NULL;
static char           g_dirLocked[DIRLOCK_MAX][128];

static bool dirLockInit(void) {
    if (SDL_ShouldInit(&g_dirLockState)) {
        g_dirLockMx = SDL_CreateMutex();
        g_dirLockCv = SDL_CreateCondition();
        SDL_SetInitialized(&g_dirLockState, g_dirLockMx != NULL && g_dirLockCv != NULL);
    }
    return g_dirLockMx != NULL && g_dirLockCv != NULL;
}

/* The last path part of dir: the block folder's name. */
static void dirKey(const char *dir, char *out, size_t sz) {
    size_t n = strlen(dir), s, len;
    while (n > 0 && (dir[n - 1] == '/' || dir[n - 1] == '\\')) n--;
    s = n;
    while (s > 0 && dir[s - 1] != '/' && dir[s - 1] != '\\') s--;
    len = n - s;
    if (len >= sz) len = sz - 1;
    memcpy(out, dir + s, len);
    out[len] = '\0';
}

static bool dirKeySame(const char *a, const char *b) {
    char ka[128], kb[128];
    if (!a || !a[0] || !b || !b[0]) return false;
    dirKey(a, ka, sizeof ka);
    dirKey(b, kb, sizeof kb);
    return ka[0] != '\0' && SDL_strcasecmp(ka, kb) == 0;
}

static int dirLockFind(const char *key) {
    for (int i = 0; i < DIRLOCK_MAX; i++) {
        if (g_dirLocked[i][0] && SDL_strcasecmp(g_dirLocked[i], key) == 0) return i;
    }
    return -1;
}

bool brainRecordDirLock(const char *dir) {
    char key[128];
    if (!dir || !dir[0]) return false;
    dirKey(dir, key, sizeof key);
    if (!key[0] || !dirLockInit()) return false;
    SDL_LockMutex(g_dirLockMx);
    for (;;) {
        if (dirLockFind(key) < 0) {
            for (int i = 0; i < DIRLOCK_MAX; i++) {
                if (!g_dirLocked[i][0]) {
                    SDL_strlcpy(g_dirLocked[i], key, sizeof g_dirLocked[i]);
                    SDL_UnlockMutex(g_dirLockMx);
                    return true;
                }
            }
        }
        SDL_WaitCondition(g_dirLockCv, g_dirLockMx);
    }
}

void brainRecordDirUnlock(const char *dir) {
    char key[128];
    int i;
    if (!dir || !dir[0]) return;
    dirKey(dir, key, sizeof key);
    if (!key[0] || !dirLockInit()) return;
    SDL_LockMutex(g_dirLockMx);
    i = dirLockFind(key);
    if (i >= 0) g_dirLocked[i][0] = '\0';
    SDL_BroadcastCondition(g_dirLockCv);
    SDL_UnlockMutex(g_dirLockMx);
}

static int compressLogsLocked(const char *dir);

int brainRecordCompressLogsNow(const char *dir) {
    if (!dir || !dir[0]) return 0;
    /* A Bot Host merge into this folder ends first. */
    bool locked = brainRecordDirLock(dir);
    int n = compressLogsLocked(dir);
    if (locked) brainRecordDirUnlock(dir);
    return n;
}

static int compressLogsLocked(const char *dir) {
    LogEnumCtx c;
    memset(&c, 0, sizeof c);
    SDL_EnumerateDirectory(dir, logEnumCb, &c);
    int done = 0;
    for (int i = 0; i < c.n; i++) {
        char path[1024];
        if (snprintf(path, sizeof path, "%s/%s", dir, c.names[i]) < (int)sizeof path &&
            compressOneLog(path))
            done++;
        SDL_free(c.names[i]);
    }
    free(c.names);
    return done;
}

#define LOGZ_MAX_JOBS 8
typedef struct {
    SDL_Thread   *th;
    SDL_AtomicInt done;   /* the thread has finished its folder */
    char          dir[512];
} LogZJob;
static LogZJob g_logzJobs[LOGZ_MAX_JOBS];
static char    g_logzDeferred[512];

static int SDLCALL logzThreadFn(void *arg) {
    LogZJob *j = (LogZJob *)arg;
    int n = brainRecordCompressLogsNow(j->dir);
    fprintf(stderr, "brain_record: compressed %d log file(s) in %s\n", n, j->dir);
    SDL_SetAtomicInt(&j->done, 1);
    return 0;
}

bool brainRecordLogCompressPending(const char *dir) {
    if (!dir || !dir[0]) return false;
    if (dirKeySame(dir, g_sessionDir)) return true;    /* the open block */
    if (dirKeySame(dir, g_logzDeferred)) return true;  /* waits for the next block */
    for (int i = 0; i < LOGZ_MAX_JOBS; i++) {
        if (g_logzJobs[i].th && SDL_GetAtomicInt(&g_logzJobs[i].done) == 0 &&
            dirKeySame(dir, g_logzJobs[i].dir))
            return true;
    }
    return false;
}

void brainRecordCompressLogsAsync(const char *dir) {
    if (!dir || !dir[0]) return;
    /* The print2 writer keeps its files open; close them first. */
    brainCoreReleaseLogFiles();
    int slot = -1;
    /* A finished thread gives back its slot first. Without this every slot
     * stays taken after eight folders, and each new folder then waits on
     * the tick thread for slot 0's gzip, which can be the newest one. */
    for (int i = 0; i < LOGZ_MAX_JOBS; i++) {
        if (g_logzJobs[i].th != NULL &&
            SDL_GetAtomicInt(&g_logzJobs[i].done) != 0) {
            SDL_WaitThread(g_logzJobs[i].th, NULL);
            g_logzJobs[i].th = NULL;
        }
    }
    for (int i = 0; i < LOGZ_MAX_JOBS; i++) {
        if (g_logzJobs[i].th == NULL) { slot = i; break; }
    }
    if (slot < 0) {
        /* All slots busy: wait for the oldest one. Blocks lasting 15
         * minutes make this rare. */
        SDL_WaitThread(g_logzJobs[0].th, NULL);
        g_logzJobs[0].th = NULL;
        slot = 0;
    }
    LogZJob *j = &g_logzJobs[slot];
    SDL_strlcpy(j->dir, dir, sizeof j->dir);
    SDL_SetAtomicInt(&j->done, 0);
    j->th = SDL_CreateThread(logzThreadFn, "brainrec_logz", j);
    if (!j->th) {
        /* No thread: do it here rather than not at all. */
        brainRecordCompressLogsNow(dir);
    }
}

void brainRecordDeferLogCompress(const char *dir) {
    if (!dir || !dir[0]) return;
    if (g_logzDeferred[0] && strcmp(g_logzDeferred, dir) != 0)
        brainRecordCompressLogsAsync(g_logzDeferred);
    SDL_strlcpy(g_logzDeferred, dir, sizeof g_logzDeferred);
}

void brainRecordRunDeferredLogCompress(void) {
    if (!g_logzDeferred[0]) return;
    char dir[512];
    SDL_strlcpy(dir, g_logzDeferred, sizeof dir);
    g_logzDeferred[0] = '\0';
    brainRecordCompressLogsAsync(dir);
}

void brainRecordFinishLogCompress(void) {
    brainRecordRunDeferredLogCompress();
    for (int i = 0; i < LOGZ_MAX_JOBS; i++) {
        if (g_logzJobs[i].th) {
            SDL_WaitThread(g_logzJobs[i].th, NULL);
            g_logzJobs[i].th = NULL;
        }
    }
}
