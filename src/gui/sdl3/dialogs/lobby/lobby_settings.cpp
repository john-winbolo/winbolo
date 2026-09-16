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

/* Which row the Visibility dialog is showing as chosen. Read off the live
 * settings each time the dialog opens, and moved by every pick and every
 * edit made while it is open.
 *
 * It exists so the machine hosting the game can sit on Custom even while
 * its custom values happen to match one of the presets. For anybody else,
 * and for a host who has not picked Custom, it simply follows the live
 * match. Nothing is put back on the lobby when the dialog closes. */
static int s_visSelectedPreset = -1;

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

/* Commits one edit made along the Custom row, or down the stacked form.
 * edited is a copy of whatever the control was showing, with the one
 * field that control changed already set on it.
 *
 * An edit like this always lands on Custom, so the dialog's chosen row
 * moves there. Classic mode comes off because the server refuses an edit
 * to any of the other six settings while it is on, and lobbyVisibilityApply
 * sends that "off" ahead of the setting itself.
 *
 * customSettings and saveCustom say what this machine keeps, and there
 * are three cases:
 *
 * A set given - the table's Custom row on the machine hosting the game.
 * The control was showing that saved set, so the edit is written back
 * into it and persisted, and the next game hosted from this machine
 * starts there. saveCustom is not read in this case.
 *
 * NULL with saveCustom true - the stacked form on the machine hosting the
 * game. The control was showing the live settings, so the new saved set
 * is those live values with this one edit on them.
 *
 * NULL with saveCustom false - an admin who is not hosting. Only the one
 * setting that changed goes to the lobby and nothing is saved here: their
 * saved set belongs to some other game and must not be touched. */
static void lobbyVisibilityCommitEdit(ClientSim *cs,
                                      VisibilitySettings *customSettings,
                                      bool saveCustom,
                                      VisibilitySettings *edited) {
    s_visSelectedPreset = (int)visibilityPresetCustom;
    edited->classicMode = false;
    if (customSettings != NULL) {
        *customSettings = *edited;
        gameFrontSetVisibilityCustom(customSettings);
    } else if (saveCustom) {
        gameFrontSetVisibilityCustom(edited);
    }
    lobbyVisibilityApply(cs, edited);
}

/* The same commit for one of the three view columns, where the mode and
 * the seconds travel as one setting and so have to be sent together
 * however the cell was changed. The seconds are clamped here, so the
 * combo, the typed value and the two step buttons all land inside the
 * range the server will take.
 *
 * src may be customSettings itself, which is why the copy is made before
 * anything is written. customSettings and saveCustom mean what they mean
 * in lobbyVisibilityCommitEdit above, and are handed straight on. */
static void lobbyVisibilityCommitView(ClientSim *cs,
                                      VisibilitySettings *customSettings,
                                      bool saveCustom,
                                      const VisibilitySettings *src, int c,
                                      int policy, int secs) {
    VisibilitySettings edited = *src;

    if (secs < VIEW_DECAY_MIN_SECS) secs = VIEW_DECAY_MIN_SECS;
    if (secs > VIEW_DECAY_MAX_SECS) secs = VIEW_DECAY_MAX_SECS;
    edited.policy[c]    = (uint8_t)policy;
    edited.decaySecs[c] = (uint16_t)secs;
    lobbyVisibilityCommitEdit(cs, customSettings, saveCustom, &edited);
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

/* Keeps the INI's memory of what the lobby is on in step with the lobby:
 * the seven per-setting values, and which named set they match. Run every
 * frame the form is drawn rather than off each control, so a change from
 * anywhere - a preset, a Details control, another admin - is written down
 * the same way. The next game hosted from this machine starts there.
 *
 * It does not write the hand-made set. A preset reaches the lobby one
 * setting at a time, so the values read here pass through mixes that
 * match no named set while it is going on, and saving those would leave
 * the player's own set replaced by a half-applied preset. That set is
 * written only where this machine edits or picks something, which is
 * lobbyVisibilityCommitEdit and the two Custom picks. A preset going on,
 * or a change made from another machine, therefore cannot overwrite it.
 *
 * Only the machine actually running the server writes: visiting somebody
 * else's lobby as an admin must not rewrite what this player hosts with.
 * The prefs setters only touch the INI when a value moves, so a frame
 * where nothing changed costs a compare. */
static void lobbyVisibilityRemember(ClientSim *cs, int myPlayerNum) {
    if (!lobbyIsHost(cs, myPlayerNum)) return;

    VisibilitySettings live;
    lobbyVisibilityRead(cs, &live);
    gameFrontRememberVisibility(&live, /*saveCustom*/ false);
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
    } else if (column == 4 &&
               v->overviewWindow == (uint8_t)overviewWindowExpanded) {
        /* The whole sentence is one string, not the Expanded word plus a
         * translated suffix, so a translator can order the parts freely. */
        SDL_snprintf(out, outSize, "%s",
                     langGetText(STR_DLGLOBBY_WINDOW_EXPANDED_LONG));
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

/* The narrowest the six value columns may be drawn. They are all one
 * width on purpose - the table is read down a column and across a row,
 * and six different widths make both harder - so the widest minimum any
 * one of them has is the minimum they all get.
 *
 * Three things are deliberately NOT in it. The column names: a header
 * clips to "..." at the minimum and comes back whole the moment the host
 * widens the popup, and a name is not worth a hundred pixels of table the
 * settings could have had. The lock badge: a lock is the exception, so it
 * is drawn beside the control when there is one rather than reserved for
 * in every column that has none. And the seconds box at its full width:
 * that row wraps instead - see the Decay branch below.
 *
 * Worked out from the text rather than from a number, so a translation
 * that needs more room gets it. */
static float lobbyVisibilityValueColumnMinWidth(float s) {
    static const int policyWords[] = {
        STR_DLGLOBBY_VIEW_ALWAYS, STR_DLGLOBBY_VIEW_KEY,
        STR_DLGLOBBY_VIEW_DECAY,  STR_DLGLOBBY_VIEW_OFF,
    };
    static const int windowWords[] = {
        STR_DLGLOBBY_WINDOW_CLASSIC, STR_DLGLOBBY_WINDOW_EXPANDED,
        STR_DLGLOBBY_WINDOW_NONE,
    };
    const ImGuiStyle &st = ImGui::GetStyle();
    const float frame = ImGui::GetFrameHeight();
    const float iconW = lobbyVisibilityIconWidth(s);
    const float combo = st.FramePadding.x * 2.0f + frame; /* padding + arrow */
    float policyW = 0.0f;
    float windowW = 0.0f;
    float w;
    float secsField;
    int   i;

    for (i = 0; i < (int)(sizeof(policyWords) / sizeof(policyWords[0])); i++) {
        float t = ImGui::CalcTextSize(langGetText(policyWords[i])).x;
        if (t > policyW) policyW = t;
    }
    for (i = 0; i < (int)(sizeof(windowWords) / sizeof(windowWords[0])); i++) {
        float t = ImGui::CalcTextSize(langGetText(windowWords[i])).x;
        if (t > windowW) windowW = t;
    }

    /* The three view columns: a sprite and a combo holding the widest
     * policy word. */
    w = iconW + policyW + combo;
    /* The overview column: the same combo holding the widest mode word,
     * and no sprite - there is none for that setting, so it must not be
     * charged for one. */
    if (windowW + combo > w) w = windowW + combo;
    /* The two Yes/No combo columns are narrower than either, so they never
     * decide this; the seconds row under a Decay combo is checked because
     * its first line is the field and its label. */
    secsField = ImGui::CalcTextSize("0000").x + st.FramePadding.x * 2.0f
              + st.ItemInnerSpacing.x
              + ImGui::CalcTextSize(langGetText(STR_DLGLOBBY_VIEW_DECAY_SECS)).x;
    if (secsField > w) w = secsField;
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
 * highlight, and the radio reads its checked state from sel, which is the
 * row the dialog has chosen.
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
    /* The Selectable and the radio drawn back over it are one target in
     * one spot, so both are kept out of the running for the window's
     * default focus - flagging only the first hands the focus to the
     * second. The dialog says where it wants the focus instead, with a
     * SetItemDefaultFocus on its Close button. */
    ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);
    /* Never drawn as selected: the radio is what says which row is on, and
     * a filled cell behind a checked radio says it twice. DontClosePopups so
     * clicking a preset row keeps the dialog open until Close or Esc. */
    if (ImGui::Selectable("##row", false,
                          ImGuiSelectableFlags_DontClosePopups,
                          ImVec2(cellW, rowH))
        && !sel) {
        clicked = true;
    }
    /* Read before the radio and the name are drawn over it, and shown after
     * EndDisabled so a non-host still gets to read what the row does. */
    bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    ImGui::SetCursorPos(start);
    if (ImGui::RadioButton("##pick", sel) && !sel) {
        clicked = true;
    }
    ImGui::PopItemFlag();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name);
    if (disabled) ImGui::EndDisabled();
    if (hovered && tip != NULL && tip[0] != '\0') {
        ImGui::SetTooltip("%s", tip);
    }
    return clicked;
}

