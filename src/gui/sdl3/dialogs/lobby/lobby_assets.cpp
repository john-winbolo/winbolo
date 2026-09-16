/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 * Name:          lobby_assets.cpp
 * Purpose:       The lobby's cached icon and tank textures
 *                — the status, bot, lock, skull and reel
 *                transport glyphs plus the three tank
 *                sprites, keyed on the renderer that made
 *                them — and the helpers that turn a game
 *                type, an AI type and a time limit into the
 *                strings the lobby displays.
 *********************************************************/

#include <cstdio>  /* FILENAME_MAX — sizes the icon path buffers */

#include <SDL3/SDL.h>

#include "lobby_internal.h"
extern "C" {
#include "control_event.h"       /* lobbyScenarioMod — which of the two is playing */
#include "../../../lang.h"
#include "../../../gamefront.h"  /* gameFrontHostingScripts / gameFrontGetServerSim */
}

const char *lobbyGameTypeStr(gameType gt) {
    switch (gt) {
        case gameOpen:             return langGetText(STR_DLGGAMEINFO_OPEN);
        case gameTournament:       return langGetText(STR_DLGGAMEINFO_TOURN);
        case gameStrictTournament: return langGetText(STR_DLGGAMEINFO_STRICT);
        case gameScripted:         return langGetText(STR_DLGGAMEINFO_SCRIPTED);
        default:                   return langGetText(STR_UNKNOWN);
    }
}

/* Whether this client may change which scenario plays: the host, and not a
 * spectator. The test the reload button has always made, now shared with the
 * Choose button beside it. */
static bool lobbyScenarioMayChoose(ClientSim *cs) {
    return clientSimGetLobbyHostSlot(cs) == clientSimGetMyPlayerNum(cs) &&
           !clientSimIsSpectator(cs);
}

/* The scenario the lobby is running, under the map's own lines in both lobby
 * layouts. The description is prose, so it goes on lines of its own rather
 * than onto the single settings row, which is built from SameLine and ends
 * with a right-aligned badge.
 *
 * Two shapes, because there are two ways a scenario comes to be playing. One
 * that came with the map names itself and nothing else: the map is already
 * named on the line above. One the host picked plays over whichever map is
 * committed, and may be standing in front of that map's own script, so it
 * names both — the map, and what is playing on it.
 *
 * The source is 0 when the round has no scenario, which covers a map with no
 * script file and a map whose script this host declined alike: the server
 * answers the same either way. So when this client is the one hosting and has
 * the preference switched off, the line says that instead of nothing, rather
 * than leaving a player wondering where the scenario went.
 *
 * A host sees the Choose button in every one of these states except that one,
 * including on a plain map with nothing playing — that is the state a host is
 * in when they go looking for a scenario, so it is the last place the way in
 * should be missing. A joiner sees no button at all.
 *
 * The button only asks for the dialog. The dialog itself is drawn from the
 * lobby's own frame, beside the map chooser's window, because this line is
 * drawn inside the Map tab and a dialog that stopped being drawn when the
 * player changed tab would be open with no way back to it. */
void lobbyRenderScenarioLine(ClientSim *cs) {
    if (cs == NULL) return;

    bool    mayChoose = lobbyScenarioMayChoose(cs);
    uint8_t source    = clientSimGetLobbyScenarioSource(cs);

    if (source != 0) {
        const char *name = clientSimGetLobbyScenarioName(cs);
        const char *desc = clientSimGetLobbyScenarioDescription(cs);
        /* A scenario that named itself nothing still came from a file. */
        if (name[0] == '\0') name = clientSimGetLobbyScenarioFileName(cs);
        if (source == lobbyScenarioMod) {
            MessageArgs args = {};
            SDL_snprintf(args.string1, sizeof(args.string1), "%s",
                         clientSimGetMapName(cs));
            SDL_snprintf(args.string2, sizeof(args.string2), "%s", name);
            ImGui::TextWrapped("%s",
                langGetTextFmt(STR_DLGLOBBY_SCENARIO_ON_MAP, &args));
        } else {
            ImGui::TextWrapped("%s %s",
                               langGetText(STR_DLGLOBBY_SCENARIO_LBL), name);
        }
        if (desc[0] != '\0') {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
            ImGui::TextWrapped("%s", desc);
            ImGui::PopStyleColor();
        }
        /* The edit-reload-play loop, for the host who has the script file on
           the machine the server is running on. The server answers with a
           line addressed to whoever asked, including what a reload that
           failed says, so nothing is reported from here. */
        if (mayChoose) {
            if (ImGui::SmallButton(
                    langGetText(STR_DLGLOBBY_RELOAD_SCENARIO))) {
                clientSimNetSendLobbyReloadScenario(cs);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(
                    langGetText(STR_DLGLOBBY_CHOOSE_SCENARIO))) {
                lobbyScenarioChooserOpen();
            }
        }
        return;
    }
    if (!gameFrontHostingScripts && gameFrontGetServerSim() != NULL) {
        /* This machine's own server runs no scripts, so there is nothing for
           a chooser to pick that could play. Say why, and offer no button. */
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_SCRIPTS_OFF));
        ImGui::PopStyleColor();
        return;
    }
    if (mayChoose) {
        if (ImGui::SmallButton(langGetText(STR_DLGLOBBY_CHOOSE_SCENARIO))) {
            lobbyScenarioChooserOpen();
        }
    }
}

