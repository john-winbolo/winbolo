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
#include "../../wb_theme.h"  /* g_theme — the Mod and Scenario tags' colours */
extern "C" {
#include "control_event.h"       /* LobbyScenarioSource — what the source accessor returns */
#include "../../../lang.h"
#include "../../../gamefront.h"  /* gameFrontHostingScripts / gameFrontGetServerSim */
#include "../../../../steam/steam_wrapper.h"  /* steam_workshop_available / open_item_page */
}

/* ── The one name tag ─────────────────────────────────────────────
 * The square chip the player list draws beside a name (HOST, ADMIN, BOT, a
 * bot's mode) and the Mods dialog draws on every row (Mod, Scenario). It was
 * a lambda inside the player row loop and the Mods dialog had a rounded pill
 * of its own; both are this now, so the two cannot drift into two different
 * chips.
 *
 * Square corners so it reads as a label and not as a status pill like READY,
 * and the label at 70% of the font size so a run of them beside a name stays
 * shorter than the name. The padding is in scaled pixels, which is why s is a
 * parameter: the lobby's scale comes from the window's height and there is no
 * global to read it back from. */
#define LOBBY_NAME_TAG_SCALE 0.70f  /* the label's share of the font size */
#define LOBBY_NAME_TAG_PAD_X 6.0f   /* unscaled pixels, either side       */
#define LOBBY_NAME_TAG_PAD_Y 2.0f   /* unscaled pixels, above and below   */

float lobbyNameTagWidth(const char *label, float s) {
    return ImGui::CalcTextSize(label).x * LOBBY_NAME_TAG_SCALE +
           LOBBY_NAME_TAG_PAD_X * 2.0f * s;
}

/* Independent of the label — the chip is as tall as the shrunken font plus
 * its padding — so a caller centring the chip on something can ask before it
 * knows which word goes in it. */
float lobbyNameTagHeight(float s) {
    return ImGui::GetFontSize() * LOBBY_NAME_TAG_SCALE +
           LOBBY_NAME_TAG_PAD_Y * 2.0f * s;
}

void lobbyDrawNameTag(const char *label, ImU32 bg, ImU32 text, ImU32 border,
                      float s) {
    float       fontSz = ImGui::GetFontSize() * LOBBY_NAME_TAG_SCALE;
    float       padX   = LOBBY_NAME_TAG_PAD_X * s;
    float       padY   = LOBBY_NAME_TAG_PAD_Y * s;
    float       w      = lobbyNameTagWidth(label, s);
    float       h      = lobbyNameTagHeight(s);
    ImVec2      pos    = ImGui::GetCursorScreenPos();
    ImDrawList *dl     = ImGui::GetWindowDrawList();

    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), bg, 0.0f);
    if (border != 0) {
        dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), border, 0.0f, 0, 1.0f);
    }
    dl->AddText(ImGui::GetFont(), fontSz, ImVec2(pos.x + padX, pos.y + padY),
                text, label);
    /* The chip is on the window's draw list and moves no cursor of its own,
       so the space it covers is booked here or the next item draws over it. */
    ImGui::Dummy(ImVec2(w, h));
}

/* The word that says which of the two kinds a script is. */
static const char *lobbyScenarioKindTagText(bool mod) {
    return langGetText(mod ? STR_DLGLOBBY_SCENARIO_TAG_MOD
                           : STR_DLGLOBBY_SCENARIO_TAG_SCENARIO);
}

/* What the tag beside a name will take, the spacing in front of it included,
   so a row can hand the name what is left before either is drawn. */
float lobbyScenarioKindTagWidth(bool mod, float s) {
    return ImGui::GetStyle().ItemInnerSpacing.x +
           lobbyNameTagWidth(lobbyScenarioKindTagText(mod), s);
}

/* The script's kind, in the same square chip the player list wears its HOST
   and BOT tags in, so the lobby has one kind of tag and not two.

   It lived in the chooser while the chooser was the only place a script's
   kind was said out loud. The map panel's links wear it now as well, so it
   moved here beside the chip it is drawn from rather than being written out
   a second time — the two would have drifted the first time either one's
   padding or colour was touched.

   Nothing to press. The tag used to be the chooser row's way into the details
   dialog — a rounded pill with an info icon inside it — and the row's name is
   that way in now, drawn as a link the way every other script name in the
   lobby is. A word that only says what a thing is has no business carrying a
   cursor change and a tooltip of its own.

   Centred on the item it follows rather than dropped at the line's top: the
   chip is shorter than a line of text, and the name beside it is what the
   eye reads the word against. The name has to be the current item when this
   is called, because its rect is what the middle is taken from. */
