/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_scenario_imgui.cpp
 * Purpose:
 *   The scenario panel: one window, a row of buttons that
 *   picks a view, and the body of whichever view is
 *   showing. Script is a text editor with Lua highlighting
 *   over the buffer mapeditor_scenario.c holds; Metadata,
 *   Lobby and Rules are forms over the manifest
 *   mapeditor_scenario_form.c holds.
 *
 *   The panel draws and reports what the user asked for;
 *   reading and writing the script file is mapeditor.c's
 *   job, the way the stats panel's Refresh works. The
 *   manifest has nowhere to be written yet, so the forms
 *   edit it and nothing else.
 *********************************************************/

#ifdef _WIN32
#include <WinSock2.h>
#endif

#include <stdio.h>
#include <string.h>

#include <string>

#include "imgui.h"
#include "TextEditor.h"

#include "mapeditor_imgui.h"
#include "mapeditor_scenario.h"
#include "mapeditor_scenario_form.h"
#include "../gui/lang.h"
#include "../gui/sim_rules_phrase.h"
#include "sim_rules_names.h"

/* How wide a rule's name column is, and how wide the box beside it. A rule
 * row is one line — name, value, the classic value, what the value does, and
 * Remove — and these two keep that line inside the window the panel opens at
 * rather than pushing the last of it off the right. */
static const float kRuleNameWidth  = 210.0f;
static const float kRuleValueWidth = 110.0f;

/* Copies src into a fixed field, always NUL-terminated. */
static void meScnCopy(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, cap, "%s", src);
}

/* Draws text in the disabled colour: a note beside a field, or the value the
 * classic game plays a rule at. */
static void meScnHint(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

/* True when needle is empty, or appears anywhere in haystack whatever the
 * case. The filter over the rule list, which is 92 rows long. */
static bool meScnContains(const char *haystack, const char *needle) {
    size_t i;
    size_t hLen;
    size_t nLen;

    if (needle == NULL || needle[0] == '\0') {
        return true;
    }
    if (haystack == NULL) {
        return false;
    }
    hLen = strlen(haystack);
    nLen = strlen(needle);
    if (nLen > hLen) {
        return false;
    }
    for (i = 0; i + nLen <= hLen; i++) {
        size_t j;
        for (j = 0; j < nLen; j++) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
        }
        if (j == nLen) {
            return true;
        }
    }
    return false;
}

/* One button in the row across the top. The view it picks is marked by
 * colour, which is what tells the four of them apart. */
static void meScnViewButton(int *view, int which, langid label) {
    const bool active = (*view == which);

    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.95f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.25f, 0.55f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(0.10f, 0.35f, 0.85f, 1.0f));
    }
    if (ImGui::Button(langGetText(label))) {
        *view = which;
    }
    if (active) {
        ImGui::PopStyleColor(3);
    }
}

/* -------------------------------------------------------
 * Script
 * ------------------------------------------------------- */
static void meScnScriptBody(MEScenarioState *st, const char *mapPath,
                            bool *wantSave, bool *wantReload) {
    /* One widget for the one view. It holds the text being edited and the
     * undo history that goes with it, so it outlives a frame. */
    static TextEditor s_editor;
    static bool       s_ready = false;
    /* Where the widget's undo history stood when the buffer below last
     * agreed with it. Every edit the widget makes adds an undo record, so a
     * different index means the text has moved on and is worth reading back.
     * This is what keeps GetText() off the per-frame path. */
    static int s_seenUndoIndex = 0;

    if (!s_ready) {
        s_editor.SetLanguageDefinition(TextEditor::LanguageDefinitionId::Lua);
        s_editor.SetShowLineNumbersEnabled(true);
        s_ready = true;
    }

    /* A new script arrived from disk. Seeding the widget is not an edit, so
     * the mark moves with it and nothing is read back for it. */
    if (st->pushToWidget) {
        s_editor.SetText(st->script != NULL ? st->script : "");
        st->pushToWidget = false;
        s_seenUndoIndex = s_editor.GetUndoIndex();
    }

    const bool haveMap = (mapPath != NULL && mapPath[0] != '\0');

    /* Where the script lives, or why there is nowhere to put it yet. */
    if (haveMap && st->scriptPath[0] != '\0') {
        ImGui::TextUnformatted(st->scriptPath);
    } else {
        ImGui::TextUnformatted(langGetText(STR_MAPEDIT_SCENARIO_NO_MAP));
    }

    ImGui::BeginDisabled(!haveMap || !st->dirty);
    if (ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_SAVE)) &&
        wantSave != NULL) {
        *wantSave = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!st->fileOnDisk);
    if (ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_RELOAD)) &&
        wantReload != NULL) {
        *wantReload = true;
    }
    ImGui::EndDisabled();

    if (st->dirty) {
        ImGui::SameLine();
        ImGui::TextUnformatted(langGetText(STR_MAPEDIT_SCENARIO_UNSAVED));
    }

    if (st->status[0] != '\0') {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextUnformatted(st->status);
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    /* The rest of the window is the editor. */
    s_editor.Render("##scenarioScript", false, ImVec2(0.0f, 0.0f), false);

    /* Read the text back only when the widget has moved on from what the
     * buffer holds. */
    const int undoIndex = s_editor.GetUndoIndex();
    if (undoIndex != s_seenUndoIndex) {
        const std::string text = s_editor.GetText();
        meScenarioSetText(st, text.c_str(), text.size());
        s_seenUndoIndex = undoIndex;
    }
}

