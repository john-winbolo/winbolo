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
 * Name:          lobby_scenario_chooser.cpp
 * Purpose:       The dialog behind the Choose button on the
 *                lobby's scenario line. Two columns: what
 *                this server offers on the left, and what
 *                this round will run on the right.
 *
 *                A dialog of its own rather than a fifth
 *                source in the map chooser's row of tabs.
 *                What it picks is not a map — it plays over
 *                whichever map is committed — and a row of
 *                tabs is poor to drive on a controller, so
 *                a button opens this and nothing is added
 *                to that row.
 *
 *                The left column has two sources, one for
 *                each place a server can be, as the map
 *                chooser has: a server in this process is
 *                read straight off its scenarios directory
 *                when the dialog opens, and a remote one is
 *                asked over the wire and read back off the
 *                ClientSim each frame. Both fill the same
 *                row shape and there is one render path
 *                below them.
 *
 *                The right column has two sources as well,
 *                for a different reason — see
 *                lobbyRoundLive.
 *
 *                Also the details dialog, which is here
 *                rather than in a file of its own because
 *                what it describes is a row of this listing.
 *                The lobby's two script lines open it on the
 *                script the round is running; a row here
 *                opens it on an entry the round is not.
 *********************************************************/

#include <cfloat>   /* FLT_MAX — no upper bound on how large the host may drag it */

#include <SDL3/SDL.h>

#include "imgui.h"
#include "lobby_internal.h"
#include "dialog_footer.h"  /* WBUI::DialogFooter — the Close row and its Esc binding */
extern "C" {
#include "client_sim.h"     /* the lobby scenario list accessors */
#include "client_net.h"     /* clientSimNetSendLobbyScenarioListRequest / SetScriptList */
#include "client_command.h" /* CMD_SCRIPT_LIST_MAX — how many names one list may carry */
#include "server_sim.h"     /* ServerScenarioEntry / serverSimEnumerateScenarioDir — the in-process read */
#include "scenario_details.h"           /* the rules and callbacks blob the dialog reads */
#include "sim_rules_names.h"            /* simRulesRuleName / simRulesClassicValue */
#include "../../../sim_rules_phrase.h"  /* simRulesPhrase — the one wording */
#include "../../../ui_mode.h"           /* uiShouldUseControllerMode — a controller has no hover */
#include "../../../lang.h"
#include "../../../gamefront.h"  /* gameFrontGetServerSim — whether the server is in this process */
#include "../../../../server/threads.h"  /* threadsWaitForMutex / Release — the in-process read runs on the render thread */
}

/* The window's ID. The caption before ### is translated and the ID after it
 * is not, so a language change cannot hand ImGui a different window. */
#define LOBBY_SCENARIO_WINDOW_ID "###lobbyScenarioChooser"

/* The details popup's, for the same reason: its caption names the file it is
 * describing, and neither a language change nor a different file may hand
 * ImGui a different popup. */
#define LOBBY_SCENARIO_DETAILS_ID "###lobbyScenarioDetails"

/* How many rows a listing may hold. This is the wire list's own cap, which
 * lives in client_sim_internal.h and a gui translation unit cannot reach, so
 * it is stated here: both sources are held to the same number and a host sees
 * as many scenarios whether the server is in this process or not. */
#define LOBBY_SCENARIO_CHOOSER_MAX 128

/* One opening's worth of state. open is whether the dialog is up, which the
 * lobby reads so Esc and its controller tab cycle stand aside; asked is
 * whether this opening's listing has been obtained, by request or by reading
 * the directory; focusedOnce raises the dialog over the lobby on the frame it
 * appears and not on every frame after, which would take focus back from
 * anything clicked behind it. */
static bool s_open        = false;
static bool s_asked       = false;
static bool s_focusedOnce = false;

/* This opening's listing, for a server in this process. Held here because the
 * read is ours and nothing else keeps it — a remote client's listing lives on
 * the ClientSim and is not copied here. */
static ServerScenarioEntry s_localRows[LOBBY_SCENARIO_CHOOSER_MAX];
static int                 s_localCount = 0;

/* One row, whichever source filled it. The strings point into the listing
 * being drawn — the ClientSim's buffers or the array above, both of which
 * outlive the frame — so the two sources meet here and the drawing below
 * reads one shape. */
struct LobbyScenarioRow {
    const char *file;
    const char *name;
    const char *description;
    int         maxPlayers;
    int         bots;
    bool        bound;
    bool        keepsWinCondition;
};

/* Whether a catalogue row is a mod. A mod keeps the round's win condition;
 * a scenario may end the round and say who won, and that is the whole of the
 * difference the two columns turn on.
 *
 * Read with bound and not instead of it. bound says the file was written
 * against one map, so its tags, its regions and its entity indices are that
 * map's; a mod and an unbound scenario are both unbound, so bound alone
 * cannot tell the two apart. The left column needs both answers: bound
 * decides whether a row may be offered at all, and this decides what happens
 * when it is taken. */
static bool lobbyScenarioRowIsMod(const LobbyScenarioRow *row) {
    return row->keepsWinCondition;
}

/* The name box over the list. Cleared every time the dialog opens rather
 * than carried over: a host who comes back to a box still holding last
 * time's word sees two rows of forty and has nothing telling them the other
 * thirty-eight are still there. Groundwork for the two-column chooser, where
 * this box heads the left column's catalogue. */
static char s_filter[64] = "";

/* The kind box beside it: 0 every row, 1 mods only, 2 scenarios only. Cleared
 * with the name box and for the same reason — a host who comes back to a
 * column set to Only mods has nothing on screen telling them the scenarios
 * are still there. */
static int s_kindFilter = 0;

/* The three settings, in the order the box lists them. All first because it
 * is what the column opens on. */
static const int s_kindFilterIds[3] = {
    STR_DLGLOBBY_SCENARIO_KIND_ALL,
    STR_DLGLOBBY_SCENARIO_KIND_MODS,
    STR_DLGLOBBY_SCENARIO_KIND_SCENARIOS,
};

/* Case-insensitive substring. Its own walk rather than a library call: the
 * haystack is one row's name and the needle is at most sixty-odd bytes, so
 * nothing here is worth a dependency, and this behaves the same wherever it
 * is built. */
static bool lobbyScenarioTextHas(const char *hay, const char *needle) {
    size_t n = SDL_strlen(needle);
    size_t h = SDL_strlen(hay);
    size_t i, j;

    if (n == 0) return true;
    if (h < n) return false;
    for (i = 0; i + n <= h; i++) {
        for (j = 0; j < n; j++) {
            if (SDL_tolower((unsigned char)hay[i + j]) !=
                SDL_tolower((unsigned char)needle[j])) {
                break;
            }
        }
        if (j == n) return true;
    }
    return false;
}

/* Over the name shown and over the file it came from. The file as well
 * because a host who installed the thing knows what they called it and may
 * never have read what its manifest named it. */
static bool lobbyScenarioRowMatches(const LobbyScenarioRow *row,
                                    const char *needle) {
    if (needle[0] == '\0') return true;
    return lobbyScenarioTextHas(row->name, needle) ||
           lobbyScenarioTextHas(row->file, needle);
}

/* ── The right column: what this round will run ───────────────────
 * One more row than a host may send. The ten are the server's own cap on a
 * set-list command; the extra one is the committed map's own scenario, which
 * is drawn here and is not part of what is sent.
 *
 * The lobby's list cap is LOBBY_SCRIPT_LIST_MAX in client_sim_internal.h,
 * which a gui translation unit cannot reach. It is the same ten. A list
 * longer than this is clamped when it is read, which only costs the rows past
 * the cap their place in the column. */
#define LOBBY_ROUND_MAX (CMD_SCRIPT_LIST_MAX + 1)

/* One row of that column. Copied rather than pointed at, because the draft
 * outlives the accessors it was read from: a new list may land between two
 * frames and the strings behind clientSimGetLobbyScriptFile would then be
 * another list's. */
struct LobbyRoundRow {
    char file[SERVER_SCENARIO_FILE_LEN];
    char name[SERVER_SCENARIO_NAME_LEN];
    bool mod;    /* keeps the round's win condition */
    bool bound;  /* written for one map, so it is the map's and not the
                    host's — it is drawn locked and never sent */
};

/* The host's draft, and the lobby's own answer the draft was taken from.
 *
 * A draft and not a send per click, for two reasons that are both in the
 * server. CMD_LOBBY_SET_SCRIPT_LIST is refused with CMD_REJECT_COOLDOWN when
 * a second list arrives within SCENARIO_RELOAD_GAP_TICKS — one second — of
 * the last, and reordering a column means several clicks in a row. And every
 * list the server accepts detaches and re-attaches the scenario and clears
 * every player's ready bit (lobbyScenarioReselect in
 * src/server/server_command_dispatch.c), so a host moving one mod down three
 * places would unready the lobby three times. One list goes out, when the
 * host confirms.
 *
 * s_live is what the lobby said the round was when the draft was taken. It is
 * compared against the lobby every frame: a map commit or a second host
 * editing replaces the round under this dialog, and a draft still describing
 * the old one would send the host's edit back over somebody else's change. */
static LobbyRoundRow s_round[LOBBY_ROUND_MAX];
static int           s_roundCount = 0;
static LobbyRoundRow s_live[LOBBY_ROUND_MAX];
static int           s_liveCount  = 0;
static bool          s_roundTaken = false;

static void lobbyRoundSetRow(LobbyRoundRow *r, const char *file,
                             const char *name, bool mod, bool bound) {
    SDL_snprintf(r->file, sizeof(r->file), "%s", file);
    /* A manifest that named nothing still came from a file, and a row with a
       gap where its name goes reads as a fault. */
    SDL_snprintf(r->name, sizeof(r->name), "%s",
                 name[0] != '\0' ? name : file);
    r->mod   = mod;
    r->bound = bound;
}