void lobbyScenarioKindTag(bool mod, float s) {
    const char *text = lobbyScenarioKindTagText(mod);
    float       mid  = (ImGui::GetItemRectMin().y +
                        ImGui::GetItemRectMax().y) * 0.5f;
    float       h    = lobbyNameTagHeight(s);

    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                         (mid - h * 0.5f - ImGui::GetCursorScreenPos().y));
    /* Through GetColorU32 so the style's alpha reaches the chip: a name
       dimmed because it is switched off dims its chip with it, where the
       theme's packed colours alone would stay at full strength. */
    lobbyDrawNameTag(
        text,
        ImGui::GetColorU32(mod ? g_theme->modTagBg : g_theme->scenarioTagBg),
        ImGui::GetColorU32(mod ? g_theme->modTagText
                               : g_theme->scenarioTagText),
        ImGui::GetColorU32(mod ? g_theme->modTagBorder
                               : g_theme->scenarioTagBorder),
        s);
}

/* The chip that says a script was published to the Steam Workshop, drawn
   after the kind chip. Nothing, and no width, for a script whose Workshop id
   is 0, so a caller hands it the id and draws and measures it on every row.

   In the style's own frame colours, as the Workshop settings draw their Map
   chip: the kind chip's two fills say which of the two a script is, and a
   third fill here would read as a third kind.

   Centred the way the kind chip centres itself. The kind chip is the item it
   follows, and that chip already sits centred on the name. */
float lobbyScenarioWorkshopTagWidth(uint64_t workshopId, float s) {
    if (workshopId == 0) return 0.0f;
    return ImGui::GetStyle().ItemInnerSpacing.x +
           lobbyNameTagWidth(langGetText(STR_DLGLOBBY_SCENARIO_TAG_WORKSHOP),
                             s);
}

void lobbyScenarioWorkshopTag(uint64_t workshopId, float s) {
    float mid;
    float h;

    if (workshopId == 0) return;
    mid = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
    h   = lobbyNameTagHeight(s);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                         (mid - h * 0.5f - ImGui::GetCursorScreenPos().y));
    lobbyDrawNameTag(langGetText(STR_DLGLOBBY_SCENARIO_TAG_WORKSHOP),
                     ImGui::GetColorU32(ImGuiCol_FrameBg),
                     ImGui::GetColorU32(ImGuiCol_Text),
                     ImGui::GetColorU32(ImGuiCol_Border), s);
}

/* The button that opens the script's Workshop page in Steam. Drawn only
   where Steam's Workshop is running: steam_workshop_available answers false
   on a build without Steam, so the same call serves every build and there is
   nothing to leave out at compile time. Nothing, and no width, for an id of
   0 either. The width counts the spacing in front of the button. */
float lobbyScenarioWorkshopLinkWidth(uint64_t workshopId) {
    if (workshopId == 0 || !steam_workshop_available()) return 0.0f;
    return ImGui::GetStyle().ItemInnerSpacing.x +
           ImGui::CalcTextSize(langGetText(STR_DLGSETTINGS_WORKSHOP_OPEN)).x +
           ImGui::GetStyle().FramePadding.x * 2.0f;
}