/* -------------------------------------------------------
 * Metadata
 * ------------------------------------------------------- */
static void meScnMetadataBody(MEScenarioForm *f) {
    /* The game types a manifest may name, and the word each one is written
     * as. The empty word is no game type at all, which plays the round as a
     * strict tournament. */
    static const struct {
        const char *word;
        langid      label;
    } kGames[] = {
        {"", STR_MAPEDIT_SCENARIO_GAME_NONE},
        {"open", STR_DLGGAMEINFO_OPEN},
        {"tournament", STR_DLGGAMEINFO_TOURN},
        {"strict", STR_DLGGAMEINFO_STRICT},
    };
    const int kGameCount = (int)(sizeof(kGames) / sizeof(kGames[0]));

    ScenarioManifest *m = &f->manifest;
    int               i;
    int               chosen = -1;
    const char       *preview;

    ImGui::TextUnformatted(langGetText(STR_MAPEDIT_SCENARIO_NAME));
    if (ImGui::InputText("##scnName", m->name, sizeof(m->name))) {
        f->dirty = true;
    }

    ImGui::TextUnformatted(langGetText(STR_MAPEDIT_SCENARIO_DESCRIPTION));
    if (ImGui::InputTextMultiline(
            "##scnDesc", m->description, sizeof(m->description),
            ImVec2(-1.0f, ImGui::GetTextLineHeight() * 3.0f +
                              ImGui::GetStyle().FramePadding.y * 2.0f))) {
        f->dirty = true;
    }

    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::InputInt(langGetText(STR_MAPEDIT_SCENARIO_API), &m->api)) {
        f->dirty = true;
    }

    /* A word the manifest already holds that is none of the four shows as
     * itself, so picking from the list is the only thing that replaces it. */
    for (i = 0; i < kGameCount; i++) {
        if (strcmp(m->game, kGames[i].word) == 0) {
            chosen = i;
            break;
        }
    }
    preview = (chosen >= 0) ? langGetText(kGames[chosen].label) : m->game;
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::BeginCombo(langGetText(STR_MAPEDIT_SCENARIO_GAME), preview)) {
        for (i = 0; i < kGameCount; i++) {
            if (ImGui::Selectable(langGetText(kGames[i].label), i == chosen)) {
                meScnCopy(m->game, sizeof(m->game), kGames[i].word);
                f->dirty = true;
            }
        }
        ImGui::EndCombo();
    }

    if (ImGui::Checkbox(langGetText(STR_MAPEDIT_SCENARIO_BOUND), &m->bound)) {
        f->dirty = true;
    }
    meScnHint(langGetText(STR_MAPEDIT_SCENARIO_BOUND_NOTE));

    if (ImGui::Checkbox(langGetText(STR_MAPEDIT_SCENARIO_FILL_TO_CAPS),
                        &m->fillToCaps)) {
        f->dirty = true;
    }
}

/* -------------------------------------------------------
 * Lobby template
 * ------------------------------------------------------- */

/* One of a team's byte-wide numbers, edited as an int and kept inside the
 * byte it is stored in. */
static void meScnByteField(MEScenarioForm *f, const char *id, langid label,
                           uint8_t *value, float width) {
    int v = (int)*value;

    ImGui::SetNextItemWidth(width);
    ImGui::PushID(id);
    if (ImGui::InputInt(langGetText(label), &v)) {
        if (v < 0) {
            v = 0;
        }
        if (v > 255) {
            v = 255;
        }
        *value   = (uint8_t)v;
        f->dirty = true;
    }
    ImGui::PopID();
}