/* The round as the lobby reports it, composed from the two places the answer
 * lives, and written into out in load order.
 *
 * Two places because the committed map's own scenario is attached without
 * ever entering the script list: only CMD_LOBBY_SET_SCRIPT_LIST and
 * CMD_LOBBY_SET_SCENARIO write that list, and a map sidecar goes through
 * neither. So a scripted map with no host picks reports
 * clientSimGetLobbyScriptCount 0 while CTRL_LOBBY_SETTINGS separately reports
 * an attached scenario with bound set. Reading the list alone would leave the
 * column empty under a map that is plainly running something.
 *
 * The list decides the order, including where the map's own script sits in
 * it. A host may now put that script anywhere, and the server both accepts a
 * list naming it and composes it at the position it was named at, so reading
 * the list straight through is what draws the round in the order it loads.
 *
 * The attached accessors are asked first only to find out whether the list
 * already carries that script. Where it does, this adds nothing of its own
 * and the row comes off the list with everything else, at its real place.
 * Where it does not — an older server, or a map whose script the list has
 * not been told about yet — the row is put at the front, which is where such
 * a server composes it. Either way one script is one row. */
static int lobbyRoundLive(ClientSim *cs, LobbyRoundRow *out, int max) {
    const char *attached = "";
    bool        listed   = false;
    int         n = 0;
    int         count;
    int         i;

    if (cs == NULL) return 0;

    count = clientSimGetLobbyScriptCount(cs);
    if (clientSimGetLobbyScenarioSource(cs) != 0) {
        attached = clientSimGetLobbyScenarioFileName(cs);
        for (i = 0; i < count; i++) {
            if (SDL_strcmp(clientSimGetLobbyScriptFile(cs, i), attached) == 0) {
                listed = true;
                break;
            }
        }
        if (!listed && n < max) {
            lobbyRoundSetRow(&out[n++], attached,
                             clientSimGetLobbyScenarioName(cs),
                             clientSimGetLobbyScenarioKeepsWinCondition(cs),
                             clientSimGetLobbyScenarioBound(cs));
        }
    }

    for (i = 0; i < count && n < max; i++) {
        lobbyRoundSetRow(&out[n++], clientSimGetLobbyScriptFile(cs, i),
                         clientSimGetLobbyScriptName(cs, i),
                         clientSimGetLobbyScriptKeepsWinCondition(cs, i),
                         clientSimGetLobbyScriptBound(cs, i));
    }
    return n;
}

/* Two compositions, by the file names in them and in that order. Names and
 * not the flags beside them: the flags come from the same manifest as the
 * name, so two lists naming the same files in the same order are the same
 * list. */
static bool lobbyRoundSameFiles(const LobbyRoundRow *a, int aCount,
                                const LobbyRoundRow *b, int bCount) {
    int i;

    if (aCount != bCount) return false;
    for (i = 0; i < aCount; i++) {
        if (SDL_strcmp(a[i].file, b[i].file) != 0) return false;
    }
    return true;
}

/* Where a file sits in the draft, and where the draft's scenario sits. Both
 * -1 for none. The scenario is found by kind and never by position: a round
 * running mods and no scenario is one a host can set up from here, and index
 * 0 would then name a mod. */
static int lobbyRoundIndexOfFile(const char *file) {
    int i;

    for (i = 0; i < s_roundCount; i++) {
        if (SDL_strcmp(s_round[i].file, file) == 0) return i;
    }
    return -1;
}

static int lobbyRoundIndexOfScenario(void) {
    int i;

    for (i = 0; i < s_roundCount; i++) {
        if (!s_round[i].mod) return i;
    }
    return -1;
}

/* How many draft rows would go into the command, which is all of them. The
 * bound row used to be left out because the server refused a list that named
 * one; it accepts the committed map's own script now, so that row takes a
 * place in CMD_SCRIPT_LIST_MAX like any other. */
static int lobbyRoundSendCount(void) {
    return s_roundCount;
}

/* Why a catalogue row cannot move over, as the lang id that says so, or 0
 * when it can. One function rather than a test at each of the two places a
 * row is taken from — the arrow and the double-click — so the button that is
 * greyed and the click that does nothing always agree about why. */
static int lobbyRoundWhyNotAdd(const LobbyScenarioRow *row) {
    int at;

    if (lobbyRoundIndexOfFile(row->file) >= 0) {
        /* The same file twice would load the same script twice, with two sets
           of its timers running against each other. The server refuses it,
           and this is what keeps the host from building one. */
        return STR_DLGLOBBY_SCENARIO_IN_ROUND;
    }
    if (!lobbyScenarioRowIsMod(row)) {
        at = lobbyRoundIndexOfScenario();
        if (at >= 0) {
            /* The committed map's own scenario is not this host's to swap
               out. Changing the map is what changes it, and that is what the
               row's own note says as well. */
            if (s_round[at].bound) return STR_DLGLOBBY_SCENARIO_MAP_LOCK;
            /* One goes out as one comes in, so the list does not grow and the
               cap below cannot be what stops it. */
            return 0;
        }
    }
    if (lobbyRoundSendCount() >= CMD_SCRIPT_LIST_MAX) {
        return STR_DLGLOBBY_SCENARIO_ROUND_FULL;
    }
    return 0;
}

/* A catalogue row into the draft.
 *
 * A scenario takes the front of the list, or the place of the scenario
 * already there. The front because that is where a host looking for the
 * script the round is decided by will look for it, and not because any layer
 * below requires it — the round reads the kind off each script's manifest and
 * composes a scenario that is not first perfectly happily. A mod goes on the
 * end. Either way the host moves it afterwards with the arrows. */
static void lobbyRoundAdd(const LobbyScenarioRow *row) {
    LobbyRoundRow *r;
    int            at;
    int            i;

    if (lobbyRoundWhyNotAdd(row) != 0) return;

    if (!lobbyScenarioRowIsMod(row)) {
        at = lobbyRoundIndexOfScenario();
        if (at < 0) {
            if (s_roundCount >= LOBBY_ROUND_MAX) return;
            for (i = s_roundCount; i > 0; i--) s_round[i] = s_round[i - 1];
            s_roundCount++;
            at = 0;
        }
    } else {
        if (s_roundCount >= LOBBY_ROUND_MAX) return;
        at = s_roundCount++;
    }
    r = &s_round[at];
    /* bound is the catalogue's own flag and is false for every row the left
       column offers. Copied rather than hard-coded false so a row that
       reached here another way is still drawn for what it is. */
    lobbyRoundSetRow(r, row->file, row->name, lobbyScenarioRowIsMod(row),
                     row->bound);
}

static void lobbyRoundDrop(int idx) {
    int i;

    if (idx < 0 || idx >= s_roundCount) return;
    if (s_round[idx].bound) return;
    for (i = idx; i + 1 < s_roundCount; i++) s_round[i] = s_round[i + 1];
    s_roundCount--;
}

/* Whether the row at idx may change places with its neighbour one step in
 * dir. Any two rows may. The only answer this gives is about the ends of the
 * list, where there is no neighbour to change places with.
 *
 * Load order is what the list is for: two scripts setting the same rule are
 * decided by which of them loads last, so a host has to be able to say, and a
 * picked scenario is as much a part of that order as a mod. The round loads
 * every script in the list in the order the host put them in —
 * scnDecideScenario in src/scenario/scenario_host.c — so a scenario that
 * is not first is still loaded and still decides the round.
 *
 * The map's own scenario moves with the rest. It used to be pinned here, and
 * that was never this dialog's rule: the server refused a list naming a bound
 * file and the composer put the map's script at position 0 whatever the host
 * asked for, so a move here would have moved the row on screen and nowhere
 * else. Both of those now do what the host says, and a region's identity no
 * longer comes from its script's position in the list, which is what made the
 * pin necessary. It still cannot be taken out of the round — that is
 * lobbyRoundDrop, and it is the map that decides it. */
static bool lobbyRoundMayMove(int idx, int dir) {
    int to = idx + dir;

    if (idx < 0 || idx >= s_roundCount) return false;
    if (to < 0 || to >= s_roundCount) return false;
    return true;
}

static void lobbyRoundMove(int idx, int dir) {
    LobbyRoundRow tmp;

    if (!lobbyRoundMayMove(idx, dir)) return;
    tmp                = s_round[idx];
    s_round[idx]       = s_round[idx + dir];
    s_round[idx + dir] = tmp;
}

/* The draft, sent as one list, every row of it in the order the host left
 * them in.
 *
 * The order is the whole message. A name's index in this array is the
 * position the round composes that script at, which is what lets a host say
 * which of two scripts setting the same rule loads last.
 *
 * The bound row goes too. It used to be left out because the server refused
 * any list naming a bound file; the CMD_SET_SCRIPT_LIST arm of
 * src/server/server_command_dispatch.c now makes one exception, for the
 * committed map's own script, so that row is named like the rest and lands
 * where the host put it. Every other bound file is still refused, and the
 * left column still never offers one. */
static void lobbyRoundSend(ClientSim *cs) {
    const char *files[CMD_SCRIPT_LIST_MAX];
    int         n = 0;
    int         i;

    for (i = 0; i < s_roundCount && n < CMD_SCRIPT_LIST_MAX; i++) {
        files[n++] = s_round[i].file;
    }
    clientSimNetSendSetScriptList(cs, files, n);
}

/* ── The details dialog's snapshot ────────────────────────────────
 * Copied out of whatever opened it rather than pointed at. A listing is
 * replaced wholesale when the server answers again and the attached script
 * goes when a map is committed, and this dialog outlives both — a row
 * described by a pointer into either would be describing freed or reused
 * bytes a frame later.
 *
 * attached says the dialog is describing the script the round is running,
 * which is where its description comes from. kind is -1 where it
 * is not known: see lobbyScenarioDetailsOpenRow. */
static bool s_detailsOpen     = false;
static bool s_detailsWantOpen = false;
static char s_detailsFile[SERVER_SCENARIO_FILE_LEN];
static char s_detailsName[SERVER_SCENARIO_NAME_LEN];
static char s_detailsDesc[SERVER_SCENARIO_DESC_LEN];
static int  s_detailsMaxPlayers = 0;
static int  s_detailsBots       = 0;
static bool s_detailsBound      = false;
/* True when this dialog describes the one attached script
   CTRL_LOBBY_SETTINGS talks about, whose description comes from there
   rather than from the directory listing. Its rules and callbacks are
   fetched by file name like every other row's. */
static bool s_detailsAttached   = false;
static int  s_detailsKind       = -1;   /* -1 unknown, 0 scenario, 1 mod */

