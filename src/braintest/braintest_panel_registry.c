#include "braintest_panel_registry.h"
#include <string.h>

static PanelRegistryEntry g_panels[PANEL_REG_MAX];
static int                g_count = 0;

void panelRegistryReset(void) {
    g_count = 0;
    memset(g_panels, 0, sizeof(g_panels));
}

int panelRegistryCount(void) { return g_count; }

const PanelRegistryEntry *panelRegistryGet(int idx) {
    if (idx < 0 || idx >= g_count) return NULL;
    return &g_panels[idx];
}

int panelRegistryFind(const char *name) {
    /* Returns the FIRST entry with this name regardless of
     * owner. Used by debug-style "is this name claimed yet"
     * checks; per-owner dedup happens inside Add directly. */
    if (!name || !name[0]) return -1;
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_panels[i].name, name) == 0) return i;
    }
    return -1;
}

int panelRegistryAdd(int bot_owner,
                     const char *name,
                     const char *type,
                     const char *lua_expr,
                     const char *shortcut) {
    if (!name || !name[0] || !lua_expr || !lua_expr[0]) return -1;
    /* Dedup by (bot_owner, name): same bot re-registering same
     * name is idempotent; different bot registering same name
     * gets its own entry (filtered to that bot at render time). */
    for (int i = 0; i < g_count; i++) {
        if (g_panels[i].bot_owner == bot_owner
            && strcmp(g_panels[i].name, name) == 0) {
            return i;
        }
    }
    if (g_count >= PANEL_REG_MAX) return -1;
    PanelRegistryEntry *e = &g_panels[g_count];
    e->bot_owner = bot_owner;
    strncpy(e->name, name, PANEL_REG_NAME_MAX - 1);
    e->name[PANEL_REG_NAME_MAX - 1] = '\0';
    const char *tp = (type && type[0]) ? type : "text";
    strncpy(e->type, tp, PANEL_REG_TYPE_MAX - 1);
    e->type[PANEL_REG_TYPE_MAX - 1] = '\0';
    strncpy(e->lua_expr, lua_expr, PANEL_REG_EXPR_MAX - 1);
    e->lua_expr[PANEL_REG_EXPR_MAX - 1] = '\0';
    if (shortcut && shortcut[0]) {
        strncpy(e->shortcut, shortcut, PANEL_REG_SHORTCUT_MAX - 1);
        e->shortcut[PANEL_REG_SHORTCUT_MAX - 1] = '\0';
    } else {
        e->shortcut[0] = '\0';
    }
    return g_count++;
}