/* Column c of the Custom row, or one row of the stacked form: editable,
 * behind the same sprite the preset rows above it carry so the column
 * reads the same all the way down. Each control is the one the panel has
 * always used, one to a cell now rather than a stack of rows.
 *
 * There are two things the cell can be showing, and customSettings says
 * which. Given a set - which is this machine's own saved custom set, and
 * only on the table's Custom row on the machine hosting the game - the
 * cell shows that set and every edit goes back into it, so the whole set
 * stays together and lands on the lobby as one. Given NULL - the stacked
 * form, and the Custom row for anybody who is not hosting - the cell
 * shows the live settings and an edit sends only the setting it holds.
 *
 * realHost says whether this machine is running the server, and decides
 * what an edit saves when the cell is showing the live settings. On the
 * hosting machine the saved set becomes those live values with this edit
 * on them, because the player made that edit here. For anybody else
 * nothing is saved: their saved set belongs to some other game. When
 * customSettings is given the flag does not matter, because the set is
 * written through customSettings instead.
 *
 * Both go out through lobbyVisibilityCommitEdit, which turns classic mode
 * off first: while it is on the server refuses an edit to any of the six. */
static void lobbyVisibilityCustomCell(ClientSim *cs, int c, bool effectiveHost,
                                      float maxCtrlW, float s,
                                      bool stackSeconds,
                                      VisibilitySettings *customSettings,
                                      bool realHost) {
    const ImGuiStyle &st = ImGui::GetStyle();
    VisibilitySettings live;
    const VisibilitySettings *src = customSettings;
    LobbyVisIcon icon;
    bool locked  = (clientSimGetLobbyServerLocks(cs)
                    & kVisColumns[c].lockBit) != 0;
    bool disable = !effectiveHost || locked;
    float ctrlW;

    if (src == NULL) {
        lobbyVisibilityRead(cs, &live);
        src = &live;
    }
    lobbyVisibilityCellValue(src, c, &icon, NULL, NULL);

    ImGui::PushID(c);
    if (disable) ImGui::BeginDisabled();
    /* The sprite is never faint here: the control beside it says what the
     * setting is, and dimming the pair would read as "disabled" on a row
     * the host is meant to edit. */
    if (icon != visIconNone) {
        lobbyRenderVisibilityValue(icon, NULL, false, kVisColumns[c].header, s);
        ImGui::SameLine(0, 4.0f * s);
    }
    /* Whatever the cell has left once the sprite is in, so a stretched
     * column hands its extra width to the control rather than leaving it
     * blank. maxCtrlW caps that where a caller wants one - the stacked
     * form does, or its combos would run the width of the popup. No room
     * is set aside for a lock badge: a lock is the exception, and it is
     * drawn after the control on the rare cell that has one. */
    ctrlW = ImGui::GetContentRegionAvail().x;
    if (maxCtrlW > 0.0f && ctrlW > maxCtrlW) ctrlW = maxCtrlW;
    if (ctrlW < ImGui::GetFrameHeight()) ctrlW = ImGui::GetFrameHeight();
    if (c <= 2) {
        const char *modes[] = {
            langGetText(STR_DLGLOBBY_VIEW_ALWAYS),
            langGetText(STR_DLGLOBBY_VIEW_KEY),
            langGetText(STR_DLGLOBBY_VIEW_DECAY),
            langGetText(STR_DLGLOBBY_VIEW_OFF),
        };
        int policy = (int)src->policy[c];
        int secs   = (int)src->decaySecs[c];
        if (secs < VIEW_DECAY_MIN_SECS) secs = VIEW_DECAY_DEFAULT_SECS;

        ImGui::SetNextItemWidth(ctrlW);
        if (ImGui::Combo("##mode", &policy, modes, 4)) {
            lobbyVisibilityCommitView(cs, customSettings, realHost, src, c,
                                      policy, secs);
        }
        /* The seconds show only while the cell is on Decay. In the table
         * they go on a second line inside the cell, because beside the
         * combo they would widen every column for a value the other three
         * modes never read; the stacked form has the room, so there they
         * sit where the old panel always put them. */
        if (policy == (int)viewPolicyDecay) {
            /* VIEW_DECAY_MAX_SECS is three digits; the box holds a fourth
             * to type into before the setter clamps it back. */
            const char *secsLbl = langGetText(STR_DLGLOBBY_VIEW_DECAY_SECS);
            float frame   = ImGui::GetFrameHeight();
            float fieldW  = ImGui::CalcTextSize("0000").x
                          + st.FramePadding.x * 2.0f;
            float btnsW   = (frame + st.ItemInnerSpacing.x) * 2.0f;
            float labelW  = st.ItemInnerSpacing.x
                          + ImGui::CalcTextSize(secsLbl).x;
            float rowW;

            if (!stackSeconds) ImGui::SameLine(0, st.ItemInnerSpacing.x);
            rowW = ImGui::GetContentRegionAvail().x;
            /* Field, its two step buttons and the word on one line when
             * there is room for them. When there is not, the buttons drop
             * to a line of their own under the field rather than squeezing
             * the number down to nothing - which is what lets a column be
             * narrower than a whole spin box and still be usable. */
            bool oneLine = (rowW >= fieldW + btnsW + labelW);
            ImGui::SetNextItemWidth(oneLine ? fieldW + btnsW : fieldW);
            if (ImGui::InputInt("##decay", &secs, oneLine ? 1 : 0,
                                oneLine ? 5 : 0,
                                ImGuiInputTextFlags_EnterReturnsTrue)) {
                lobbyVisibilityCommitView(cs, customSettings, realHost, src, c,
                                          policy, secs);
            }
            ImGui::SameLine(0, st.ItemInnerSpacing.x);
            ImGui::TextUnformatted(secsLbl);
            if (!oneLine) {
                /* The step buttons InputInt would have drawn, on the next
                 * line and the same size, so the cell reads as one control
                 * that has folded rather than two different ones. */
                if (ImGui::Button("-", ImVec2(frame, frame))) {
                    lobbyVisibilityCommitView(cs, customSettings, realHost,
                                              src, c, policy, secs - 1);
                }
                ImGui::SameLine(0, st.ItemInnerSpacing.x);
                if (ImGui::Button("+", ImVec2(frame, frame))) {
                    lobbyVisibilityCommitView(cs, customSettings, realHost,
                                              src, c, policy, secs + 1);
                }
            }
        }
    } else if (c == 3) {
        const char *yesNo[] = {
            langGetText(STR_YES),
            langGetText(STR_NO),
        };
        int sel = src->alliesInTrees ? 0 : 1;
        ImGui::SetNextItemWidth(ctrlW);
        if (ImGui::Combo("##trees", &sel, yesNo, 2)) {
            VisibilitySettings edited = *src;
            edited.alliesInTrees = (sel == 0);
            lobbyVisibilityCommitEdit(cs, customSettings, realHost, &edited);
        }
    } else if (c == 4) {
        const char *windows[] = {
            langGetText(STR_DLGLOBBY_WINDOW_EXPANDED),
            langGetText(STR_DLGLOBBY_WINDOW_CLASSIC),
            langGetText(STR_DLGLOBBY_WINDOW_NONE),
        };
        int window = (int)src->overviewWindow;
        ImGui::SetNextItemWidth(ctrlW);
        if (ImGui::Combo("##window", &window, windows,
                         (int)OVERVIEW_WINDOW_COUNT)) {
            VisibilitySettings edited = *src;
            edited.overviewWindow = (uint8_t)window;
            lobbyVisibilityCommitEdit(cs, customSettings, realHost, &edited);
        }
    } else {
        const char *yesNo[] = {
            langGetText(STR_YES),
            langGetText(STR_NO),
        };
        int sel = (src->lineOfSight != (uint8_t)lineOfSightOff) ? 0 : 1;
        ImGui::SetNextItemWidth(ctrlW);
        if (ImGui::Combo("##sight", &sel, yesNo, 2)) {
            VisibilitySettings edited = *src;
            edited.lineOfSight = (sel == 0)
                                     ? (uint8_t)lineOfSightBuildingsAndTrees
                                     : (uint8_t)lineOfSightOff;
            lobbyVisibilityCommitEdit(cs, customSettings, realHost, &edited);
        }
    }
    /* The hover is read before the badge draws, so the tooltip belongs to
     * the control and not to the badge — which carries its own "locked by
     * the server" tooltip — and held until after EndDisabled so it draws
     * at full contrast rather than dimmed with the cell. */
    if (disable) ImGui::EndDisabled();
    if (locked) lobbyRenderLockBadge();
    ImGui::PopID();
}