const char *lobbyAiTypeStr(uint8_t ai) {
    switch (ai) {
        case 0:  return langGetText(STR_NO);
        case 1:  return langGetText(STR_YES);
        case 2:  return langGetText(STR_DLGGAMEINFO_AIADV);
        case 3:  return langGetText(STR_DLGGAMEINFO_AIFULL);
        default: return langGetText(STR_UNKNOWN);
    }
}

void lobbyFormatTimeLimit(int32_t ticks, char *buf, int bufSize) {
    if (ticks <= 0) {
        SDL_snprintf(buf, bufSize, "%s", langGetText(STR_DLGGAMEINFO_UNLIMITED));
        return;
    }
    int totalSecs = ticks / 50;
    int hours = totalSecs / 3600;
    int mins = (totalSecs % 3600) / 60;
    int secs = totalSecs % 60;
    if (hours > 0) {
        MessageArgs args = {};
        args.number = hours;
        SDL_snprintf(args.string1, sizeof(args.string1), "%02d", mins);
        SDL_snprintf(args.string2, sizeof(args.string2), "%02d", secs);
        SDL_snprintf(buf, bufSize, "%s", langGetTextFmt(STR_DLGLOBBY_TIME_HMS, &args));
    } else if (mins > 0) {
        MessageArgs args = {};
        args.number = mins;
        SDL_snprintf(args.string1, sizeof(args.string1), "%02d", secs);
        SDL_snprintf(buf, bufSize, "%s", langGetTextFmt(STR_DLGLOBBY_TIME_MS, &args));
    } else {
        MessageArgs args = {};
        args.number = secs;
        SDL_snprintf(buf, bufSize, "%s", langGetTextFmt(STR_DLGLOBBY_TIME_S, &args));
    }
}

static LobbyIconCache s_icons = {};

/* Assets is a leaf: players, status, recap, reel and clipgif all read the
 * cached textures out of here and nothing here reaches back. */
LobbyIconCache *lobbyIcons(void) {
    return &s_icons;
}

/* stb_image entry points — defined in C, declared with C linkage
 * so the C++ linker finds them. Mirror of how imgui_welcome.cpp
 * pulls them in (its include of stb_image.h provides the same). */
extern "C" {
    unsigned char *stbi_load_from_memory(const unsigned char *, int,
                                         int *, int *, int *, int);
    void stbi_image_free(void *);
}

/* Minimal PNG loader (mirror of imgui_welcome.cpp's loadPng) — uses
 * SDL_IOFromFile + stb_image so it works on Android APK assets and
 * regular filesystem alike. */
static SDL_Texture *loadLobbyPng(SDL_Renderer *renderer, const char *filename) {
    SDL_IOStream *io = SDL_IOFromFile(filename, "rb");
    if (!io) {
        char path[512];
        SDL_snprintf(path, sizeof(path), "data/%s", filename);
        io = SDL_IOFromFile(path, "rb");
    }
    if (!io) return nullptr;
    Sint64 size = SDL_GetIOSize(io);
    if (size <= 0) { SDL_CloseIO(io); return nullptr; }
    unsigned char *buf = (unsigned char *)SDL_malloc((size_t)size);
    if (!buf) { SDL_CloseIO(io); return nullptr; }
    SDL_ReadIO(io, buf, (size_t)size);
    SDL_CloseIO(io);
    int w, h, ch;
    unsigned char *data = stbi_load_from_memory(buf, (int)size, &w, &h, &ch, 4);
    SDL_free(buf);
    if (!data) return nullptr;
    SDL_Surface *surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, data, w * 4);
    if (!surf) { stbi_image_free(data); return nullptr; }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_DestroySurface(surf);
    stbi_image_free(data);
    return tex;
}

