#pragma once

#ifdef __APPLE__

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Window;

/* Build and install the native macOS NSMenu on NSApp.mainMenu.
 * Safe to call once after the main SDL_Window and ImGui context exist.
 * `win` is the main game window (saved for future menu items that need
 * to drive AppKit on a specific window); `clientSim` is the ClientSim
 * pointer that future trampolines will route to. Either may be NULL —
 * the skeleton phase only wires Quit, which needs neither. Callers that
 * do not have a ClientSim in scope should pass NULL and route the
 * pointer through a separate setter once it is live. */
void mac_menubar_install(struct SDL_Window *win, void *clientSim);

#ifdef __cplusplus
}
#endif

#endif /* __APPLE__ */