/* Multi-row tooltip showing each setting in order with its icon and value.
 * Used on the top server line's summary and on the Custom preset entry. */
static void lobbyRenderVisibilityTooltip(const VisibilitySettings *v,
                                         VisibilityPreset preset, float s) {
    if (v == NULL) return;

    ImGui::BeginTooltip();

    /* The preset's own sentence first, the same one the Details table's
     * row hover gives. Custom has no sentence about what it does — what
     * it does is the rows under it — so it goes straight to them. */
    if (preset != visibilityPresetCustom) {
        ImGui::TextUnformatted(langGetText(visibilityPresetDescId(preset)));
        ImGui::Separator();
    }

    /* One row per setting: every Details column in the table's order.
     * The decay seconds ride inside the view rows' value words — where
     * lobbyVisibilityColumnText puts them, and the only place they can
     * go, since each of the three view settings carries its own.
     *
     * The values line up under one another rather than running on after
     * their labels: six rows of two words each are read down the value
     * column. The offset is measured off the longest label so a
     * translation that needs more room gets it. */
    {
        const float startX = ImGui::GetCursorPosX();
        float       labelW = 0.0f;
        int c;

        for (c = 0; c < VIS_COLUMN_COUNT; c++) {
            float w = ImGui::CalcTextSize(
                langGetText(lobbyVisibilityColumnLabelId(c))).x;
            if (w > labelW) labelW = w;
        }
        labelW += 12.0f * s;

        for (c = 0; c < VIS_COLUMN_COUNT; c++) {
            ImGui::TextUnformatted(
                langGetText(lobbyVisibilityColumnLabelId(c)));
            ImGui::SameLine(startX + labelW);
            lobbyRenderVisibilityColumn(v, c, s);
        }
    }

    ImGui::EndTooltip();
}