void lobbyScenarioWorkshopLink(uint64_t workshopId) {
    if (workshopId == 0 || !steam_workshop_available()) return;
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    /* Under an id of its own, because the label is translated and a row
       may carry another item whose label reads the same. */
    ImGui::PushID("workshopOpen");
    if (ImGui::SmallButton(langGetText(STR_DLGSETTINGS_WORKSHOP_OPEN))) {
        steam_workshop_open_item_page(workshopId);
    }
    imguiHandOnHover();
    ImGui::PopID();
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

/* Whether this client may change which scripts play. The same three ways in
 * the server itself accepts a script list on — the host's own slot, a slot
 * the server flagged ADMIN, or anybody connected while Open Host is on — so
 * that this test and lobbyClientMayEdit in
 * src/server/udp/udp_server_maptransfer.c answer alike. Change either and
 * the other has to move with it; each names the other so the pair can be
 * found from whichever end you start at.
 *
 * It was the host slot alone until now, which was stricter than the server
 * and told an admin, and every client of an Open Host server, that they
 * could not reorder a list the server would have taken from them: the
 * chooser opened with its arrows hidden and its OK swapped for a Close.
 *
 * The spectator term is this side's own and has no counterpart on the
 * server. It is deliberate. The client refuses a spectator every lobby
 * setting there is, not this one in particular, and a spectator reordering
 * the round they are watching is not something the lobby offers anywhere
 * else; the server is free to be laxer because nothing there asks a
 * spectator to send in the first place.
 *
 * Shared with the chooser rather than repeated there. The chooser is what
 * this answer is really about — its add, drop and reorder arrows and its OK
 * are all drawn on it — and the two Details buttons that open the dialog are
 * shown to everybody, so the dialog cannot lean on who was let in. */
bool lobbyScenarioMayChoose(ClientSim *cs) {
    int myPlayerNum = (int)clientSimGetMyPlayerNum(cs);

    if (clientSimIsSpectator(cs)) return false;
    if (lobbyIsHost(cs, myPlayerNum)) return true;
    if (clientSimGetLobbyOpenHost(cs)) return true;
    return myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
           (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags &
            PLAYER_FLAG_ADMIN) != 0;
}

/* ── What is running, told in two halves ──────────────────────────
 * A round has at most one scenario and any number of mods, and the lobby
 * reads them out on two separate lines. The split is the manifest's own
 * kind and nothing else: a scenario may end the round and say who won, a
 * mod changes how the game plays and leaves winning alone, and
 * clientSimGetLobbyScenarioKeepsWinCondition is that answer for the script
 * the round has attached.
 *
 * Deliberately not LobbyScenarioSource, which is a different axis — where
 * the script came from, the committed map or the host's pick. A host may
 * pick a scenario out of the server's directory, so a source of
 * lobbyScenarioMod says nothing about which of the two lines it belongs on,
 * and reading it here would file a picked scenario under Mods.
 *
 * Both halves read the ordered script list - clientSimGetLobbyScriptCount
 * and the accessors beside it - and classify every row by its own kind
 * rather than by where it sits in the list. Position would be the shorter
 * test, since the list loads the scenario first, but it answers wrongly for
 * a round running mods and no scenario at all, which is a round a host can
 * set up from the chooser.
 *
 * A client that has not been sent a list yet falls back to the one attached
 * slot the single-slot accessors have always carried. The two cannot
 * disagree, because the fallback is only taken when the list is empty. Take
 * the fallback out once no server in the field publishes a lobby without
 * CTRL_LOBBY_SCRIPT_LIST. */
static int lobbyScriptRowCount(ClientSim *cs) {
    int n = clientSimGetLobbyScriptCount(cs);
    if (n > 0) return n;
    return clientSimGetLobbyScenarioSource(cs) != 0 ? 1 : 0;
}

static bool lobbyScriptRowIsMod(ClientSim *cs, int i) {
    if (clientSimGetLobbyScriptCount(cs) > 0) {
        return clientSimGetLobbyScriptKeepsWinCondition(cs, i);
    }
    return clientSimGetLobbyScenarioKeepsWinCondition(cs);
}

/* A row's Workshop item, 0 for none. The single attached slot the fallback
 * reads carries no Workshop id, so a client that has no list yet says 0. */
static uint64_t lobbyScriptRowWorkshopId(ClientSim *cs, int i) {
    if (i < 0 || clientSimGetLobbyScriptCount(cs) <= 0) return 0;
    return clientSimGetLobbyScriptWorkshopId(cs, i);
}

/* Whether a row is the committed map's own script. The fallback's single
 * slot says the same about itself, so a client with no list yet still gets
 * an answer. */
static bool lobbyScriptRowBound(ClientSim *cs, int i) {
    if (clientSimGetLobbyScriptCount(cs) > 0) {
        return clientSimGetLobbyScriptBound(cs, i);
    }
    return clientSimGetLobbyScenarioBound(cs);
}

/* Whether the Mods/Scenario setting keeps a row out of the round. Off, the
 * round composes none of the host's picks, mods and picked scenarios alike;
 * the map's own script is the one row it leaves playing, which is the same
 * line scnDecideScenario draws on the server. */
static bool lobbyScriptRowSwitchedOff(ClientSim *cs, int i) {
    return !clientSimGetLobbyModsEnabled(cs) && !lobbyScriptRowBound(cs, i);
}

/* A row's name, and its file name where it named itself nothing: a script
 * with no name in its manifest still came from a file, and a line with a
 * gap in it reads as a fault. */
static const char *lobbyScriptRowName(ClientSim *cs, int i) {
    const char *name;

    if (clientSimGetLobbyScriptCount(cs) > 0) {
        name = clientSimGetLobbyScriptName(cs, i);
        if (name[0] == '\0') name = clientSimGetLobbyScriptFile(cs, i);
        return name;
    }
    name = clientSimGetLobbyScenarioName(cs);
    if (name[0] == '\0') name = clientSimGetLobbyScenarioFileName(cs);
    return name;
}

/* Where the first scenario row, or the first mod row, sits in that list.
 * -1 for none of that kind. The name links hand this straight to the details
 * dialog, so the dialog describes the script whose name was pressed rather
 * than whatever the round has attached. */
int lobbyScriptRowIndexOfKind(ClientSim *cs, bool mod) {
    int n = lobbyScriptRowCount(cs);
    int i;

    for (i = 0; i < n; i++) {
        if (lobbyScriptRowIsMod(cs, i) == mod) return i;
    }
    return -1;
}

static int lobbyScenarioModCount(ClientSim *cs) {
    int n = lobbyScriptRowCount(cs);
    int i, mods = 0;

    for (i = 0; i < n; i++) {
        if (lobbyScriptRowIsMod(cs, i)) mods++;
    }
    return mods;
}

/* A name that opens what it names. Every place the lobby says what is
 * running now draws the name this way: link-coloured, underlined on hover
 * and with the hand cursor on it, which is what says a word can be pressed
 * without a button's frame around it in a column this narrow.
 *
 * The id is pushed rather than written into the label. A label is the name a
 * script gave itself, two scripts on one list may share it, and a "###" tail
 * would be part of what a translator sees. True when it was pressed. */
static bool lobbyScenarioNameLink(const char *name, const char *id) {
    bool hit;

    ImGui::PushID(id);
    hit = ImGui::TextLink(name);
    ImGui::PopID();
    imguiHandOnHover();
    return hit;
}

/* The scripts, each one a link that opens its own details, laid out along
 * the line the caller is on and wrapped by hand where the next name would
 * run past the edge.
 *
 * By hand because these are items and not prose: TextWrapped breaks a
 * paragraph for itself, and a row of links has to be measured one name at a
 * time. right is read before the first of them and from the cursor, so this
 * wraps inside whichever column it was called in rather than at the window's
 * own edge.
 *
 * The comma goes on the link, not between the links. A comma drawn as an
 * item of its own would be one more thing to wrap and could start a line.
 *
 * Every name wears its kind chip, the Mod or Scenario tag the chooser's rows
 * wear, and it is what s is carried here for: the chip is sized in scaled
 * pixels and there is no global to read the lobby's scale back from. The
 * chip is measured into the width the wrap test works with, or a name that
 * fits on its own would be put on a line its chip then hangs off the end of.
 *
 * withScenario is the Server Settings row's list, which is every script the
 * round carries in load order, the scenario included: the setting at the
 * head of that row switches both kinds, so the row names both. Without it,
 * the mods alone: the map panel names the scenario on the line above.
 *
 * Either way each name the setting keeps out of the round is dimmed on its
 * own, since the map's own script plays either way, mod or scenario, and
 * dimming the whole list would say it did not. Dimmed with an alpha style
 * var and not BeginDisabled, because a disabled link cannot be pressed. */
static void lobbyScenarioScriptLinks(ClientSim *cs, const char *idTag,
                                     float s, bool withScenario) {
    const ImGuiStyle &st    = ImGui::GetStyle();
    float             right = ImGui::GetCursorScreenPos().x +
                              ImGui::GetContentRegionAvail().x;
    int               n     = lobbyScriptRowCount(cs);
    bool              first = true;
    int               i;

    ImGui::PushID(idTag);
    for (i = 0; i < n; i++) {
        char     shown[96];
        float    w;
        bool     mod = lobbyScriptRowIsMod(cs, i);
        bool     dim;
        bool     more = false;
        uint64_t workshopId;
        int      j;

        if (!mod && !withScenario) continue;
        /* Whether another name follows this one, which is what the comma
           says. The rows this line skips are no part of the answer. */
        for (j = i + 1; j < n && !more; j++) {
            more = withScenario || lobbyScriptRowIsMod(cs, j);
        }
        dim        = lobbyScriptRowSwitchedOff(cs, i);
        workshopId = lobbyScriptRowWorkshopId(cs, i);

        SDL_snprintf(shown, sizeof(shown), "%s%s",
                     lobbyScriptRowName(cs, i), more ? "," : "");
        w = ImGui::CalcTextSize(shown).x + lobbyScenarioKindTagWidth(mod, s) +
            lobbyScenarioWorkshopTagWidth(workshopId, s);
        if (!first &&
            ImGui::GetItemRectMax().x + st.ItemInnerSpacing.x + w <= right) {
            ImGui::SameLine(0.0f, st.ItemInnerSpacing.x);
        }
        first = false;
        /* Pushed and popped around the one name and its chips, with nothing
           that can return between the two. */
        if (dim) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, st.Alpha * 0.45f);
        }
        ImGui::PushID(i);
        if (ImGui::TextLink(shown)) {
            lobbyScenarioDetailsOpenScript(cs, i);
        }
        imguiHandOnHover();
        /* Drawn while the link is still the current item: the chip centres
           itself on whatever it follows, and anything between the two would
           be what it lined up against instead. */
        lobbyScenarioKindTag(mod, s);
        /* The word only, and no button to the item's page: this line is a
           readout, and the details dialog the name opens is where that
           button is. */
        lobbyScenarioWorkshopTag(workshopId, s);
        ImGui::PopID();
        if (dim) ImGui::PopStyleVar();
    }
    ImGui::PopID();
}

