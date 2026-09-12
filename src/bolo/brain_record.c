/*********************************************************
 *NAME:          brain_record.c
 *PURPOSE:       Server-side brain-decision recorder (see brain_record.h).
 *********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include <SDL3/SDL.h>   /* SDL_GetPerformanceCounter for the perf log */

#include "brain_record.h"
#include "server_sim.h"
#include "input_packet.h"
#include "brain_overlay.h"
#include "types.h"       /* MAP_ARRAY_SIZE, map */
#include "game_sim.h"    /* GameSim internals: ->mp->mapItem */
#include "players.h"     /* players / struct playersObj: per-player alliance bitmap */
#include "pillbox.h"     /* pillSetInTank — the pill flags byte */

#define BRAINREC_MAP_TILES (MAP_ARRAY_SIZE * MAP_ARRAY_SIZE)

/* ---- module state ---------------------------------------------------- */
/* The .btr is written as a streaming gzip: each frame is compressed on the fly
 * by gzwrite (no raw buffering, no compress-at-end). g_sinceFlush throttles
 * gzflush so a hard kill loses at most ~1-2s while keeping a good ratio. */
static bool     g_enabled  = false;
static gzFile   g_gz       = NULL;
static uint32_t g_sinceFlush = 0;
static bool     g_failed   = false;   /* gave up opening (no dir / gzopen err) */
static uint32_t g_lastTick = 0xFFFFFFFFu;
static int      g_openAttempts = 0;
static char     g_sessionDir[512] = {0};  /* resolved DEBUG_SESSION_DIR */
static bool     g_skipViz[256] = {0};     /* overlay viz_idx values to drop */
static bool     g_haveSkip = false;

/* Map-terrain delta tracking. g_prevMap holds the last frame's terrain so a
 * non-keyframe frame writes only changed tiles. */
static BYTE     g_prevMap[BRAINREC_MAP_TILES];
static bool     g_hasPrevMap = false;
static uint32_t g_frameCount = 0;

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
    fprintf(g_perf,
        "PERF t=%u wall_s=%.2f eff_hz=%.1f think_ms=%.2f rec_snap=%.3f "
        "rec_map=%.3f rec_ovl=%.3f rec_pool=%.3f ovl_cmds=%.0f pool_B=%.0f\n",
        tick, wallS, (double)g_pfTicks / (wallS > 0 ? wallS : 1),
        g_pfThinkMs / n, g_pfSnapMs / n, g_pfMapMs / n, g_pfOvlMs / n,
        g_pfPoolMs / n, (double)g_pfOvlCmds / n, (double)g_pfPoolBytes / n);
    fflush(g_perf);
    g_pfSnapMs = g_pfMapMs = g_pfOvlMs = g_pfPoolMs = g_pfThinkMs = 0;
    g_pfOvlCmds = g_pfPoolBytes = 0;
    g_pfTicks = 0;
    g_perfWallStart = SDL_GetPerformanceCounter();
}

/* Small write helpers — native byte order (same build both ends), streamed
 * through gzip. */
static void wr_u8 (uint8_t v)  { gzwrite(g_gz, &v, 1); }
static void wr_u16(uint16_t v) { gzwrite(g_gz, &v, (unsigned)sizeof v); }
static void wr_u32(uint32_t v) { gzwrite(g_gz, &v, (unsigned)sizeof v); }
static void wr_f32(float v)    { gzwrite(g_gz, &v, (unsigned)sizeof v); }
static void wr_buf(const void *p, size_t n) { if (n) gzwrite(g_gz, p, (unsigned)n); }

/* Write one overlay command in packed form: fixed numeric fields + a
 * length-prefixed text string (0 bytes when the command has no label, which is
 * ~99% of them). Replaces the raw 160-byte struct memcpy — same data, no dead
 * text padding. */
static void wr_overlay_packed(const OverlayCmd *c) {
    wr_u8((uint8_t)c->type);
    wr_f32(c->x1); wr_f32(c->y1); wr_f32(c->x2); wr_f32(c->y2);
    wr_f32(c->radius);
    wr_u8(c->r); wr_u8(c->g); wr_u8(c->b); wr_u8(c->a);
    wr_u8(c->anchor);
    wr_u8(c->viz_idx);
    uint8_t tl = 0;
    while (tl < OVERLAY_TEXT_MAX && c->text[tl] != '\0') tl++;
    wr_u8(tl);
    if (tl) wr_buf(c->text, tl);
}

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

/* Lazily open <sessionDir>/brainrec.btr. The session dir normally comes
 * straight from brainRecordSetSessionDir() (winbolods creates it under
 * -braindebug). As a fallback, if it wasn't set we read DEBUG_SESSION_DIR off
 * the first bot's Lua state. Returns true once g_gz is ready. */