/* The preset dropdown: the five named sets and Custom, each entry saying
 * on its own hover what it does. The lobby's Visibility row has one and so
 * does the Details popup when it is too narrow for the table, and they are
 * the same control from here so the two can never offer different sets or
 * different words for them.
 *
 * The preview and every entry read as the name of a set and nothing more,
 * so the control stays the width of a name; what a hand-made set holds is
 * on the Custom entry's hover, which has room for it.
 *
 * width is the item width to use. Pass 0 to have it sized to the widest
 * entry, which is what a caller with room to spare wants.
 *
 * realHost says whether this machine is running the server, and decides
 * whether the Custom entry can be pressed - see the entry itself below.
 * The preset entries do not read it: a preset is the same set of values
 * wherever it is picked from, so anybody the lobby lets edit may pick one.
 *
 * dialogSelectedPreset is the Details dialog's chosen row, handed in so
 * the dropdown inside that dialog shows and moves the same row the table
 * would. The lobby's own Visibility row passes nothing and simply reads
 * whichever set the live settings match. */
static void lobbyVisibilityPresetCombo(ClientSim *cs, const char *id,
                                       float width, bool disabled,
                                       uint32_t visLocks, float s,
                                       bool realHost,
                                       int *dialogSelectedPreset = NULL) {
    const ImGuiStyle &st = ImGui::GetStyle();
    VisibilitySettings live;

    lobbyVisibilityRead(cs, &live);
    VisibilityPreset livePreset = visibilityPresetMatch(&live);
    const char *preview;
    if (dialogSelectedPreset != NULL && *dialogSelectedPreset == (int)visibilityPresetCustom) {
        preview = langGetText(STR_DLGLOBBY_PRESET_CUSTOM);
    } else if (dialogSelectedPreset != NULL && *dialogSelectedPreset >= 0 &&
               *dialogSelectedPreset < (int)VISIBILITY_PRESET_COUNT) {
        preview = langGetText(visibilityPresetNameId((VisibilityPreset)*dialogSelectedPreset));
    } else {
        preview = langGetText(visibilityPresetNameId(livePreset));
    }

    if (width <= 0.0f) {
        width = ImGui::CalcTextSize(
            langGetText(STR_DLGLOBBY_PRESET_CUSTOM)).x;
        for (int p = 0; p < (int)VISIBILITY_PRESET_COUNT; p++) {
            float w = ImGui::CalcTextSize(
                langGetText(visibilityPresetNameId((VisibilityPreset)p))).x;
            if (w > width) width = w;
        }
        width += st.FramePadding.x * 2.0f + ImGui::GetFrameHeight();
    }
    if (width < 40.0f * s) width = 40.0f * s;

    if (disabled) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(width);
    bool comboOpen = ImGui::BeginCombo(id, preview);
    /* Read off the combo's own button, before the open list draws items of
     * its own and takes the last-item slot. The entries say what they do on
     * their own hovers, so the button itself only has something to say when
     * it is disabled. */
    bool comboHovered =
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    if (comboOpen) {
        for (int p = 0; p < (int)VISIBILITY_PRESET_COUNT; p++) {
            VisibilityPreset preset = (VisibilityPreset)p;
            bool sel = (dialogSelectedPreset != NULL)
                           ? (*dialogSelectedPreset == (int)preset)
                           : (livePreset == preset);
            if (ImGui::Selectable(langGetText(visibilityPresetNameId(preset)),
                                  sel)
                && !sel) {
                if (dialogSelectedPreset != NULL) {
                    *dialogSelectedPreset = (int)preset;
                }
                lobbyVisibilityApplyPreset(cs, preset);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s",
                                  langGetText(visibilityPresetDescId(preset)));
            }
        }
        /* Custom is listed whether or not a set has ever been made here -
         * the row the settings can land on must never be missing from the
         * list - but only the machine hosting the game can press it. What
         * it puts on the lobby is this machine's own saved set, and that
         * set belongs to the game this machine hosts, not to somebody
         * else's lobby that this player happens to be an admin in. Picking
         * it also writes that set back, which a machine that is not
         * hosting has no business doing. Everybody else lands on Custom by
         * editing one of the settings, which sends only what they changed.
         * Same rule as the table's Custom row.
         *
         * Listed and disabled rather than left out: the entry still names
         * the row the settings can be on, and still spells the set out on
         * its hover, which is read with AllowWhenDisabled below so a
         * non-host can read it too. */
        bool customSel = (dialogSelectedPreset != NULL)
                             ? (*dialogSelectedPreset == (int)visibilityPresetCustom)
                             : (livePreset == visibilityPresetCustom);
        if (!realHost) ImGui::BeginDisabled();
        if (ImGui::Selectable(langGetText(STR_DLGLOBBY_PRESET_CUSTOM),
                              customSel)
            && !customSel) {
            if (dialogSelectedPreset != NULL) {
                *dialogSelectedPreset = (int)visibilityPresetCustom;
            }
            gameFrontVisibilityCustom.classicMode = false;
            gameFrontSetVisibilityCustom(&gameFrontVisibilityCustom);
            lobbyVisibilityApply(cs, &gameFrontVisibilityCustom);
        }
        bool customHovered =
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        if (!realHost) ImGui::EndDisabled();
        /* Custom has no fixed description - what it does is the set itself,
         * so the hover is that set spelled out with the same rich tooltip as
         * the top server line. With none saved it says what the entry is for
         * instead. */
        if (customHovered) {
            const VisibilitySettings *customSettings = NULL;
            if (gameFrontVisibilityCustomSaved) {
                customSettings = &gameFrontVisibilityCustom;
            } else if (livePreset == visibilityPresetCustom) {
                customSettings = &live;
            }
            if (customSettings != NULL) {
                lobbyRenderVisibilityTooltip(customSettings,
                                             visibilityPresetCustom, s);
            } else {
                ImGui::SetTooltip("%s",
                                  langGetText(STR_DLGLOBBY_PRESET_CUSTOM_DESC));
            }
        }
        ImGui::EndCombo();
    }
    if (disabled) ImGui::EndDisabled();
    if (visLocks != 0) lobbyRenderLockBadge();
    if (comboHovered && visLocks != 0) {
        ImGui::SetTooltip("%s",
                          langGetText(STR_DLGLOBBY_VIS_PRESET_LOCKED_TIP));
    }
}

/* The Details body when the popup is too narrow for the table: the same
 * dropdown the lobby row carries, then the six settings stacked one to a
 * row the way the panel laid them out before the table existed.
 *
 * The controls here always show the live settings and send one setting at
 * a time, which is why they are handed no saved set to write into. The
 * chosen row is the one thing the two bodies share, so resizing past the
 * threshold changes the shape and how far one edit reaches, nothing more.
 *
 * realHost is passed down to the cells so that an edit made here on the
 * machine hosting the game still saves a custom set - the live values
 * with that edit on them. Without it, the only way to save a set would be
 * the table form, which a narrow popup never shows. */