/* The scenario's name, or NULL where the round has none. NULL rather than
 * an empty string so a caller drops the whole line: a label with nothing
 * after it says less than no line at all. */
static const char *lobbyScenarioName(ClientSim *cs) {
    int i = lobbyScriptRowIndexOfKind(cs, false);

    if (i < 0) return NULL;
    return lobbyScriptRowName(cs, i);
}

/* ── The Server Settings column's mods and scenario row ───────────
 * One row: the checkbox, Details and the count, then the names on the line
 * under them, each wearing its Mod or Scenario tag.
 *
 *   [x] Mods/Scenario Enabled [Details] 3 active:
 *   Soccer, [Scenario] Faster Base Recharge, [Mod] No LGM Deaths [Mod]
 *
 * The checkbox is a real server setting and not a display of one. Unchecked,
 * the round composes none of the scripts the host picked, mods and picked
 * scenarios alike; the list itself is left where it is and every name comes
 * back the moment it is checked again, the same way the password box above
 * keeps its text while the box beside it is unchecked. The committed map's
 * own script is the one thing it leaves playing, because a map that brings
 * its own rules was not picked here. The wire byte is still LST_MODS_OFF;
 * what changed is what the server skips when it is set.
 *
 * The label is the checkbox's own and the summary is not part of it. ImGui
 * draws a checkbox's label inside the item, so a summary folded into the
 * label would make "3 active" a place to click that toggles the setting.
 *
 * Disabled on the same test as the time-limit and password rows beside it:
 * this viewer is not the effective host, or the operator locked the setting.
 * Details stays live under both, because what the round runs is still worth
 * reading when nobody here may change it — the same answer the Visibility
 * row gives. Details opens the chooser rather than the details dialog, since
 * the chooser is where scripts are added, removed and ordered; a name on the
 * line below is the way into the details dialog, so the tooltip says which
 * of the two this button opens.
 *
 * The count is of the scripts that will run: with the box off it leaves out
 * each name the setting keeps out, and those names are dimmed. The names are
 * still what the host picked, so they stay on the row, but counting them
 * beside an unchecked box would say they are running when they are not. The
 * map's own script runs either way, so it is counted either way and the
 * count itself is never dimmed.
 *
 * Details sits next to the box and the count comes last, which is the order
 * the row is read in: the setting, the way to change it, then what it
 * currently holds. The names go on a line of their own under all three: with
 * a chip beside every one of them they are the part that outgrows the
 * column, and they wrap there by hand, so the fixed-width items above keep
 * their places whatever the column does. */
