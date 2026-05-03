/*********************************************************
 * braintest_panel_types.h
 *
 * Map from panel "type" string (e.g. "NewAutopilot:pool_grid")
 * to the C render function that draws it. Per-bot panel
 * modules in brains/<bot>/braintest_panels/*.cpp call
 * panelTypeRegister() at static-init time to add themselves;
 * panelwindow.cpp's dispatcher calls panelTypeFind() per
 * frame to route a panel's body text to the right renderer.
 *
 * Built-in types ("text", etc.) are registered explicitly
 * by BrainTest's main(). Per-bot types are auto-registered
 * by their compilation unit's static initializer running
 * before main().
 *********************************************************/

#ifndef BRAINTEST_PANEL_TYPES_H
#define BRAINTEST_PANEL_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

/* Poll cadence for panel data refresh — used by both the P-window
 * dispatcher (panelwindow.cpp) and the per-shortcut bot windows
 * (botwindow.cpp). 100ms / 10 Hz is a comfortable interactive rate
 * that doesn't burn CPU re-encoding the brain's view per render. */
#define PANEL_POLL_INTERVAL_MS 100

/* Render a panel's body.
 *  - registry_idx: stable identity of the panel within this
 *    process. Renderers that keep persistent UI state
 *    (selection, popup, animation) MUST key it by this idx
 *    so two bots' windows for the same type don't share
 *    state. Pass -1 from harnesses that have no idx.
 *  - body: whatever the brain's Lua expression most recently
 *    returned; for typed panels it's JSON the renderer
 *    parses with cJSON. NULL = no fresh data — renderer
 *    should draw a placeholder, not assert. */
typedef void (*PanelRenderFn)(int registry_idx, const char *body);

/* Register a renderer for `type_name`. Last-write-wins on
 * collision (intentional — a per-bot module can override a
 * built-in if it wants). NULL fn unregisters. */
void          panelTypeRegister(const char *type_name, PanelRenderFn fn);
PanelRenderFn panelTypeFind(const char *type_name);

#ifdef __cplusplus
}
#endif

#endif /* BRAINTEST_PANEL_TYPES_H */
