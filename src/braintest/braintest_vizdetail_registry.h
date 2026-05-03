/*********************************************************
 * BrainTest viz-detail registry — per-tick map of clickable
 * primitives the brain has annotated with rich text. The
 * brain calls overlay_detail(detail_id, kind, geom, label)
 * each tick to register a region; overlay_detail_text(id,
 * line) appends body lines. The dialog (D key) lists every
 * registered id; the map click handler hit-tests
 * geometries to scroll/expand the matching entry.
 *
 * Cleared at the START of each brain tick so stale entries
 * never display. Registry is shared across all bots — the
 * caller (BrainTest main) clears once per think batch.
 *********************************************************/

#ifndef BRAINTEST_VIZDETAIL_REGISTRY_H
#define BRAINTEST_VIZDETAIL_REGISTRY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VIZDETAIL_REG_MAX           512
#define VIZDETAIL_ID_MAX            64
#define VIZDETAIL_LABEL_MAX         128
#define VIZDETAIL_BODY_LINES_MAX    32
#define VIZDETAIL_BODY_LINE_MAX     192

typedef enum {
    VIZDETAIL_KIND_RECT   = 0,
    VIZDETAIL_KIND_CIRCLE = 1,
    VIZDETAIL_KIND_TEXT   = 2,
} VizDetailKind;

typedef struct {
    char         id[VIZDETAIL_ID_MAX];
    VizDetailKind kind;
    /* Geometry in TILE coords (1.0 = one tile). For RECT: (x1,y1)-
     * (x2,y2). For CIRCLE: (x1,y1) center, x2 = radius (y2 unused).
     * For TEXT: (x1,y1) anchor (y2 unused). All in float so sub-tile
     * primitives register at full precision. */
    float        x1, y1, x2, y2;
    char         label[VIZDETAIL_LABEL_MAX];
    char         body[VIZDETAIL_BODY_LINES_MAX][VIZDETAIL_BODY_LINE_MAX];
    int          body_line_count;
    int          bot_owner;
} VizDetailEntry;

/* Clear all entries (called by host at the start of each think batch). */
void vizDetailRegistryClear(void);

/* Register or update by id. Returns the entry index, or -1 on full
 * registry / bad input. If an entry with this id already exists, its
 * kind/geom/label are overwritten and body_line_count stays so
 * subsequent appends preserve the per-tick body chain. */
int  vizDetailRegister(int bot_owner, const char *id, VizDetailKind kind,
                        float x1, float y1, float x2, float y2,
                        const char *label);

/* Append a body line to the entry for `id`. If no entry yet, creates a
 * stub one with empty geometry so the body still surfaces (rare —
 * normally callers register first). */
int  vizDetailAppendBody(int bot_owner, const char *id, const char *line);

int  vizDetailCount(void);
const VizDetailEntry *vizDetailGet(int idx);

/* Lookup by id (string). Returns the entry index, -1 if missing.
 * Pass bot_owner >= 0 to restrict to that bot; -1 searches all bots
 * and returns the first match. */
int  vizDetailFindByID(const char *id, int bot_owner);

/* Hit-test: returns the index of the smallest-area entry whose
 * geometry contains the tile-space point (tx, ty). Smallest-area-
 * wins so nested primitives surface the inner one. -1 if no hit.
 * Pass bot_owner >= 0 to restrict hits to that bot; -1 for no filter. */
int  vizDetailHitTest(float tx, float ty, int bot_owner);

/* Playback override. While set, vizDetailCount/Get/FindByID/HitTest
 * read from the supplied frozen array instead of the live registry.
 * Used by the recording playback path: brain doesn't run during
 * scrub, so the inspector dialog would otherwise go blank — this
 * lets it show whatever the brain registered at the recorded tick.
 * The pointer must stay valid until ClearPlaybackView is called. */
void vizDetailSetPlaybackView(const VizDetailEntry *entries, int count);
void vizDetailClearPlaybackView(void);

#ifdef __cplusplus
}
#endif

#endif /* BRAINTEST_VIZDETAIL_REGISTRY_H */