static void lobbyRenderModsRow(ClientSim *cs, bool effectiveHost,
                               int scriptCount, float s) {
    const char *cbLbl  = langGetText(STR_DLGLOBBY_MODS_ENABLED_CB);
    const char *detLbl = langGetText(STR_DLGLOBBY_SCENARIO_DETAILS);
    /* Copied out rather than held as a pointer: langGetTextFmt answers from
       a ring of buffers, and the names are built before the row draws. */
    char summary[256];
    bool modsOn  = clientSimGetLobbyModsEnabled(cs);
    bool locked  = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_MODS) != 0;
    bool disable = !effectiveHost || locked;
    int  active  = 0;
    int  i;

    /* The scripts that will run, which is every row with the box on and the
       map's own script alone with it off. */
    for (i = 0; i < scriptCount; i++) {
        if (!lobbyScriptRowSwitchedOff(cs, i)) active++;
    }

    if (scriptCount == 0) {
        SDL_strlcpy(summary, langGetText(STR_DLGLOBBY_MODS_NONE),
                    sizeof(summary));
    } else {
        /* The count only, the scenario counted with the mods because the
           line below names both. The names follow it as links, drawn one at
           a time by lobbyScenarioScriptLinks below, so nothing here has to
           cut a list of them to a message argument's 64 bytes. */
        MessageArgs args = {};
        args.number = active;
        SDL_strlcpy(summary, langGetTextFmt(STR_DLGLOBBY_MODS_ACTIVE, &args),
                    sizeof(summary));
    }

    if (disable) ImGui::BeginDisabled();
    if (ImGui::Checkbox(cbLbl, &modsOn)) {
        /* Negative on the wire: the byte says the mods are OFF, so it is the
           inverse of the box. The same shape the smart-pings checkbox uses,
           and for the same reason — a server that never learned the setting
           has to read as composing them. */
        uint8_t v = modsOn ? 0 : 1;
        lobbySendSetting(cs, LST_MODS_OFF, &v, 1);
    }
    if (disable) ImGui::EndDisabled();
    if (locked) lobbyRenderLockBadge();

    ImGui::SameLine();
    /* Under an id of its own. The visibility row a little further down the
       same window has a Details button too, and both labels read "Details"
       in English, so without this the two buttons share one ImGui id: the
       hover lands on the wrong row and ImGui says so. The id is pushed
       rather than written into the label because the label is translated
       and a "###" tail is not a translator's business. */
    ImGui::PushID("modsDetails");
    if (ImGui::Button(detLbl)) {
        lobbyScenarioChooserOpen();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_MODS_DETAILS_TIP));
    }
    ImGui::PopID();

    ImGui::SameLine();
    /* Not dimmed. The count already leaves out what the setting keeps out,
       so it is true with the box off as well. The names below dim one at a
       time, and only the ones the setting keeps out: a name is still the
       name of a thing a host may want to read about, and a disabled link
       cannot be pressed to read it. */
    ImGui::TextUnformatted(summary);
    /* On the line after the row, not beside the count: no SameLine. */
    if (scriptCount > 0) {
        lobbyScenarioScriptLinks(cs, "modsRowLinks", s, true);
    }
}

