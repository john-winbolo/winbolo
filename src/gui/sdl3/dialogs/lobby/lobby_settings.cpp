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
 * Name:          lobby_settings.cpp
 * Purpose:       The lobby's editable game-settings panel.
 *                The collapsing header and its open state —
 *                the Steam Deck default, and the fold-away
 *                and restore across the post-game view's
 *                edge — plus the form body that renders the
 *                game type, computer-player policy, hidden
 *                mines, time limit, visibility, open-host
 *                and password controls and dispatches each
 *                edit to the host. Visibility is a row of
 *                named sets plus a Details table that holds
 *                the seven settings behind them.
 *********************************************************/

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_internal.h"  /* ImGui::GetCurrentWindow — the body's font scale */
#include "lobby_internal.h"
#include "dialog_footer.h"   /* WBUI::DialogFooter — the Visibility popup's Close */
extern "C" {
#include "client_sim.h"
#include "client_net.h"  /* clientSimNetSendLobbyOpenHost / SetPassword */
#include "../../../lang.h"
#include "../../../ui_mode.h"  /* uiModeIsSteamDeck — the header's default state */
#include "../../../visibility_presets.h"  /* the named visibility sets */
#include "../../../gamefront.h"  /* the remembered preset and custom set */
#include "../../sdl3draw.h"    /* sdl3DrawGetRenderer — the summary's sprites */
}

/* ── Layout A — editable game settings panel ──────────────────────
 * Renders the four mockup setting groups (Game Type / AI / Other /
 * Time Limit). Locked settings render disabled with a lock badge.
 * Edits dispatch as PACKET_LOBBY_SET_SETTING via the new wire
 * commands. Host-only or anyone if openHost. */
/* Forward decl — the form body is defined just after the panel, but the
 * panel (and the controller Settings tab) call it. */
void lobbyRenderGameSettingsBody(ClientSim *cs, int myPlayerNum, float s);

/* Width for an InputInt that carries step buttons and still shows its
 * number. ImGui lays the two buttons out inside the item width — a frame
 * height each, with an inner spacing in front of each — and gives the field
 * whatever is left, which a flat width leaves as almost nothing once the
 * font is scaled up and the frames grow with it. So ask for the digits and
 * add the buttons on rather than the other way round. `widest` is the
 * longest value the box has to hold. */
static float lobbyStepInputWidth(const char *widest) {
    const ImGuiStyle &st = ImGui::GetStyle();
    return ImGui::CalcTextSize(widest).x + st.FramePadding.x * 2.0f
         + (ImGui::GetFrameHeight() + st.ItemInnerSpacing.x) * 2.0f;
}

/* ── Visibility ───────────────────────────────────────────────────
 * The lobby carries seven visibility values. The row on the form offers
 * them as five named sets plus Custom, and the Details popup shows the
 * same six as a table with the individual controls on the Custom row.
 *
 * None of that is on the wire. Every client already has all seven values,
 * so each one works out for itself which preset they add up to and they
 * all show the same row; picking a preset just sends the values behind
 * it. See visibility_presets.h. */

/* Every lock bit that covers a value a preset writes. If the server has
 * set any of them the presets are unavailable, because a preset is
 * all-or-nothing and half of one is not a preset. */
static const uint32_t kVisibilityLockMask =
    LOBBY_LOCK_PILL_VIEW | LOBBY_LOCK_BASE_VIEW | LOBBY_LOCK_ALLY_VIEW |
    LOBBY_LOCK_CLASSIC_MODE | LOBBY_LOCK_ALLIES_IN_TREES |
    LOBBY_LOCK_OVERVIEW_WINDOW | LOBBY_LOCK_LINE_OF_SIGHT;

/* The live lobby settings in the shape the preset table compares. */
static void lobbyVisibilityRead(ClientSim *cs, VisibilitySettings *out) {
    SDL_memset(out, 0, sizeof(*out));
    for (int c = 0; c < (int)VIEW_CATEGORY_COUNT; c++) {
        out->policy[c] = (uint8_t)clientSimGetViewPolicy(cs, (ViewCategory)c);
        out->decaySecs[c] = clientSimGetViewDecaySecs(cs, (ViewCategory)c);
    }
    out->classicMode    = clientSimGetClassicMode(cs);
    out->overviewWindow = clientSimGetOverviewWindow(cs);
    out->lineOfSight    = clientSimGetLineOfSight(cs);
    out->alliesInTrees  = clientSimGetAlliesInTrees(cs);
}

/* Sends one category's [policy][decay hi][decay lo]. Shared by the preset
 * apply and the Custom row's own controls so both clamp the same way. */
static void lobbyVisibilitySendView(ClientSim *cs, uint8_t lst, int policy,
                                    int secs) {
    if (secs < VIEW_DECAY_MIN_SECS) secs = VIEW_DECAY_MIN_SECS;
    if (secs > VIEW_DECAY_MAX_SECS) secs = VIEW_DECAY_MAX_SECS;
    uint8_t v[3] = { (uint8_t)policy,
                     (uint8_t)((secs >> 8) & 0xFF),
                     (uint8_t)(secs & 0xFF) };
    lobbySendSetting(cs, lst, v, 3);
}

/* Moves the lobby to want, one setting at a time and only where it
 * differs — an edit that changes nothing would still clear everybody's
 * ready flag.
 *
 * Classic mode goes first or last, never in the middle: while it is on
 * the server refuses an edit to any of the other six, so it has to come
 * off before they can be written. Going the other way there is nothing to
 * write at all — turning it on sets the other six itself, to exactly the
 * values the Classic row holds. */
static void lobbyVisibilityApply(ClientSim *cs, const VisibilitySettings *want) {
    VisibilitySettings cur;
    lobbyVisibilityRead(cs, &cur);

    if (cur.classicMode && !want->classicMode) {
        uint8_t off = 0;
        lobbySendSetting(cs, LST_CLASSIC_MODE, &off, 1);
        cur.classicMode = false;
    }
    if (!want->classicMode) {
        static const uint8_t lst[VIEW_CATEGORY_COUNT] = {
            LST_PILL_VIEW, LST_BASE_VIEW, LST_ALLY_VIEW
        };
        for (int c = 0; c < (int)VIEW_CATEGORY_COUNT; c++) {
            if (cur.policy[c] != want->policy[c] ||
                cur.decaySecs[c] != want->decaySecs[c]) {
                lobbyVisibilitySendView(cs, lst[c], (int)want->policy[c],
                                        (int)want->decaySecs[c]);
            }
        }
        if (cur.alliesInTrees != want->alliesInTrees) {
            uint8_t v = want->alliesInTrees ? 1 : 0;
            lobbySendSetting(cs, LST_ALLIES_IN_TREES, &v, 1);
        }
        if (cur.overviewWindow != want->overviewWindow) {
            uint8_t v = want->overviewWindow;
            lobbySendSetting(cs, LST_OVERVIEW_WINDOW, &v, 1);
        }
        if (cur.lineOfSight != want->lineOfSight) {
            uint8_t v = want->lineOfSight;
            lobbySendSetting(cs, LST_LINE_OF_SIGHT, &v, 1);
        }
    } else if (!cur.classicMode) {
        uint8_t on = 1;
        lobbySendSetting(cs, LST_CLASSIC_MODE, &on, 1);
    }
}

/* Puts a preset on the lobby. The decay seconds are not part of a preset,
 * so the lobby's own go back on before it is applied. */
static void lobbyVisibilityApplyPreset(ClientSim *cs, VisibilityPreset p) {
    VisibilitySettings want;
    VisibilitySettings cur;

    if (!visibilityPresetSettings(p, &want)) return;
    lobbyVisibilityRead(cs, &cur);
    for (int c = 0; c < (int)VIEW_CATEGORY_COUNT; c++) {
        want.decaySecs[c] = cur.decaySecs[c];
    }
    lobbyVisibilityApply(cs, &want);
}

/* The one-line version of a set, for the Custom row of the dropdown and
 * the lobby's header line. Short labels and the same value words the
 * controls use, with the two on/off rules named only while they are on,
 * so the stock set reads in about fifty characters. */