void lobbyScenarioDetailsReset(void) {
    s_detailsOpen     = false;
    s_detailsWantOpen = false;
    s_detailsAttached = false;
    s_detailsKind     = -1;
    s_detailsFile[0]  = '\0';
    s_detailsName[0]  = '\0';
    s_detailsDesc[0]  = '\0';
}

bool lobbyScenarioDetailsIsOpen(void) {
    return s_detailsOpen;
}

void lobbyScenarioChooserReset(void) {
    s_open        = false;
    s_asked       = false;
    s_focusedOnce = false;
    s_localCount  = 0;
    s_filter[0]   = '\0';
    s_kindFilter  = 0;
    s_roundTaken  = false;
    s_roundCount  = 0;
    s_liveCount   = 0;
    lobbyScenarioDetailsReset();
}

bool lobbyScenarioChooserIsOpen(void) {
    return s_open;
}

void lobbyScenarioChooserOpen(void) {
    s_open        = true;
    s_asked       = false;
    s_focusedOnce = false;
    s_localCount  = 0;
    s_filter[0]   = '\0';
    s_kindFilter  = 0;
    /* The draft is not taken here. It is taken on the first frame the window
       renders, where the ClientSim is in hand — this is called from a button
       that has nothing but the fact it was pressed. */
    s_roundTaken  = false;
    s_roundCount  = 0;
    s_liveCount   = 0;
}

/* The numbers under a row: the human cap the scenario asks for and the seats
 * its lobby holds for bots. Each is drawn only where the manifest gave one —
 * 0 means it said nothing rather than none — so a scenario that named neither
 * gets no line instead of a line of zeroes. */
static void lobbyScenarioRowCaps(int maxPlayers, int bots) {
    MessageArgs args = {};
    bool        drew = false;

    if (maxPlayers > 0) {
        args.number = maxPlayers;
        ImGui::TextDisabled("%s",
            langGetTextFmt(STR_DLGLOBBY_SCENARIO_MAXPLAYERS, &args));
        drew = true;
    }
    if (bots > 0) {
        MessageArgs botArgs = {};
        botArgs.number = bots;
        if (drew) ImGui::SameLine(0.0f, 16.0f);
        ImGui::TextDisabled("%s",
            langGetTextFmt(STR_DLGLOBBY_SCENARIO_BOTS, &botArgs));
    }
}

/* The map a bound script belongs to, read off the script's file name: the
 * same stem with .scenario.lua where the map has .map. Any directory in
 * front of it goes too, though what the lobby carries is a file name and not
 * a path. A name with no such tail is copied whole. */
static void lobbyScenarioMapOfFile(const char *file, char *out,
                                   size_t outLen) {
    const char *base = file;
    const char *at;
    size_t      n;

    for (at = file; *at != '\0'; at++) {
        if (*at == '/' || *at == '\\') base = at + 1;
    }
    n = SDL_strlen(base);
    if (n > 13 && SDL_strcmp(base + n - 13, ".scenario.lua") == 0) {
        n -= 13;
    } else if (n > 4 && SDL_strcmp(base + n - 4, ".lua") == 0) {
        n -= 4;
    }
    if (n + 1 > outLen) n = outLen - 1;
    SDL_memcpy(out, base, n);
    out[n] = '\0';
}

/* The one-word tag that says which of the two a row is, drawn after the name
 * it belongs to. It puts its own SameLine in front of itself, so a caller
 * draws the name and then calls this.
 *
 * A filled pill rather than another line of grey prose. A row already carries
 * a caps line in grey under it, and a third grey line saying "Mod" would be
 * the least visible thing on a row where it is the one fact that decides what
 * the arrow beside it does. It is also why the list and the details dialog
 * call the same function: a tag that looked like two different things in two
 * places would be read as two different facts.
 *
 * Two fixed fill colours and the theme's own text colour over them. The fills
 * are fixed because what they separate is a fixed pair — there is no third
 * kind for a palette to run out of — and the text is not, so a theme change
 * cannot leave the word unreadable. */
/* A line of text leaned over to the right. One font is loaded and it has no
 * italic cut, so the lean is put on the glyphs after they are laid out: the
 * text is drawn the ordinary way, and every vertex it added is then pushed
 * sideways by how far it sits above the line's own bottom. The bottom does
 * not move and the top moves most, which is the shape of an italic.
 *
 * It is the note under a heading and not a heading itself, so it is drawn in
 * the dim text colour the rest of the lobby uses for a note. */
static void lobbyScenarioItalicNote(const char *text) {
    ImDrawList *dl    = ImGui::GetWindowDrawList();
    int         first = dl->VtxBuffer.Size;
    int         last;
    float       base;
    int         v;

    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();

    /* Nothing was drawn when the line is scrolled out of sight or clipped
       away, and there is then nothing to lean. */
    last = dl->VtxBuffer.Size;
    if (last == first) {
        return;
    }
    base = ImGui::GetItemRectMax().y;
    for (v = first; v < last; v++) {
        ImDrawVert *vert = &dl->VtxBuffer[v];
        vert->pos.x += (base - vert->pos.y) * 0.22f;
    }
}

/* Both columns of the chooser build their head out of the same three slots —
 * a heading, one row under it, and the rule that ends the head — so the two
 * lists start at the same y and a host reads one row of the left against one
 * row of the right. Two lists side by side whose first rows do not line up
 * read as two lists of different things.
 *
 * The left column's second slot holds its filter box, so the slot is as tall
 * as a framed control. This is what a column whose second slot is one line of
 * text calls instead of drawing that line itself: the line is centred in the
 * taller slot and the cursor is left where the filter box would have left it.
 * Cursor moves and not a Dummy either side, because a Dummy is an item and
 * would put its own spacing between the two halves of one slot. */
static void lobbyScenarioHeadNote(const char *text) {
    float slack = ImGui::GetFrameHeight() - ImGui::GetTextLineHeight();
    float top   = slack * 0.5f;

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + top);
    lobbyScenarioItalicNote(text);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (slack - top));
}

/* The Mod / Scenario chip every row below wears, and the width a row measures
   its name against before drawing either, are lobbyScenarioKindTag and
   lobbyScenarioKindTagWidth — declared in lobby_internal.h and defined in
   lobby_assets.cpp beside the plain name tag they are drawn from. They were
   written here, and moved when the map panel's script links started wearing
   the same chip: two copies of one chip's padding and colours are two chips
   waiting to stop matching each other. */

/* One line of grey prose under a row, wrapped to the dialog's width. The same
 * grey the scenario line itself uses for a description, so the two read as
 * one voice. */