/* What is running, for the host who sets it. The foot of the settings form's
 * Server Settings column, under the time limit and the password, which are
 * the other two things a host sets about the server rather than about the
 * round.
 *
 * The scenario and the mods share one row, the Mods/Scenario row, because
 * the checkbox at its head switches the host's picked scenario as well as
 * the mods. Each name wears its Scenario or Mod tag, so the two kinds are
 * still told apart: the scenario decides the round, a mod changes how it
 * plays.
 *
 * The row is drawn even at none, and says so. That is the bug this
 * shape fixes: the column used to fall through to a bare Choose button with
 * no label over it, so the narrowest column of the four ended in an
 * unexplained button. It is also deliberately different from the map panel,
 * which drops an empty mods line instead of saying "none" — that panel is a
 * summary of what is loaded, where an empty line is noise, and this one is
 * the control, where the label is what says the control exists. Do not make
 * one match the other.
 *
 * The source is 0 when the round has no script at all, which covers a map
 * with no script file and a map whose script this host declined alike: the
 * server answers the same either way. So when this client is the one hosting
 * and has the preference switched off, the column says that instead, rather
 * than leaving a host wondering where the scenario went.
 *
 * No buttons follow the row any more. The Rules button went because the
 * details dialog behind each script's name shows the same rules table, and
 * the row's own Details button is the way into the chooser.
 *
 * The Details button and the name links only ask for their dialogs. Every one of those
 * dialogs is drawn from the lobby's own frame, because this line is drawn
 * inside something that can stop being drawn — the settings form is a tab of
 * its own in the tabbed layout and a collapsing header on the desktop — and a
 * dialog that went away with it would be open with no way back to it. */
void lobbyRenderScenarioLine(ClientSim *cs, bool effectiveHost, float s) {
    int scriptCount;

    if (cs == NULL) return;

    /* Every row, scenario and mods together, because the row below names
       both and counts both. */
    scriptCount = lobbyScriptRowCount(cs);

    if (scriptCount == 0 &&
        !gameFrontHostingScripts && gameFrontGetServerSim() != NULL) {
        /* This machine's own server runs no scripts, so there is nothing for
           a chooser to pick that could play. Say why, and offer no button. */
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_SCRIPTS_OFF));
        ImGui::PopStyleColor();
        return;
    }

    /* No scenario line of its own here. The Game Type column names it on
       the Scenario row it puts the round on, with the same link to the same
       details. The scenario is named again on the row below, among the mods
       and wearing its Scenario tag, because the checkbox at the head of that
       row now switches the host's picked scenario as well as the mods, and a
       row that switched a thing it did not name would leave the host
       guessing what the box does. */
    lobbyRenderModsRow(cs, effectiveHost, scriptCount, s);
}

/* The same two halves, read-only, for the map panel — under the map's name
 * and its pill, base and start counts, which are the rest of what is loaded.
 *
 * A renderer of its own rather than a flag on the one above, because the two
 * are not the same line with the buttons hidden. This one names the scenario
 * "Scenario:" where the settings column names it by its game type, lists the
 * mods by name with no count, and drops a line that has nothing to say
 * instead of saying "none". The map panel is what is loaded and everyone
 * sees it; the settings column is where a host changes it and only a host
 * sees it. Keeping them apart is what stops a joiner being shown a control
 * they cannot use and a host reading the same thing twice.
 *
 * Drawn for everyone, host included. It was drawn for !gsEffectiveHost alone
 * while one function served both places; two renderers is what lets the host
 * have it in both without the duplicate coming back.
 *
 * It carries the way into the chooser as well, and that is not a duplicate of
 * the settings column's Details button: the column it sits in is drawn for an
 * effective host and nobody else, so until this button existed a joiner or a
 * spectator had no way to open the dialog at all and no way to read what the
 * round was running beyond the names on these two lines. The dialog itself
 * has always known how to be read-only — lobbyScenarioMayChoose hides its
 * arrows and swaps its OK for a Close — so the only thing missing was a door.
 *
 * s is the caller's own dialog scale. The kind chips on the lines below are
 * sized in scaled pixels and there is no global that holds the lobby's scale,
 * so it is handed down the same way the chooser hands it to its own rows. */
