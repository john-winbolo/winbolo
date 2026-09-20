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
 *                A row also says what its rule is for, not
 *                only what the scenario did to it: the name
 *                carries the description on hover, and an
 *                Info button beside it opens the same words
 *                in a popup of its own. The hover answers a
 *                mouse; the button is what a controller
 *                lands on and what a thumb hits.
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

/* The detail popup over the list, opened by a row's Info button. Its ID is
 * fixed for the same reason the list's is: the caption names the rule and is
 * translated, and neither may change which popup ImGui is being asked for. */
#define LOBBY_SCENARIO_RULE_DETAIL_ID "###lobbyScenarioRuleDetail"

/* Asked for by the button on the scenario line, consumed by the render below
 * on the frame it opens the popup. Two flags rather than one: ImGui wants
 * OpenPopup called once, and BeginPopupModal called on every frame the popup
 * is up. */
static bool s_wantOpen = false;
static bool s_open     = false;

/* Which rule the detail popup is describing, and the ask to open it. The rule
 * index rather than the row, so a manifest whose rows move under the popup is
 * followed by name rather than by position. -1 is nothing chosen. */
static bool s_wantDetail = false;
static int  s_detailRule = -1;

/* The detail popup is a view on to a row of the list, so it goes when the
 * list does: a scenario that is replaced or detached takes the rule the popup
 * was describing with it. */
static void lobbyScenarioRulesDropDetail(void) {
    s_wantDetail = false;
    s_detailRule = -1;
}