void lobbyVisibilityDetailsLine(const VisibilitySettings *v, char *out,
                                size_t outSize) {
    static const int kLabels[VIEW_CATEGORY_COUNT] = {
        STR_DLGLOBBY_VIS_SHORT_PILLS,
        STR_DLGLOBBY_VIS_SHORT_BASES,
        STR_DLGLOBBY_VIS_SHORT_ALLIES,
    };
    static const int kWords[4] = {
        STR_DLGLOBBY_VIEW_ALWAYS, STR_DLGLOBBY_VIEW_KEY,
        STR_DLGLOBBY_VIEW_DECAY,  STR_DLGLOBBY_VIEW_OFF,
    };
    /* Written as its bytes rather than as the character, because the
     * sources here are compiled without a source-encoding switch. */
    const char *sep = " \xC2\xB7 ";
    size_t used = 0;

    out[0] = '\0';
    for (int c = 0; c < (int)VIEW_CATEGORY_COUNT; c++) {
        int policy = (int)v->policy[c];
        if (policy < 0 || policy > (int)viewPolicyOff) policy = (int)viewPolicyAlways;
        if (policy == (int)viewPolicyDecay) {
            used += (size_t)SDL_snprintf(out + used, outSize - used, "%s%s %s %u %s",
                                         (c > 0) ? sep : "",
                                         langGetText(kLabels[c]),
                                         langGetText(kWords[policy]),
                                         (unsigned)v->decaySecs[c],
                                         langGetText(STR_DLGLOBBY_VIEW_DECAY_SECS));
        } else {
            used += (size_t)SDL_snprintf(out + used, outSize - used, "%s%s %s",
                                         (c > 0) ? sep : "",
                                         langGetText(kLabels[c]),
                                         langGetText(kWords[policy]));
        }
        if (used >= outSize) return;
    }
    used += (size_t)SDL_snprintf(out + used, outSize - used, "%s%s %s", sep,
                                 langGetText(STR_DLGLOBBY_VIS_SHORT_OVERVIEW),
                                 langGetText(
                                     v->overviewWindow ==
                                             (uint8_t)overviewWindowNone
                                         ? STR_DLGLOBBY_WINDOW_NONE
                                     : v->overviewWindow ==
                                             (uint8_t)overviewWindowClassic
                                         ? STR_DLGLOBBY_WINDOW_CLASSIC
                                         : STR_DLGLOBBY_WINDOW_EXPANDED));
    if (used >= outSize) return;
    if (v->lineOfSight != (uint8_t)lineOfSightOff) {
        used += (size_t)SDL_snprintf(out + used, outSize - used, "%s%s", sep,
                                     langGetText(STR_DLGLOBBY_LINE_OF_SIGHT_CB));
        if (used >= outSize) return;
    }
    if (v->alliesInTrees) {
        used += (size_t)SDL_snprintf(out + used, outSize - used, "%s%s", sep,
                                     langGetText(STR_DLGLOBBY_ALLIES_TREES_CB));
        if (used >= outSize) return;
    }
    if (v->classicMode) {
        SDL_snprintf(out + used, outSize - used, "%s%s", sep,
                     langGetText(STR_DLGLOBBY_CLASSIC_MODE_CB));
    }
}

/* What the dropdown shows for a set: the preset's name, or "Custom: " and
 * the line above. */
static void lobbyVisibilityLabel(const VisibilitySettings *v, char *out,
                                 size_t outSize) {
    VisibilityPreset p = visibilityPresetMatch(v);
    if (p != visibilityPresetCustom) {
        SDL_snprintf(out, outSize, "%s", langGetText(visibilityPresetNameId(p)));
        return;
    }
    char line[192];
    lobbyVisibilityDetailsLine(v, line, sizeof(line));
    SDL_snprintf(out, outSize, "%s: %s",
                 langGetText(STR_DLGLOBBY_PRESET_CUSTOM), line);
}

/* Keeps the INI's memory of the host's choice in step with the lobby.
 * Run every frame the form is drawn rather than off each control, so an
 * edit made anywhere — a preset, a Details control, another admin's
 * change on this same machine — is remembered the same way.
 *
 * Only the machine actually running the server writes: visiting somebody
 * else's lobby as an admin must not rewrite what this player hosts with.
 * The prefs setters only touch the INI when a value moves, so a frame
 * where nothing changed costs a compare. */
static void lobbyVisibilityRemember(ClientSim *cs, int myPlayerNum) {
    if (!lobbyIsHost(cs, myPlayerNum)) return;

    VisibilitySettings live;
    lobbyVisibilityRead(cs, &live);
    gameFrontRememberVisibility(&live);
}

/* ── The Details table's columns ──────────────────────────────────
 * One per setting, in the order they read: what you see of pillboxes,
 * of bases, of allied tanks, then the three rules that shape all of it.
 *
 * There is no classic-mode column. Classic mode forces exactly the
 * values the Classic row holds, so the row is classic mode: picking it
 * turns it on and picking any other row turns it off. A set with classic
 * mode on can therefore never read as anything but the Classic row,
 * which is why there is nothing here for a Custom row to switch on. */
typedef struct VisColumn {
    int      header;  /* column name, and which control the Custom row puts under it */
    uint32_t lockBit;
    int      tip;     /* hover on the header */
} VisColumn;

/* The count lives in lobby_internal.h: the browser and the in-game info
 * panel loop over the same columns through the renderer below. */
#define VIS_COLUMN_COUNT LOBBY_VIS_COLUMN_COUNT

static const VisColumn kVisColumns[VIS_COLUMN_COUNT] = {
    { STR_DLGLOBBY_VIEW_PILL, LOBBY_LOCK_PILL_VIEW,
      STR_DLGLOBBY_VIEW_POLICY_TIP },
    { STR_DLGLOBBY_VIEW_BASE, LOBBY_LOCK_BASE_VIEW,
      STR_DLGLOBBY_VIEW_POLICY_TIP },
    { STR_DLGLOBBY_VIEW_ALLY, LOBBY_LOCK_ALLY_VIEW,
      STR_DLGLOBBY_VIEW_POLICY_TIP },
    { STR_DLGLOBBY_ALLIES_TREES_CB, LOBBY_LOCK_ALLIES_IN_TREES,
      STR_DLGLOBBY_ALLIES_TREES_TIP },
    { STR_DLGLOBBY_OVERVIEW_WINDOW, LOBBY_LOCK_OVERVIEW_WINDOW,
      STR_DLGLOBBY_OVERVIEW_WINDOW_TIP },
    { STR_DLGLOBBY_LINE_OF_SIGHT_CB, LOBBY_LOCK_LINE_OF_SIGHT,
      STR_DLGLOBBY_LINE_OF_SIGHT_TIP },
};

/* Which sprite stands for a setting on the lobby's header line. The last
 * two rules have never had one — there is no square to draw for "what
 * blocks sight" — so they read as the word on its own. */
typedef enum {
    visIconPill = 0,
    visIconBase,
    visIconAlly,
    visIconTrees,   /* an allied tank on a forest square, drawn as a pair */
    visIconNone
} LobbyVisIcon;

/* ── The one place a visibility value is drawn ────────────────────
 * A sprite and the word the setting is on, side by side, faint together
 * when the setting is off. The lobby's header line is made of these and
 * so is every value cell of the Details table, from this one function, so
 * the two pictures of the same setting cannot drift apart.
 *
 * Centring a sprite against its word takes two drops, not one: text is
 * drawn at the cursor plus the line's text-base offset — which the lobby
 * header line sets, because it carries framed items — while an image is
 * drawn at the cursor itself, so the sprite starts that much high before
 * either has been centred at all. On top of that whichever of the two is
 * shorter takes half the difference in height, so the pair reads as one
 * row however the font is scaled.
 *
 * fallbackLabel names the setting in words for a build whose sprites
 * failed to load, so the line still says what belongs to what. Leaves the
 * whole thing as one item, so the caller's IsItemHovered covers it.
 * Passing NULL for word draws the sprite alone, which is what a Custom
 * row cell wants in front of its control. */
