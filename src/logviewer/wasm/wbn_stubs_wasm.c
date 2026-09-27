/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * wbn_stubs_wasm.c - WinBolo.net stubs for the WASM log viewer
 *
 * The desktop viewer reaches WinBolo.net through http.c, the log browser
 * dialog and the comments panel. http.c carries a hard #error under
 * Emscripten because it embeds the WBN signing key, so none of the three
 * is built here. The ImGui panels this target does build still call into
 * them; these answer those calls with nothing.
 */

#include <string.h>

#include "winbolonet/http.h"
#include "gui/sdl3/dialogs/imgui_wbn_browser.h"
#include "imgui/imgui_comments.h"

/* imgui_main_menu.cpp re-creates the HTTP client after the log browser
 * closes. There is no client to create. */
bool httpCreate(void) {
    return false;
}

/* File > Browse WinBolo.net. The browser answers as if the user closed it
 * at once, so the menu does nothing. */
WbnBrowserResult imguiWbnBrowserShow(struct SDL_Window *window,
                                     struct SDL_Renderer *renderer) {
    WbnBrowserResult res;
    (void)window;
    (void)renderer;
    memset(&res, 0, sizeof(res));
    res.action = WBN_BROWSER_CLOSE;
    return res;
}

/* imgui_game_info.cpp hands the loaded log's WBN key to the comments panel,
 * and lvHostTeardownCommon shuts the panel down. There is no panel. */
void lv_imgui_comments_set_key(const char *wbnKey) {
    (void)wbnKey;
}

void lv_imgui_comments_shutdown(void) {
}
