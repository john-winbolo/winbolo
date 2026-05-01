/*********************************************************
 * braintest_viz_registry.h
 *
 * Runtime list of viz toggles. Each entry pairs a viz_id
 * (the string the brain stamps on overlay commands) with a
 * V-dialog row (label + short/long descriptions) and the
 * current on/off state.
 *
 * Brain code populates this at startup via the
 * `braintest_viz_register` Lua binding so BrainTest doesn't
 * need to know about any specific brain's viz_ids at
 * compile time. INI persistence keys by id string.
 *
 * Insertion order = stable index. Index is what gets stamped
 * onto OverlayCmd.viz_idx so the renderer can filter at
 * draw time. Indexes survive INI save/load because the
 * brain re-registers in the same order on startup.
 *********************************************************/

#ifndef BRAINTEST_VIZ_REGISTRY_H
#define BRAINTEST_VIZ_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VIZ_REG_MAX           256
#define VIZ_REG_ID_MAX         64
#define VIZ_REG_LABEL_MAX      96
#define VIZ_REG_SHORT_MAX     224
#define VIZ_REG_LONG_MAX      640
#define VIZ_REG_KEY_HINT_MAX    8

typedef struct {
    char  id[VIZ_REG_ID_MAX];               /* viz_id; "" for native rows */
    char  label[VIZ_REG_LABEL_MAX];         /* row name in V dialog */
    char  short_desc[VIZ_REG_SHORT_MAX];    /* one-liner */
    char  long_desc[VIZ_REG_LONG_MAX];      /* details paragraph */
    char  key_hint[VIZ_REG_KEY_HINT_MAX];   /* "1", "-", "H", ... */
    bool  default_on;
    bool  is_on;
    bool  from_brain;                        /* registered via Lua, not native */
} VizRegistryEntry;

void  vizRegistryReset(void);
int   vizRegistryCount(void);
const VizRegistryEntry *vizRegistryGet(int idx);
VizRegistryEntry       *vizRegistryGetMutable(int idx);

/* Look up by id string. Returns -1 if not present. */
int   vizRegistryFind(const char *id);

/* Append a row. If `id` already exists, returns its existing
 * index without adding (idempotent — multiple bots all call
 * the same register, only the first one creates the row).
 * Returns the entry index, or -1 if the registry is full. */
int   vizRegistryAddBrain(const char *id,
                          const char *label,
                          const char *short_desc,
                          const char *long_desc,
                          bool default_on);

/* Append a native row (e.g. Influence/Path). No viz_id; the
 * caller drives the bool externally and just uses the entry
 * for display + INI persistence under `ini_key`. */
int   vizRegistryAddNative(const char *label,
                           const char *short_desc,
                           const char *long_desc,
                           const char *key_hint,
                           bool default_on);

/* INI persistence keyed by id (or by label for native rows
 * with empty id). Atomic: read all then assign. */
void  vizRegistrySaveIni(const char *path);
void  vizRegistryLoadIni(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* BRAINTEST_VIZ_REGISTRY_H */
