/*********************************************************
 *NAME:          brain_record.c
 *PURPOSE:       Server-side brain-decision recorder (see brain_record.h).
 *********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brain_record.h"
#include "server_sim.h"
#include "input_packet.h"
#include "brain_overlay.h"
#include "types.h"       /* MAP_ARRAY_SIZE, map */
#include "game_sim.h"    /* GameSim internals: ->mp->mapItem */

#define BRAINREC_MAP_TILES (MAP_ARRAY_SIZE * MAP_ARRAY_SIZE)

/* ---- module state ---------------------------------------------------- */
static bool     g_enabled  = false;
static FILE    *g_fp       = NULL;
static bool     g_failed   = false;   /* gave up opening (no dir / fopen err) */
static uint32_t g_lastTick = 0xFFFFFFFFu;
static int      g_openAttempts = 0;
static char     g_sessionDir[512] = {0};  /* resolved DEBUG_SESSION_DIR */

/* Map-terrain delta tracking. g_prevMap holds the last frame's terrain so a
 * non-keyframe frame writes only changed tiles. */
static BYTE     g_prevMap[BRAINREC_MAP_TILES];
static bool     g_hasPrevMap = false;
static uint32_t g_frameCount = 0;

/* Small write helpers — native byte order (same build both ends). */
static void wr_u8 (uint8_t v)  { fwrite(&v, 1, 1, g_fp); }
static void wr_u16(uint16_t v) { fwrite(&v, sizeof v, 1, g_fp); }
static void wr_u32(uint32_t v) { fwrite(&v, sizeof v, 1, g_fp); }
static void wr_f32(float v)    { fwrite(&v, sizeof v, 1, g_fp); }
static void wr_buf(const void *p, size_t n) { if (n) fwrite(p, 1, n, g_fp); }

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
 * the first bot's Lua state. Returns true once g_fp is ready. */
static bool ensureOpen(ServerSim *sim) {
    if (g_fp)     return true;
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

    g_fp = fopen(path, "wb");
    if (!g_fp) {
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
    fwrite(&hdr, sizeof hdr, 1, g_fp);

    fprintf(stderr, "brain_record: recording brain decisions to '%s'\n", path);
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
    wr_u8(hdr.baseCount);   wr_buf(bases,  (size_t)hdr.baseCount  * sizeof(BaseSnapshot));
    wr_u8(hdr.pillCount);   wr_buf(pills,  (size_t)hdr.pillCount  * sizeof(PillSnapshot));

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

        /* Overlay (visualizer) commands the brain emitted this tick. */
        OverlayCmdBuffer *ovl = serverSimGetBotOverlayCmds(sim, slot);
        uint32_t ocount = (ovl && ovl->count > 0) ? (uint32_t)ovl->count : 0;
        wr_u32(ocount);
        if (ocount) wr_buf(ovl->cmds, (size_t)ocount * sizeof(OverlayCmd));

        /* Pool-breakdown JSON (what the BrainTest Pool Info panel renders). */
        char *pj = serverSimBotEvalLuaString(sim, slot,
                                             "return brain.get_pool_breakdown_json()");
        uint32_t plen = pj ? (uint32_t)strlen(pj) : 0;
        wr_u32(plen);
        if (plen) wr_buf(pj, plen);
        if (pj) free(pj);
    }

    /* Flush each frame so a hard-killed server (Ctrl-C) still leaves a
     * loadable file up to the last completed tick. Debug-only cost. */
    fflush(g_fp);
}

void brainRecordShutdown(void) {
    if (g_fp) {
        fflush(g_fp);
        fclose(g_fp);
        g_fp = NULL;
    }
    g_enabled    = false;
    g_lastTick   = 0xFFFFFFFFu;
    g_hasPrevMap = false;
    g_frameCount = 0;
}
