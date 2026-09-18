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
 * Name:          lobby_scenario_rules.cpp
 * Purpose:       The popup behind the Rules button on the
 *                lobby's scenario line: one row per rule
 *                the attached scenario's manifest sets,
 *                with the rule's name, what the classic
 *                game plays it at, what the scenario set it
 *                to, and what that does to it in words.
 *
 *                A mod whose whole content is a rules table
 *                has nothing else to say what it does, so
 *                this is the only place a host can answer
 *                "what are the mods doing?" without opening
 *                the file.
 *
 *                The words come from simRulesPhrase and
 *                nowhere else: the viewer's rules display
 *                and the editor's rules form render the
 *                same SimRuleChange through the same call,
 *                so a value is described one way across the
 *                whole program.
 *
 *                A modal popup rather than a window of its
 *                own, and opened at the lobby window's id
 *                scope the way the bot docs dialog is: the
 *                button that asks for it sits inside the
 *                Map tab, and BeginPopupModal only finds a
 *                popup opened at the same scope. A modal
 *                also takes the controller's focus, which a
 *                four-column list a player has to read
 *                wants.
 *********************************************************/

#include <cfloat>   /* FLT_MIN — the table fills the width it is given */

#include <SDL3/SDL.h>

#include "imgui.h"
#include "lobby_internal.h"
#include "dialog_footer.h"  /* WBUI::DialogFooter — the Close row and its Esc binding */
extern "C" {
#include "client_sim.h"        /* the scenario rules reads */
#include "sim_rules_names.h"   /* simRulesRuleName / simRulesClassicValue */
#include "../../../sim_rules_phrase.h" /* simRulesPhrase — the one wording */
#include "../../../lang.h"
}

/* The popup's ID. The caption before ### is translated and names the
 * scenario, and the ID after it is not, so neither a language change nor a
 * different scenario hands ImGui a different popup. */
#define LOBBY_SCENARIO_RULES_POPUP_ID "###lobbyScenarioRules"

/* Asked for by the button on the scenario line, consumed by the render below
 * on the frame it opens the popup. Two flags rather than one: ImGui wants
 * OpenPopup called once, and BeginPopupModal called on every frame the popup
 * is up. */
static bool s_wantOpen = false;
static bool s_open     = false;

void lobbyScenarioRulesReset(void) {
    s_wantOpen = false;
    s_open     = false;
}

void lobbyScenarioRulesOpen(void) {
    s_wantOpen = true;
    s_open     = true;
}

/* Whether there is anything to open. The button and the popup both ask, so a
 * scenario that detaches while the popup is up closes it rather than leaving
 * an empty table on screen. */
static bool lobbyScenarioRulesHaveAny(ClientSim *cs) {
    return cs != NULL && clientSimGetLobbyScenarioSource(cs) != 0 &&
           clientSimGetScenarioRulesCount(cs) > 0;
}

/* A rule's value as a player reads it: whole numbers without a decimal
 * point, rates to two places with the trailing zeros trimmed. The same
 * trimming simRulesPhrase does to a multiple, because the two numbers sit in
 * neighbouring columns and must not disagree about how a number looks. */
static void lobbyScenarioRulesNumber(double value, char *buf, size_t bufLen) {
    size_t len;

    SDL_snprintf(buf, bufLen, "%.2f", value);
    if (SDL_strchr(buf, '.') == NULL) return;
    len = SDL_strlen(buf);
    while (len > 0 && buf[len - 1] == '0') buf[--len] = '\0';
    if (len > 0 && buf[len - 1] == '.') buf[--len] = '\0';
}

void lobbyScenarioRulesRenderModal(ClientSim *cs) {
    char        title[192];
    MessageArgs args;
    const char *name;
    bool        open = true;
    int         count;
    int         i;

    if (s_wantOpen) {
        s_wantOpen = false;
        ImGui::OpenPopup(LOBBY_SCENARIO_RULES_POPUP_ID);
    }
    if (!s_open) return;
    if (!lobbyScenarioRulesHaveAny(cs)) {
        /* The scenario went while this was up — a map committed under the
           host, or a mod cleared. Nothing to list, so nothing to show. */
        s_open = false;
        return;
    }

    name = clientSimGetLobbyScenarioName(cs);
    if (name[0] == '\0') name = clientSimGetLobbyScenarioFileName(cs);
    SDL_memset(&args, 0, sizeof(args));
    SDL_snprintf(args.string1, sizeof(args.string1), "%s", name);
    SDL_snprintf(title, sizeof(title), "%s%s",
                 langGetTextFmt(STR_DLGLOBBY_SCENARIO_RULES_TITLE, &args),
                 LOBBY_SCENARIO_RULES_POPUP_ID);

    {
        ImVec2 vp = ImGui::GetMainViewport()->Size;
        ImGui::SetNextWindowSize(ImVec2(SDL_min(640.0f, vp.x * 0.85f),
                                        SDL_min(460.0f, vp.y * 0.85f)),
                                 ImGuiCond_Appearing);
    }
    if (!ImGui::BeginPopupModal(title, &open, ImGuiWindowFlags_NoCollapse)) {
        /* Not begun means it is closed — by the title-bar X, or by Escape
           reaching the popup itself. */
        s_open = false;
        return;
    }

    count = clientSimGetScenarioRulesCount(cs);

    {
        const float footerH = ImGui::GetStyle().ItemSpacing.y * 3.0f + 1.0f +
                              ImGui::GetFrameHeightWithSpacing();
        if (ImGui::BeginTable("##scnrules", 4,
                              ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_ScrollY,
                              ImVec2(-FLT_MIN, -footerH))) {
            ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_RULES_COL_RULE),
                                    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_RULES_COL_CLASSIC),
                                    ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize("00000").x);
            ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_RULES_COL_SCENARIO),
                                    ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize("00000").x);
            ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_RULES_COL_CHANGE),
                                    ImGuiTableColumnFlags_WidthStretch);
            /* The header row stays put while the list scrolls: a set of
               ninety rules is longer than the popup. */
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            for (i = 0; i < count; i++) {
                int    rule  = clientSimGetScenarioRuleIndex(cs, i);
                double value = clientSimGetScenarioRuleValue(cs, i);
                char   classicText[32];
                char   valueText[32];
                char   phrase[96];

                if (rule < 0) continue;   /* a row naming no rule */

                lobbyScenarioRulesNumber(simRulesClassicValue(rule),
                                         classicText, sizeof(classicText));
                lobbyScenarioRulesNumber(value, valueText, sizeof(valueText));
                simRulesPhrase(rule, value, phrase, sizeof(phrase));

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(simRulesRuleName(rule));
                ImGui::TableSetColumnIndex(1);
                /* The classic number is what the row is read against rather
                   than what the scenario did, so it is the quieter of the
                   two. */
                ImGui::TextDisabled("%s", classicText);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(valueText);
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(phrase);
            }
            ImGui::EndTable();
        }
    }

    /* A real Close button, not only the title-bar X: a controller needs
       something focusable to leave by, and so does a player who has scrolled
       the list. DialogFooter's left slot is its cancel slot and there is
       nothing to confirm here, so the one button sits on the right. */
    if (WBUI::DialogFooter(NULL, langGetText(STR_CLOSE)) != WBUI::FOOTER_NONE ||
        !open) {
        s_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

bool lobbyScenarioRulesAvailable(ClientSim *cs) {
    return lobbyScenarioRulesHaveAny(cs);
}
