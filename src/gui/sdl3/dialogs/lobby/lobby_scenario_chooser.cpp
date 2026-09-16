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
 *                lobby's scenario line: what the server
 *                offers on its own, and the host's pick.
 *
 *                A dialog of its own rather than a fifth
 *                source in the map chooser's row of tabs.
 *                What it picks is not a map — it plays over
 *                whichever map is committed — and a row of
 *                tabs is poor to drive on a controller, so
 *                a button opens this and nothing is added
 *                to that row.
 *
 *                Two sources, one for each place a server
 *                can be, as the map chooser has: a server in
 *                this process is read straight off its
 *                scenarios directory when the dialog opens,
 *                and a remote one is asked over the wire and
 *                read back off the ClientSim each frame.
 *                Both fill the same row shape and there is
 *                one render path below them.
 *********************************************************/

#include <cfloat>   /* FLT_MAX — no upper bound on how large the host may drag it */

#include <SDL3/SDL.h>

#include "imgui.h"
#include "lobby_internal.h"
#include "dialog_footer.h"  /* WBUI::DialogFooter — the Close row and its Esc binding */
extern "C" {
#include "client_sim.h"     /* the lobby scenario list accessors */
#include "client_net.h"     /* clientSimNetSendLobbyScenarioListRequest / SetScenario */
#include "server_sim.h"     /* ServerScenarioEntry / serverSimEnumerateScenarioDir — the in-process read */
#include "../../../lang.h"
#include "../../../gamefront.h"  /* gameFrontGetServerSim — whether the server is in this process */
#include "../../../../server/threads.h"  /* threadsWaitForMutex / Release — the in-process read runs on the render thread */
}

/* The window's ID. The caption before ### is translated and the ID after it
 * is not, so a language change cannot hand ImGui a different window. */
#define LOBBY_SCENARIO_WINDOW_ID "###lobbyScenarioChooser"

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
};

void lobbyScenarioChooserReset(void) {
    s_open        = false;
    s_asked       = false;
    s_focusedOnce = false;
    s_localCount  = 0;
}

bool lobbyScenarioChooserIsOpen(void) {
    return s_open;
}