static void meScnInitTable(MEScenarioForm *f, ScnTable *init) {
    int k;
    int removeAt = -1;

    ImGui::TextUnformatted(langGetText(STR_MAPEDIT_SCENARIO_INIT));
    for (k = 0; k < (int)init->count; k++) {
        ImGui::PushID(k);
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::InputTextWithHint(
                "##key", langGetText(STR_MAPEDIT_SCENARIO_INIT_KEY),
                init->kv[k].key, sizeof(init->kv[k].key))) {
            f->dirty = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::InputTextWithHint(
                "##value", langGetText(STR_MAPEDIT_SCENARIO_INIT_VALUE),
                init->kv[k].value, sizeof(init->kv[k].value))) {
            f->dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_REMOVE))) {
            removeAt = k;
        }
        ImGui::PopID();
    }

    if (removeAt >= 0) {
        memmove(&init->kv[removeAt], &init->kv[removeAt + 1],
                (size_t)((int)init->count - removeAt - 1) *
                    sizeof(init->kv[0]));
        init->count--;
        memset(&init->kv[init->count], 0, sizeof(init->kv[0]));
        f->dirty = true;
    }

    if (init->count >= SCN_TABLE_MAX) {
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_INIT_FULL));
    } else if (ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_ADD_PAIR))) {
        memset(&init->kv[init->count], 0, sizeof(init->kv[0]));
        init->count++;
        f->dirty = true;
    }
}

static void meScnLobbyBody(MEScenarioForm *f) {
    ScnManifestLobby *lob = &f->manifest.lobby;
    int               i;
    int               removeTeam = -1;
    int               maxPlayers = (int)lob->maxPlayers;

    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::InputInt(langGetText(STR_MAPEDIT_SCENARIO_MAX_PLAYERS),
                        &maxPlayers)) {
        if (maxPlayers < 0) {
            maxPlayers = 0;
        }
        if (maxPlayers > 255) {
            maxPlayers = 255;
        }
        lob->maxPlayers = (uint8_t)maxPlayers;
        f->dirty        = true;
    }
    meScnHint(langGetText(STR_MAPEDIT_SCENARIO_MAX_PLAYERS_ANY));

    if (ImGui::Checkbox(langGetText(STR_MAPEDIT_SCENARIO_EXTRA_TEAMS),
                        &lob->extraTeams)) {
        f->dirty = true;
    }

    if (lob->numTeams == 0) {
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_NO_TEAMS));
    }

    for (i = 0; i < (int)lob->numTeams; i++) {
        ScnManifestTeam *t = &lob->teams[i];

        ImGui::PushID(i);
        ImGui::Separator();
        ImGui::Text("%s %d", langGetText(STR_MAPEDIT_SCENARIO_TEAM), i + 1);
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_REMOVE_TEAM))) {
            removeTeam = i;
        }

        meScnByteField(f, "id", STR_MAPEDIT_SCENARIO_TEAM_ID, &t->id, 120.0f);
        meScnByteField(f, "bots", STR_MAPEDIT_SCENARIO_TEAM_BOTS, &t->bots,
                       120.0f);
        meScnByteField(f, "maxbots", STR_MAPEDIT_SCENARIO_TEAM_MAX_BOTS,
                       &t->maxBots, 120.0f);
        if (ImGui::Checkbox(langGetText(STR_MAPEDIT_SCENARIO_TEAM_FIELDED),
                            &t->fielded)) {
            f->dirty = true;
        }

        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::InputText(langGetText(STR_MAPEDIT_SCENARIO_TEAM_BRAIN),
                             t->brain, sizeof(t->brain))) {
            f->dirty = true;
        }
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_BRAIN_IS_NAME));

        meScnInitTable(f, &t->init);
        ImGui::PopID();
    }

    if (removeTeam >= 0) {
        meScenarioFormRemoveTeam(f, removeTeam);
    }

    ImGui::Separator();
    if (lob->numTeams >= MAX_TANKS - 1) {
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_TEAMS_FULL));
    } else if (ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_ADD_TEAM))) {
        meScenarioFormAddTeam(f);
    }
}

/* -------------------------------------------------------
 * Rules
 * ------------------------------------------------------- */