static void lobbyScenarioRowNote(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

/* A row of this listing, described. PACKET_LOBBY_SCENARIO_LIST_RSP packs
 * file, name, description, maxPlayers, bots, bound and keepsWinCondition, and
 * ServerScenarioEntry matches it, so every field the dialog shows is the
 * manifest's own answer about this row and none of it is inferred.
 *
 * The kind is read from keepsWinCondition and never from bound. Guessing it
 * from bound would be wrong twice over: a scenario may be unbound and a mod
 * may be written for one map. */
static void lobbyScenarioDetailsOpenRow(const LobbyScenarioRow *row) {
    SDL_snprintf(s_detailsFile, sizeof(s_detailsFile), "%s", row->file);
    SDL_snprintf(s_detailsName, sizeof(s_detailsName), "%s", row->name);
    SDL_snprintf(s_detailsDesc, sizeof(s_detailsDesc), "%s", row->description);
    s_detailsMaxPlayers = row->maxPlayers;
    s_detailsBots       = row->bots;
    s_detailsBound      = row->bound;
    s_detailsAttached   = false;
    s_detailsKind       = lobbyScenarioRowIsMod(row) ? 1 : 0;
    s_detailsOpen       = true;
    s_detailsWantOpen   = true;
}

/* The script the round is running, described. The kind is known here and
 * nowhere else: clientSimGetLobbyScenarioKeepsWinCondition is the manifest's
 * own answer, carried on CTRL_LOBBY_SETTINGS for the attached script alone.
 *
 * The player and bot counts are the listing's numbers and do not travel with
 * the attachment, so they are left at zero, which lobbyScenarioRowCaps reads
 * as "said nothing" rather than as none. bound is the attached script's
 * own flag, clientSimGetLobbyScenarioBound, and not the source: the source
 * says where the script came from, which is a different question from
 * which map it was written for. A host may pick a bound script out of the
 * server's directory, so the source cannot stand in for it.
 *
 * lobbyScenarioDetailsOpenScript below is what the lobby's own icons call.
 * This one is kept for the client that has no script list yet, where there
 * is one attached script and no row to index. */
void lobbyScenarioDetailsOpenAttached(ClientSim *cs) {
    const char *name;

    if (cs == NULL) return;
    name = clientSimGetLobbyScenarioName(cs);
    if (name[0] == '\0') name = clientSimGetLobbyScenarioFileName(cs);

    SDL_snprintf(s_detailsFile, sizeof(s_detailsFile), "%s",
                 clientSimGetLobbyScenarioFileName(cs));
    SDL_snprintf(s_detailsName, sizeof(s_detailsName), "%s", name);
    SDL_snprintf(s_detailsDesc, sizeof(s_detailsDesc), "%s",
                 clientSimGetLobbyScenarioDescription(cs));
    s_detailsMaxPlayers = 0;
    s_detailsBots       = 0;
    s_detailsBound      = clientSimGetLobbyScenarioBound(cs);
    s_detailsAttached   = true;
    s_detailsKind       = clientSimGetLobbyScenarioKeepsWinCondition(cs) ? 1 : 0;
    s_detailsOpen       = true;
    s_detailsWantOpen   = true;
}

/* The same dialog for one row of the lobby's ordered script list. The icons
 * beside the scenario line and the mods line both land here, each with its
 * own row, so a round running a scenario and a mod at once gives two icons
 * that describe two different scripts.
 *
 * The list carries no description: the catalogue response is what carries
 * every description, keyed by file name. So the description is taken from
 * the attached-scenario accessors only when the file names agree, and left
 * empty otherwise. An empty description drops that block of the dialog,
 * which is better than showing one script's prose under another's name.
 *
 * A negative index means the caller found no row of that kind, which cannot
 * happen from a line that drew itself, and draws nothing rather than opening
 * an empty dialog. */
/* The directory listing this file answers descriptions out of, fetched
 * once. Split out of the chooser window so a details dialog opened from a
 * line in the lobby -- the map panel's links, the Game Type row's -- can ask
 * for it as well: those lines are drawn whether or not the chooser was ever
 * opened, and a description nothing can look up is a dialog with a name and
 * no word about what the thing does.
 *
 * A server in this process is read directly, under the sim mutex, because
 * the request is a UDP packet and a lobby hosted here has no transport to
 * send it on. A remote server is asked and answers later. */
static void lobbyScenarioCatalogueEnsure(ClientSim *cs) {
    ServerSim *sim;

    if (s_asked || cs == NULL) return;
    sim = gameFrontGetServerSim();
    if (sim != NULL) {
        threadsWaitForMutex();
        s_localCount = serverSimEnumerateScenarioDir(
            sim, s_localRows, LOBBY_SCENARIO_CHOOSER_MAX);
        threadsReleaseMutex();
    } else {
        clientSimNetSendLobbyScenarioListRequest(cs);
    }
    s_asked = true;
}

/* What the directory says a file is for, by file name. The script list on
 * the wire carries no description -- LobbyScriptEntry in control_event.h
 * says so and points here -- so this is where the description of a script
 * the round is already running comes from.
 *
 * Empty when the listing does not hold the file, or has not arrived yet: a
 * remote server answers over UDP and a dialog can open before the reply
 * lands, which is why the dialog asks again on every frame it has nothing. */
static const char *lobbyScenarioDescOfFile(ClientSim *cs, const char *file) {
    int i, n;

    if (file == NULL || file[0] == '\0') return "";
    for (i = 0; i < s_localCount; i++) {
        if (SDL_strcmp(s_localRows[i].file, file) == 0) {
            return s_localRows[i].description;
        }
    }
    if (cs == NULL) return "";
    n = clientSimGetLobbyScenarioListCount(cs);
    for (i = 0; i < n; i++) {
        if (SDL_strcmp(clientSimGetLobbyScenarioListFile(cs, i), file) == 0) {
            return clientSimGetLobbyScenarioListDescription(cs, i);
        }
    }
    return "";
}

void lobbyScenarioDetailsOpenScript(ClientSim *cs, int idx) {
    const char *file;
    const char *name;
    bool        same;

    if (cs == NULL || idx < 0) return;
    lobbyScenarioCatalogueEnsure(cs);
    if (clientSimGetLobbyScriptCount(cs) <= 0) {
        lobbyScenarioDetailsOpenAttached(cs);
        return;
    }

    file = clientSimGetLobbyScriptFile(cs, idx);
    name = clientSimGetLobbyScriptName(cs, idx);
    if (name[0] == '\0') name = file;

    /* The attached-script accessors answer for one file. This row is that
       file or it is not, and where it is not the description comes from the
       directory instead. The rules table and the callbacks are fetched by
       file name for every row (lobbyScenarioDetailsOfFile). */
    same = SDL_strcmp(file, clientSimGetLobbyScenarioFileName(cs)) == 0;

    SDL_snprintf(s_detailsFile, sizeof(s_detailsFile), "%s", file);
    SDL_snprintf(s_detailsName, sizeof(s_detailsName), "%s", name);
    if (same) {
        SDL_snprintf(s_detailsDesc, sizeof(s_detailsDesc), "%s",
                     clientSimGetLobbyScenarioDescription(cs));
    } else {
        /* Not the composed base, so the attached-scenario accessors describe
           some other file and the directory is asked instead. A mod running
           beside a map's own scenario is always this case, and it used to
           leave the dialog with no description at all. */
        SDL_snprintf(s_detailsDesc, sizeof(s_detailsDesc), "%s",
                     lobbyScenarioDescOfFile(cs, file));
    }
    s_detailsMaxPlayers = 0;
    s_detailsBots       = 0;
    s_detailsBound      = clientSimGetLobbyScriptBound(cs, idx);
    s_detailsAttached   = same;
    s_detailsKind       = clientSimGetLobbyScriptKeepsWinCondition(cs, idx)
                              ? 1 : 0;
    s_detailsOpen       = true;
    s_detailsWantOpen   = true;
}

/* A rule's value as a player reads it: whole numbers without a decimal
 * point, rates to two places with the trailing zeros trimmed. The same
 * trimming simRulesPhrase does to a multiple, because the two numbers land
 * in neighbouring columns and must not disagree about how a number looks. */
static void lobbyScenarioDetailsNumber(double value, char *buf,
                                       size_t bufLen) {
    size_t len;

    SDL_snprintf(buf, bufLen, "%.2f", value);
    if (SDL_strchr(buf, '.') == NULL) return;
    len = SDL_strlen(buf);
    while (len > 0 && buf[len - 1] == '0') buf[--len] = '\0';
    if (len > 0 && buf[len - 1] == '.') buf[--len] = '\0';
}

/* The details of a file (scenario_details.h): the file's own rules table and
 * what its callbacks do, looked up by file name, so the same lookup serves a
 * row nobody has picked, a row of the host's draft, a row of the round's
 * committed list and the committed map's own script.
 *
 * The ClientSim keeps them per file. A file it has no answer for yet is
 * asked for: a server in this process is read at once, under the sim mutex,
 * because a lobby hosted here has no transport to send a request on; a
 * remote server is sent a request, which the transport resends while no
 * answer comes and gives up on after a few tries. The dialog forgets every
 * answer when it opens (lobbyScenarioDetailsRenderModal), so each opening
 * asks again and a file that got no answer last time gets another chance.
 *
 * The state says which answer is in hand. FOUND with *len 0 is a file that
 * declares nothing; WAITING is one whose answer has not come; NONE is one
 * the server does not know or that got no answer this opening. The bytes
 * are NULL unless the state is FOUND and there are some. */
static ClientScnDetailsState lobbyScenarioDetailsOfFile(ClientSim *cs,
                                                        const char *file,
                                                        const uint8_t **bytes,
                                                        size_t *len) {
    ClientScnDetailsState state;
    ServerSim            *sim;

    *bytes = NULL;
    *len   = 0;
    if (cs == NULL || file == NULL || file[0] == '\0') {
        return CLIENT_SCN_DETAILS_NONE;
    }
    state = clientSimGetLobbyScenarioDetails(cs, file, bytes, len);
    if (state != CLIENT_SCN_DETAILS_UNKNOWN) return state;

    sim = gameFrontGetServerSim();
    if (sim != NULL) {
        uint8_t blob[SCN_DETAILS_MAX];
        int     got;

        threadsWaitForMutex();
        got = serverSimScenarioDetails(sim, file, blob, sizeof(blob));
        threadsReleaseMutex();
        clientSimLobbyScenarioDetailsPut(cs, file, got >= 0, blob,
                                         got > 0 ? (size_t)got : 0);
    } else {
        clientSimLobbyScenarioDetailsWant(cs, file);
    }
    state = clientSimGetLobbyScenarioDetails(cs, file, bytes, len);
    /* Every slot waiting on an answer is the one way the ask above is not
       taken; reading that as waiting asks again on the next frame. */
    return state == CLIENT_SCN_DETAILS_UNKNOWN ? CLIENT_SCN_DETAILS_WAITING
                                               : state;
}

/* The order the dialog judges "overridden" against: the host's draft while
 * the chooser is open and holding one, since that is the order the host is
 * looking at and about to send, and the round as the lobby reports it
 * otherwise. Either way it is load order, first row first. */
static int lobbyScenarioDetailsOrder(ClientSim *cs, LobbyRoundRow *out,
                                     int max) {
    int i;

    if (s_open && s_roundTaken) {
        for (i = 0; i < s_roundCount && i < max; i++) out[i] = s_round[i];
        return i;
    }
    return lobbyRoundLive(cs, out, max);
}

/* The width shared by the rules table's three number columns (Classic, New
 * value, Change), so the three read as one block: wide enough for each header
 * whole and for the numbers and short phrases the columns hold. A longer
 * phrase, and the overridden line, wrap inside the Change column. Measured
 * each frame, because the font and the language can change under the lobby. */
static float lobbyScenarioDetailsNumberColumnWidth(void) {
    static const int texts[] = {
        STR_DLGLOBBY_RULES_COL_CLASSIC,
        STR_DLGLOBBY_DETAILS_COL_NEW_VALUE,
        STR_DLGLOBBY_RULES_COL_CHANGE,
        STR_RULE_UNCHANGED,
        STR_RULE_ON,
        STR_RULE_OFF,
    };
    MessageArgs args = {};
    float       w    = ImGui::CalcTextSize("-00000.00").x;
    size_t      i;

    for (i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        w = SDL_max(w, ImGui::CalcTextSize(langGetText(texts[i])).x);
    }
    /* A multiple, the longest kind of phrase the column holds on one line. */
    SDL_snprintf(args.string1, sizeof(args.string1), "%s", "2.5");
    w = SDL_max(w,
                ImGui::CalcTextSize(langGetTextFmt(STR_RULE_FASTER, &args)).x);
    return w;
}

/* The narrowest the dialog may be: a rule name of ordinary length beside the
 * three number columns, each with its cell padding, inside the window's own
 * padding and a scrollbar. */
static float lobbyScenarioDetailsMinWidth(void) {
    const ImGuiStyle &st = ImGui::GetStyle();

    return ImGui::CalcTextSize("tank_full_shells").x +
           3.0f * lobbyScenarioDetailsNumberColumnWidth() +
           8.0f * st.CellPadding.x + 2.0f * st.WindowPadding.x +
           st.ScrollbarSize + 2.0f * st.ItemSpacing.x;
}

/* The rules table's header row, shared by both tables below. False when the
 * table did not begin and nothing more is to be drawn. The rule's name takes
 * whatever the three equal number columns leave. The value column is headed
 * "New value" for every kind of script; the host's Rules popup keeps its own
 * "Scenario" header, which is why the two ids differ. */
static bool lobbyScenarioDetailsRulesBegin(void) {
    float w = lobbyScenarioDetailsNumberColumnWidth();

    ImGui::Spacing();
    ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_SCENARIO_RULES));
    if (!ImGui::BeginTable("##detailRules", 4,
                           ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_BordersInnerH)) {
        return false;
    }
    ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_RULES_COL_RULE),
                            ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_RULES_COL_CLASSIC),
                            ImGuiTableColumnFlags_WidthFixed, w);
    ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_DETAILS_COL_NEW_VALUE),
                            ImGuiTableColumnFlags_WidthFixed, w);
    ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_RULES_COL_CHANGE),
                            ImGuiTableColumnFlags_WidthFixed, w);
    ImGui::TableHeadersRow();
    return true;
}