void lobbyScenarioChooserOpen(void) {
    s_open        = true;
    s_asked       = false;
    s_focusedOnce = false;
    s_localCount  = 0;
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

/* One line of grey prose under a row, wrapped to the dialog's width. The same
 * grey the scenario line itself uses for a description, so the two read as
 * one voice. */
static void lobbyScenarioRowNote(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
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
    if (!s_asked) {
        if (sim != NULL) {
            /* Under the sim mutex: this runs on the render thread and the
               host timer is ticking the same sim, as the map chooser's
               in-process branch does for serverSimReloadMap. */
            threadsWaitForMutex();
            s_localCount = serverSimEnumerateScenarioDir(
                sim, s_localRows, LOBBY_SCENARIO_CHOOSER_MAX);
            threadsReleaseMutex();
        } else {
            clientSimNetSendLobbyScenarioListRequest(cs);
        }
        s_asked = true;
    }

    char title[128];
    SDL_snprintf(title, sizeof(title), "%s" LOBBY_SCENARIO_WINDOW_ID,
                 langGetText(STR_DLGLOBBY_SCENARIO_TITLE));

    /* A modest default size, centred on first open, and resizable after
       that — a host with a long directory can drag it taller. Held inside
       the lobby so neither default sits off the edge of a small window. */
    float lineH = ImGui::GetTextLineHeightWithSpacing();
    float winW  = 460.0f * s;
    float winH  = lineH * 20.0f;
    if (screenW > 0 && winW > (float)screenW * 0.9f) winW = (float)screenW * 0.9f;
    if (screenH > 0 && winH > (float)screenH * 0.8f) winH = (float)screenH * 0.8f;
    ImGui::SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2((float)screenW * 0.5f,
                                   (float)screenH * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f * s, lineH * 8.0f),
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
    const char *pick = NULL;   /* the file name picked this frame, "" for none */

    if (ImGui::Begin(title, &stay,
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoCollapse)) {
        /* Both sources into the one shape, before anything is drawn. A
           directory read is finished by the time its rows are shown, so an
           in-process listing is ready and never in flight — the waiting line
           below belongs to a remote client alone. */
        LobbyScenarioRow rows[LOBBY_SCENARIO_CHOOSER_MAX];
        int  count    = 0;
        bool ready    = true;
        bool inFlight = false;

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
            }
        }

        /* The rows scroll and the Close row below them does not, so a server
           with a full directory cannot push the way out of the dialog off the
           bottom. The height is reserved at the window level, the way the map
           chooser reserves its own action bar. NavFlattened so a controller's
           B closes the dialog from a row rather than first stepping out of
           the list. */
        float btnBarH = ImGui::GetFrameHeight() + 14.0f * s;
        ImGui::BeginChild("##scenarioRows", ImVec2(0.0f, -btnBarH),
                          ImGuiChildFlags_NavFlattened);

        /* None first, and always available. It is how a host clears a mod,
           and how a map that carries its own scenario is played as the plain
           map it also is. */
        if (ImGui::Selectable(langGetText(STR_DLGLOBBY_SCENARIO_NONE))) {
            pick = "";
        }
        /* Where a controller's focus starts, so the D-pad has somewhere to
           step from on the frame the dialog appears. */
        ImGui::SetItemDefaultFocus();

        ImGui::Separator();

        if (!ready && inFlight) {
            lobbyScenarioRowNote(langGetText(STR_DLGLOBBY_SCENARIO_WAITING));
        } else if (count == 0) {
            /* Said out loud rather than shown as an empty list: a host who
               sees nothing cannot tell a server with no scenarios from a
               dialog that failed to ask. */
            lobbyScenarioRowNote(langGetText(STR_DLGLOBBY_SCENARIO_EMPTY));
        } else {
            for (int i = 0; i < count; i++) {
                const char *file = rows[i].file;
                const char *name = rows[i].name;
                const char *desc = rows[i].description;
                bool bound = rows[i].bound;

                /* A manifest that named nothing still came from a file. */
                if (name[0] == '\0') name = file;

                ImGui::PushID(i);
                /* A bound scenario is listed and cannot be taken. The server
                   refuses one anyway, so this is courtesy: a host who can see
                   their file greyed with the reason beside it learns something
                   a host who cannot find it at all does not. */
                if (bound) ImGui::BeginDisabled();
                if (ImGui::Selectable(name)) {
                    pick = file;
                }
                if (bound) ImGui::EndDisabled();

                ImGui::Indent();
                if (desc[0] != '\0') {
                    lobbyScenarioRowNote(desc);
                }
                lobbyScenarioRowCaps(rows[i].maxPlayers, rows[i].bots);
                if (bound) {
                    lobbyScenarioRowNote(
                        langGetText(STR_DLGLOBBY_SCENARIO_BOUND));
                }
                ImGui::Unindent();
                ImGui::PopID();
            }
        }

        ImGui::EndChild();

        /* [Close] only — there is nothing to confirm, because a pick is sent
           the moment a row is taken. The footer carries the Esc and Ctrl+W
           bindings, and a controller's B arrives as an injected Escape. */
        if (WBUI::DialogFooter(/*cancelLabel*/ nullptr,
                               /*confirmLabel*/ langGetText(STR_CLOSE))
            != WBUI::FOOTER_NONE) {
            wantClose = true;
        }

        /* A pick is a commit and not a preview, so the dialog has nothing
           further to ask and closes behind it. */
        if (pick != NULL) {
            clientSimNetSendLobbySetScenario(cs, pick);
            wantClose = true;
        }
    }
    ImGui::End();

    /* The title bar's X, and everything above that asked to close. */
    if (!stay || wantClose) s_open = false;
}