static void lobbyRenderVisibilityValue(LobbyVisIcon icon, const char *word,
                                       bool dim, int fallbackLabel, float s) {
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    ImGuiWindow  *win      = ImGui::GetCurrentWindow();
    const float iconSize = 16.0f * s;
    const float textH    = ImGui::GetTextLineHeight();
    const float imgDrop  = (textH > iconSize) ? (textH - iconSize) * 0.5f : 0.0f;
    const float textDrop = (iconSize > textH) ? (iconSize - textH) * 0.5f : 0.0f;
    SDL_Texture *tex     = NULL;
    SDL_Texture *tank    = NULL;

    /* Idempotent, and reloads the sprites if the renderer changed. The
     * header line draws before the player list, which is what usually
     * loads the cache, so it has to happen here. */
    if (renderer) lobbyLoadStatusIconsOnce(renderer, s);
    if (renderer) {
        switch (icon) {
        case visIconPill:  tex = lobbyGetPillbox15Texture(renderer);  break;
        case visIconBase:  tex = lobbyGetBaseGoodTexture(renderer);   break;
        case visIconAlly:  tex = lobbyGetTankGood04Texture(renderer); break;
        case visIconTrees:
            tex  = lobbyGetForestTexture(renderer);
            tank = lobbyGetTankGood04Texture(renderer);
            if (tank == NULL) tex = NULL;
            break;
        default: break;
        }
    }

    /* Off reads as "this setting is switched off" by being faint, rather
     * than by a colour the sprites would have to carry. */
    if (dim) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                            ImGui::GetStyle().Alpha * 0.45f);
    }
    ImGui::BeginGroup();
    const float baseY = ImGui::GetCursorPosY();
    if (tex) {
        ImGui::SetCursorPosY(baseY + win->DC.CurrLineTextBaseOffset + imgDrop);
        if (icon == visIconTrees) {
            /* The tank goes on at three quarters of the square, centred: a
             * full-size one covers the tile it is standing in, which is
             * the one thing this icon has to show it is not doing. */
            ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::Image((ImTextureID)tex, ImVec2(iconSize, iconSize));
            float inset = iconSize * 0.125f;
            ImGui::GetWindowDrawList()->AddImage(
                (ImTextureID)tank,
                ImVec2(at.x + inset, at.y + inset),
                ImVec2(at.x + iconSize - inset, at.y + iconSize - inset),
                ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f)));
        } else {
            ImGui::Image((ImTextureID)tex, ImVec2(iconSize, iconSize));
        }
    } else if (icon != visIconNone) {
        /* Sprite missing — the setting's own label stands in for it. */
        ImGui::SetCursorPosY(baseY + textDrop);
        ImGui::TextUnformatted(langGetText(fallbackLabel));
    }
    if (word != NULL) {
        if (tex || icon != visIconNone) ImGui::SameLine(0, 4.0f * s);
        ImGui::SetCursorPosY(baseY + textDrop);
        ImGui::TextUnformatted(word);
    }
    ImGui::EndGroup();
    if (dim) ImGui::PopStyleVar();
}

/* What a row of the table puts in column c: the sprite, the word and
 * whether the pair is faint. The three view columns read in the four
 * policy words; the two on/off rules read yes or no, the way the header
 * line has always said allies in trees; the overview window names its
 * mode and is never faint, because neither mode is "off".
 *
 * outWord and outDim take NULL, for the Custom row, which wants the
 * sprite and puts a control where the word would go. */
static void lobbyVisibilityCellValue(const VisibilitySettings *v, int c,
                                     LobbyVisIcon *outIcon,
                                     const char **outWord, bool *outDim) {
    const char *word;
    bool        dim;
    static const int policyWords[4] = {
        STR_DLGLOBBY_VIEW_ALWAYS, STR_DLGLOBBY_VIEW_KEY,
        STR_DLGLOBBY_VIEW_DECAY,  STR_DLGLOBBY_VIEW_OFF,
    };
    static const LobbyVisIcon viewIcons[3] = {
        visIconPill, visIconBase, visIconAlly
    };

    if (c <= 2) {
        uint8_t p = v->policy[c];
        if (p > (uint8_t)viewPolicyOff) p = (uint8_t)viewPolicyAlways;
        *outIcon = viewIcons[c];
        word = langGetText(policyWords[p]);
        dim  = (p == (uint8_t)viewPolicyOff);
    } else if (c == 3) {
        *outIcon = visIconTrees;
        word = langGetText(v->alliesInTrees ? STR_YES : STR_NO);
        dim  = !v->alliesInTrees;
    } else if (c == 4) {
        *outIcon = visIconNone;
        word = langGetText(
            v->overviewWindow == (uint8_t)overviewWindowNone
                ? STR_DLGLOBBY_WINDOW_NONE
                : v->overviewWindow == (uint8_t)overviewWindowClassic
                      ? STR_DLGLOBBY_WINDOW_CLASSIC
                      : STR_DLGLOBBY_WINDOW_EXPANDED);
        /* None is a named mode like the other two, not an off state, so it
         * is not faint. */
        dim  = false;
    } else {
        bool on = (v->lineOfSight != (uint8_t)lineOfSightOff);
        *outIcon = visIconNone;
        word = langGetText(on ? STR_YES : STR_NO);
        dim  = !on;
    }
    if (outWord != NULL) *outWord = word;
    if (outDim != NULL)  *outDim  = dim;
}

int lobbyVisibilityColumnLabelId(int column) {
    if (column < 0 || column >= VIS_COLUMN_COUNT) column = 0;
    return kVisColumns[column].header;
}

void lobbyVisibilityColumnText(const VisibilitySettings *v, int column,
                               char *out, size_t outSize) {
    LobbyVisIcon icon;
    const char  *word;
    bool         dim;

    if (out == NULL || outSize == 0) return;
    out[0] = 0;
    if (v == NULL || column < 0 || column >= VIS_COLUMN_COUNT) return;
    lobbyVisibilityCellValue(v, column, &icon, &word, &dim);
    /* Decay is the one value that is not a word on its own: how long it
     * lasts is the setting. Added here rather than at each caller so the
     * header line, the browser and the info panel all say it the same way.
     * No preset uses Decay, so a Details table row never takes this path. */
    if (column < (int)VIEW_CATEGORY_COUNT &&
        v->policy[column] == (uint8_t)viewPolicyDecay) {
        SDL_snprintf(out, outSize, "%s %u %s", word,
                     (unsigned)v->decaySecs[column],
                     langGetText(STR_DLGLOBBY_VIEW_DECAY_SECS));
    } else {
        SDL_snprintf(out, outSize, "%s", word);
    }
}

void lobbyRenderVisibilityColumn(const VisibilitySettings *v, int column,
                                 float s) {
    LobbyVisIcon icon;
    const char  *word;
    bool         dim;
    char         text[96];

    if (v == NULL || column < 0 || column >= VIS_COLUMN_COUNT) return;
    lobbyVisibilityCellValue(v, column, &icon, &word, &dim);
    lobbyVisibilityColumnText(v, column, text, sizeof(text));
    lobbyRenderVisibilityValue(icon, text, dim, kVisColumns[column].header, s);
}

/* The sprite and the gap after it, which every value column carries in
 * front of whatever else is in the cell. */
static float lobbyVisibilityIconWidth(float s) {
    return 16.0f * s + 4.0f * s;
}

/* One width for all six value columns: the widest thing any of them can
 * ever hold. They are all the same width on purpose — the table is read
 * down a column and across a row, and six different widths make both
 * harder — so this is worked out once and handed to every column.
 *
 * Worked out from the text rather than from a number, so a translation
 * that needs more room gets it. */
static float lobbyVisibilityValueColumnWidth(float s) {
    static const int valueWords[] = {
        STR_DLGLOBBY_VIEW_ALWAYS, STR_DLGLOBBY_VIEW_KEY,
        STR_DLGLOBBY_VIEW_DECAY,  STR_DLGLOBBY_VIEW_OFF,
        STR_YES, STR_NO,
        STR_DLGLOBBY_WINDOW_CLASSIC, STR_DLGLOBBY_WINDOW_EXPANDED,
        STR_DLGLOBBY_WINDOW_NONE,
    };
    const ImGuiStyle &st = ImGui::GetStyle();
    const float frame = ImGui::GetFrameHeight();
    const float iconW = lobbyVisibilityIconWidth(s);
    float widest = 0.0f;
    float w;
    float secs;
    int   i;

    for (i = 0; i < (int)(sizeof(valueWords) / sizeof(valueWords[0])); i++) {
        float t = ImGui::CalcTextSize(langGetText(valueWords[i])).x;
        if (t > widest) widest = t;
    }
    /* A preset row: sprite and word. The Custom row's combo holds the same
     * words inside a frame, with the arrow on the end, so it is the wider
     * of the two and decides the column. A tick box is narrower than
     * either. */
    w = iconW + widest + st.FramePadding.x * 2.0f + frame;

    /* The seconds box the Decay cell grows on its second line, which
     * starts at the cell's left edge rather than under the combo. */
    secs = lobbyStepInputWidth("000") + st.ItemInnerSpacing.x
         + ImGui::CalcTextSize(langGetText(STR_DLGLOBBY_VIEW_DECAY_SECS)).x;
    if (secs > w) w = secs;

    /* Room for a lock badge beside whatever is in the cell. */
    w += st.ItemInnerSpacing.x + frame;

    /* And never narrower than the widest column name, or the header would
     * be the one thing in the table that gets clipped. */
    for (i = 0; i < VIS_COLUMN_COUNT; i++) {
        float t = ImGui::CalcTextSize(langGetText(kVisColumns[i].header)).x;
        if (t > w) w = t;
    }
    return w;
}