static void lobbyRenderVisibilityStackedForm(ClientSim *cs, bool effectiveHost,
                                             uint32_t visLocks, float s,
                                             bool realHost) {
    const ImGuiStyle &st = ImGui::GetStyle();
    float labelW = 0.0f;
    int   c;

    lobbyVisibilityPresetCombo(cs, "##visformpreset", 0.0f,
                               !effectiveHost || visLocks != 0, visLocks, s,
                               realHost, &s_visSelectedPreset);
    ImGui::Separator();

    /* One column of labels, so the controls beside them line up. */
    for (c = 0; c < VIS_COLUMN_COUNT; c++) {
        float w = ImGui::CalcTextSize(langGetText(kVisColumns[c].header)).x;
        if (w > labelW) labelW = w;
    }
    labelW += st.ItemSpacing.x;

    for (c = 0; c < VIS_COLUMN_COUNT; c++) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(langGetText(kVisColumns[c].header));
        /* The same explanation the table puts on that column's header. */
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", langGetText(kVisColumns[c].tip));
        }
        ImGui::SameLine(labelW);
        /* The sprite, the control and the lock badge exactly as the table's
         * Custom row draws them - only the seconds move, from a second line
         * to beside the combo, because this form has the width for them. */
        lobbyVisibilityCustomCell(cs, c, effectiveHost,
                                  lobbyVisibilityValueColumnMinWidth(s)
                                      + 60.0f * s,
                                  s, false, NULL, realHost);
    }
}

/* Default natural dimensions of the visibility popup, worked out from its
 * longest headers and labels so the table form fits whole on first open. */
