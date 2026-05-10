/*********************************************************
 * braintest_botwindow.h
 *
 * Per-panel SDL window + ImGui context for panels that
 * registered with a `shortcut` key. Each one is lazily
 * created on first toggle, hidden/shown on subsequent
 * key presses, and rendered every frame it's visible.
 *
 * Indexed by registry idx so the registry stays the
 * single source of truth — no parallel name table.
 *********************************************************/

#ifndef BRAINTEST_BOTWINDOW_H
#define BRAINTEST_BOTWINDOW_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Toggle (or create-and-show on first call) the bot window
 * for `registry_idx`. No-op if the entry doesn't have a
 * shortcut configured. Returns true on success. */
bool botWindowToggle(int registry_idx);

/* Forward an SDL event to every live bot window's ImGui
 * context. Cheap when no windows exist. */
void botWindowProcessEvent(SDL_Event *ev);

/* Render every visible bot window. Hidden by `followBot`
 * filter — windows whose owner != followBot get auto-hidden
 * (the user is no longer watching that bot). `onPollPanel`
 * is the same poll callback the P window uses. */
void botWindowRenderAll(int followBot,
                        char *(*onPollPanel)(int registry_idx));

/* Tear down every live window. Call at shutdown. */
void botWindowShutdownAll(void);

/* Returns true while any bot window's filter / text-input
 * field has keyboard focus. (Reserved for future use — we
 * don't have any input fields in bot windows yet, but
 * mirrors the V dialog API for symmetry.) */
bool botWindowWantsTextInput(void);

#ifdef __cplusplus
}
#endif

#endif /* BRAINTEST_BOTWINDOW_H */