void lobbyRenderScenarioInfoLines(ClientSim *cs, float s) {
    const char *scenario;
    int         modCount;

    if (cs == NULL) return;

    scenario = lobbyScenarioName(cs);
    modCount = lobbyScenarioModCount(cs);

    /* This machine's own server runs no scripts, so the round has none and a
       chooser opened here could offer none either. Nothing is drawn at all,
       not even the note the settings column puts in the same case: that
       column is where the control lives and owes the host a word about why
       the control is missing, and this panel is a list of what is loaded,
       where a line saying "nothing" is noise. */
    if (scenario == NULL && modCount == 0 &&
        !gameFrontHostingScripts && gameFrontGetServerSim() != NULL) {
        return;
    }

    /* One line a scenario row. A round that plays has one such row, but
       with Mods/Scenario off on a map that brings its own script the list
       holds two: the map's own, which plays, and the host's picked one,
       which the server keeps for when the box goes back on. Both are drawn,
       so a joiner is shown the switched-off pick the same way a plain map
       shows it. */
    for (int scnRow = 0; scenario != NULL && scnRow < lobbyScriptRowCount(cs);
         scnRow++) {
        /* A scenario the host picked is kept out of the round while the
           Mods/Scenario setting is off, the same as the mods, so it is
           dimmed and noted the way the mods line below is. The map's own
           script plays either way and is drawn as it always was. Pushed
           here and popped after the note, with nothing that can return
           between the two. */
        bool scnOff;

        if (lobbyScriptRowIsMod(cs, scnRow)) continue;
        scnOff = lobbyScriptRowSwitchedOff(cs, scnRow);

        if (scnOff) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                ImGui::GetStyle().Alpha * 0.45f);
        }
        ImGui::PushID(scnRow);
        ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_SCENARIO_LBL));
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        if (lobbyScenarioNameLink(lobbyScriptRowName(cs, scnRow),
                                  "scnLinkMap")) {
            lobbyScenarioDetailsOpenScript(cs, scnRow);
        }
        /* The kind chip, the same one the chooser's rows wear. The two lines
           here are the only place in the lobby that names a script without
           saying which of the two kinds it is, and the difference is the
           whole of what a reader wants: a scenario may end the round and say
           who won, a mod may not. */
        lobbyScenarioKindTag(false, s);
        /* The word only, as on the mods line below. */
        lobbyScenarioWorkshopTag(lobbyScriptRowWorkshopId(cs, scnRow), s);
        /* No info icon on the end of the link any more. It opened the same
           dialog the name opens, and the name is drawn as a link, so the
           icon was a second way to press the same thing. */
        ImGui::PopID();
        if (scnOff) {
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_MODS_OFF_NOTE));
            ImGui::PopStyleVar();
        }
    }
    if (modCount > 0) {
        /* The server keeps publishing the mods it was given whether or not
           the Mods/Scenario setting lets the round compose them — the skip is
           made when the round is built, not when the list goes out — so a
           line drawn straight off that list tells a joiner mods are running
           when they are not. The note is what answers it.

           Each name is dimmed on its own, by lobbyScenarioScriptLinks, and
           not the line as a whole: a map whose own script is a mod still
           plays it with the box off, and that name stays at full strength.
           The label and the note are dimmed only as far as the names are —
           the label when every mod on the line is off, the note when any is.
           Dimmed with an alpha style var and not BeginDisabled, because the
           names are links and a disabled link cannot be pressed. Each push
           is popped with nothing that can return between the two. */
        int modsOff = 0;

        for (int i = 0; i < lobbyScriptRowCount(cs); i++) {
            if (lobbyScriptRowIsMod(cs, i) &&
                lobbyScriptRowSwitchedOff(cs, i)) {
                modsOff++;
            }
        }
        /* One link a mod rather than one icon for the lot of them. A round
           may run several, and an icon at the end of the line could only
           ever open the first — a host reading "No LGM Deaths" had no way to
           ask about that one in particular. */
        if (modsOff == modCount) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                ImGui::GetStyle().Alpha * 0.45f);
        }
        ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_MODS_LBL));
        if (modsOff == modCount) ImGui::PopStyleVar();
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        lobbyScenarioScriptLinks(cs, "modLinksMap", s, false);
        if (modsOff > 0) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                ImGui::GetStyle().Alpha * 0.45f);
            ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_MODS_OFF_NOTE));
            ImGui::PopStyleVar();
        }
    }

    /* A server started with -allow-unsafe-scripts runs every script with the
       full Lua library and no limits, and a joiner is owed that before they
       play under one. Said under the lines that name the scripts, in the
       colour a server-imposed setting is drawn in, and not at all on a
       sandboxed server: the sandbox is what every player expects, so only
       its absence is news. The server sends the flag only beside an attached
       script, so a round with none draws nothing here either. */
    if (clientSimGetLobbyScenarioUnsafe(cs)) {
        ImGui::PushStyleColor(ImGuiCol_Text, wbThemeColor(g_theme->lockBadge));
        ImGui::TextWrapped("%s", langGetText(STR_DLGLOBBY_SCENARIO_UNSAFE));
        ImGui::PopStyleColor();
    }

    /* The door into the chooser, drawn last so the two lines above read as
       what is loaded and this reads as what to do about it. Its own id,
       because the settings column's Details button and the visibility row's
       both carry this same translated label and ImGui keys a button on its
       label — without three separate ids the hover lands on whichever of
       them drew first and ImGui says so. The id is pushed rather than
       written into the label, since a "###" tail is not a translator's
       business.

       Drawn whatever the two lines above came to. A round with no scripts on
       it is exactly the round somebody wants to browse the server's
       directory from, and what the server has to offer is not something this
       client can know before the dialog asks: the catalogue request goes out
       when the chooser opens. The one case that is certainly empty — our own
       server with scripts switched off — has already returned above. */
    ImGui::PushID("mapScenarioChooser");
    if (ImGui::SmallButton(langGetText(STR_DLGLOBBY_SCENARIO_DETAILS))) {
        lobbyScenarioChooserOpen();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_MODS_DETAILS_TIP));
    }
    ImGui::PopID();
}