/* The three tank getters below guard only on their own attempted flag —
 * a renderer swap is handled for them by lobbyLoadStatusIconsOnce, which
 * destroys these textures and clears those flags. Every path that reaches
 * a getter runs it first in the same frame: the only callers are in
 * lobbyRenderTeamGroupedPlayers, which loads the icons at its top and draws
 * the tank column further down, against the same renderer. */
SDL_Texture *lobbyGetTankSelf04Texture(SDL_Renderer *renderer) {
    if (s_icons.tankSelfAttempted) return s_icons.tankSelf04;
    s_icons.tankSelfAttempted = true;
    s_icons.tankSelf04 = loadLobbyPng(renderer, "svg/tank_self_04.png");
    if (s_icons.tankSelf04) {
        SDL_SetTextureScaleMode(s_icons.tankSelf04, SDL_SCALEMODE_LINEAR);
    }
    return s_icons.tankSelf04;
}

/* Red enemy tank — used next to player rows on teams different from
 * the local player's. Loaded lazily on first use, same pattern as
 * lobbyGetTankSelf04Texture. */
SDL_Texture *lobbyGetTankEvil04Texture(SDL_Renderer *renderer) {
    if (s_icons.tankEvilAttempted) return s_icons.tankEvil04;
    s_icons.tankEvilAttempted = true;
    s_icons.tankEvil04 = loadLobbyPng(renderer, "svg/tank_evil_04.png");
    if (s_icons.tankEvil04) {
        SDL_SetTextureScaleMode(s_icons.tankEvil04, SDL_SCALEMODE_LINEAR);
    }
    return s_icons.tankEvil04;
}

/* "Good" ally tank (yellow tone) — used to distinguish the local
 * player's own row from the rest of their team. Falls back to
 * tank_self_04 if the asset isn't there. */
SDL_Texture *lobbyGetTankGood04Texture(SDL_Renderer *renderer) {
    if (s_icons.tankGoodAttempted) return s_icons.tankGood04;
    s_icons.tankGoodAttempted = true;
    s_icons.tankGood04 = loadLobbyPng(renderer, "svg/tank_good_04.png");
    if (s_icons.tankGood04) {
        SDL_SetTextureScaleMode(s_icons.tankGood04, SDL_SCALEMODE_LINEAR);
    }
    return s_icons.tankGood04;
}

/* Friendly pillbox at full armour — the pill half of the lobby's
 * view-policy summary. Same lazy load and renderer-swap handling as
 * the tank sprites above. */
SDL_Texture *lobbyGetPillbox15Texture(SDL_Renderer *renderer) {
    if (s_icons.pillbox15Attempted) return s_icons.pillbox15;
    s_icons.pillbox15Attempted = true;
    s_icons.pillbox15 = loadLobbyPng(renderer, "svg/pillbox_good_15.png");
    if (s_icons.pillbox15) {
        SDL_SetTextureScaleMode(s_icons.pillbox15, SDL_SCALEMODE_LINEAR);
    }
    return s_icons.pillbox15;
}

/* Friendly base — the base half of the view-policy summary. */
SDL_Texture *lobbyGetBaseGoodTexture(SDL_Renderer *renderer) {
    if (s_icons.baseGoodAttempted) return s_icons.baseGood;
    s_icons.baseGoodAttempted = true;
    s_icons.baseGood = loadLobbyPng(renderer, "svg/base_good.png");
    if (s_icons.baseGood) {
        SDL_SetTextureScaleMode(s_icons.baseGood, SDL_SCALEMODE_LINEAR);
    }
    return s_icons.baseGood;
}

/* A forest square — the backdrop the summary's allies-in-trees entry draws
 * its tank on. forest.png rather than forest_single.png: both are a full
 * tile of foliage, and the plain one is what a run of trees is drawn from. */