/* A row's first cell: the radio that shows whether the lobby is on this
 * row, the row's name, and the whole of the cell as the thing you click
 * to pick it. Clicking a circle five pixels across to change the game is
 * needlessly fiddly, and the name beside it looks clickable anyway.
 *
 * The target is a Selectable laid down first, at the cell's full width
 * and a row high, which is what takes the hover and gives the cell the
 * highlight. The radio and the name are then drawn back over it from the
 * same cursor: they are picture only, because an item added second at the
 * same place does not take the hover from the one already there. So the
 * circle, the name and the blank space beside it are one target with one
 * highlight, and the radio still reads its checked state from the live
 * match rather than from anything this remembers.
 *
 * tip is what the row means, the same sentence the dropdown puts on its
 * entry for the same set. It is read off the target rather than off the
 * name, so it covers the whole cell the way the highlight does.
 *
 * Returns true only on a click that actually changes the row, so a caller
 * never sends a lobby-wide edit for the row that is already chosen. */
static bool lobbyVisibilityRowPick(const char *name, bool sel, bool disabled,
                                   const char *tip) {
    bool   clicked = false;
    ImVec2 start   = ImGui::GetCursorPos();
    float  cellW   = ImGui::GetContentRegionAvail().x;
    float  rowH    = ImGui::GetFrameHeight();

    if (disabled) ImGui::BeginDisabled();
    /* Never drawn as selected: the radio is what says which row is on, and
     * a filled cell behind a checked radio says it twice. */
    if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_None,
                          ImVec2(cellW, rowH))
        && !sel) {
        clicked = true;
    }
    /* Read before the radio and the name are drawn over it, and shown after
     * EndDisabled so a non-host still gets to read what the row does. */
    bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    ImGui::SetCursorPos(start);
    ImGui::RadioButton("##pick", sel);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name);
    if (disabled) ImGui::EndDisabled();
    if (hovered && tip != NULL && tip[0] != '\0') {
        ImGui::SetTooltip("%s", tip);
    }
    return clicked;
}

/* Column c of the Custom row: the live setting, editable, behind the same
 * sprite the preset rows above it carry so the column reads the same all
 * the way down. Each control is the one the panel has always used, one to
 * a cell now rather than a stack of rows. Classic mode holds every one of
 * them: while it is on the server refuses the edit, so the cell goes
 * disabled rather than letting the host click it for nothing. */
static void lobbyVisibilityCustomCell(ClientSim *cs, int c, bool effectiveHost,
                                      float colW, float s) {
    static const uint8_t viewLst[3] = {
        LST_PILL_VIEW, LST_BASE_VIEW, LST_ALLY_VIEW
    };
    const ImGuiStyle &st = ImGui::GetStyle();
    VisibilitySettings live;
    LobbyVisIcon icon;
    bool classic = clientSimGetClassicMode(cs);
    bool locked  = (clientSimGetLobbyServerLocks(cs)
                    & kVisColumns[c].lockBit) != 0;
    bool disable = !effectiveHost || locked || classic;
    bool hovered = false;
    float ctrlW;

    lobbyVisibilityRead(cs, &live);
    lobbyVisibilityCellValue(&live, c, &icon, NULL, NULL);

    /* What is left of the column once the lock badge's room — and the
     * sprite, in the four cells that carry one — are taken out of it. The
     * last two columns have no sprite, so they must not be charged for
     * one or their combo loses width it could have had. */
    ctrlW = colW - (st.ItemInnerSpacing.x + ImGui::GetFrameHeight());
    if (icon != visIconNone) ctrlW -= lobbyVisibilityIconWidth(s);
    if (ctrlW < ImGui::GetFrameHeight()) ctrlW = ImGui::GetFrameHeight();

    ImGui::PushID(c);
    if (disable) ImGui::BeginDisabled();
    /* The sprite is never faint here: the control beside it says what the
     * setting is, and dimming the pair would read as "disabled" on a row
     * the host is meant to edit. */
    if (icon != visIconNone) {
        lobbyRenderVisibilityValue(icon, NULL, false, kVisColumns[c].header, s);
        ImGui::SameLine(0, 4.0f * s);
    }
    if (c <= 2) {
        const char *modes[] = {
            langGetText(STR_DLGLOBBY_VIEW_ALWAYS),
            langGetText(STR_DLGLOBBY_VIEW_KEY),
            langGetText(STR_DLGLOBBY_VIEW_DECAY),
            langGetText(STR_DLGLOBBY_VIEW_OFF),
        };
        int policy = (int)live.policy[c];
        int secs   = (int)live.decaySecs[c];
        if (secs < VIEW_DECAY_MIN_SECS) secs = VIEW_DECAY_DEFAULT_SECS;

        ImGui::SetNextItemWidth(ctrlW);
        if (ImGui::Combo("##mode", &policy, modes, 4)) {
            lobbyVisibilitySendView(cs, viewLst[c], policy, secs);
        }
        hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        /* The seconds go on a second line inside the same cell, and only
         * while the cell is on Decay: beside the combo they would widen
         * the table for a value the other three modes never read. */
        if (policy == (int)viewPolicyDecay) {
            /* VIEW_DECAY_MAX_SECS is three digits; the box holds a fourth
             * to type into before the setter clamps it back. */
            ImGui::SetNextItemWidth(lobbyStepInputWidth("000"));
            if (ImGui::InputInt("##decay", &secs, 1, 5,
                                ImGuiInputTextFlags_EnterReturnsTrue)) {
                lobbyVisibilitySendView(cs, viewLst[c], policy, secs);
            }
            ImGui::SameLine(0, st.ItemInnerSpacing.x);
            ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_VIEW_DECAY_SECS));
        }
    } else if (c == 3) {
        bool trees = live.alliesInTrees;
        if (ImGui::Checkbox("##trees", &trees)) {
            uint8_t v = trees ? 1 : 0;
            lobbySendSetting(cs, LST_ALLIES_IN_TREES, &v, 1);
        }
        hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    } else if (c == 4) {
        const char *windows[] = {
            langGetText(STR_DLGLOBBY_WINDOW_EXPANDED),
            langGetText(STR_DLGLOBBY_WINDOW_CLASSIC),
            langGetText(STR_DLGLOBBY_WINDOW_NONE),
        };
        int window = (int)live.overviewWindow;
        ImGui::SetNextItemWidth(ctrlW);
        if (ImGui::Combo("##window", &window, windows,
                         (int)OVERVIEW_WINDOW_COUNT)) {
            uint8_t v = (uint8_t)window;
            lobbySendSetting(cs, LST_OVERVIEW_WINDOW, &v, 1);
        }
        hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    } else {
        bool sight = (live.lineOfSight != (uint8_t)lineOfSightOff);
        if (ImGui::Checkbox("##sight", &sight)) {
            uint8_t v = (uint8_t)(sight ? lineOfSightBuildingsAndTrees
                                        : lineOfSightOff);
            lobbySendSetting(cs, LST_LINE_OF_SIGHT, &v, 1);
        }
        hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    }
    /* The hover is read before the badge draws, so the tooltip belongs to
     * the control and not to the badge — which carries its own "locked by
     * the server" tooltip — and held until after EndDisabled so it draws
     * at full contrast rather than dimmed with the cell. */
    if (disable) ImGui::EndDisabled();
    if (locked) lobbyRenderLockBadge();
    /* Classic mode gets no badge — a server lock and a classic-mode
     * grey-out are different reasons for the same disabled cell, and only
     * the lock is badged. Say why in a tooltip instead, when classic mode
     * is the only thing holding it. */
    if (hovered && classic && !locked && effectiveHost) {
        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_CLASSIC_MODE_TIP));
    }
    ImGui::PopID();
}

/* Open state of the settings CollapsingHeader. File-scope rather than a
 * panel-local static because the lobby's post-game edge handler below
 * drives it from outside the panel. */
typedef struct LobbySettingsState {
    bool open     = true;
    bool openInit = false;
    /* Set while the header sits collapsed on the post-game view's initiative,
     * so the recap clearing knows there is something to put back. */
    bool autoCollapsed = false;
    bool preCollapse   = true;
    /* Last frame's post-game-view flag, so the handler below can act on the
     * transitions into and out of that view rather than every frame. */
    bool prevShowLastRound = false;
} LobbySettingsState;

static LobbySettingsState s_settings = {};

/* Default collapsed on the Steam Deck: its small screen needs the
 * vertical room for the player list and the Ready button, and the
 * map / game-type summary is already on the lobby's top status bar.
 * Desktop keeps it open. Runs from whichever of the panel or the
 * post-game edge handler comes first, so a first-frame init can't
 * clobber an auto-collapse that already happened. */