/* ── The header line's mods readout ───────────────────────────────
 * "Mods: Yes (3)" or "Mods: No", on the lobby's own header line beside the
 * smart-ping answer and the visibility set, with the names on the hover.
 *
 * It is there for the same reason those two are: the Mods Enabled checkbox
 * and the mods row that lists the names both sit in the settings form's
 * Server Settings column, which is drawn for an effective host and for
 * nobody else. A joiner or a spectator could read the names off the map
 * panel and still not know whether the round would run them. This line is
 * the one place everyone is told.
 *
 * Yes means both halves of the answer: the setting is on AND the round
 * actually carries mods. Either one alone is a No, because either one alone
 * means no mod runs, and a line that said Yes to an empty list would be true
 * about the setting and wrong about the round. Which of the two Nos it is,
 * the hover says.
 *
 * The hover is a function of its own only because the readout has three
 * cases and a reader following the alpha push through them should not have
 * to read the whole tooltip to find the pop. */
static void lobbyRenderModsTooltip(ClientSim *cs, bool modsOn, int modCount) {
    int n = lobbyScriptRowCount(cs);
    int i, shown = 0, listed = modCount;

    /* Off, the hover lists every pick the setting keeps out, the picked
       scenario as well as the mods: every row that is not the map's own. */
    if (!modsOn) {
        listed = 0;
        for (i = 0; i < n; i++) {
            if (!lobbyScriptRowBound(cs, i)) listed++;
        }
    }

    ImGui::BeginTooltip();
    /* A round with no mods still gets a tooltip. The line says "No" and the
       two reasons it can say No — none loaded, or loaded and switched off —
       are a different thing to know, so the hover has to separate them
       rather than leaving a reader to guess which No this is. */
    if (listed == 0) {
        ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_MODS_TIP_NONE));
        ImGui::EndTooltip();
        return;
    }

    ImGui::TextUnformatted(langGetText(modsOn ? STR_DLGLOBBY_MODS_TIP_ON
                                              : STR_DLGLOBBY_MODS_TIP_OFF));
    ImGui::Separator();

    /* Walked over the whole script list rather than over the mods alone, so
       the numbers are the load order the server published and not an order
       this end invented. On, the count beside each name is its place among
       the mods, which is what the header's "(3)" counted; the scenario the
       list may also carry is on the map panel's own line and is not a mod.
       Off, it is the place among the picks that will not run, and a picked
       scenario is one of those. */
    if (!modsOn) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                            ImGui::GetStyle().Alpha * 0.45f);
    }
    for (i = 0; i < n; i++) {
        if (modsOn ? !lobbyScriptRowIsMod(cs, i)
                   : lobbyScriptRowBound(cs, i)) {
            continue;
        }
        shown++;
        ImGui::Text("%d. %s", shown, lobbyScriptRowName(cs, i));
    }
    if (!modsOn) ImGui::PopStyleVar();

    ImGui::EndTooltip();
}

/* No AlignTextToFramePadding here, for lobbyRenderVisibilitySummary's
 * reason: the header line carries framed items and their text-base offset
 * already centres this against them, so setting it from inside would push it
 * off them. */
void lobbyRenderModsSummary(ClientSim *cs, float s) {
    bool modsOn   = clientSimGetLobbyModsEnabled(cs);
    int  modCount = lobbyScenarioModCount(cs);
    bool running  = modsOn && modCount > 0;
    char head[160];

    /* s is taken and not used. It is here because the two summaries this one
       sits between on the header line both take it, and a reader moving
       along that line should not have to check which of the three is the odd
       one; it is also what a hover that grew a sprite or a chip would need,
       and neither call site has the scale handy to add later. */
    (void)s;

    SDL_snprintf(head, sizeof(head), "%s %s",
                 langGetText(STR_DLGLOBBY_MODS_LBL),
                 langGetText(running ? STR_YES : STR_NO));
    if (running) {
        /* Copied out rather than held as a pointer: langGetTextFmt answers
           from a ring of buffers and the head above is built before this is
           appended to it. */
        MessageArgs args = {};
        char        count[32];

        args.number = modCount;
        SDL_strlcpy(count, langGetTextFmt(STR_DLGLOBBY_MODS_HEAD_N, &args),
                    sizeof(count));
        SDL_strlcat(head, " ", sizeof(head));
        SDL_strlcat(head, count, sizeof(head));
    }

    /* Faint on No, the way the smart-ping entry beside it goes faint on its
       own No. Pushed and popped around the one draw with no return between
       them — an unbalanced style var in this window is an assert the player
       sees. The hover is read after the pop, which costs nothing: the item
       is already registered and the tooltip draws at full contrast. */
    if (!running) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                            ImGui::GetStyle().Alpha * 0.45f);
    }
    ImGui::TextUnformatted(head);
    if (!running) ImGui::PopStyleVar();

    if (ImGui::IsItemHovered()) {
        lobbyRenderModsTooltip(cs, modsOn, modCount);
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