void lobbyScenarioRulesReset(void) {
    s_wantOpen = false;
    s_open     = false;
    lobbyScenarioRulesDropDetail();
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

/* The value this scenario sets a rule to, found by the rule rather than by
 * the row it was on when the player pressed Info. False when the scenario no
 * longer sets it, which is the popup's cue to go. */
static bool lobbyScenarioRulesValueOf(ClientSim *cs, int rule, double *value) {
    int count = clientSimGetScenarioRulesCount(cs);
    int i;

    for (i = 0; i < count; i++) {
        if (clientSimGetScenarioRuleIndex(cs, i) == rule) {
            *value = clientSimGetScenarioRuleValue(cs, i);
            return true;
        }
    }
    return false;
}

/* One labelled line of the detail popup: the label quiet, the value beside
 * it. Two widgets rather than one string so no punctuation is invented
 * between a translated label and an untranslated number. */
static void lobbyScenarioRulesDetailLine(langid label, const char *value) {
    ImGui::TextDisabled("%s", langGetText(label));
    ImGui::SameLine();
    ImGui::TextUnformatted(value);
}

/* What one rule does, over the list. A modal inside a modal: this is called
 * from inside the list's own BeginPopupModal, because BeginPopupModal only
 * finds a popup opened at the same id scope, and the row that asked for it
 * has already popped its per-row id by the time this runs. */
static void lobbyScenarioRulesRenderDetail(ClientSim *cs) {
    char        title[192];
    char        number[32];
    char        phrase[96];
    MessageArgs args;
    const char *ruleName;
    double      value = 0.0;
    bool        open  = true;
    float       footerH;

    if (s_wantDetail) {
        s_wantDetail = false;
        ImGui::OpenPopup(LOBBY_SCENARIO_RULE_DETAIL_ID);
    }
    if (s_detailRule < 0) return;
    if (!lobbyScenarioRulesValueOf(cs, s_detailRule, &value)) {
        /* The scenario stopped setting this rule while the popup was up. */
        lobbyScenarioRulesDropDetail();
        return;
    }

    /* A rule's name is what the manifest, a script and an operator line all
       spell, so the caption carries it as it is written. */
    ruleName = simRulesRuleName(s_detailRule);
    SDL_memset(&args, 0, sizeof(args));
    SDL_snprintf(args.string1, sizeof(args.string1), "%s", ruleName);
    SDL_snprintf(title, sizeof(title), "%s%s",
                 langGetTextFmt(STR_DLGLOBBY_RULE_DETAIL_TITLE, &args),
                 LOBBY_SCENARIO_RULE_DETAIL_ID);

    {
        ImVec2 vp = ImGui::GetMainViewport()->Size;
        ImGui::SetNextWindowSize(ImVec2(SDL_min(460.0f, vp.x * 0.8f),
                                        SDL_min(300.0f, vp.y * 0.8f)),
                                 ImGuiCond_Appearing);
    }
    if (!ImGui::BeginPopupModal(title, &open, ImGuiWindowFlags_NoCollapse)) {
        lobbyScenarioRulesDropDetail();
        return;
    }

    /* The body scrolls in a box of its own so the Close row stays at the
       bottom of the popup, where the list's own Close row is. */
    footerH = ImGui::GetStyle().ItemSpacing.y * 3.0f + 1.0f +
              ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##ruleDetailBody", ImVec2(-FLT_MIN, -footerH));
    {
        ImGui::TextUnformatted(ruleName);
        ImGui::Separator();
        /* A description runs to about a hundred characters, so it wraps. */
        ImGui::TextWrapped("%s", simRulesRuleDescription(s_detailRule));
        ImGui::Spacing();

        lobbyScenarioRulesNumber(simRulesClassicValue(s_detailRule), number,
                                 sizeof(number));
        lobbyScenarioRulesDetailLine(STR_DLGLOBBY_RULES_COL_CLASSIC, number);

        lobbyScenarioRulesNumber(value, number, sizeof(number));
        lobbyScenarioRulesDetailLine(STR_DLGLOBBY_RULES_COL_SCENARIO, number);

        simRulesPhrase(s_detailRule, value, phrase, sizeof(phrase));
        lobbyScenarioRulesDetailLine(STR_DLGLOBBY_RULES_COL_CHANGE, phrase);

        simRulesRangePhrase(s_detailRule, phrase, sizeof(phrase));
        lobbyScenarioRulesDetailLine(STR_DLGLOBBY_RULES_RANGE, phrase);
    }
    ImGui::EndChild();

    /* Closed the same way the list is, so a controller leaves both by the
       same button in the same place. */
    if (WBUI::DialogFooter(NULL, langGetText(STR_CLOSE)) != WBUI::FOOTER_NONE ||
        !open) {
        lobbyScenarioRulesDropDetail();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
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
        lobbyScenarioRulesDropDetail();
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
           reaching the popup itself. The detail popup is a view on to a row
           of a list that is no longer up, so it goes with it. */
        s_open = false;
        lobbyScenarioRulesDropDetail();
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

                /* Every row draws a button with the same label, and identical
                   labels in a loop are one id: without this each of them
                   would drive the first row. */
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(simRulesRuleName(rule));
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", simRulesRuleDescription(rule));
                }
                /* A hover answers a mouse and nothing else. The button is
                   what a controller lands on and what a thumb hits, so it is
                   a real button of the frame's own height rather than a
                   glyph, and it opens the same words in a popup. */
                ImGui::SameLine();
                {
                    const float h = ImGui::GetFrameHeight();
                    const char *info = langGetText(STR_DLGLOBBY_RULES_INFO);
                    float       w = ImGui::CalcTextSize(info).x +
                              ImGui::GetStyle().FramePadding.x * 2.0f;
                    if (w < h * 2.0f) w = h * 2.0f;
                    if (ImGui::Button(info, ImVec2(w, h))) {
                        s_detailRule = rule;
                        s_wantDetail = true;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", simRulesRuleDescription(rule));
                    }
                }
                ImGui::TableSetColumnIndex(1);
                /* The classic number is what the row is read against rather
                   than what the scenario did, so it is the quieter of the
                   two. */
                ImGui::TextDisabled("%s", classicText);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(valueText);
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(phrase);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    /* Inside the list's own popup, so ImGui finds the popup the row above
       asked for, and so the detail sits over the list rather than beside
       it. Read before it draws, because the detail's own footer clears
       s_detailRule on its way out and a test below would find it already
       gone. */
    const bool detailWasOpen = (s_detailRule >= 0);
    lobbyScenarioRulesRenderDetail(cs);

    /* A real Close button, not only the title-bar X: a controller needs
       something focusable to leave by, and so does a player who has scrolled
       the list. DialogFooter's left slot is its cancel slot and there is
       nothing to confirm here, so the one button sits on the right. */
    const int footer = WBUI::DialogFooter(NULL, langGetText(STR_CLOSE));

    /* One Escape reaches both footers in the same frame: the detail's closes
       the detail, and this one sees the same key still pressed and takes the
       list away behind it, so a single B left nothing on screen. A frame that
       had the detail up is one this footer says nothing about. The button is
       still drawn either way — the list is painted behind the modal — and it
       cannot be clicked while the modal is over it, so there is no press to
       lose. The title-bar X is the list window's own and is not reachable
       then at all, so it stays outside. */
    if ((!detailWasOpen && footer != WBUI::FOOTER_NONE) || !open) {
        s_open = false;
        lobbyScenarioRulesDropDetail();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

bool lobbyScenarioRulesAvailable(ClientSim *cs) {
    return lobbyScenarioRulesHaveAny(cs);
}