static void lobbySettingsHeaderInit(void) {
    if (s_settings.openInit) return;
    s_settings.open     = !uiModeIsSteamDeck();
    s_settings.openInit = true;
}

/* The post-game recap needs the vertical room the settings form takes,
 * so entering the post-game view folds the header away — once, on the
 * edge, never re-forced per frame, so the chevron still re-opens it.
 *
 * Restore policy on the way out (countdown clears the summary): put back
 * the pre-collapse state only if the header is still exactly as the
 * auto-collapse left it. Re-opening it during the recap clears the flag,
 * so a manual choice outranks the remembered state and survives into the
 * next round. */
void lobbySettingsPostGameEdge(bool showLastRound) {
    lobbySettingsHeaderInit();
    if (showLastRound && !s_settings.prevShowLastRound) {
        s_settings.preCollapse   = s_settings.open;
        /* Already collapsed → nothing was taken away, nothing to give back. */
        s_settings.autoCollapsed = s_settings.open;
        s_settings.open          = false;
    } else if (showLastRound) {
        if (s_settings.open) s_settings.autoCollapsed = false;
    } else if (s_settings.prevShowLastRound) {
        if (s_settings.autoCollapsed) s_settings.open = s_settings.preCollapse;
        s_settings.autoCollapsed = false;
    }
    s_settings.prevShowLastRound = showLastRound;
}

void lobbyRenderGameSettingsPanel(ClientSim *cs,
                                    int myPlayerNum, float s) {
    const bool spectator = clientSimIsSpectator(cs);
    bool effectiveHost = !spectator && (lobbyIsHost(cs, myPlayerNum) || clientSimGetLobbyOpenHost(cs) ||
                         (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                          (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                           & PLAYER_FLAG_ADMIN)));

    /* Non-privileged players get nothing — neither the form nor the
     * CollapsingHeader. The same map / game-type / pill-count /
     * AI-policy info is already on the lobby's top status bar, so
     * showing a disabled-out duplicate just clutters the view. */
    if (!effectiveHost) {
        return;
    }

    /* Drive the CollapsingHeader's open state explicitly so a "Hide
     * Settings" button at the bottom of the panel — and the post-game
     * auto-collapse — can fold it away once the host is happy with the
     * configuration. The chevron still toggles it either way; see
     * lobbySettingsHeaderInit / lobbySettingsPostGameEdge above. */
    lobbySettingsHeaderInit();
    ImGui::SetNextItemOpen(s_settings.open, ImGuiCond_Always);
    /* Capture screen-Y of the header before drawing so the
     * right-aligned openHost control can be overlaid on the same
     * line via SetCursorScreenPos. */
    ImVec2 headerStart = ImGui::GetCursorScreenPos();
    bool headerOpen = ImGui::CollapsingHeader(langGetText(STR_DLGLOBBY_SETTINGS_HEADER));

    /* When the host has flipped on "Allow all players to change
     * settings", surface that on the header so non-host players
     * understand why the form is interactive for them. Right-anchored
     * within the header's row using the captured headerStart Y plus
     * the row's right edge minus the label width. */
    if (clientSimGetLobbyOpenHost(cs)) {
        const char *noteText = langGetText(STR_DLGLOBBY_OPENHOST_NOTE);
        ImVec2 textSize = ImGui::CalcTextSize(noteText);
        float headerH   = ImGui::GetFrameHeight();
        float availW    = ImGui::GetWindowContentRegionMax().x
                        - ImGui::GetWindowContentRegionMin().x;
        ImVec2 winPos   = ImGui::GetWindowPos();
        float pad       = ImGui::GetStyle().FramePadding.x;
        ImVec2 notePos(
            winPos.x + ImGui::GetWindowContentRegionMin().x + availW
                - textSize.x - pad,
            headerStart.y + (headerH - textSize.y) * 0.5f);
        ImGui::GetWindowDrawList()->AddText(
            notePos, IM_COL32(180, 180, 180, 220), noteText);
    }

    if (!headerOpen) {
        s_settings.open = false;
        return;
    }
    s_settings.open = true;

    /* The openHost ("Allow all players to change settings") state is
     * intentionally invisible to regular players — they shouldn't even
     * know they got their edit access via that particular toggle, only
     * that the settings happen to be interactable for them. The actual
     * editable checkbox is rendered for host/admin only in the "Other"
     * column below. */

    lobbyRenderGameSettingsBody(cs, myPlayerNum, s);
}

/* The game-settings form proper (game type / AI policy / mines / time
 * limit / password). Split out of lobbyRenderGameSettingsPanel so the
 * controller Settings tab can render it flat, without the desktop
 * collapsing-header chrome. Host-gated by every caller. */
