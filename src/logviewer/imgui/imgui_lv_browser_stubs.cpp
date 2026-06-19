/*
 * imgui_lv_browser_stubs.cpp - LogViewer stubs for the shared WBN browser dialog
 *
 * Copyright (c) 2024
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The main-game WBN browser dialog (src/gui/sdl3/dialogs/imgui_wbn_browser.cpp)
 * is reused in LogViewer so we don't carry two implementations. The dialog
 * pulls in a few symbols from the main-game frontend that LogViewer doesn't
 * link (bg_game, gamefront, imgui_winbolonet). This file provides the minimal
 * stubs needed to satisfy the linker.
 *
 * - bgGameGetShared returns NULL: the dialog's `hasBg` check then skips the
 *   animated background entirely. The two BgGame functions are unreachable
 *   at runtime but still need to resolve.
 * - gameFrontGetWinbolonetToken reads the same token the main game writes,
 *   straight from the shared WinBolo.ini.
 * - imguiWinbolonetDrawSection renders a short "sign in via WinBolo (in the
 *   main game)" message. LogViewer has no SRP login flow of its own.
 */

#include <cstdint>
#include <cstring>

#include <SDL3/SDL.h>
#include "imgui.h"
#include "platform_config.h"
#include "../../gui/lang.h"

extern "C" {

/* ---- bg_game stubs ----------------------------------------------- */
typedef struct BgGame BgGame;

BgGame *bgGameGetShared(void) { return nullptr; }

void bgGameTickFixed(BgGame *bg, Uint64 *lastTickTime) {
    (void)bg; (void)lastTickTime;
}

void bgGameRenderWithOverlay(BgGame *bg, SDL_Renderer *renderer,
                              int screenW, int screenH) {
    (void)bg; (void)renderer; (void)screenW; (void)screenH;
}

/* ---- gamefront token getter -------------------------------------- */
void gameFrontGetWinbolonetToken(char *token, char *expiry) {
    if (token)  token[0]  = '\0';
    if (expiry) expiry[0] = '\0';

    char buf[256] = {0};
    lv_platform_config_get_string("WINBOLO.NET", "Token",      "", buf, sizeof(buf));
    if (token) {
        /* Caller passes a buffer it expects to be large enough — we follow
         * the main-game prototype which uses FILENAME_MAX. */
        SDL_strlcpy(token, buf, FILENAME_MAX);
    }

    buf[0] = '\0';
    lv_platform_config_get_string("WINBOLO.NET", "TokenExpiry", "", buf, sizeof(buf));
    if (expiry) {
        SDL_strlcpy(expiry, buf, FILENAME_MAX);
    }
}

/* ---- imgui_winbolonet stub --------------------------------------- */
void imguiWinbolonetReset(void)             {}
void imguiWinbolonetStartValidation(void)   {}

void imguiWinbolonetDrawSection(bool inGame) {
    (void)inGame;
    ImGui::TextWrapped("%s", langGetText(STR_LV_INFO_SIGNIN_TO_COMMENT));
}

/* ---- on-screen keyboard support stubs ---------------------------- */
/* imgui_keyboard.cpp (the controller text-entry overlay) is shared into
 * LogViewer so the WBN browser's fields are editable, but the main-game
 * glyph atlas and gamepad-arbitration layer aren't linked here. Both
 * call sites NULL-check: a NULL gamepad handle skips the controller
 * cursor (mouse entry still works) and a NULL glyph falls back to the
 * text legend. */
SDL_Texture *glyphForActionAuto(const char *action_name) {
    (void)action_name;
    return nullptr;
}

SDL_Gamepad *inputGamepadGetActiveHandle(void) {
    return nullptr;
}

} /* extern "C" */