static ImVec2 lobbyVisibilityInitialWindowSize(float s) {
    const ImGuiStyle &tst = ImGui::GetStyle();
    float colW  = lobbyVisibilityValueColumnMinWidth(s);
    float nameW = 0.0f;
    float headW = colW;
    int   c;

    for (int p = 0; p <= (int)VISIBILITY_PRESET_COUNT; p++) {
        int id = (p == (int)VISIBILITY_PRESET_COUNT)
                     ? STR_DLGLOBBY_PRESET_CUSTOM
                     : visibilityPresetNameId((VisibilityPreset)p);
        float w = ImGui::CalcTextSize(langGetText(id)).x;
        if (w > nameW) nameW = w;
    }
    nameW += ImGui::GetFrameHeight() + tst.ItemSpacing.x;

    for (c = 0; c < VIS_COLUMN_COUNT; c++) {
        float t = ImGui::CalcTextSize(langGetText(kVisColumns[c].header)).x;
        if (t > headW) headW = t;
    }

    float naturalW = nameW + tst.CellPadding.x * 2.0f
                   + ((float)VIS_COLUMN_COUNT
                      * (headW + tst.CellPadding.x * 2.0f));
    float rowH = ImGui::GetFrameHeight() + tst.CellPadding.y * 2.0f;
    float bodyH = rowH * (float)(VISIBILITY_PRESET_COUNT + 2)
                + ImGui::GetFrameHeight() + tst.ItemSpacing.y;

    ImVec2 want(naturalW + tst.WindowPadding.x * 2.0f,
                bodyH + tst.WindowPadding.y * 2.0f
                      + ImGui::GetFrameHeight() * 2.0f
                      + tst.ItemSpacing.y * 3.0f);
    ImVec2 room = ImGui::GetMainViewport()->WorkSize;
    float maxInitialW = room.x * 0.90f;
    room.x -= 40.0f * s;
    room.y -= 40.0f * s;
    if (want.x > maxInitialW) want.x = maxInitialW;
    if (want.x > room.x) want.x = room.x;
    if (want.y > room.y) want.y = room.y;
    return want;
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
    /* The one machine actually running the server, which is not the same
     * thing as effectiveHost below - that is also true in an open-host
     * lobby and for an admin. Only the real host writes its own saved
     * visibility set from this dialog, so only there is that set worth
     * showing and worth editing in place. */
    bool realHost = lobbyIsHost(cs, myPlayerNum);
    /* Same effective-host test the panel computes, recomputed here so the
     * per-control disabled state is identical whether the body renders in
     * the desktop collapsing header or the controller Settings tab. */
    bool effectiveHost = !spectator && (realHost || clientSimGetLobbyOpenHost(cs) ||
                         (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                          (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                           & PLAYER_FLAG_ADMIN)));

    /* Write down what the visibility settings have been put on, so the
     * next game hosted from this machine starts there. Done once for the
     * form rather than off each control, so a change from the dropdown,
     * from the Details popup or from another admin is all caught the same
     * way. This writes the values and which named set they match; the
     * saved custom set is not written here, only where this machine edits
     * or picks something. */
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
            langGetText(STR_DLGGAMEINFO_SCRIPTED),
        };
        /* One source for how many rows there are: the loop below used to
         * carry a literal that had to match this array by hand. */
        const int itemCount = (int)(sizeof(items) / sizeof(items[0]));
        bool rankedNow = clientSimGetLobbyRanked(cs);
        for (int i = 0; i < itemCount; i++) {
            /* gameType enum is 1-based (gameOpen=1, gameTournament=2,
             * gameStrictTournament=3), so the array index → enum
             * mapping is i+1. The previous (gameType)i comparison
             * read the wrong row as "checked" — Open showed as
             * Unknown, Tournament showed as Open, etc. */
            int enumVal = i + 1;
            /* The scripted type has a row only while the lobby is on it, so
               a scripted lobby has a checked row instead of an empty group
               and every other lobby looks exactly as it did. It is never
               offered: a host cannot pick it and the server refuses the
               value, so committing a map with a script beside it is the only
               thing that sets it. */
            if ((gameType)enumVal == gameScripted &&
                clientSimGetLobbyGameType(cs) != gameScripted) {
                continue;
            }
            /* Ranked games forbid the "Open" type — grey it out. */
            bool optDisabled = rankedNow && (gameType)enumVal == gameOpen;
            if ((gameType)enumVal == gameScripted) optDisabled = true;
            /* A scripted lobby is on the type its map commit set, and the
               server refuses every other value while the scenario is there.
               Greying the whole group says so, rather than letting a row be
               picked and snap back when the refusal arrives.
               Keyed on the scenario, which is what the server's own refusal
               reads: the type and the scenario can disagree for a moment —
               a lobby that has a scenario but has not been committed onto it
               yet — and the client would then offer a row the server turns
               down. */
            if (clientSimGetLobbyScenarioSource(cs) != 0) {
                optDisabled = true;
            }
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
            /* A scenario fields its own bots, so the server refuses the row
               that takes every bot off the roster and admits the other
               three. Only that row is greyed: the host still picks how hard
               the bots play. */
            bool rowDisabled = (i == (int)aiNone &&
                                clientSimGetLobbyScenarioSource(cs) != 0);
            if (rowDisabled) ImGui::BeginDisabled();
            if (ImGui::RadioButton(rid, checked) && !checked) {
                uint8_t v = (uint8_t)i;
                lobbySendSetting(cs, LST_AI_POLICY, &v, 1);
            }
            if (rowDisabled) ImGui::EndDisabled();
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

        /* Smart pings. The checkbox is the positive question ("allow"),
         * which is why it reads through clientSimIsLobbyAllowSmartPings;
         * the wire field it sends is the negative one, so the byte is the
         * inverse of the box. Same host/lock gating as Hidden Mines above. */
        bool pingLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_SMART_PINGS) != 0;
        bool pingV = clientSimIsLobbyAllowSmartPings(cs);
        bool pingDisabled = !effectiveHost || pingLocked;
        if (pingDisabled) ImGui::BeginDisabled();
        if (ImGui::Checkbox(langGetText(STR_DLGLOBBY_SMART_PINGS_CB), &pingV)) {
            uint8_t v = pingV ? 0 : 1;
            lobbySendSetting(cs, LST_SMART_PINGS_OFF, &v, 1);
        }
        if (pingDisabled) ImGui::EndDisabled();
        if (pingLocked) lobbyRenderLockBadge();

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
            uint32_t visLocks =
                clientSimGetLobbyServerLocks(cs) & kVisibilityLockMask;
            bool visDisabled = !effectiveHost || visLocks != 0;

            const char *detailsLbl = langGetText(STR_DLGLOBBY_VIS_DETAILS_BTN);
            const ImGuiStyle &st = ImGui::GetStyle();
            float detailsW = ImGui::CalcTextSize(detailsLbl).x
                           + st.FramePadding.x * 2.0f;
            float avail    = ImGui::GetContentRegionAvail().x;
            char visLbl[64];
            SDL_snprintf(visLbl, sizeof(visLbl), "%s:",
                         langGetText(STR_DLGLOBBY_VISIBILITY_LBL));
            float labelW = ImGui::CalcTextSize(visLbl).x + st.ItemSpacing.x;

            /* Only as wide as the entries it actually lists, which the
             * shared combo works out for itself when it is given no width.
             * A fixed width would leave a hole beside "Classic". */
            float comboW = ImGui::CalcTextSize(
                langGetText(STR_DLGLOBBY_PRESET_CUSTOM)).x;
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

            lobbyVisibilityPresetCombo(cs, "##vispreset", comboW, visDisabled,
                                       visLocks, s, realHost);

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
        /* The size the host last dragged this to, kept for the session.
         * Zero until the first open works out a default from the real font
         * metrics - which cannot be done out here, because the popup is its
         * own window and does not inherit the settings body's font scale.
         * Applied on appearing only, so a drag is never fought. */
        static ImVec2 s_visWinSize(0.0f, 0.0f);
        static float  s_visTargetH = 0.0f;
        static bool   s_visWinForceResize = false;

        float enclosingW = ImGui::GetWindowWidth();
        if (enclosingW <= 0.0f || enclosingW > ImGui::GetMainViewport()->WorkSize.x) {
            enclosingW = ImGui::GetMainViewport()->WorkSize.x;
        }
        float maxW = enclosingW * 0.90f;
        float minW = 320.0f * s;
        if (minW > maxW) minW = maxW;

        if (s_visWinSize.x <= 0.0f) {
            ImGui::SetWindowFontScale(1.0f);
            s_visWinSize = lobbyVisibilityInitialWindowSize(s);
            ImGui::SetWindowFontScale(0.85f);
            if (s_visWinSize.x > maxW) {
                s_visWinSize.x = maxW;
            }
            s_visTargetH = s_visWinSize.y;
            s_visWinForceResize = true;
        }
        if (s_visWinSize.x > maxW) {
            s_visWinSize.x = maxW;
            s_visWinForceResize = true;
        }

        if (s_visWinForceResize) {
            ImGui::SetNextWindowSize(s_visWinSize, ImGuiCond_Always);
            s_visWinForceResize = false;
        } else if (s_visWinSize.x > 0.0f) {
            ImGui::SetNextWindowSize(s_visWinSize, ImGuiCond_Appearing);
        }
        /* Lock vertical height to programmatic / auto sizing so the user
         * cannot vertically drag the height. Constrain width to at most 90%
         * of the enclosing window width. */
        {
            float lockH = (s_visTargetH > 0.0f) ? s_visTargetH : -1.0f;
            ImGui::SetNextWindowSizeConstraints(ImVec2(minW, lockH),
                                                ImVec2(maxW, lockH));
        }
        /* Resizable on purpose: the table is six columns wide and a small
         * window cannot hold it, so the host is given the edge to drag
         * rather than a body that quietly hides its last two columns. */
        if (ImGui::BeginPopupModal(visTitle, &s_visOpen, 0)) {
            /* Each opening starts on whatever the lobby is actually set
             * to, so the dialog never opens showing a row from last time. */
            if (ImGui::IsWindowAppearing()) {
                VisibilitySettings liveInit;
                lobbyVisibilityRead(cs, &liveInit);
                s_visSelectedPreset = (int)visibilityPresetMatch(&liveInit);
            }
            /* One table: a row per way of playing, a column per setting,
             * and in each cell the value that row runs. A preset row
             * reads as words; the Custom row carries the controls
             * themselves, so the table shows what every preset does and
             * what the host can do instead in the same grid.
             *
             * The radio says which row is chosen. That follows the live
             * settings, so an edit made anywhere moves it; the one thing
             * it remembers is the real host sitting on Custom. */
            VisibilitySettings live;
            lobbyVisibilityRead(cs, &live);
            VisibilityPreset livePreset = visibilityPresetMatch(&live);
            uint32_t visLocks =
                clientSimGetLobbyServerLocks(cs) & kVisibilityLockMask;
            bool presetDisabled = !effectiveHost || visLocks != 0;
            /* What the Custom row shows and edits. On the machine hosting
             * the game the row shows that machine's own saved set, and an
             * edit along the row goes back into it. Nobody else has a set
             * worth showing here: theirs is left over from some other
             * game, so for them the row shows the live settings and each
             * control edits the one setting it holds.
             *
             * The saved set is no longer kept in step with the lobby every
             * frame, so on Custom it can differ from the live settings -
             * a change made by another admin from another machine does not
             * reach it. The radio still says Custom in that case, and the
             * row shows the set this machine would host with. */
            VisibilitySettings *customRow =
                realHost ? &gameFrontVisibilityCustom : NULL;
            /* The chosen row follows the live match, because the lobby is
             * what the dialog is showing. The one exception is the real
             * host sitting on Custom on purpose: the set behind that row
             * can match a preset, and the pick has to stick anyway. */
            if (!realHost ||
                s_visSelectedPreset != (int)visibilityPresetCustom) {
                s_visSelectedPreset = (int)livePreset;
            }
            /* With no set ever saved, the globals hold the hosting
             * settings read at start-up. Two things in them the row cannot
             * show: classic mode on, which has no column and would grey
             * the whole row out, and a decay below the least the server
             * takes. Put both on something the row can show first. */
            if (customRow != NULL && !gameFrontVisibilityCustomSaved) {
                gameFrontVisibilityCustom.classicMode = false;
                for (int ci = 0; ci < (int)VIEW_CATEGORY_COUNT; ci++) {
                    if (gameFrontVisibilityCustom.decaySecs[ci] < VIEW_DECAY_MIN_SECS) {
                        gameFrontVisibilityCustom.decaySecs[ci] = VIEW_DECAY_DEFAULT_SECS;
                    }
                }
            }

            const ImGuiStyle &tst = ImGui::GetStyle();
            /* Every value column is the same width as every other, so the
             * table reads straight down a column as well as across a row.
             * Only the name column is its own size, because nothing lines
             * up with it.
             *
             * The narrowest a value column may be drawn. Below this the
             * body switches to the stacked form; above it the six columns
             * share every extra pixel equally. */
            float colW  = lobbyVisibilityValueColumnMinWidth(s);
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

            /* The narrowest the whole table can be drawn: the name column
             * at its own width, and six value columns at their minimum. */
            outerW = nameW + tst.CellPadding.x * 2.0f
                   + ((float)VIS_COLUMN_COUNT
                      * (colW + tst.CellPadding.x * 2.0f));

            if (visLocks != 0) {
                lobbyRenderLockBadge();
                ImGui::SameLine();
                ImGui::TextUnformatted(
                    langGetText(STR_DLGLOBBY_TOOLTIP_RANKED_LOCKED));
            }

            /* Table or stacked form, decided by the room there actually is
             * rather than by the screen: the host drags the edge and the
             * body follows on the next frame. The chosen row carries
             * across the switch; how far one edit reaches does not,
             * because the stacked form always sends a single setting.
             *
             * The table never scrolls sideways. A column scrolled off the
             * right edge is a column the host does not know exists, which
             * is exactly what went wrong when it could. */
            bool wideEnough = (ImGui::GetContentRegionAvail().x >= outerW);

            /* The six value columns stretch and the name column does not,
             * so the table fills whatever width the popup has and every
             * value column grows by the same amount. Equal weights, so they
             * stay equal at any size - the grid is read down a column as
             * much as across a row. */
            ImGuiTableFlags tflags = ImGuiTableFlags_BordersInnerH
                                   | ImGuiTableFlags_BordersInnerV
                                   | ImGuiTableFlags_RowBg
                                   | ImGuiTableFlags_SizingStretchSame
                                   | ImGuiTableFlags_PadOuterX;

            if (!wideEnough) {
                lobbyRenderVisibilityStackedForm(cs, effectiveHost, visLocks, s,
                                                 realHost);
            } else if (ImGui::BeginTable("##vispresets", VIS_COLUMN_COUNT + 1,
                                         tflags, ImVec2(0.0f, 0.0f))) {
                ImGui::TableSetupColumn("##name",
                                        ImGuiTableColumnFlags_WidthFixed |
                                            ImGuiTableColumnFlags_NoHide,
                                        nameW);
                for (c = 0; c < VIS_COLUMN_COUNT; c++) {
                    ImGui::TableSetupColumn(
                        langGetText(kVisColumns[c].header),
                        ImGuiTableColumnFlags_WidthStretch, 1.0f);
                }

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
                    bool sel = (s_visSelectedPreset == (int)preset);

                    if (!visibilityPresetSettings(preset, &row)) continue;
                    ImGui::PushID(p);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (lobbyVisibilityRowPick(
                            langGetText(visibilityPresetNameId(preset)), sel,
                            presetDisabled,
                            langGetText(visibilityPresetDescId(preset)))) {
                        s_visSelectedPreset = (int)preset;
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
                    bool sel = (s_visSelectedPreset == (int)visibilityPresetCustom);
                    /* Only the machine hosting the game can pick this row:
                     * picking it puts that machine's saved custom set on
                     * the lobby, and with none ever saved it puts whatever
                     * the hosting settings held. Everybody else lands on
                     * Custom by changing one of the controls along the row,
                     * which is why they get no radio to press. */
                    bool pickable = realHost && visLocks == 0;
                    /* Custom has no fixed description - what it does is
                     * the set itself - so its hover is that set spelled
                     * out. Which set is the same question the row's cells
                     * answer: the hosting machine's saved one, or the live
                     * settings while the lobby is on Custom. With neither,
                     * the hover says what the row is for instead. Same as
                     * the dropdown's Custom entry. */
                    const VisibilitySettings *rowSet = NULL;
                    char customRowLine[192];
                    customRowLine[0] = '\0';
                    if (customRow != NULL && gameFrontVisibilityCustomSaved) {
                        rowSet = &gameFrontVisibilityCustom;
                    } else if (livePreset == visibilityPresetCustom) {
                        rowSet = &live;
                    }
                    if (rowSet != NULL) {
                        lobbyVisibilityDetailsLine(rowSet, customRowLine,
                                                   sizeof(customRowLine));
                    }
                    ImGui::PushID("custom");
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (lobbyVisibilityRowPick(
                            langGetText(STR_DLGLOBBY_PRESET_CUSTOM), sel,
                            !pickable,
                            rowSet != NULL
                                ? customRowLine
                                : langGetText(STR_DLGLOBBY_PRESET_CUSTOM_DESC))) {
                        s_visSelectedPreset = (int)visibilityPresetCustom;
                        gameFrontVisibilityCustom.classicMode = false;
                        gameFrontSetVisibilityCustom(&gameFrontVisibilityCustom);
                        lobbyVisibilityApply(cs, &gameFrontVisibilityCustom);
                    }
                    for (c = 0; c < VIS_COLUMN_COUNT; c++) {
                        ImGui::TableSetColumnIndex(c + 1);
                        /* No cap: a stretched cell hands its width to
                         * the control inside it. */
                        lobbyVisibilityCustomCell(cs, c, effectiveHost,
                                                  0.0f, s, true, customRow,
                                                  realHost);
                    }
                    ImGui::PopID();
                }

                ImGui::EndTable();
            }

            /* A real Close button, not just the title-bar X: ImGui's
             * NavCancel leaves modals open, so a controller needs
             * something focusable to leave by. */
            bool wantClose = !s_visOpen;
            if (WBUI::DialogFooter(/*cancelLabel*/ NULL,
                                   /*confirmLabel*/ langGetText(STR_CLOSE))
                != WBUI::FOOTER_NONE) {
                wantClose = true;
            }
            /* The Close button is the last item the footer draws, so this
             * lands on it. The nav cursor is always drawn in this app, so
             * some item shows the focus outline the moment the dialog
             * opens. The Close button is a better place for it than the
             * first cell of the grid, where it looks like a value about to
             * be changed. */
            ImGui::SetItemDefaultFocus();
            if (wantClose) {
                ImGui::CloseCurrentPopup();
            }

            /* Fully programmatic height: calculate the exact height needed
             * to fit the content, padding, and the Close button. When the
             * content expands (e.g. Decay chosen) or contracts (e.g. Preset
             * chosen), adapt the window height automatically. User vertical
             * resizing is locked out via SetNextWindowSizeConstraints.
             *
             * What the host has dragged horizontally is read every frame
             * for the next open, because a popup can close by a click
             * outside it as well as by the button. */
            {
                float btnBottom = ImGui::GetItemRectMax().y + ImGui::GetScrollY();
                float neededH   = (btnBottom - ImGui::GetWindowPos().y) + tst.WindowPadding.y;
                float currentH  = ImGui::GetWindowHeight();
                ImVec2 room     = ImGui::GetMainViewport()->WorkSize;
                room.y -= 40.0f * s;
                if (neededH > room.y) neededH = room.y;

                s_visTargetH = neededH;

                float currentW = ImGui::GetWindowWidth();
                if (currentW > maxW) currentW = maxW;
                if (s_visWinSize.x > currentW && ImGui::IsWindowAppearing()) {
                    currentW = s_visWinSize.x;
                }
                if (currentW > maxW) currentW = maxW;

                if (fabsf(neededH - currentH) > 1.0f) {
                    ImVec2 newSize(currentW, neededH);
                    ImGui::SetWindowSize(newSize);
                    ImGui::SetScrollY(0.0f);
                    s_visWinSize = newSize;
                    s_visWinForceResize = true;
                } else {
                    s_visWinSize.x = currentW;
                    s_visWinSize.y = neededH;
                }
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
 * What the host has set, read only: the word Visibility and the name of
 * the set the values add up to, and nothing else on the line. Drawn on
 * the lobby header line, which is how a joiner or spectator — who never
 * sees the settings panel — finds out what the host chose; the host reads
 * the same line above the panel rather than a second copy inside it.
 *
 * What the name stands for is on the hover, which lists every visibility
 * setting the lobby carries, one to a row, in the Details table's order
 * and words and drawn by the same helper that fills that table's value
 * cells — so the two pictures of the same setting cannot drift apart.
 * The line itself used to carry four of them as sprites, which read well
 * but was never all of them: a reader could not tell from it which way
 * the overview window, line of sight or classic mode went. A name and a
 * hover that holds the lot says more than four sprites did. */
void lobbyRenderVisibilitySummary(ClientSim *cs, float s) {
    VisibilitySettings live;
    VisibilityPreset   preset;
    char head[224];

    lobbyVisibilityRead(cs, &live);
    preset = visibilityPresetMatch(&live);

    /* "Visibility: Max view". A joiner reads the name and knows what game
     * this is; a set no preset names reads as Custom. Either way the
     * hover spells it out.
     *
     * No AlignTextToFramePadding here: the header line carries framed
     * items, and their text-base offset already centres this against
     * them. Setting it from inside would push it off them. */
    SDL_snprintf(head, sizeof(head), "%s: %s",
                 langGetText(STR_DLGLOBBY_VISIBILITY_LBL),
                 langGetText(visibilityPresetNameId(preset)));
    ImGui::TextUnformatted(head);
    if (!ImGui::IsItemHovered()) return;

    lobbyRenderVisibilityTooltip(&live, preset, s);
}

/* ── Smart-ping summary ───────────────────────────────────────────
 * Whether the host lets players drop ping markers, on the same header line
 * and in the same shape as the visibility entries above: the marker the
 * player would be dropping, then yes or no. Read only, and drawn for
 * everyone, because the settings panel that owns the checkbox is host-only
 * and a joiner has no other way to find out.
 *
 * The mechanics below are lobbyRenderVisibilitySummary's, deliberately —
 * the idempotent icon-cache load, the two drops that centre a sprite
 * against its word, the faint No, and the label that stands in when the
 * sprite is missing. See the long comments there for why each is needed. */
void lobbyRenderSmartPingSummary(ClientSim *cs, float s) {
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (renderer) lobbyLoadStatusIconsOnce(renderer, s);

    ImGuiWindow *win = ImGui::GetCurrentWindow();
    const float iconSize = 16.0f * s;
    const float textH    = ImGui::GetTextLineHeight();
    const float imgDrop  = (textH > iconSize) ? (textH - iconSize) * 0.5f : 0.0f;
    const float textDrop = (iconSize > textH) ? (iconSize - textH) * 0.5f : 0.0f;

    bool allow = clientSimIsLobbyAllowSmartPings(cs);
    const char *word = langGetText(allow ? STR_YES : STR_NO);

    if (!allow) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                            ImGui::GetStyle().Alpha * 0.45f);
    }
    ImGui::BeginGroup();
    const float baseY = ImGui::GetCursorPosY();
    SDL_Texture *ping = renderer ? lobbyGetPingStandardTexture(renderer) : nullptr;
    if (ping) {
        ImGui::SetCursorPosY(baseY + win->DC.CurrLineTextBaseOffset + imgDrop);
        ImGui::Image((ImTextureID)ping, ImVec2(iconSize, iconSize));
    } else {
        /* Sprite missing — the label stands in for it, as above. */
        ImGui::SetCursorPosY(baseY + textDrop);
        ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_SMART_PINGS_CB));
    }
    ImGui::SameLine(0, 4.0f * s);
    ImGui::SetCursorPosY(baseY + textDrop);
    ImGui::TextUnformatted(word);
    ImGui::EndGroup();
    if (!allow) ImGui::PopStyleVar();

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s: %s",
                          langGetText(STR_DLGLOBBY_SMART_PINGS_CB), word);
    }
}
