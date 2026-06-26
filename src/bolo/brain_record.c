/*********************************************************
 *NAME:          brain_record.c
 *PURPOSE:       Server-side brain-decision recorder (see brain_record.h).
 *********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "brain_record.h"
#include "server_sim.h"
#include "input_packet.h"
#include "brain_overlay.h"
#include "types.h"       /* MAP_ARRAY_SIZE, map */
#include "game_sim.h"    /* GameSim internals: ->mp->mapItem */
#include "players.h"     /* players / struct playersObj: per-player alliance bitmap */

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
     * label/filter recorded overlays. Evaluated off any bot (all share it). */
    char *legend = NULL;
    int lslot = firstBotSlot(sim);
    if (lslot >= 0) {
        legend = serverSimBotEvalLuaString(sim, (BYTE)lslot,
                                           "return brain.viz_legend_json()");
    }
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
            ps2.armourInTank = (uint8_t)((gs->pb->item[i].armour & 0x0F)
                                         | (gs->pb->item[i].inTank ? 0x10 : 0));
            wr_buf(&ps2, sizeof ps2);
        }
    }

    /* ── Map terrain (keyframe / delta) — also carries mines + boats ── */
    writeMap(sim);

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
        wr_f32((float)serverSimGetBotLastThinkMs(sim, slot));

        writeGoalInfo(sim, slot);

        /* Overlay (visualizer) commands the brain emitted this tick, written
         * packed (variable-length), minus the skip-listed categories
         * (label_overlays etc.) which are dropped from disk entirely. */
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

        /* Pool-breakdown JSON (what the BrainTest Pool Info panel renders). */
        char *pj = serverSimBotEvalLuaString(sim, slot,
                                             "return brain.get_pool_breakdown_json()");
        uint32_t plen = pj ? (uint32_t)strlen(pj) : 0;
        wr_u32(plen);
        if (plen) wr_buf(pj, plen);
        if (pj) free(pj);
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
}