/* One row of a rules table. overriddenBy is the name of the script whose
 * value plays instead, or NULL when this row's value is the one that plays;
 * an overridden row is drawn in the disabled colour and its last column says
 * who wins and with what. */
static void lobbyScenarioDetailsRuleRow(int rule, double value,
                                        const char *overriddenBy,
                                        double winning) {
    char classicText[32];
    char valueText[32];
    char phrase[96];

    lobbyScenarioDetailsNumber(simRulesClassicValue(rule), classicText,
                               sizeof(classicText));
    lobbyScenarioDetailsNumber(value, valueText, sizeof(valueText));
    simRulesPhrase(rule, value, phrase, sizeof(phrase));

    ImGui::TableNextRow();
    if (overriddenBy != NULL) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    }
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(simRulesRuleName(rule));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", simRulesRuleDescription(rule));
    }
    ImGui::TableSetColumnIndex(1);
    /* The classic number is what the row is read against rather than what
       the script did, so it is the quieter of the two. */
    ImGui::TextDisabled("%s", classicText);
    ImGui::TableSetColumnIndex(2);
    ImGui::TextUnformatted(valueText);
    ImGui::TableSetColumnIndex(3);
    if (overriddenBy != NULL) {
        MessageArgs args = {};

        SDL_snprintf(args.string1, sizeof(args.string1), "%s", overriddenBy);
        lobbyScenarioDetailsNumber(winning, args.string2,
                                   sizeof(args.string2));
        ImGui::TextWrapped("%s",
                           langGetTextFmt(STR_DLGLOBBY_RULES_OVERRIDDEN, &args));
        ImGui::PopStyleColor();
    } else {
        ImGui::TextWrapped("%s", phrase);
    }
}

/* What the script's rules table says, folded into this dialog.
 *
 * The Rules button stayed in the Server Settings column, which is the host's
 * deep view — a row per rule with an Info popup a controller can land on. It
 * is gone from the map panel, which is now text and one icon, and a joiner's
 * only real question about a scenario is what it changes. So the table comes
 * along here, read-only and without the per-rule popup: a popup over a popup
 * over a window is more stack than the answer is worth, and the rule's own
 * description is on the row's hover.
 *
 * The table is the file's own, as its author wrote it, for every kind of row
 * the dialog opens on. Which script's value plays where two set the same
 * rule is worked out here, against the order the host is looking at (see
 * lobbyScenarioDetailsOrder): the first script on it that sets the rule
 * wins, as the round composes it, and the rows this script loses say so. A
 * mod on a server with mods turned off loads nothing, so it neither wins a
 * rule over another script nor has its own rules play, and a note under its
 * table says so rather than the table going missing. A mod that sets no rule
 * has no table and still gets the note, because the mod does not load
 * either way. */
static void lobbyScenarioDetailsRules(ClientSim *cs) {
    LobbyRoundRow  order[LOBBY_ROUND_MAX];
    const uint8_t *blobs[LOBBY_ROUND_MAX];
    size_t         lens[LOBBY_ROUND_MAX];
    const uint8_t *mine    = NULL;
    size_t         mineLen = 0;
    bool           modsOn  = clientSimGetLobbyModsEnabled(cs);
    bool           waiting = false;
    int            n;
    int            self = -1;
    int            count;
    int            i;

    n = lobbyScenarioDetailsOrder(cs, order, LOBBY_ROUND_MAX);
    for (i = 0; i < n; i++) {
        if (SDL_strcmp(order[i].file, s_detailsFile) == 0) {
            self = i;
            break;
        }
    }
    /* Every script on the list is asked for, in list order, so the answers
       the overridden column needs are coming while this one's own is. */
    for (i = 0; i < n; i++) {
        blobs[i] = NULL;
        lens[i]  = 0;
        if (order[i].mod && !modsOn) continue;
        if (lobbyScenarioDetailsOfFile(cs, order[i].file, &blobs[i],
                                       &lens[i]) ==
                CLIENT_SCN_DETAILS_WAITING &&
            i < self) {
            waiting = true;
        }
    }

    count = 0;
    if (lobbyScenarioDetailsOfFile(cs, s_detailsFile, &mine, &mineLen) ==
        CLIENT_SCN_DETAILS_FOUND) {
        count = scnDetailsRuleCount(mine, mineLen);
    }
    /* Not drawn until every script ahead of this one has answered, so a row
       is never shown as playing and then, a moment later, as overridden. */
    if (count > 0 && !waiting && lobbyScenarioDetailsRulesBegin()) {
        for (i = 0; i < count; i++) {
            int    rule;
            double value;
            double winning = 0.0;
            int    winner;

            if (!scnDetailsRuleAt(mine, mineLen, i, &rule, &value)) continue;
            winner = scnDetailsRuleWinner(blobs, lens, self, rule, &winning);
            lobbyScenarioDetailsRuleRow(
                rule, value, winner >= 0 ? order[winner].name : NULL,
                winning);
        }
        ImGui::EndTable();
    }

    /* Shown whether or not the table is: a mod on a server with mods off
       loads nothing, rules or not, and that does not wait on any answer. */
    if (!modsOn && s_detailsKind == 1) {
        lobbyScenarioRowNote(langGetText(STR_DLGLOBBY_DETAILS_MODS_OFF));
    }
}

/* The Type column's word for a row's SCN_CB_TYPE_*. */
static const char *lobbyScenarioCallbackType(uint8_t type) {
    switch (type) {
        case SCN_CB_TYPE_QUERY:
            return langGetText(STR_DLGLOBBY_DETAILS_TYPE_QUERY);
        case SCN_CB_TYPE_TRIGGER:
            return langGetText(STR_DLGLOBBY_DETAILS_TYPE_TRIGGER);
        default:
            return langGetText(STR_DLGLOBBY_DETAILS_TYPE_EVENT);
    }
}

/* What the script implements: one row per callback it describes, from the
 * callbacks block of its manifest, in the order the load kept them. Method is
 * the callback's own name, as the author finds it in the file; Type is what
 * the engine does with it (worked out on the server, see
 * scenario_callbacks.h); the overview is the author's sentence, wrapped.
 * Drawn under the rules table, or where it would be for a script that sets no
 * rule, and not at all for a script that describes nothing.
 *
 * The same look as the rules table. Method and Type are as wide as their
 * longest entry or header, and the overview takes the rest. */
static void lobbyScenarioDetailsCallbacks(ClientSim *cs) {
    static const int types[] = {
        STR_DLGLOBBY_DETAILS_COL_TYPE,
        STR_DLGLOBBY_DETAILS_TYPE_EVENT,
        STR_DLGLOBBY_DETAILS_TYPE_QUERY,
        STR_DLGLOBBY_DETAILS_TYPE_TRIGGER,
    };
    const uint8_t *blob;
    const uint8_t *cb;
    size_t         len;
    size_t         cbLen;
    size_t         t;
    float          methodW;
    float          typeW = 0.0f;
    int            rows;
    int            i;

    if (lobbyScenarioDetailsOfFile(cs, s_detailsFile, &blob, &len) !=
        CLIENT_SCN_DETAILS_FOUND) {
        return;
    }
    cb   = scnDetailsCallbacks(blob, len, &cbLen);
    rows = scnCallbacksBlobCount(cb, cbLen);
    if (rows <= 0) return;

    methodW =
        ImGui::CalcTextSize(langGetText(STR_DLGLOBBY_DETAILS_COL_METHOD)).x;
    for (i = 0; i < rows; i++) {
        char name[SCN_CALLBACK_NAME_LEN];

        if (scnCallbacksBlobRow(cb, cbLen, i, NULL, name, sizeof(name), NULL,
                                0)) {
            methodW = SDL_max(methodW, ImGui::CalcTextSize(name).x);
        }
    }
    for (t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
        typeW = SDL_max(typeW, ImGui::CalcTextSize(langGetText(types[t])).x);
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(
        langGetText(s_detailsKind == 1
                        ? STR_DLGLOBBY_DETAILS_IMPLEMENTS_MOD
                        : STR_DLGLOBBY_DETAILS_IMPLEMENTS_SCENARIO));
    if (!ImGui::BeginTable("##detailCallbacks", 3,
                           ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_BordersInnerH)) {
        return;
    }
    ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_DETAILS_COL_METHOD),
                            ImGuiTableColumnFlags_WidthFixed, methodW);
    ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_DETAILS_COL_TYPE),
                            ImGuiTableColumnFlags_WidthFixed, typeW);
    ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_DETAILS_COL_OVERVIEW),
                            ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (i = 0; i < rows; i++) {
        char    name[SCN_CALLBACK_NAME_LEN];
        char    text[SCN_CALLBACK_TEXT_LEN];
        uint8_t type;

        if (!scnCallbacksBlobRow(cb, cbLen, i, &type, name, sizeof(name),
                                 text, sizeof(text))) {
            continue;
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(name);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(lobbyScenarioCallbackType(type));
        ImGui::TableSetColumnIndex(2);
        ImGui::TextWrapped("%s", text);
    }
    ImGui::EndTable();
}

/* One scenario or mod, described in full.
 *
 * Rendered from the lobby's own frame and not from any of the three places
 * that open it. BeginPopupModal only finds a popup opened at its own id
 * scope, and the three asks are at three different ones — the scenario line
 * inside the settings form, the same line inside the map panel, and a row
 * inside the chooser, which is a top-level window of its own drawn after the
 * lobby window has ended. No single scope sees all three, so none of them
 * calls OpenPopup: each sets a flag and this call, at the lobby window's
 * scope, is what turns the flag into a popup. The rules dialog and the bot
 * docs dialog are here for the same reason.
 *
 * A modal rather than a window beside the chooser, because a modal takes the
 * controller's focus and, by taking it, stops the chooser's own footer from
 * reading the Escape that is meant for this one — DialogFooter gates on
 * IsWindowFocused, so the window underneath answers nothing while this is
 * up. */