SDL_Texture *lobbyGetForestTexture(SDL_Renderer *renderer) {
    if (s_icons.forestAttempted) return s_icons.forest;
    s_icons.forestAttempted = true;
    s_icons.forest = loadLobbyPng(renderer, "svg/forest.png");
    if (s_icons.forest) {
        SDL_SetTextureScaleMode(s_icons.forest, SDL_SCALEMODE_LINEAR);
    }
    return s_icons.forest;
}

/* Two-path load for a white-mask icon: relative to the working directory
 * first, then relative to the executable, which is where an installed build
 * keeps its data/ tree. Same fallback the coloured icons above do inline. */
static SDL_Texture *loadWhiteIcon(SDL_Renderer *renderer, const char *relPath,
                                  int iconPx) {
    SDL_Texture *tex = imguiLoadSvgIconWhite(renderer, relPath, iconPx);
    if (tex == nullptr) {
        char basePathBuf[FILENAME_MAX];
        const char *base = SDL_GetBasePath();
        if (base) {
            SDL_snprintf(basePathBuf, sizeof(basePathBuf), "%s%s", base, relPath);
            tex = imguiLoadSvgIconWhite(renderer, basePathBuf, iconPx);
        }
    }
    return tex;
}

/* The default ping marker — the icon half of the header summary's
 * smart-ping entry. An authored white alpha mask (see PingKindStyle in
 * src/gui/ping_kinds.h: every drawer tints it), so it loads through
 * loadWhiteIcon rather than loadLobbyPng.
 *
 * Rasterised at a fixed 32 px because the lazy getters take no scale: the
 * summary draws it at 16 logical px, so 32 still reads on a 2x display and
 * scales down cleanly below that. */
SDL_Texture *lobbyGetPingStandardTexture(SDL_Renderer *renderer) {
    if (s_icons.pingStandardAttempted) return s_icons.pingStandard;
    s_icons.pingStandardAttempted = true;
    s_icons.pingStandard = loadWhiteIcon(renderer, "data/ui/ping/standard.svg", 32);
    if (s_icons.pingStandard) {
        SDL_SetTextureScaleMode(s_icons.pingStandard, SDL_SCALEMODE_LINEAR);
    }
    return s_icons.pingStandard;
}

