/*********************************************************
 * braintest_panel_registry.h
 *
 * Generic "named text panel" registry. Brains call
 * `braintest_panel_register("name", "lua_expr")` at
 * brain.open(); BrainTest displays each registered panel
 * as a tab in its Q-toggled side window and polls the
 * Lua expression on the active tab at ~10 Hz.
 *
 * The Lua expression should `return <string>` — that
 * string becomes the tab's body. Format is up to the brain.
 *
 * Insertion-order indexing (same shape as the viz registry).
 * No INI persistence: panels come and go with the brain,
 * the only persistent state is the window's last-active
 * tab + size, kept in BrainTestPanels.ini.
 *********************************************************/

#ifndef BRAINTEST_PANEL_REGISTRY_H
#define BRAINTEST_PANEL_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ServerSim;

/* Accessor for the braintest binary's current sim, used by
 * panel renderers (under brains/<name>/braintest_panels/) that
 * need to call serverSim* wrappers but don't otherwise have
 * a sim handle. Returns the file-static sim mirror updated
 * each frame by braintest_main.c's panel-render path. */
struct ServerSim *braintestGetCurrentSim(void);

#define PANEL_REG_MAX           32
#define PANEL_REG_NAME_MAX      64
#define PANEL_REG_TYPE_MAX      24
#define PANEL_REG_EXPR_MAX     256
#define PANEL_REG_SHORTCUT_MAX   8

typedef struct {
    /* Player slot (0..MAX_TANKS-1) of the bot that registered
     * this panel. The window filters tabs by current followBot
     * so each bot's panels only show when that bot is being
     * watched. */
    int   bot_owner;
    char  name[PANEL_REG_NAME_MAX];
    /* Renderer the host should invoke for this panel's body.
     * Already namespaced by brain ("NewAutopilot:pool_grid")
     * so two bots with different schemas registering the same
     * short type don't collide. Unknown types fall back to
     * "text" with a one-time warning. */
    char  type[PANEL_REG_TYPE_MAX];
    char  lua_expr[PANEL_REG_EXPR_MAX]; /* "return brain.get_pool_breakdown_json()" */
    /* Optional keyboard shortcut. Empty → panel shows as a tab
     * in the main P window. Non-empty (single uppercase letter)
     * → panel gets its own SDL window toggled by that key. */
    char  shortcut[PANEL_REG_SHORTCUT_MAX];
} PanelRegistryEntry;

void  panelRegistryReset(void);
int   panelRegistryCount(void);
const PanelRegistryEntry *panelRegistryGet(int idx);

/* Append a panel. Dedup key is (bot_owner, name) — different
 * bots may register the same panel name independently; same
 * bot re-registering is idempotent. `type` defaults to "text"
 * when NULL/empty. Returns the entry index, or -1 if the
 * registry is full. */
int   panelRegistryAdd(int bot_owner,
                       const char *name,
                       const char *type,
                       const char *lua_expr,
                       const char *shortcut /* NULL or "" for tab */);

/* Find a panel by display name. Returns -1 if not present. */
int   panelRegistryFind(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* BRAINTEST_PANEL_REGISTRY_H */