void lobbyScenarioDetailsRenderModal(ClientSim *cs, float s) {
    char        title[256];
    MessageArgs args;
    bool        open = true;
    float       footerH;

    if (s_detailsWantOpen) {
        s_detailsWantOpen = false;
        /* Each opening asks the server afresh (lobbyScenarioDetailsOfFile),
           so a script edited since, or one whose answer was lost, is right
           the next time the dialog is opened. */
        clientSimLobbyScenarioDetailsForget(cs);
        ImGui::OpenPopup(LOBBY_SCENARIO_DETAILS_ID);
    }
    if (!s_detailsOpen) return;
    if (cs == NULL) {
        s_detailsOpen = false;
        return;
    }

    SDL_memset(&args, 0, sizeof(args));
    SDL_snprintf(args.string1, sizeof(args.string1), "%s", s_detailsName);
    SDL_snprintf(title, sizeof(title), "%s%s",
                 langGetTextFmt(STR_DLGLOBBY_SCENARIO_DETAILS_TITLE, &args),
                 LOBBY_SCENARIO_DETAILS_ID);

    {
        ImVec2 vp = ImGui::GetMainViewport()->Size;
        ImGui::SetNextWindowSize(ImVec2(SDL_min(560.0f, vp.x * 0.85f),
                                        SDL_min(420.0f, vp.y * 0.85f)),
                                 ImGuiCond_Appearing);
        /* No narrower than the rules table needs to show its three number
           columns and their headers whole, where the screen has the room. */
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(SDL_min(lobbyScenarioDetailsMinWidth(), vp.x), 0.0f),
            ImVec2(FLT_MAX, FLT_MAX));
    }
    if (!ImGui::BeginPopupModal(title, &open, ImGuiWindowFlags_NoCollapse)) {
        /* Not begun means it is closed — by the title-bar X, or by an
           Escape that reached the popup itself. */
        s_detailsOpen = false;
        return;
    }

    /* The body scrolls in a box of its own so the Close row stays at the
       bottom, where every other dialog in the lobby keeps it. */
    footerH = ImGui::GetStyle().ItemSpacing.y * 3.0f + 1.0f +
              ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##scnDetailBody", ImVec2(0.0f, -footerH));
    {
        ImGui::TextUnformatted(s_detailsName);
        /* The same tag the list draws, beside the name for the same reason:
           which of the two this is answers the first question a host has
           about it. Drawn here only when the kind is known — a row opened
           from a source that did not carry it gets no tag rather than a
           guessed one. */
        if (s_detailsKind >= 0) lobbyScenarioKindTag(s_detailsKind == 1, s);
        /* The file under the name and quieter than it, because the name is
           what a host picked the thing by and the file is how they find it
           on disk when they want to read it. */
        ImGui::TextDisabled("%s %s", langGetText(STR_MAPEDIT_IMG_FILE),
                            s_detailsFile);
        ImGui::Separator();

        /* Asked again on every frame it has nothing. A remote server's
           listing arrives over UDP and can land after this dialog opened, and
           a description that turned up a frame late is still the
           description. */
        if (s_detailsDesc[0] == '\0' && !s_detailsAttached) {
            SDL_snprintf(s_detailsDesc, sizeof(s_detailsDesc), "%s",
                         lobbyScenarioDescOfFile(cs, s_detailsFile));
        }
        if (s_detailsDesc[0] != '\0') {
            ImGui::TextWrapped("%s", s_detailsDesc);
            ImGui::Spacing();
        }

        lobbyScenarioRowCaps(s_detailsMaxPlayers, s_detailsBots);
        if (s_detailsBound) {
            MessageArgs args = {};

            /* The map is named from the script's own file name. A bound
               script is the one read from beside its map, which is the same
               name with .scenario.lua where the map has .map, so the file
               says which map holds it without the lobby having to carry a
               second name for it. A file that somehow arrived without the
               tail is printed as it stands, which is still the closest thing
               to a map name there is here. */
            lobbyScenarioMapOfFile(s_detailsFile, args.string1,
                                   sizeof(args.string1));
            lobbyScenarioRowNote(
                langGetTextFmt(STR_DLGLOBBY_SCENARIO_BOUND, &args));
        }

        /* Asked on every frame for the reason the description is: a remote
           server's listing, and each script's details, can land after this
           dialog opened. Nothing is drawn for a table whose answer has not
           come. */
        lobbyScenarioCatalogueEnsure(cs);
        lobbyScenarioDetailsRules(cs);
        lobbyScenarioDetailsCallbacks(cs);
    }
    ImGui::EndChild();

    /* [Close] only — nothing here is edited, so there is nothing to
       confirm and nothing to back out of. */
    if (WBUI::DialogFooter(/*cancelLabel*/ nullptr,
                           /*confirmLabel*/ langGetText(STR_CLOSE))
            != WBUI::FOOTER_NONE ||
        !open) {
        s_detailsOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/* One of the four arrows, with the greying and the reason in one place.
 *
 * The reason is asked for with AllowWhenDisabled, because the arrow that most
 * needs one is the arrow that will not work. Without that flag ImGui reports
 * a disabled item as not hovered and the host is left with a grey button and
 * no word about it. */
static bool lobbyScenarioArrow(const char *id, ImGuiDir dir, bool enabled,
                               const char *tip) {
    bool clicked;

    if (!enabled) ImGui::BeginDisabled();
    clicked = ImGui::ArrowButton(id, dir);
    if (!enabled) ImGui::EndDisabled();
    if (enabled) imguiHandOnHover();
    if (tip != NULL &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip |
                             ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tip);
    }
    return clicked && enabled;
}

/* The details dialog for a row of the right column.
 *
 * The lobby's own list is asked first, by file name: a script the round is
 * already running is described by what the server said about it, which is
 * where the description and the rules table come from. A row the host has
 * only put in the draft is not in that list, so the catalogue row it came
 * from answers instead. Last comes the committed map's own scenario, which is
 * in neither — it is not in the scenarios directory and it does not enter the
 * script list — and the attached-scenario accessors describe it. */
static void lobbyRoundOpenDetails(ClientSim *cs, const LobbyRoundRow *r,
                                  LobbyScenarioRow *rows, int count) {
    int n = clientSimGetLobbyScriptCount(cs);
    int i;

    for (i = 0; i < n; i++) {
        if (SDL_strcmp(clientSimGetLobbyScriptFile(cs, i), r->file) == 0) {
            lobbyScenarioDetailsOpenScript(cs, i);
            return;
        }
    }
    for (i = 0; i < count; i++) {
        if (SDL_strcmp(rows[i].file, r->file) == 0) {
            lobbyScenarioDetailsOpenRow(&rows[i]);
            return;
        }
    }
    if (SDL_strcmp(clientSimGetLobbyScenarioFileName(cs), r->file) == 0) {
        lobbyScenarioDetailsOpenAttached(cs);
    }
}

/* ── The left column: what this server offers ─────────────────────
 * Every row of the catalogue that a host may put in a list, which is every
 * row that is not bound.
 *
 * bound and not kind is the test, and the two are not interchangeable. A
 * bound file was written against one map: its tags, its regions and its
 * entity indices are that map's, so it means nothing over another, and the
 * server refuses a whole list that names one. That is true of a bound mod as
 * much as of a bound scenario, and a mod is bound unless its manifest says
 * otherwise — the reader defaults the field to true. Offering rows by kind
 * would put a bound mod in this column and hand the host a list the server
 * throws out.
 *
 * Kind still decides what happens when a row is taken, which is why the
 * column reads both answers. */
