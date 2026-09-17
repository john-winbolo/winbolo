/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_scenario_imgui.cpp
 * Purpose:
 *   The scenario script pane: a text editor with Lua
 *   highlighting and line numbers over the buffer
 *   mapeditor_scenario.c holds. The pane draws and reports
 *   what the user asked for; reading and writing the file
 *   is mapeditor.c's job, the way the stats panel's
 *   Refresh works.
 *********************************************************/

#ifdef _WIN32
#include <WinSock2.h>
#endif

#include <string>

#include "imgui.h"
#include "TextEditor.h"

#include "mapeditor_imgui.h"
#include "mapeditor_scenario.h"
#include "../gui/lang.h"

void mapEditorImguiScenarioScript(MEScenarioState *st, const char *mapPath,
                                  bool *p_open, bool *wantSave,
                                  bool *wantReload) {
    /* One widget for the one pane. It holds the text being edited and the
     * undo history that goes with it, so it outlives a frame. */
    static TextEditor s_editor;
    static bool       s_ready = false;
    /* Where the widget's undo history stood when the buffer below last
     * agreed with it. Every edit the widget makes adds an undo record, so a
     * different index means the text has moved on and is worth reading back.
     * This is what keeps GetText() off the per-frame path. */
    static int s_seenUndoIndex = 0;

    if (st == NULL || p_open == NULL || !*p_open) {
        return;
    }

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

    ImGui::SetNextWindowSize(ImVec2(680, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(langGetText(STR_MAPEDIT_SCENARIO_TITLE), p_open,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
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

    ImGui::End();
}