void lobbyLoadStatusIconsOnce(SDL_Renderer *renderer, float scale) {
    /* If we've loaded against this exact renderer already, nothing
     * to do. If the renderer pointer differs (game→lobby may have
     * recreated it; SDL3 textures don't survive that), destroy the
     * stale textures and reload. That covers the tank sprites too:
     * they live in the same cache but are loaded lazily by the getters
     * above, so clearing their attempted flags is what reloads them. */
    if (s_icons.attempted && s_icons.renderer == renderer) return;
    if (s_icons.attempted && s_icons.renderer != renderer) {
        if (s_icons.success)     { SDL_DestroyTexture(s_icons.success);     s_icons.success     = nullptr; }
        if (s_icons.error)       { SDL_DestroyTexture(s_icons.error);       s_icons.error       = nullptr; }
        if (s_icons.info)        { SDL_DestroyTexture(s_icons.info);        s_icons.info        = nullptr; }
        if (s_icons.settings)    { SDL_DestroyTexture(s_icons.settings);    s_icons.settings    = nullptr; }
        if (s_icons.botCpuGreen) { SDL_DestroyTexture(s_icons.botCpuGreen); s_icons.botCpuGreen = nullptr; }
        if (s_icons.botCpuRed)   { SDL_DestroyTexture(s_icons.botCpuRed);   s_icons.botCpuRed   = nullptr; }
        if (s_icons.botCpuGrey)  { SDL_DestroyTexture(s_icons.botCpuGrey);  s_icons.botCpuGrey  = nullptr; }
        if (s_icons.locked)      { SDL_DestroyTexture(s_icons.locked);      s_icons.locked      = nullptr; }
        if (s_icons.skull)       { SDL_DestroyTexture(s_icons.skull);       s_icons.skull       = nullptr; }
        if (s_icons.picture)     { SDL_DestroyTexture(s_icons.picture);     s_icons.picture     = nullptr; }
        if (s_icons.play)        { SDL_DestroyTexture(s_icons.play);        s_icons.play        = nullptr; }
        if (s_icons.pause)       { SDL_DestroyTexture(s_icons.pause);       s_icons.pause       = nullptr; }
        if (s_icons.tankSelf04)  { SDL_DestroyTexture(s_icons.tankSelf04);  s_icons.tankSelf04  = nullptr; }
        if (s_icons.tankEvil04)  { SDL_DestroyTexture(s_icons.tankEvil04);  s_icons.tankEvil04  = nullptr; }
        if (s_icons.tankGood04)  { SDL_DestroyTexture(s_icons.tankGood04);  s_icons.tankGood04  = nullptr; }
        if (s_icons.pillbox15)   { SDL_DestroyTexture(s_icons.pillbox15);   s_icons.pillbox15   = nullptr; }
        if (s_icons.baseGood)    { SDL_DestroyTexture(s_icons.baseGood);    s_icons.baseGood    = nullptr; }
        if (s_icons.forest)      { SDL_DestroyTexture(s_icons.forest);      s_icons.forest      = nullptr; }
        if (s_icons.pingStandard){ SDL_DestroyTexture(s_icons.pingStandard);s_icons.pingStandard= nullptr; }
        s_icons.tankSelfAttempted  = false;
        s_icons.tankEvilAttempted  = false;
        s_icons.tankGoodAttempted  = false;
        s_icons.pillbox15Attempted = false;
        s_icons.baseGoodAttempted  = false;
        s_icons.forestAttempted    = false;
        s_icons.pingStandardAttempted = false;
    }
    s_icons.attempted = true;
    s_icons.renderer  = renderer;

    int iconPx = (int)(18.0f * scale);
    if (iconPx < 16) iconPx = 16;

    struct {
        SDL_Texture **target;
        const char   *relPath;
    } icons[] = {
        { &s_icons.success,  "data/ui/dialog-success.svg" },
        { &s_icons.error,    "data/ui/dialog-error.svg" },
        { &s_icons.info,     "data/ui/dialog-info.svg" },
        { &s_icons.settings, "data/ui/settings.svg" },
        { &s_icons.botCpuGreen, "data/ui/bot-cpu-green.svg" },
        { &s_icons.botCpuRed,   "data/ui/bot-cpu-red.svg" },
        { &s_icons.botCpuGrey,  "data/ui/bot-cpu-grey.svg" },
    };

    for (int i = 0; i < (int)(sizeof(icons) / sizeof(icons[0])); i++) {
        *icons[i].target = imguiLoadSvgIcon(renderer, icons[i].relPath, iconPx);
        if (*icons[i].target == nullptr) {
            char basePathBuf[FILENAME_MAX];
            const char *base = SDL_GetBasePath();
            if (base) {
                SDL_snprintf(basePathBuf, sizeof(basePathBuf),
                             "%s%s", base, icons[i].relPath);
                *icons[i].target = imguiLoadSvgIcon(renderer, basePathBuf, iconPx);
            }
        }
    }

    /* Lock badge — rasterised as a white alpha mask so the lockBadge
     * theme color tints it at draw time (matches the previous orange
     * "[locked]" pill). */
    s_icons.locked = imguiLoadSvgIconWhite(renderer, "data/ui/mapeditor/locked.svg", iconPx);
    if (s_icons.locked == nullptr) {
        char basePathBuf[FILENAME_MAX];
        const char *base = SDL_GetBasePath();
        if (base) {
            SDL_snprintf(basePathBuf, sizeof(basePathBuf),
                         "%sdata/ui/mapeditor/locked.svg", base);
            s_icons.locked = imguiLoadSvgIconWhite(renderer, basePathBuf, iconPx);
        }
    }

    /* Skull for the recap scoreboard's death columns — a white alpha mask
     * like the lock badge, so the header can tint it to the text colour. */
    s_icons.skull = imguiLoadSvgIconWhite(renderer, "data/ui/skull.svg", iconPx);
    if (s_icons.skull == nullptr) {
        char basePathBuf[FILENAME_MAX];
        const char *base = SDL_GetBasePath();
        if (base) {
            SDL_snprintf(basePathBuf, sizeof(basePathBuf),
                         "%sdata/ui/skull.svg", base);
            s_icons.skull = imguiLoadSvgIconWhite(renderer, basePathBuf, iconPx);
        }
    }

    /* The reel's transport and export glyphs — white alpha masks like the two
     * above, so each button tints them to its surrounding text colour. */
    s_icons.picture = loadWhiteIcon(renderer, "data/ui/picture.svg", iconPx);
    s_icons.play    = loadWhiteIcon(renderer, "data/ui/play.svg", iconPx);
    s_icons.pause   = loadWhiteIcon(renderer, "data/ui/pause.svg", iconPx);
}