static void lobbyScenarioChooserCatalogue(LobbyScenarioRow *rows, int count,
                                          bool ready, bool inFlight,
                                          bool mayEdit, float s) {
    int shown = 0;
    int i;

    ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_SCENARIO_OFFERED));

    /* The name box over the list, not in it: it filters the rows and is not
       one of them, and a controller stepping down the list must not land in a
       text field on the way. It stays put while the rows scroll, because it
       is what decides which rows there are to scroll. */
    {
        float kindW = 0.0f;
        int   k;

        /* Wide enough for whichever of the three words this language makes
           longest, so switching between them never moves the box. */
        for (k = 0; k < 3; k++) {
            float w = ImGui::CalcTextSize(langGetText(s_kindFilterIds[k])).x;
            if (w > kindW) kindW = w;
        }
        kindW += ImGui::GetStyle().FramePadding.x * 2.0f +
                 ImGui::GetFrameHeight();

        /* Name box and kind box on one line, the name taking what the kind
           leaves. They ask the same question about the same rows, so they
           belong together; two lines would push the list itself down in a
           column that is half a dialog wide. */
        ImGui::SetNextItemWidth(
            -(kindW + ImGui::GetStyle().ItemInnerSpacing.x));
        ImGui::InputTextWithHint("##scenarioFilter",
                                 langGetText(STR_DLGLOBBY_SCENARIO_FILTER),
                                 s_filter, sizeof(s_filter));
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::SetNextItemWidth(kindW);
        /* A combo and not two toggles: the three states are one choice, and
           toggles would let a host pick both and mean nothing by it. The rest
           of the lobby asks a three-way question this way. */
        if (ImGui::BeginCombo("##scenarioKind",
                              langGetText(s_kindFilterIds[s_kindFilter]))) {
            for (k = 0; k < 3; k++) {
                if (ImGui::Selectable(langGetText(s_kindFilterIds[k]),
                                      s_kindFilter == k)) {
                    s_kindFilter = k;
                }
            }
            ImGui::EndCombo();
        }
        imguiHandOnHover();
    }

    /* Under the filter row and not over it. The rule is what ends the head,
       and the filter box belongs to the head: it says which rows there are,
       so it is not one of them. It also puts this rule at the same y as the
       other column's. */
    ImGui::Separator();

    ImGui::BeginChild("##scenarioRows", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_NavFlattened);

    if (!ready && inFlight) {
        lobbyScenarioRowNote(langGetText(STR_DLGLOBBY_SCENARIO_WAITING));
        ImGui::EndChild();
        return;
    }

    for (i = 0; i < count; i++) {
        char                 nameBuf[SERVER_SCENARIO_FILE_LEN + 4];
        float                minW, arrowW, tagW, nameW;
        int                  why;
        bool                 can;

        if (rows[i].bound) continue;
        /* A row the round already holds is off this column for as long as
           it is in the round, and the left arrow beside its row on the right
           is what puts it back. It used to be drawn here, greyed, with
           "Already in this round" on its arrow, which is a row a host reads
           twice and can do nothing with either time. */
        if (lobbyRoundIndexOfFile(rows[i].file) >= 0) continue;
        /* Both filters are counted the same way: both are the host's own
           doing and both are undone the same way. */
        if (s_kindFilter == 1 && !lobbyScenarioRowIsMod(&rows[i])) continue;
        if (s_kindFilter == 2 && lobbyScenarioRowIsMod(&rows[i])) continue;
        if (!lobbyScenarioRowMatches(&rows[i], s_filter)) continue;
        shown++;

        why = lobbyRoundWhyNotAdd(&rows[i]);
        can = mayEdit && why == 0;

        ImGui::PushID(i);
        /* The name takes the row less the tag and the arrow, and a name
           longer than that is cut short rather than pushing either of them
           off the row. A link draws whatever it is given, where the
           Selectable this used to be clipped its own label to the size it
           was handed. Under a width that cannot hold all three, the name
           takes the row and the other two wrap under it rather than being
           squeezed to nothing. */
        minW   = (ImGui::GetTextLineHeight() +
                  ImGui::GetStyle().ItemInnerSpacing.x) * 2.0f;
        arrowW = mayEdit ? ImGui::GetFrameHeight() +
                               ImGui::GetStyle().ItemInnerSpacing.x
                         : 0.0f;
        tagW   = lobbyScenarioKindTagWidth(lobbyScenarioRowIsMod(&rows[i]), s);
        nameW  = ImGui::GetContentRegionAvail().x - arrowW - tagW;
        if (nameW < minW) nameW = 0.0f;
        lobbyTruncateName(rows[i].name, nameW, nameBuf, sizeof(nameBuf));

        /* The name is the way into the details, drawn as a link the way every
           other script name in the lobby is drawn. The tag beside it used to
           be that way in and the name used to add the row on a double click;
           the arrow is the one way over now, which is the control that says
           what it does.

           Not dimmed on a row that cannot be added, though it used to be. The
           dim said "this cannot move over", and with the way into the details
           on the name it would grey out the only thing left to press — every
           one of them, for a client that is not the host and can add nothing
           at all. The arrow beside the row is what is greyed, and the arrow
           is what carries the reason.

           The id is pushed by the row and not written into the label: a label
           is the name a script gave itself and two rows may share one. */
        if (ImGui::TextLink(nameBuf)) {
            lobbyScenarioDetailsOpenRow(&rows[i]);
        }
        imguiHandOnHover();
        /* Read off the name before the tag draws, because both of these ask
           about the last item and the tag would become it. */
        if (shown == 1) {
            /* Where a controller's focus starts, so the D-pad has somewhere
               to step from on the frame the dialog appears. */
            ImGui::SetItemDefaultFocus();
        }
        lobbyScenarioKindTag(lobbyScenarioRowIsMod(&rows[i]), s);

        if (mayEdit) {
            /* No tooltip. An arrow pointing at the other column says what it
               does, and a host moving several rows over had one popping up
               under the pointer on every one of them. */
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            if (lobbyScenarioArrow("##add", ImGuiDir_Right, can, NULL)) {
                lobbyRoundAdd(&rows[i]);
            }
        }

        /* The caps and not the description. The description is as long as
           its author made it, and a column of them turns a list a host reads
           down into a page they scroll. Every name opens the details, which
           is where the whole description is, so nothing is lost by leaving
           it off the row.

           The caps stay: two short numbers that say whether a row fits the
           game being set up, which is the question the list itself is being
           read to answer. */
        ImGui::Indent();
        lobbyScenarioRowCaps(rows[i].maxPlayers, rows[i].bots);
        ImGui::Unindent();
        ImGui::PopID();
    }

    /* The one note left, and the only one a host can do anything about: a
       filter of theirs hid every row, and clearing it brings them back. A
       column empty for any other reason -- a server with nothing to offer, a
       server whose whole catalogue is already in the round -- is left empty,
       because a host can do nothing about either and a line saying so is a
       line they read once per opening for nothing.

       Keyed on a filter actually being set. Without that test an empty server
       would be reported as a name that matched nothing, naming a filter the
       host never typed. */
    if (shown == 0 && (s_filter[0] != '\0' || s_kindFilter != 0)) {
        lobbyScenarioRowNote(langGetText(STR_DLGLOBBY_SCENARIO_NO_MATCH));
    }

    ImGui::EndChild();
}

/* ── The right column: what this round will run ─────────────────
 * The draft, in load order, with the buttons that edit it.
 *
 * A left arrow to take a row out, matching the right arrow that put it there.
 * No label, so the row keeps its width for the name in every language — this
 * column is half a dialog wide. A row taken out goes back to being offered on
 * the left, which is what a left arrow says and what a cross would not.
 *
 * The map's own row keeps that same arrow, drawn and disabled, rather than a
 * gap where it would be: a host who wants the row gone looks for the control
 * that removes things, and a row with nothing there reads as a row that has
 * not finished drawing. The reason is on its hover.
 *
 * Up and down sit at the right edge and are drawn only while the pointer is
 * over their row. Reordering is the rarer of the two jobs and the one a host
 * repeats, so it is the one that belongs out of the way. Their width is held
 * open on every row, so nothing moves as the pointer goes down the list, and
 * a controller is given them on every row — it has no pointer, and hover
 * would put them out of its reach for good. */
static void lobbyScenarioChooserRound(ClientSim *cs, LobbyScenarioRow *rows,
                                      int count, bool mayEdit, float s) {
    int drop = -1;
    int move = -1;
    int dir  = 0;
    int i;

    ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_SCENARIO_ROUND));
    /* Above the separator, not below it: it says what the heading means, so
       it belongs on the heading's side of the line. It takes the slot the
       left column gives its filter box, which is what keeps the first row of
       the two lists level. */
    lobbyScenarioHeadNote(langGetText(STR_DLGLOBBY_SCENARIO_ORDER_NOTE));
    ImGui::Separator();

    ImGui::BeginChild("##roundRows", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_NavFlattened);

    for (i = 0; i < s_roundCount; i++) {
        LobbyRoundRow *r      = &s_round[i];
        ImGuiStyle    &st     = ImGui::GetStyle();
        float          inner  = st.ItemInnerSpacing.x;
        float          frameH = ImGui::GetFrameHeight();
        float          moveW  = mayEdit ? frameH * 2.0f + inner : 0.0f;
        ImVec2         rowMin = ImGui::GetCursorScreenPos();
        float          rowX   = ImGui::GetCursorPosX();
        float          avail  = ImGui::GetContentRegionAvail().x;
        bool           over;

        /* Asked before the row draws, because what it decides is drawn at the
           end of the row and there is nothing there yet to ask about. The
           window test is what keeps a dialog opened over this one from
           putting buttons under a pointer that is not on this list at all.

           AllowWhenBlockedByActiveItem, and the two move buttons do not work
           without it. A plain IsWindowHovered answers false for every frame
           any item is held down (imgui.cpp, the ActiveId test in
           IsWindowHovered), and one of the buttons this decides whether to
           draw is exactly such an item. Holding it took this to false, which
           took the button off the row on the very next frame, which threw
           away the press before the release that a button reports on could
           arrive: the arrows drew, greyed nothing, and did nothing. The
           arrow beside them on the left never had the fault because it is
           drawn whether the row is hovered or not. */
        over = ImGui::IsWindowHovered(
                   ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
               ImGui::IsMouseHoveringRect(
                   rowMin, ImVec2(rowMin.x + avail, rowMin.y + frameH));

        ImGui::PushID(i);
        if (mayEdit) {
            /* Disabled and not absent for the map's own row. The sentence is
               the whole of what a host is told, so it goes on the control a
               host presses to ask. */
            /* No tooltip here either, for the reason the add arrow has
               none. A bound row's arrow is greyed and the row's own tag says
               it belongs to the map. */
            if (lobbyScenarioArrow("##drop", ImGuiDir_Left, !r->bound,
                                   NULL)) {
                drop = i;
            }
            ImGui::SameLine(0.0f, inner);
        }

        /* The name sits on the arrow's baseline, which is a framed item and
           taller than a line of text. */
        ImGui::AlignTextToFramePadding();
        /* The name is the way into the details here as well, so a host reads
           and opens a row of this column the same way as a row of the other
           one. The id is the row's, pushed above. */
        if (ImGui::TextLink(r->name)) {
            lobbyRoundOpenDetails(cs, r, rows, count);
        }
        imguiHandOnHover();
        lobbyScenarioKindTag(r->mod, s);

        if (mayEdit) {
            /* Where the last thing drawn ended, in this window's own
               coordinates, so the two can be compared without caring what
               the row started at. */
            float used   = ImGui::GetItemRectMax().x - rowMin.x + rowX;
            float target = rowX + avail - moveW;

            if (used + inner > target) {
                /* The name run already reaches the strip the two buttons are
                   drawn in. Following on normally puts them past the right
                   edge, which ImGui wraps to the next line, and a wrapped row
                   is better than two buttons drawn over a name. */
                ImGui::SameLine(0.0f, inner);
            } else {
                ImGui::SameLine(target);
            }
            if (over || uiShouldUseControllerMode()) {
                /* The map's own row gets the one sentence on all three of its
                   controls. Its position is fixed for the same reason its
                   place in the round is: the row is the map's, not the
                   host's, and a disabled button with no reason on it is a
                   host asking the same question three times. */
                if (lobbyScenarioArrow("##up", ImGuiDir_Up,
                                       lobbyRoundMayMove(i, -1), NULL)) {
                    move = i;
                    dir  = -1;
                }
                ImGui::SameLine(0.0f, inner);
                if (lobbyScenarioArrow("##down", ImGuiDir_Down,
                                       lobbyRoundMayMove(i, 1), NULL)) {
                    move = i;
                    dir  = 1;
                }
            } else {
                /* Nothing drawn, but the line is closed off at the height the
                   two buttons would have given it, so a row does not change
                   height as the pointer passes over it. */
                ImGui::Dummy(ImVec2(moveW, frameH));
            }
        }
        ImGui::PopID();
    }

    ImGui::EndChild();

    /* Applied after the loop and not inside it: taking a row out moves every
       row behind it up one, and the loop is reading those rows. */
    if (drop >= 0) lobbyRoundDrop(drop);
    if (move >= 0) lobbyRoundMove(move, dir);
}