void lobbyRenderGameSettingsBody(ClientSim *cs, int myPlayerNum, float s) {
    const bool spectator = clientSimIsSpectator(cs);
    /* Same effective-host test the panel computes, recomputed here so the
     * per-control disabled state is identical whether the body renders in
     * the desktop collapsing header or the controller Settings tab. */
    bool effectiveHost = !spectator && (lobbyIsHost(cs, myPlayerNum) || clientSimGetLobbyOpenHost(cs) ||
                         (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                          (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                           & PLAYER_FLAG_ADMIN)));

    /* Remember what the visibility settings have been put on, so the next
     * game hosted from this machine starts there. Done once for the form
     * rather than off each control, so an edit from the dropdown, from the
     * Details popup or from another admin on this machine is all caught
     * the same way. */
    lobbyVisibilityRemember(cs, myPlayerNum);

    /* Settings body uses a smaller font than the rest of the lobby so
     * the 3-column form doesn't dominate the visual hierarchy. */
    float settingsOldScale = ImGui::GetCurrentWindow()->FontWindowScale;
    ImGui::SetWindowFontScale(settingsOldScale * 0.85f);

    ImGui::Spacing();
    /* Set by the Visibility button under the time limit below. The popup is
     * opened after the columns close, next to the BeginPopupModal that
     * answers it. */
    bool openVisibility = false;
    ImGui::Columns(3, "##settingsCols", false);

    /* ── Game Type ──────────────────────────────────────────── */
    bool gtLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_GAME_TYPE) != 0;
    {
        ImGui::Text("%s", langGetText(STR_DLGLOBBY_GAMETYPE_LBL));
        if (gtLocked) lobbyRenderLockBadge();
        bool disable = !effectiveHost || gtLocked;
        if (disable) ImGui::BeginDisabled();
        const char *items[] = {
            langGetText(STR_DLGGAMESETUP_RADIO1),
            langGetText(STR_DLGGAMESETUP_RADIO2),
            langGetText(STR_DLGGAMESETUP_RADIO3),
        };
        bool rankedNow = clientSimGetLobbyRanked(cs);
        for (int i = 0; i < 3; i++) {
            /* gameType enum is 1-based (gameOpen=1, gameTournament=2,
             * gameStrictTournament=3), so the array index → enum
             * mapping is i+1. The previous (gameType)i comparison
             * read the wrong row as "checked" — Open showed as
             * Unknown, Tournament showed as Open, etc. */
            int enumVal = i + 1;
            /* Ranked games forbid the "Open" type — grey it out. */
            bool optDisabled = rankedNow && (gameType)enumVal == gameOpen;
            if (optDisabled) ImGui::BeginDisabled();
            char rid[80];
            SDL_snprintf(rid, sizeof(rid), "%s##gt%d", items[i], i);
            bool checked = (clientSimGetLobbyGameType(cs) == (gameType)enumVal);
            if (ImGui::RadioButton(rid, checked) && !checked) {
                uint8_t v = (uint8_t)enumVal;
                lobbySendSetting(cs, LST_GAME_TYPE, &v, 1);
            }
            if (optDisabled) ImGui::EndDisabled();
        }
        if (disable) ImGui::EndDisabled();
    }
    ImGui::NextColumn();

    /* ── Computer Players ────────────────────────────────────── */
    bool aiLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_AI_POLICY) != 0;
    {
        ImGui::Text("%s", langGetText(STR_DLGLOBBY_AI_SECTION_LBL));
        if (aiLocked) lobbyRenderLockBadge();
        /* Ranked games force "No computer tanks" — disable the
         * whole AI block since none of the alternatives are valid. */
        bool disable = !effectiveHost || aiLocked
                        || clientSimGetLobbyRanked(cs);
        if (disable) ImGui::BeginDisabled();
        const char *items[] = {
            langGetText(STR_DLGLOBBY_AI_NONE),
            langGetText(STR_DLGLOBBY_AI_ALLOW),
            langGetText(STR_DLGLOBBY_AI_ADVANTAGE),
            langGetText(STR_DLGLOBBY_AI_FULLADV),
        };
        for (int i = 0; i < 4; i++) {
            /* Same trick as the Game Type radios — embed the label so
             * the whole row is clickable. */
            char rid[80];
            SDL_snprintf(rid, sizeof(rid), "%s##ai%d", items[i], i);
            bool checked = (clientSimGetLobbyAiType(cs) == (uint8_t)i);
            if (ImGui::RadioButton(rid, checked) && !checked) {
                uint8_t v = (uint8_t)i;
                lobbySendSetting(cs, LST_AI_POLICY, &v, 1);
            }
        }
        if (disable) ImGui::EndDisabled();
    }
    ImGui::NextColumn();

    /* ── Other (mines / time limit / autoLockOnGameStart) ────── */
    {
        ImGui::Text("%s", langGetText(STR_DLGLOBBY_OTHER_LBL));

        bool minesLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_MINES) != 0;
        bool minesV = clientSimIsLobbyHiddenMines(cs);
        bool minesDisabled = !effectiveHost || minesLocked;
        if (minesDisabled) ImGui::BeginDisabled();
        if (ImGui::Checkbox(langGetText(STR_DLGGAMESETUP_HIDDENMINES), &minesV)) {
            uint8_t v = minesV ? 1 : 0;
            lobbySendSetting(cs, LST_HIDDEN_MINES, &v, 1);
        }
        if (minesDisabled) ImGui::EndDisabled();
        if (minesLocked) lobbyRenderLockBadge();

        /* "Allow all players to change settings" — toggles openHost
         * (the same flag that gates per-team manage-bots authority).
         * Visible to host / admin only. Regular players who got their
         * edit access via this very toggle don't see the control or
         * any read-only mirror of it — surfacing it would just
         * advertise the mechanism and tempt them to flip it off
         * (which they can't anyway, but the absence avoids the
         * noise). Sits next to "Allow Hidden Mines" so the related
         * authority-gating controls cluster together at the top of
         * the Other column. */
        if (!clientSimIsSinglePlayer(cs)) {
            bool isHostLocal = lobbyIsHost(cs, myPlayerNum);
            bool isAdminLocal = (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                                 (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                                  & PLAYER_FLAG_ADMIN));
            if (isHostLocal || isAdminLocal) {
                bool oh = clientSimGetLobbyOpenHost(cs);
                bool openHostLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_OPEN_HOST) != 0;
                if (openHostLocked) ImGui::BeginDisabled();
                if (ImGui::Checkbox(langGetText(STR_DLGLOBBY_OPENHOST_CB),
                                    &oh)) {
                    clientSimNetSendLobbyOpenHost(cs, oh);
                }
                if (openHostLocked) ImGui::EndDisabled();
                if (openHostLocked) lobbyRenderLockBadge();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (openHostLocked) {
                        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_RANKED_LOCKED));
                    } else {
                        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_OPENHOST_TOOLTIP));
                    }
                }
            }
        }

        bool timeLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_TIME_LIMIT) != 0;
        bool timeV = clientSimGetLobbyTimeLimit(cs) > 0;
        bool timeDisabled = !effectiveHost || timeLocked;
        if (timeDisabled) ImGui::BeginDisabled();
        if (ImGui::Checkbox(langGetText(STR_DLGGAMESETUP_TIMELIMIT), &timeV)) {
            uint8_t v = timeV ? 1 : 0;
            lobbySendSetting(cs, LST_TIME_LIMIT, &v, 1);
        }
        if (timeV) {
            int mins = clientSimGetLobbyTimeLimit(cs) > 0
                ? (int)(clientSimGetLobbyTimeLimit(cs) / (50 * 60))
                : 30;
            ImGui::SameLine();
            /* LOBBY_TIME_MINUTES_MAX is four digits. */
            ImGui::SetNextItemWidth(lobbyStepInputWidth("0000"));
            if (ImGui::InputInt("##tmin", &mins, 1, 5,
                                ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (mins < LOBBY_TIME_MINUTES_MIN) mins = LOBBY_TIME_MINUTES_MIN;
                if (mins > LOBBY_TIME_MINUTES_MAX) mins = LOBBY_TIME_MINUTES_MAX;
                uint8_t v[2] = { (uint8_t)((mins >> 8) & 0xFF),
                                 (uint8_t)(mins & 0xFF) };
                lobbySendSetting(cs, LST_TIME_MINUTES, v, 2);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_TIMELIMIT_MIN));
        }
        if (timeDisabled) ImGui::EndDisabled();
        if (timeLocked) lobbyRenderLockBadge();

        /* Visibility, under the time limit and in the column the rest of
         * the odds and ends sit in. The five named sets are on the form
         * itself, because picking one is the whole job for most hosts;
         * Details opens the popup with every setting in it. The popup is
         * opened after the columns close, where it is declared.
         *
         * The dropdown reads the live settings rather than anything it
         * remembers, so it follows an edit made in the popup — or by
         * another admin — without being told. */
        {
            VisibilitySettings live;
            lobbyVisibilityRead(cs, &live);
            VisibilityPreset livePreset = visibilityPresetMatch(&live);

            uint32_t visLocks =
                clientSimGetLobbyServerLocks(cs) & kVisibilityLockMask;
            bool visDisabled = !effectiveHost || visLocks != 0;

            /* The closed combo and every entry in it read as the name of
             * a set and nothing more, so the control stays the width of a
             * name. What a hand-made set actually holds is on the Custom
             * entry's hover, which has room for it. */
            const char *preview = langGetText(visibilityPresetNameId(livePreset));
            char customLine[192];
            customLine[0] = '\0';
            if (gameFrontVisibilityCustomSaved) {
                lobbyVisibilityDetailsLine(&gameFrontVisibilityCustom,
                                           customLine, sizeof(customLine));
            }

            const char *detailsLbl = langGetText(STR_DLGLOBBY_VIS_DETAILS_BTN);
            const ImGuiStyle &st = ImGui::GetStyle();
            float detailsW = ImGui::CalcTextSize(detailsLbl).x
                           + st.FramePadding.x * 2.0f;
            float avail    = ImGui::GetContentRegionAvail().x;
            char visLbl[64];
            SDL_snprintf(visLbl, sizeof(visLbl), "%s:",
                         langGetText(STR_DLGLOBBY_VISIBILITY_LBL));
            float labelW = ImGui::CalcTextSize(visLbl).x + st.ItemSpacing.x;

            /* Only as wide as the entries it actually lists: the widest of
             * the five preset names and "Custom", plus the frame padding
             * either side of the text and the arrow, which is one frame
             * square. A fixed width would leave a hole beside "Classic". */
            float comboW =
                ImGui::CalcTextSize(langGetText(STR_DLGLOBBY_PRESET_CUSTOM)).x;
            for (int p = 0; p < (int)VISIBILITY_PRESET_COUNT; p++) {
                float w = ImGui::CalcTextSize(
                    langGetText(visibilityPresetNameId((VisibilityPreset)p))).x;
                if (w > comboW) comboW = w;
            }
            comboW += st.FramePadding.x * 2.0f + ImGui::GetFrameHeight();

            /* Narrow column: the label drops to its own line to buy the
             * combo back its width, and past that the combo gives up what
             * it has to so the Details button stays on screen. */
            float room  = avail - labelW - st.ItemSpacing.x - detailsW;
            bool  stack = (comboW > room);

            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(visLbl);
            if (stack) {
                room = avail - st.ItemSpacing.x - detailsW;
            } else {
                ImGui::SameLine();
            }
            if (comboW > room) comboW = room;
            if (comboW < 40.0f * s) comboW = 40.0f * s;

            if (visDisabled) ImGui::BeginDisabled();
            ImGui::SetNextItemWidth(comboW);
            bool comboOpen = ImGui::BeginCombo("##vispreset", preview);
            /* Read off the combo's own button, before the open list draws
             * items of its own and takes the last-item slot. The entries
             * say what they do on their own hovers, so the button itself
             * only has something to say when it is disabled. */
            bool comboHovered =
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            if (comboOpen) {
                for (int p = 0; p < (int)VISIBILITY_PRESET_COUNT; p++) {
                    VisibilityPreset preset = (VisibilityPreset)p;
                    bool sel = (livePreset == preset);
                    if (ImGui::Selectable(
                            langGetText(visibilityPresetNameId(preset)), sel)
                        && !sel) {
                        lobbyVisibilityApplyPreset(cs, preset);
                    }
                    /* What the set does, on the entry itself: the table in
                     * Details has no room for a sentence per row. */
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "%s", langGetText(visibilityPresetDescId(preset)));
                    }
                }
                /* Custom is listed whether or not the host has ever made
                 * a set — the row the settings can land on must never be
                 * missing from the list — but with none made it names
                 * nothing and picking it does nothing. */
                bool customSel = (livePreset == visibilityPresetCustom);
                if (ImGui::Selectable(langGetText(STR_DLGLOBBY_PRESET_CUSTOM),
                                      customSel)
                    && !customSel && gameFrontVisibilityCustomSaved) {
                    lobbyVisibilityApply(cs, &gameFrontVisibilityCustom);
                }
                /* Custom has no fixed description — what it does is the
                 * set itself, so the hover is that set spelled out. With
                 * none saved it says what the entry is for instead. */
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "%s", gameFrontVisibilityCustomSaved
                                  ? customLine
                                  : langGetText(STR_DLGLOBBY_PRESET_CUSTOM_DESC));
                }
                ImGui::EndCombo();
            }
            if (visDisabled) ImGui::EndDisabled();
            if (visLocks != 0) lobbyRenderLockBadge();
            if (comboHovered && visLocks != 0) {
                ImGui::SetTooltip("%s",
                                  langGetText(STR_DLGLOBBY_VIS_PRESET_LOCKED_TIP));
            }

            /* Details stays live under a lock: the settings are still
             * worth reading even when nobody here may change them. */
            ImGui::SameLine();
            if (ImGui::Button(detailsLbl)) {
                openVisibility = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_VIS_DETAILS_TIP));
            }
        }

        /* Password protection — host or admin only (NOT openHost;
         * we don't want random connected players to be able to lock
         * the host out of their own server). MP only — SP has no
         * remote clients to keep out. */
        if (!clientSimIsSinglePlayer(cs)) {
            bool isHostLocal  = lobbyIsHost(cs, myPlayerNum);
            bool isAdminLocal = (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                                 (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                                  & PLAYER_FLAG_ADMIN));
            if (isHostLocal || isAdminLocal) {
                /* Two pieces of UI state, both UI-layer-owned (the
                 * server never echoes the password value, so we
                 * can't reconstruct them from sim state):
                 *   s_pwOn   — the "Password" checkbox's intent.
                 *              Toggled directly by the click,
                 *              persists across frames.
                 *   s_pwBuf  — the typed password. When the user
                 *              unchecks we wipe it AND clear the
                 *              server-side value. */
                static char s_pwBuf[200] = {0};
                static bool s_pwOn       = false;

                bool pwLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_PASSWORD) != 0;
                if (pwLocked) ImGui::BeginDisabled();
                if (ImGui::Checkbox(langGetText(STR_DLGLOBBY_PASSWORD_CB), &s_pwOn)) {
                    if (!s_pwOn) {
                        s_pwBuf[0] = '\0';
                        clientSimNetSendLobbySetPassword(cs, "");
                    }
                    /* Checking with an empty buffer doesn't send
                     * anything yet — wait for the user to type. */
                }
                if (pwLocked) ImGui::EndDisabled();
                if (pwLocked) lobbyRenderLockBadge();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (pwLocked) {
                        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_RANKED_LOCKED));
                    } else {
                        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_PASSWORD_TOOLTIP));
                    }
                }
                if (s_pwOn && !pwLocked) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(180.0f * s);
                    if (ImGui::InputText("##serverpw", s_pwBuf,
                                         sizeof(s_pwBuf),
                                         ImGuiInputTextFlags_Password |
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                        clientSimNetSendLobbySetPassword(cs, s_pwBuf);
                    }
                    /* Send on blur too so the host doesn't have to
                     * remember to hit Enter. ImGui surfaces this via
                     * IsItemDeactivatedAfterEdit. */
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        clientSimNetSendLobbySetPassword(cs, s_pwBuf);
                    }
                }
            }
        }

        /* "Allow new players" / "Disallow new players once game has
         * started" moved to the team-list header (right of "+ Add Team")
         * so all join-related controls live in one row. See
         * lobbyRenderTeamGroupedPlayers. */

    }

    ImGui::Columns(1);

    /* ── Visibility (pillboxes / bases / allied tanks) ─────────────
     * The editor behind the button under the time limit, because the three
     * rows took more room on the lobby surface than the rest of the form
     * put together. The rows themselves are unchanged, they just live
     * in the popup now: one per category, a 4-way combo plus a
     * decay-seconds input that is only enabled while that row's combo reads
     * Decay, each edit sending the 3-byte [policy][decay hi][decay lo]
     * payload for that category's LST_* id. What the rows are set to is on
     * the lobby's header line, which the host reads along with everyone
     * else, so the form carries no second copy. */
    if (openVisibility) {
        ImGui::OpenPopup("###visibility");
    }

    /* Same ID scope as the OpenPopup above — BeginPopupModal only finds
     * a popup opened at its own scope. The title is composed so the
     * translated label is the caption and everything after ### is the
     * ID, which a translation therefore cannot change. */
    {
        char visTitle[128];
        SDL_snprintf(visTitle, sizeof(visTitle), "%s###visibility",
                     langGetText(STR_DLGLOBBY_VISIBILITY_LBL));
        static bool s_visOpen = true; s_visOpen = true;
        if (ImGui::BeginPopupModal(visTitle, &s_visOpen,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            /* One table: a row per way of playing, a column per setting,
             * and in each cell the value that row runs. A preset row
             * reads as words; the Custom row carries the controls
             * themselves, so the table shows what every preset does and
             * what the host can do instead in the same grid.
             *
             * The radio says which row the lobby is on, and that is read
             * off the settings rather than remembered, so an edit made
             * anywhere moves it. */
            VisibilitySettings live;
            lobbyVisibilityRead(cs, &live);
            VisibilityPreset livePreset = visibilityPresetMatch(&live);
            uint32_t visLocks =
                clientSimGetLobbyServerLocks(cs) & kVisibilityLockMask;
            bool presetDisabled = !effectiveHost || visLocks != 0;

            const ImGuiStyle &tst = ImGui::GetStyle();
            /* Every value column is the same width — the widest thing any
             * of them can hold — so the table reads straight down a column
             * as well as across a row. Only the name column is its own
             * size, because nothing lines up with it.
             *
             * Then twenty pixels off each of them: the widest thing a
             * column can hold and the widest thing it usually holds are
             * not the same, and sized for the first the table was wider
             * than it looked like it needed. Scaled with everything else,
             * so it shrinks by the same proportion at every font size. */
            float colW  = lobbyVisibilityValueColumnWidth(s) - 20.0f * s;
            if (colW < ImGui::GetFrameHeight()) colW = ImGui::GetFrameHeight();
            float nameW = 0.0f;
            float outerW;
            int   c;

            /* The first column holds a radio and the longest row name. */
            for (int p = 0; p <= (int)VISIBILITY_PRESET_COUNT; p++) {
                int id = (p == (int)VISIBILITY_PRESET_COUNT)
                             ? STR_DLGLOBBY_PRESET_CUSTOM
                             : visibilityPresetNameId((VisibilityPreset)p);
                float w = ImGui::CalcTextSize(langGetText(id)).x;
                if (w > nameW) nameW = w;
            }
            nameW += ImGui::GetFrameHeight() + tst.ItemSpacing.x;

            outerW = nameW + tst.CellPadding.x * 2.0f
                   + ((float)VIS_COLUMN_COUNT
                      * (colW + tst.CellPadding.x * 2.0f));

            if (visLocks != 0) {
                lobbyRenderLockBadge();
                ImGui::SameLine();
                ImGui::TextUnformatted(
                    langGetText(STR_DLGLOBBY_TOOLTIP_RANKED_LOCKED));
            }

            /* Held under the screen so a small one still shows the table.
             * The columns are all fixed, so the overflow scrolls sideways
             * rather than squeezing a combo down to nothing; the name
             * column and the header row stay put while it does. */
            ImGuiTableFlags tflags = ImGuiTableFlags_BordersInnerH
                                   | ImGuiTableFlags_BordersInnerV
                                   | ImGuiTableFlags_RowBg
                                   | ImGuiTableFlags_SizingFixedFit
                                   | ImGuiTableFlags_PadOuterX;
            float roomW  = ImGui::GetMainViewport()->WorkSize.x - 80.0f * s;
            float outerH = 0.0f;
            if (outerW > roomW) {
                outerW = roomW;
                tflags |= ImGuiTableFlags_ScrollX;
                /* A scrolling table is a child window, and one asked for
                 * no height takes every pixel left in the popup rather
                 * than the height of its rows. So say how tall it is: a
                 * header, five preset rows, the Custom row at the height
                 * a Decay cell grows it to, and the scrollbar. */
                float rowH  = ImGui::GetFrameHeight()
                            + tst.CellPadding.y * 2.0f;
                float tallH = rowH + ImGui::GetFrameHeight()
                            + tst.ItemSpacing.y;
                outerH = rowH * (float)(VISIBILITY_PRESET_COUNT + 1)
                       + tallH + tst.ScrollbarSize;
            }

            if (ImGui::BeginTable("##vispresets", VIS_COLUMN_COUNT + 1, tflags,
                                  ImVec2(outerW, outerH))) {
                ImGui::TableSetupColumn("##name",
                                        ImGuiTableColumnFlags_WidthFixed |
                                            ImGuiTableColumnFlags_NoHide,
                                        nameW);
                for (c = 0; c < VIS_COLUMN_COUNT; c++) {
                    ImGui::TableSetupColumn(
                        langGetText(kVisColumns[c].header),
                        ImGuiTableColumnFlags_WidthFixed, colW);
                }
                ImGui::TableSetupScrollFreeze(1, 1);

                /* Headers drawn by hand rather than with TableHeadersRow,
                 * so each one can carry what its setting means. */
                ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
                ImGui::TableSetColumnIndex(0);
                ImGui::TableHeader("");
                for (c = 0; c < VIS_COLUMN_COUNT; c++) {
                    ImGui::TableSetColumnIndex(c + 1);
                    ImGui::TableHeader(ImGui::TableGetColumnName(c + 1));
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s",
                                          langGetText(kVisColumns[c].tip));
                    }
                }

                for (int p = 0; p < (int)VISIBILITY_PRESET_COUNT; p++) {
                    VisibilityPreset preset = (VisibilityPreset)p;
                    VisibilitySettings row;
                    bool sel = (livePreset == preset);

                    if (!visibilityPresetSettings(preset, &row)) continue;
                    ImGui::PushID(p);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (lobbyVisibilityRowPick(
                            langGetText(visibilityPresetNameId(preset)), sel,
                            presetDisabled,
                            langGetText(visibilityPresetDescId(preset)))) {
                        lobbyVisibilityApplyPreset(cs, preset);
                    }
                    for (c = 0; c < VIS_COLUMN_COUNT; c++) {
                        LobbyVisIcon icon;
                        const char  *word;
                        bool         dim;
                        lobbyVisibilityCellValue(&row, c, &icon, &word, &dim);
                        ImGui::TableSetColumnIndex(c + 1);
                        lobbyRenderVisibilityValue(icon, word, dim,
                                                   kVisColumns[c].header, s);
                    }
                    ImGui::PopID();
                }

                {
                    bool sel = (livePreset == visibilityPresetCustom);
                    /* A Custom row with no set behind it cannot be picked —
                     * there is nothing to go back to. Changing one of the
                     * controls along it is how the lobby lands here. */
                    bool pickable = effectiveHost && visLocks == 0 &&
                                    gameFrontVisibilityCustomSaved;
                    char customRowLine[192];
                    customRowLine[0] = '\0';
                    if (gameFrontVisibilityCustomSaved) {
                        lobbyVisibilityDetailsLine(&gameFrontVisibilityCustom,
                                                   customRowLine,
                                                   sizeof(customRowLine));
                    }
                    ImGui::PushID("custom");
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    /* Custom has no fixed description - what it does is
                     * the set itself - so its hover is that set spelled
                     * out, and with none saved it says what the row is for
                     * instead. Same as the dropdown's Custom entry. */
                    if (lobbyVisibilityRowPick(
                            langGetText(STR_DLGLOBBY_PRESET_CUSTOM), sel,
                            !pickable,
                            gameFrontVisibilityCustomSaved
                                ? customRowLine
                                : langGetText(STR_DLGLOBBY_PRESET_CUSTOM_DESC))) {
                        lobbyVisibilityApply(cs, &gameFrontVisibilityCustom);
                    }
                    for (c = 0; c < VIS_COLUMN_COUNT; c++) {
                        ImGui::TableSetColumnIndex(c + 1);
                        lobbyVisibilityCustomCell(cs, c, effectiveHost,
                                                  colW, s);
                    }
                    ImGui::PopID();
                }

                ImGui::EndTable();
            }

            /* A real Close button, not just the title-bar X: ImGui's
             * NavCancel leaves modals open, so a controller needs
             * something focusable to leave by. */
            if (WBUI::DialogFooter(/*cancelLabel*/ NULL,
                                   /*confirmLabel*/ langGetText(STR_CLOSE))
                != WBUI::FOOTER_NONE) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    /* Restore the larger lobby-font scale before the Hide button so
     * its text isn't shrunk to the 0.85x the columns body uses. The
     * game-settings section's content auto-sizes to fit the columns
     * above — no explicit padding under it. */
    ImGui::SetWindowFontScale(settingsOldScale);

    /* "Hide Settings" — temporarily disabled. The form itself is
     * collapsible via its CollapsingHeader's chevron, so the
     * standalone button was redundant. Keeping the code in a
     * commented block so it's easy to restore if the section
     * tries to grow back. */
#if 0
    {
        const char *label = "Hide Settings";
        ImVec4 baseBtn = ImGui::GetStyleColorVec4(ImGuiCol_Button);
        ImVec4 baseTxt = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(baseBtn.x, baseBtn.y, baseBtn.z, baseBtn.w * 0.35f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(baseBtn.x, baseBtn.y, baseBtn.z, baseBtn.w * 0.65f));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(baseTxt.x, baseTxt.y, baseTxt.z, baseTxt.w * 0.65f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(ImGui::GetStyle().FramePadding.x, 2.0f * s));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        float btnW = ImGui::CalcTextSize(label).x + 16.0f * s;
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btnW);
        if (ImGui::Button(label)) {
            s_settings.open = false;
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
    }
#endif
}