static bool ensureOpen(ServerSim *sim) {
    if (g_gz)     return true;
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

    /* Streaming gzip. "wb1" = fastest deflate level — the data (zero-padded
     * structs, repetitive JSON) is highly compressible, so level 1 keeps most
     * of the ratio while minimising per-tick CPU on the sim thread. */
    g_gz = gzopen(path, "wb1");
    if (!g_gz) {
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
    gzwrite(g_gz, &hdr, (unsigned)sizeof hdr);

    /* One-time legend: overlay viz_idx -> category name, so the loader can
     * label/filter recorded overlays. Taken from the first bot that has a
     * legend (visualizer brains share the same one), so a non-viz bot in a
     * lower slot (e.g. a scripted test victim) doesn't blank it out. */
    char *legend = NULL;
    int lslot = firstVizBotSlot(sim, &legend);
    uint32_t llen = legend ? (uint32_t)strlen(legend) : 0u;
    gzwrite(g_gz, &llen, (unsigned)sizeof llen);
    if (llen) gzwrite(g_gz, legend, llen);
    if (legend) free(legend);

    /* Recorder skip set: viz_idx values the brain flags as too-heavy/cosmetic
     * (label_overlays, attack_scan_spots_all_pills). These overlay commands
     * are dropped from disk (still drawn live in BrainTest). CSV of indices. */
    char *skipcsv = NULL;
    if (lslot >= 0) {
        skipcsv = serverSimBotEvalLuaString(sim, (BYTE)lslot,
                                            "return brain.viz_record_skip_csv()");
    }
    int nskip = 0;
    if (skipcsv) {
        const char *p = skipcsv;
        while (*p) {
            int v = atoi(p);
            if (v >= 0 && v < 256 && !g_skipViz[v]) { g_skipViz[v] = true; g_haveSkip = true; nskip++; }
            while (*p && *p != ',') p++;
            if (*p == ',') p++;
        }
        free(skipcsv);
    }

    fprintf(stderr, "brain_record: recording brain decisions to '%s' "
                    "(viz legend %u bytes, %d skipped viz)\n",
            path, (unsigned)llen, nskip);
    return true;
}

/* Write the compact goal-info block for one bot. */
static void writeGoalInfo(ServerSim *sim, BYTE slot) {
    BrainGoalInfo gi;
    bool ok = serverSimGetBotGoalInfo(sim, slot, &gi);
    if (!ok) {
        /* empty kind + 0 candidates */
        char empty[32]; memset(empty, 0, sizeof empty);
        wr_buf(empty, 32);                 /* kind */
        wr_u32(0); wr_u32(0); wr_u32(0);   /* mx, my, target_id */
        wr_buf(empty, 32);                 /* substate */
        wr_u16(0);                         /* numCandidates */
        return;
    }
    wr_buf(gi.kind, 32);
    wr_u32((uint32_t)gi.mx);
    wr_u32((uint32_t)gi.my);
    wr_u32((uint32_t)gi.target_id);
    wr_buf(gi.substate, 32);

    int nc = gi.num_candidates;
    if (nc < 0) nc = 0;
    if (nc > BRAIN_GOAL_MAX_CANDIDATES) nc = BRAIN_GOAL_MAX_CANDIDATES;
    wr_u16((uint16_t)nc);
    for (int i = 0; i < nc; i++) {
        BrainRecCandidate c;
        memset(&c, 0, sizeof c);
        memcpy(c.desc, gi.candidates[i].desc, sizeof c.desc);
        c.desc[sizeof c.desc - 1] = '\0';
        c.cost         = gi.candidates[i].cost;
        c.winner       = gi.candidates[i].winner ? 1 : 0;
        c.phase_weight = gi.candidates[i].phase_weight;
        wr_buf(&c, sizeof c);
    }
}

/* Write the terrain block: keyframe (full map) or delta (changed tiles only).
 * Keyframe on the first frame, every N frames, or when a huge fraction of the
 * map changed (reload / mass terrain event) so deltas stay small. The terrain
 * bytes carry mines + boats too, so this replays all three. */
static void writeMap(ServerSim *sim) {
    GameSim    *gs  = serverSimGetGameSim(sim);
    const BYTE *cur = (gs && gs->mp) ? &gs->mp->mapItem[0][0] : NULL;
    if (!cur) { wr_u8(0); wr_u32(0); return; }   /* no map this frame */

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

    wr_u32(BRAINREC_FRAME_MAGIC);
    wr_u32(tick);

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
    writeMap(sim);
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
        double thinkMs = serverSimGetBotLastThinkMs(sim, slot);
        g_pfThinkMs += thinkMs;
        wr_f32((float)thinkMs);

        writeGoalInfo(sim, slot);

        /* Overlay (visualizer) commands the brain emitted this tick, written
         * packed (variable-length), minus the skip-listed categories
         * (label_overlays etc.) which are dropped from disk entirely.
         * -bd-noviz: write an empty block (the brain also stops emitting,
         * so ocount is 0 anyway — this keeps the file valid regardless). */
        uint64_t _tb0 = SDL_GetPerformanceCounter();
        if (!g_recViz) {
            wr_u32(0);
        } else {
            OverlayCmdBuffer *ovl = serverSimGetBotOverlayCmds(sim, slot);
            uint32_t ocount = (ovl && ovl->count > 0) ? (uint32_t)ovl->count : 0;
            uint32_t kept = ocount;
            if (g_haveSkip && ocount) {
                kept = 0;
                for (uint32_t k = 0; k < ocount; k++)
                    if (!g_skipViz[ovl->cmds[k].viz_idx]) kept++;
            }
            wr_u32(kept);
            for (uint32_t k = 0; k < ocount; k++) {
                if (g_haveSkip && g_skipViz[ovl->cmds[k].viz_idx]) continue;
                wr_overlay_packed(&ovl->cmds[k]);
            }
            g_pfOvlCmds += kept;
        }
        uint64_t _tb1 = SDL_GetPerformanceCounter();
        g_pfOvlMs += perf_ms(_tb0, _tb1);

        /* Pool-breakdown JSON (what the BrainTest Pool Info panel renders).
         * -bd-nopool: skip the per-bot Lua JSON build entirely — it's the
         * eval itself (string building + GC) that costs, not the bytes.
         * Round-robin mode captures ONE bot per frame. The string normally
         * arrives PREFETCHED from the bot's own worker thread (built in
         * parallel right after its think — see runBotThinkJob), so this
         * serial section only writes; the inline eval is the fallback for
         * a budget-killed think or a bot that skipped its job. */
        if (!brainRecordPoolCaptureWanted(sim, slot)) {
            wr_u32(0);
        } else {
            char *pj = g_poolStash[slot];
            g_poolStash[slot] = NULL;
            if (!pj) {
                pj = serverSimBotEvalLuaString(sim, slot,
                         "return brain.get_pool_breakdown_json()");
            }
            uint32_t plen = pj ? (uint32_t)strlen(pj) : 0;
            wr_u32(plen);
            if (plen) wr_buf(pj, plen);
            if (pj) free(pj);
            g_pfPoolBytes += plen;
        }
        g_pfPoolMs += perf_ms(_tb1, SDL_GetPerformanceCounter());
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

    /* Throttled gzip flush: a Z_SYNC_FLUSH every ~64 frames keeps the on-disk
     * gzip valid (a hard kill loses at most ~1-2s) without the ratio hit of
     * flushing every frame. A clean shutdown gzcloses for the final bytes. */
    if (++g_sinceFlush >= 64) {
        gzflush(g_gz, Z_SYNC_FLUSH);
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
    if (g_gz) {
        gzclose(g_gz);
        g_gz = NULL;
    }
    g_sinceFlush   = 0;
    g_lastTick     = 0xFFFFFFFFu;
    g_failed       = false;
    g_openAttempts = 0;
    g_hasPrevMap   = false;
    g_frameCount   = 0;
    g_haveSkip     = false;
    memset(g_skipViz, 0, sizeof g_skipViz);
    if (g_perf) { fclose(g_perf); g_perf = NULL; }
    g_perfTried = false;
    g_perfWallStart = 0;
    g_pfSnapMs = g_pfMapMs = g_pfOvlMs = g_pfPoolMs = g_pfThinkMs = 0;
    g_pfOvlCmds = g_pfPoolBytes = 0;
    g_pfTicks = 0;
    perf_free_pool_stash();
}

void brainRecordShutdown(void) {
    if (g_gz) {
        gzclose(g_gz);
        g_gz = NULL;
    }
    g_sinceFlush = 0;
    g_enabled    = false;
    g_lastTick   = 0xFFFFFFFFu;
    g_hasPrevMap = false;
    g_frameCount = 0;
    g_haveSkip   = false;
    memset(g_skipViz, 0, sizeof g_skipViz);
    if (g_perf) { fclose(g_perf); g_perf = NULL; }
    g_perfTried = false;
    g_perfWallStart = 0;
    perf_free_pool_stash();
}