void lobbyScenarioChooserRenderWindow(ClientSim *cs, float s,
                                      int screenW, int screenH) {
    if (!s_open) return;
    if (cs == NULL) {
        s_open = false;
        return;
    }

    ServerSim *sim = gameFrontGetServerSim();

    /* One listing per opening, obtained from here rather than from the button
       so a dialog opened again after the directory changed reads it again.
       A server in this process is read directly: the list request is a UDP
       packet and a lobby hosted here has no UDP transport to send it on, so
       asking would leave the list empty. A remote server is asked. */
    lobbyScenarioCatalogueEnsure(cs);

    char title[128];
    SDL_snprintf(title, sizeof(title), "%s" LOBBY_SCENARIO_WINDOW_ID,
                 langGetText(STR_DLGLOBBY_SCENARIO_TITLE));

    /* A default size that holds a row of each column whole, centred on first
       open and resizable after that — a host with a long directory can drag
       it taller. Held inside the lobby so neither default sits off the edge
       of a small window.

       The width is added up from what a row carries rather than picked, so
       adding to a row moves it. Left: the name, the kind tag and the arrow
       over. Right: the arrow out, the name, the kind tag and the two move
       buttons. Both columns keep a scrollbar's width and the child's border
       padding, and the window keeps its own padding and the gap between the
       two. The tag is measured at its widest word, which is the one that is
       there when the column is under most pressure.

       The floor is the same sum with a shorter name: under it the arrows and
       the tag take the whole row and the names have nowhere left to go. */
    float lineH  = ImGui::GetTextLineHeightWithSpacing();
    float frameH = ImGui::GetFrameHeight();
    float inner  = ImGui::GetStyle().ItemInnerSpacing.x;
    float tagW   = lobbyScenarioKindTagWidth(false, s);
    float chromeW;
    float nameW  = 200.0f * s;
    float winW;
    float winH   = lineH * 26.0f;

    chromeW = /* left: name + tag + arrow over */
              tagW + frameH + inner * 2.0f
              /* right: arrow out + name + tag + up and down */
              + frameH + inner + tagW + frameH * 2.0f + inner * 2.0f
              /* a scrollbar and a border either side of each column */
              + (ImGui::GetStyle().ScrollbarSize
                 + ImGui::GetStyle().WindowPadding.x) * 2.0f
              /* the window's own padding and the gap between the columns */
              + ImGui::GetStyle().WindowPadding.x * 2.0f
              + ImGui::GetStyle().ItemSpacing.x;
    winW = chromeW + nameW * 2.0f;
    if (screenW > 0 && winW > (float)screenW * 0.9f) winW = (float)screenW * 0.9f;
    if (screenH > 0 && winH > (float)screenH * 0.8f) winH = (float)screenH * 0.8f;
    ImGui::SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2((float)screenW * 0.5f,
                                   (float)screenH * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(chromeW + 90.0f * s * 2.0f, lineH * 8.0f),
        ImVec2(FLT_MAX, FLT_MAX));
    /* Over the lobby on the frame it appears, and not on any frame after —
       raising it every frame would take focus back from whatever the player
       clicked behind it. */
    if (!s_focusedOnce) {
        ImGui::SetNextWindowFocus();
        s_focusedOnce = true;
    }

    bool stay      = true;
    bool wantClose = false;
    bool wantSend  = false;

    if (ImGui::Begin(title, &stay,
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoCollapse)) {
        /* Both sources into the one shape, before anything is drawn. A
           directory read is finished by the time its rows are shown, so an
           in-process listing is ready and never in flight — the waiting line
           in the left column belongs to a remote client alone. */
        LobbyScenarioRow rows[LOBBY_SCENARIO_CHOOSER_MAX];
        int  count    = 0;
        bool ready    = true;
        bool inFlight = false;
        LobbyRoundRow live[LOBBY_ROUND_MAX];
        int  liveCount;
        bool mayEdit  = lobbyScenarioMayChoose(cs);

        if (sim != NULL) {
            count = s_localCount;
            if (count > LOBBY_SCENARIO_CHOOSER_MAX) {
                count = LOBBY_SCENARIO_CHOOSER_MAX;
            }
            for (int i = 0; i < count; i++) {
                rows[i].file        = s_localRows[i].file;
                rows[i].name        = s_localRows[i].name;
                rows[i].description = s_localRows[i].description;
                rows[i].maxPlayers  = s_localRows[i].maxPlayers;
                rows[i].bots        = s_localRows[i].bots;
                rows[i].bound       = s_localRows[i].bound;
                rows[i].keepsWinCondition =
                    s_localRows[i].keepsWinCondition;
            }
        } else {
            ready    = clientSimGetLobbyScenarioListReady(cs);
            inFlight = clientSimGetLobbyScenarioListInFlight(cs);
            count    = clientSimGetLobbyScenarioListCount(cs);
            if (count > LOBBY_SCENARIO_CHOOSER_MAX) {
                count = LOBBY_SCENARIO_CHOOSER_MAX;
            }
            for (int i = 0; i < count; i++) {
                rows[i].file = clientSimGetLobbyScenarioListFile(cs, i);
                rows[i].name = clientSimGetLobbyScenarioListName(cs, i);
                rows[i].description =
                    clientSimGetLobbyScenarioListDescription(cs, i);
                rows[i].maxPlayers =
                    clientSimGetLobbyScenarioListMaxPlayers(cs, i);
                rows[i].bots  = clientSimGetLobbyScenarioListBots(cs, i);
                rows[i].bound = clientSimGetLobbyScenarioListBound(cs, i);
                rows[i].keepsWinCondition =
                    clientSimGetLobbyScenarioListKeepsWinCondition(cs, i);
            }
        }
        /* A manifest that named nothing still came from a file. Done here
           rather than where a name is drawn, so the filter, the details
           dialog and the draft all read the one the host is looking at. */
        for (int i = 0; i < count; i++) {
            if (rows[i].name[0] == '\0') rows[i].name = rows[i].file;
        }

        /* The draft against what the lobby says the round is. Taken on the
           first frame, and taken again whenever the two stop agreeing: a map
           commit or a second host editing replaces the round under this
           dialog, and a draft still describing the old one would send the
           host's edit back over somebody else's change.
           clientSimGetLobbyScriptSeq would be the cheaper test and is not
           enough — it does not move for the half of the answer that arrives
           on CTRL_LOBBY_SETTINGS. */
        liveCount = lobbyRoundLive(cs, live, LOBBY_ROUND_MAX);
        if (!s_roundTaken ||
            !lobbyRoundSameFiles(live, liveCount, s_live, s_liveCount)) {
            SDL_memcpy(s_live, live, (size_t)liveCount * sizeof(live[0]));
            SDL_memcpy(s_round, live, (size_t)liveCount * sizeof(live[0]));
            s_liveCount  = liveCount;
            s_roundCount = liveCount;
            s_roundTaken = true;
        }

        /* The two columns, side by side, each scrolling on its own. The
           footer's height is reserved at the window level, the way the map
           chooser reserves its own action bar, so a server with a full
           directory cannot push the way out of the dialog off the bottom.
           NavFlattened so a controller crosses from one column to the other
           and out to the footer without first stepping out of a list. */
        /* What the footer below the two columns takes, so the columns end
           where it starts and the window never has to scroll to reach OK.
           Added up from the parts DialogFooter draws rather than guessed at:
           a Spacing, a Separator, a Spacing and the button row, each of
           which also advances the cursor by one item spacing, and the child
           above them advances by one more of its own. A guess was here
           before and it was about 19 pixels short at the default scale,
           which is most of a scrollbar's reason to exist. */
        float btnBarH = ImGui::GetFrameHeight()
                      + ImGui::GetStyle().ItemSpacing.y * 4.0f + 1.0f;
        float colW    = (ImGui::GetContentRegionAvail().x -
                         ImGui::GetStyle().ItemSpacing.x) * 0.5f;

        ImGui::BeginChild("##scnOffered", ImVec2(colW, -btnBarH),
                          ImGuiChildFlags_Borders |
                              ImGuiChildFlags_NavFlattened);
        lobbyScenarioChooserCatalogue(rows, count, ready, inFlight, mayEdit,
                                      s);
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("##scnRound", ImVec2(0.0f, -btnBarH),
                          ImGuiChildFlags_Borders |
                              ImGuiChildFlags_NavFlattened);
        lobbyScenarioChooserRound(cs, rows, count, mayEdit, s);
        ImGui::EndChild();

        /* [Cancel] [OK], and not a send on every arrow.
           CMD_LOBBY_SET_SCRIPT_LIST is refused with CMD_REJECT_COOLDOWN when
           a second list arrives within a second of the last, and moving one
           row down three places is three clicks inside that second. Every
           list the server does take also re-attaches the script and clears
           every player's ready bit. So the edits are made here and one list
           goes out when the host says they are done.

           A client that may not edit gets [Close] instead: there is nothing
           to confirm when nothing on screen can be changed. */
        if (!mayEdit) {
            if (WBUI::DialogFooter(/*cancelLabel*/ nullptr,
                                   /*confirmLabel*/ langGetText(STR_CLOSE))
                != WBUI::FOOTER_NONE) {
                wantClose = true;
            }
        } else {
            int footer = WBUI::DialogFooter(langGetText(STR_CANCEL),
                                            langGetText(STR_OK));
            if (footer == WBUI::FOOTER_CONFIRM) {
                wantSend  = true;
                wantClose = true;
            } else if (footer == WBUI::FOOTER_CANCEL) {
                wantClose = true;
            }
        }

        /* Nothing is sent when the draft says what the round already says.
           An identical list is not free: the server detaches and re-attaches
           the script and unreadies everyone for it, so a host who opened the
           dialog to read it and pressed OK would restart the round's script
           for nothing. */
        if (wantSend &&
            !lobbyRoundSameFiles(s_round, s_roundCount, s_live, s_liveCount)) {
            lobbyRoundSend(cs);
        }
    }
    ImGui::End();

    /* The title bar's X, and everything above that asked to close. */
    if (!stay || wantClose) s_open = false;
}