/* ── Visibility summary ───────────────────────────────────────────
 * What the host has set, read only, on one line: the word Visibility and
 * the name of the set the values add up to, then the pill, base, allied
 * tank and tank-in-forest sprites, each followed by what it is set to,
 * with the seconds added under Decay and the whole group dimmed under
 * Off. Drawn on the lobby header line, which is how a joiner or spectator
 * — who never sees the settings panel — finds out what the host chose;
 * the host reads the same line above the panel rather than a second copy
 * inside it. Hovering a group names the setting and its value in full.
 *
 * Every sprite-and-word pair comes from lobbyRenderVisibilityValue, which
 * is also what fills the Details table's value cells, so the two pictures
 * of the same setting cannot drift apart. The overview window and line of
 * sight are not on this line: there is no square to draw for either, and
 * the set's name already says which way they go. */
void lobbyRenderVisibilitySummary(ClientSim *cs, float s) {
    VisibilitySettings live;
    VisibilityPreset   preset;
    const float iconSize = 16.0f * s;
    const float textH    = ImGui::GetTextLineHeight();
    const float textDrop = (iconSize > textH) ? (iconSize - textH) * 0.5f : 0.0f;

    lobbyVisibilityRead(cs, &live);
    preset = visibilityPresetMatch(&live);

    /* "Visibility: Max view". A joiner reads the name and knows what game
     * this is; the sprites after it say exactly what that means. A set no
     * preset names reads as Custom, and the hover spells it out in words. */
    {
        char head[224];
        SDL_snprintf(head, sizeof(head), "%s: %s",
                     langGetText(STR_DLGLOBBY_VISIBILITY_LBL),
                     langGetText(visibilityPresetNameId(preset)));
        /* No AlignTextToFramePadding here: the line's text-base offset is
         * what the sprite drops are measured against, and setting it from
         * inside would move them. */
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textDrop);
        ImGui::TextUnformatted(head);
        if (ImGui::IsItemHovered()) {
            char label[224];
            lobbyVisibilityLabel(&live, label, sizeof(label));
            ImGui::SetTooltip("%s: %s",
                              langGetText(STR_DLGLOBBY_VISIBILITY_LBL), label);
        }
    }

    /* The first four columns of the Details table, in the same order and
     * drawn by the same helper. */
    for (int c = 0; c < 4; c++) {
        const char  *word;
        LobbyVisIcon icon;
        bool         dim;

        ImGui::SameLine(0, 12.0f * s);
        lobbyRenderVisibilityColumn(&live, c, s);
        if (ImGui::IsItemHovered()) {
            /* The hover names the setting and its bare value; the seconds
             * the line itself shows under Decay would only repeat here. */
            lobbyVisibilityCellValue(&live, c, &icon, &word, &dim);
            ImGui::SetTooltip("%s: %s",
                              langGetText(lobbyVisibilityColumnLabelId(c)),
                              word);
        }
    }
}
