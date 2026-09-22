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
#include "control_event.h"       /* LobbyScenarioSource — what the source accessor returns */
#include "../../../lang.h"
#include "../../../gamefront.h"  /* gameFrontHostingScripts / gameFrontGetServerSim */
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

const char *lobbyGameTypeStr(gameType gt) {
    switch (gt) {
        case gameOpen:             return langGetText(STR_DLGGAMEINFO_OPEN);
        case gameTournament:       return langGetText(STR_DLGGAMEINFO_TOURN);
        case gameStrictTournament: return langGetText(STR_DLGGAMEINFO_STRICT);
        case gameScripted:         return langGetText(STR_DLGGAMEINFO_SCRIPTED);
        default:                   return langGetText(STR_UNKNOWN);
    }
}

/* Whether this client may change which scripts play: the host, and not a
 * spectator. The test the reload button used to make, now the Choose
 * button's own.
 *
 * Shared with the chooser rather than repeated there. The chooser is reached
 * through the button below, which only a host is shown, so the two can only
 * ever disagree by drifting apart — and the chooser's arrows and its OK are
 * the controls this answer is really about. */
bool lobbyScenarioMayChoose(ClientSim *cs) {
    return clientSimGetLobbyHostSlot(cs) == clientSimGetMyPlayerNum(cs) &&
           !clientSimIsSpectator(cs);
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
 * -1 for none of that kind. The icons hand this straight to the details
 * dialog, so the dialog describes the script its icon sits beside rather
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

/* Which row of the script list the idx'th mod is. The mods are a subset of
 * that list and the details dialog is opened by list row, so a line that
 * walks the mods in order needs this to say which row the name it just drew
 * came from. -1 when there is no such mod. */
static int lobbyScenarioModRow(ClientSim *cs, int idx) {
    int n = lobbyScriptRowCount(cs);
    int i, seen = 0;

    for (i = 0; i < n; i++) {
        if (!lobbyScriptRowIsMod(cs, i)) continue;
        if (seen++ == idx) return i;
    }
    return -1;
}

static const char *lobbyScenarioModName(ClientSim *cs, int idx) {
    int at = lobbyScenarioModRow(cs, idx);

    if (at < 0) return "";
    return lobbyScriptRowName(cs, at);
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

/* The mods, each one a link that opens its own details, laid out along the
 * line the caller is on and wrapped by hand where the next name would run
 * past the edge.
 *
 * By hand because these are items and not prose: TextWrapped breaks a
 * paragraph for itself, and a row of links has to be measured one name at a
 * time. right is read before the first of them and from the cursor, so this
 * wraps inside whichever column it was called in rather than at the window's
 * own edge.
 *
 * The comma goes on the link, not between the links. A comma drawn as an
 * item of its own would be one more thing to wrap and could start a line. */
static void lobbyScenarioModLinks(ClientSim *cs, const char *idTag) {
    const ImGuiStyle &st    = ImGui::GetStyle();
    float             right = ImGui::GetCursorScreenPos().x +
                              ImGui::GetContentRegionAvail().x;
    int               n     = lobbyScenarioModCount(cs);
    int               i;

    ImGui::PushID(idTag);
    for (i = 0; i < n; i++) {
        char  shown[96];
        float w;

        SDL_snprintf(shown, sizeof(shown), "%s%s",
                     lobbyScenarioModName(cs, i), (i + 1 < n) ? "," : "");
        w = ImGui::CalcTextSize(shown).x;
        if (i > 0 &&
            ImGui::GetItemRectMax().x + st.ItemInnerSpacing.x + w <= right) {
            ImGui::SameLine(0.0f, st.ItemInnerSpacing.x);
        }
        ImGui::PushID(i);
        if (ImGui::TextLink(shown)) {
            lobbyScenarioDetailsOpenScript(cs, lobbyScenarioModRow(cs, i));
        }
        imguiHandOnHover();
        ImGui::PopID();
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

/* The icon that opens the details dialog, at the end of a line that names
 * something. An icon and not a button: both lines it sits on are prose, the
 * map panel is text with no room for a button beside it, and the Server
 * Settings column is the narrowest of the four.
 *
 * id carries a visible "i" before its ##, because the fallback draws its
 * label and ImageButton does not — an icon that failed to load must still
 * leave something to press. */
bool lobbyScenarioInfoButton(const char *id) {
    SDL_Texture *icon = lobbyIcons()->info;
    bool         clicked;

    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    if (icon != NULL) {
        /* Sized off the text and with the frame padding taken away, so the
           icon sits inside the line it belongs to instead of making that
           line a button's height taller. */
        float h = ImGui::GetTextLineHeight();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        clicked = ImGui::ImageButton(id, (ImTextureID)icon, ImVec2(h, h));
        ImGui::PopStyleVar();
    } else {
        clicked = ImGui::SmallButton(id);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_SCENARIO_DETAILS));
    }
    imguiHandOnHover();
    return clicked;
}

/* ── The Server Settings column's mods row ────────────────────────
 * One row: the checkbox, the summary the dash binds to it, and Details.
 *
 *   [x] Mods Enabled - 3 active (Faster Base Recharge, No LGM Deaths) [Details]
 *
 * The checkbox is a real server setting and not a display of one. Unchecked,
 * the round composes none of the mods on the pick list; the list itself is
 * left where it is and every name comes back the moment it is checked again,
 * the same way the password box above keeps its text while the box beside it
 * is unchecked. It is mods only — a scenario on the same list plays either
 * way, which is why the summary keeps naming them while the box is off.
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
 * the chooser is where mods are added, removed and ordered; the row has no
 * icon left to carry that difference, so the tooltip says it.
 *
 * The summary is dimmed while the box is off. The names are still what the
 * host picked, so they stay on the row, but "3 active" beside an unchecked
 * box would say they are running when they are not.
 *
 * Details sits next to the box and the summary comes last, which is the
 * order the row is read in: the setting, the way to change it, then what it
 * currently holds. Nothing here measures a width. The summary is the only
 * part that can outgrow the column and it wraps, so the two fixed-width
 * items keep their places whatever the column does. */
static void lobbyRenderModsRow(ClientSim *cs, bool effectiveHost,
                               int modCount) {
    const char *cbLbl  = langGetText(STR_DLGLOBBY_MODS_ENABLED_CB);
    const char *detLbl = langGetText(STR_DLGLOBBY_SCENARIO_DETAILS);
    /* Copied out rather than held as a pointer: langGetTextFmt answers from
       a ring of buffers, and the names are built before the row draws. */
    char summary[256];
    bool modsOn  = clientSimGetLobbyModsEnabled(cs);
    bool locked  = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_MODS) != 0;
    bool disable = !effectiveHost || locked;

    if (modCount == 0) {
        SDL_strlcpy(summary, langGetText(STR_DLGLOBBY_MODS_NONE),
                    sizeof(summary));
    } else {
        /* The count only. The names follow it as links, drawn one at a time
           by lobbyScenarioModLinks below, so nothing here has to cut a list
           of them to a message argument's 64 bytes. */
        MessageArgs args = {};
        args.number = modCount;
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
    /* The count is dimmed with the setting and the names are not. "2 active"
       beside an unchecked box would say they are running when they are not,
       which is what the dimming answers; a name is still the name of a thing
       a host may want to read about, and a disabled link cannot be pressed
       to read it. */
    if (!modsOn) ImGui::BeginDisabled();
    ImGui::TextUnformatted(summary);
    if (!modsOn) ImGui::EndDisabled();
    if (modCount > 0) {
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        lobbyScenarioModLinks(cs, "modsRowLinks");
    }
}

/* ── The scenario line's button row ───────────────────────────────
 * The line ends in up to two SmallButtons that want to sit on one row. The
 * Server Settings column they sit in is the narrowest of the four, so the row
 * does not always have the width for all of them, and a column clips what it
 * cannot hold rather than wrapping it. There is no undoing a SameLine either,
 * so the width has to be known before the button is drawn.
 *
 * Begin records how much width this caller has — the map panel gives the line
 * more than the settings column does — and Fits answers, for the button about
 * to be drawn, whether it still fits beside the last one. It books the space
 * as it answers: true means put a SameLine in front of the button, false means
 * the button starts a row of its own and the booking restarts from it. */
struct ScenarioButtonRow {
    float avail;  /* width of one row */
    float used;   /* width of the row being filled, 0 before the first button */
};

static void lobbyScenarioButtonRowBegin(ScenarioButtonRow *row) {
    row->avail = ImGui::GetContentRegionAvail().x;
    row->used  = 0.0f;
}

static bool lobbyScenarioButtonFits(ScenarioButtonRow *row, int stringId) {
    /* A SmallButton is its label plus the frame padding either side — it
       drops FramePadding.y only, not FramePadding.x. */
    float w = ImGui::CalcTextSize(langGetText(stringId)).x
            + ImGui::GetStyle().FramePadding.x * 2.0f;
    if (row->used > 0.0f) {
        float need = row->used + ImGui::GetStyle().ItemSpacing.x + w;
        if (need <= row->avail) {
            row->used = need;
            return true;
        }
    }
    row->used = w;
    return false;
}

/* What is running, for the host who sets it. The foot of the settings form's
 * Server Settings column, under the time limit and the password, which are
 * the other two things a host sets about the server rather than about the
 * round.
 *
 * The scenario and the mods are two lines and not one, because they are not
 * one question. The scenario decides the round; the mods change how it
 * plays. A host who picks a mod must see the scenario line unmoved, and a
 * scenario must never turn up among the mods — so each line reads its own
 * half and neither is drawn from the other's.
 *
 * The mods line is drawn even at none, and says so. That is the bug this
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
 * The Rules button stays here and does not follow the scenario line into the
 * map panel. It is the deep view — a row per rule, each with an Info popup a
 * controller can land on — and the map panel's way to the same answer is now
 * the details dialog behind the icon, which carries the rules table in
 * summary. So a host keeps the full table, a joiner gets the answer, and the
 * map panel stays text plus one icon.
 *
 * The buttons still measure themselves before the row is built. Only two of
 * them are left and they fit side by side in English, but the column's floor
 * is set by the password box rather than by this row, so nothing forces the
 * column to be wide enough for two buttons in a language whose words are
 * longer. Measuring is what keeps a button that will not fit on a row of its
 * own instead of clipped at the column edge.
 *
 * The buttons and the icons only ask for their dialogs. Every one of those
 * dialogs is drawn from the lobby's own frame, because this line is drawn
 * inside something that can stop being drawn — the settings form is a tab of
 * its own in the tabbed layout and a collapsing header on the desktop — and a
 * dialog that went away with it would be open with no way back to it. */
void lobbyRenderScenarioLine(ClientSim *cs, bool effectiveHost) {
    const char *scenario;
    int         modCount;

    if (cs == NULL) return;

    scenario = lobbyScenarioName(cs);
    modCount = lobbyScenarioModCount(cs);

    if (scenario == NULL && modCount == 0 &&
        !gameFrontHostingScripts && gameFrontGetServerSim() != NULL) {
        /* This machine's own server runs no scripts, so there is nothing for
           a chooser to pick that could play. Say why, and offer no button. */
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_SCRIPTS_OFF));
        ImGui::PopStyleColor();
        return;
    }

    /* No scenario line here. The Game Type column names it on the Scenario
       row it puts the round on, with the same link to the same details, and
       this column used to say the identical thing four columns over. What
       stays is the mods row, which is a control and not a readout: the
       scenario is the map's and a host changes it by committing a map, where
       the mods are picked right here.

       scenario is still read above, because the guard over it decides
       whether this column has anything at all to say. */
    (void)scenario;

    lobbyRenderModsRow(cs, effectiveHost, modCount);

    /* The row the button below sits on, measured against the width this
       caller has. lobbyScenarioButtonFits reports whether the next button
       still fits beside the last one and books the space when it does. It is
       one button now and the answer is always yes, and the row is kept
       because a second button here is one line of code away.

       Choose used to be the first of them and opened the chooser. The mods
       row above opens the same dialog from its own Details button, so the
       two said the same thing twice and Choose said it in a word that
       matches no title the dialog carries. */
    ScenarioButtonRow row;
    lobbyScenarioButtonRowBegin(&row);
    /* What the scenario's rules table says. A script whose whole content is
       a rules table has nothing else to say what it does. */
    if (lobbyScenarioRulesAvailable(cs)) {
        if (lobbyScenarioButtonFits(&row, STR_DLGLOBBY_SCENARIO_RULES)) {
            ImGui::SameLine();
        }
        if (ImGui::SmallButton(langGetText(STR_DLGLOBBY_SCENARIO_RULES))) {
            lobbyScenarioRulesOpen();
        }
    }
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
 * have it in both without the duplicate coming back. */
void lobbyRenderScenarioInfoLines(ClientSim *cs) {
    const char *scenario;
    int         modCount;

    if (cs == NULL) return;

    scenario = lobbyScenarioName(cs);
    modCount = lobbyScenarioModCount(cs);

    if (scenario != NULL) {
        ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_SCENARIO_LBL));
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        if (lobbyScenarioNameLink(scenario, "scnLinkMap")) {
            lobbyScenarioDetailsOpenScript(
                cs, lobbyScriptRowIndexOfKind(cs, false));
        }
        /* The icon stays on the end of the link and opens the same dialog.
           The link says the name can be pressed and the icon says what
           pressing it gives, and a panel read at a glance can use both. */
        if (lobbyScenarioInfoButton("i##scnInfoMap")) {
            lobbyScenarioDetailsOpenScript(
                cs, lobbyScriptRowIndexOfKind(cs, false));
        }
    }
    if (modCount > 0) {
        /* One link a mod rather than one icon for the lot of them. A round
           may run several, and an icon at the end of the line could only
           ever open the first — a host reading "No LGM Deaths" had no way to
           ask about that one in particular. */
        ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_MODS_LBL));
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        lobbyScenarioModLinks(cs, "modLinksMap");
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
