/*********************************************************
 * BrainTest P window — tabbed panels populated by the
 * brain via braintest_panel_register. Each tab polls its
 * registered Lua expression at ~10 Hz; only the active tab
 * is polled so the cost stays bounded regardless of how
 * many panels the brain advertises.
 *********************************************************/

#ifndef BRAINTEST_PANELWINDOW_H
#define BRAINTEST_PANELWINDOW_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void panelWindowInit(SDL_Window *window, SDL_Renderer *renderer);
void panelWindowShutdown(void);
void panelWindowProcessEvent(SDL_Event *ev);

/* Render one frame.
 *  - followBot: only show tabs whose bot_owner matches; cache
 *    is cleared whenever this changes (different bot = different
 *    data shape, no point keeping stale text).
 *  - onPollPanel: invoked when the active tab is due for a
 *    refresh. Returns a malloc'd string the window takes
 *    ownership of (NULL = no refresh this tick). The panel_idx
 *    is the absolute index into the registry (not a filtered
 *    sub-index) so the host can look up the entry's lua_expr. */
void panelWindowRender(SDL_Renderer *renderer, int winW, int winH,
                       int followBot,
                       char *(*onPollPanel)(int panel_idx));

#ifdef __cplusplus
}
#endif

#endif /* BRAINTEST_PANELWINDOW_H */