static void meScnRulesBody(MEScenarioForm *f) {
    /* What the author has typed into the Add Rule filter. One panel, so one
     * box, and it survives a switch away and back. */
    static char s_filter[64] = "";

    ScenarioManifest *m = &f->manifest;
    int               i;
    int               removeAt = -1;
    char              classic[64];
    char              phrase[128];

    if (m->numRules == 0) {
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_NO_RULES));
    }

    for (i = 0; i < (int)m->numRules; i++) {
        const int rule = (int)m->rules[i].rule;

        ImGui::PushID(i);
        /* A rule's name is the name the manifest, a script and an operator
         * line all spell it with, so it is shown as it is written rather
         * than translated. */
        ImGui::TextUnformatted(simRulesRuleName(rule));
        ImGui::SameLine(kRuleNameWidth);

        ImGui::SetNextItemWidth(kRuleValueWidth);
        if (simRulesRuleValueKind(rule) == SIM_RULE_VALUE_FLOAT) {
            float v = (float)m->rules[i].value;
            if (ImGui::InputFloat("##value", &v, 0.0f, 0.0f, "%.3f")) {
                meScenarioFormSetRule(f, rule, (double)v);
            }
        } else {
            int v = (int)m->rules[i].value;
            if (ImGui::InputInt("##value", &v, 0, 0)) {
                meScenarioFormSetRule(f, rule, (double)v);
            }
        }

        /* The value the classic game plays this rule at, and what the value
         * beside it does to the rule. Both are read again every frame, so
         * the words follow the box as the author types. */
        ImGui::SameLine();
        snprintf(classic, sizeof(classic), "%s %g",
                 langGetText(STR_MAPEDIT_SCENARIO_CLASSIC),
                 simRulesClassicValue(rule));
        meScnHint(classic);

        ImGui::SameLine();
        simRulesPhrase(rule, m->rules[i].value, phrase, sizeof(phrase));
        ImGui::TextUnformatted(phrase);

        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_REMOVE))) {
            removeAt = i;
        }
        ImGui::PopID();
    }

    if (removeAt >= 0) {
        meScenarioFormRemoveRule(f, removeAt);
    }

    ImGui::Separator();
    ImGui::TextUnformatted(langGetText(STR_MAPEDIT_SCENARIO_ADD_RULE));

    if (m->numRules >= SCN_MANIFEST_RULES_MAX) {
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_RULES_FULL));
        return;
    }

    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##ruleFilter",
                             langGetText(STR_MAPEDIT_SCENARIO_FILTER),
                             s_filter, sizeof(s_filter));

    /* The rule list is long, so it gets a box of its own to scroll in rather
     * than pushing the rules above it off the top of the window. */
    ImGui::BeginChild("##ruleList", ImVec2(0.0f, 180.0f),
                      ImGuiChildFlags_Borders);
    {
        const int count = simRulesRuleCount();
        int       r;

        for (r = 0; r < count; r++) {
            const char *name = simRulesRuleName(r);

            if (meScenarioFormFindRule(f, r) >= 0) {
                continue;
            }
            if (!meScnContains(name, s_filter)) {
                continue;
            }
            if (ImGui::Selectable(name)) {
                /* A rule starts at the value the classic game plays it at,
                 * so the row reads "unchanged" until the author moves it. */
                meScenarioFormSetRule(f, r, simRulesClassicValue(r));
            }
        }
    }
    ImGui::EndChild();
}

/* -------------------------------------------------------
 * The panel
 * ------------------------------------------------------- */
void mapEditorImguiScenarioPanel(MEScenarioState *st, MEScenarioForm *form,
                                 const char *mapPath, int *view, bool *p_open,
                                 bool *wantSave, bool *wantReload) {
    if (st == NULL || form == NULL || view == NULL || p_open == NULL ||
        !*p_open) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(680, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(langGetText(STR_MAPEDIT_SCENARIO_TITLE), p_open,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    meScnViewButton(view, ME_SCENARIO_VIEW_SCRIPT,
                    STR_MAPEDIT_SCENARIO_VIEW_SCRIPT);
    ImGui::SameLine();
    meScnViewButton(view, ME_SCENARIO_VIEW_METADATA,
                    STR_MAPEDIT_SCENARIO_VIEW_METADATA);
    ImGui::SameLine();
    meScnViewButton(view, ME_SCENARIO_VIEW_LOBBY,
                    STR_MAPEDIT_SCENARIO_VIEW_LOBBY);
    ImGui::SameLine();
    meScnViewButton(view, ME_SCENARIO_VIEW_RULES,
                    STR_MAPEDIT_SCENARIO_VIEW_RULES);
    ImGui::Separator();

    switch (*view) {
        case ME_SCENARIO_VIEW_METADATA:
            meScnMetadataBody(form);
            break;
        case ME_SCENARIO_VIEW_LOBBY:
            meScnLobbyBody(form);
            break;
        case ME_SCENARIO_VIEW_RULES:
            meScnRulesBody(form);
            break;
        default:
            meScnScriptBody(st, mapPath, wantSave, wantReload);
            break;
    }

    ImGui::End();
}
