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

/* Render a panel's body. The body string is whatever the
 * brain's Lua expression most recently returned; for typed
 * panels (pool_grid etc.) it'll be JSON the renderer
 * parses with cJSON. NULL = no fresh data — renderer should
 * draw a placeholder, not assert. */
typedef void (*PanelRenderFn)(const char *body);

/* Register a renderer for `type_name`. Last-write-wins on
 * collision (intentional — a per-bot module can override a
 * built-in if it wants). NULL fn unregisters. */
void          panelTypeRegister(const char *type_name, PanelRenderFn fn);
PanelRenderFn panelTypeFind(const char *type_name);

#ifdef __cplusplus
}
#endif

#endif /* BRAINTEST_PANEL_TYPES_H */
