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
 *   Lobby, Rules and Tags are forms over the manifest
 *   mapeditor_scenario_form.c holds.
 *
 *   Functions lists every hook and policy a scenario may
 *   define, marks the ones this script has written, and
 *   starts one it has not. The catalogue behind that list is
 *   a C header naming Lua types, so the rows reach this file
 *   as plain text through mapeditor_scenario_fndesc.h.
 *
 *   The tags view lists the map's own pills, bases and
 *   starts, which this file cannot ask the map for: those
 *   lists are bolo internals and nothing here reaches into
 *   them. mapeditor.c passes the counts and the positions in,
 *   and the rectangle the selection tool is holding with
 *   them, because that rectangle is where a region's bounds
 *   come from. Entity indices here are the editor's own,
 *   counting from 0; the form module is what turns one into
 *   the manifest's 1-based entry.
 *
 *   The panel draws and reports what the user asked for;
 *   reading and writing files is mapeditor.c's job, the way
 *   the stats panel's Refresh works. That includes the two
 *   buttons at the foot of the metadata view, which say the
 *   scenario should be written on to the map or out as a
 *   mod and leave the writing to the caller.
 *********************************************************/

#ifdef _WIN32
#include <WinSock2.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>

#include "imgui.h"
#include "TextEditor.h"

#include "mapeditor_imgui.h"
#include "mapeditor_scenario.h"
#include "mapeditor_scenario_check.h"
#include "mapeditor_scenario_fndesc.h"
#include "mapeditor_scenario_fnscan.h"
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

/* Set while the script view draws its widget. The panel is not called at all
 * while it is closed, and the body below is not reached when another view is
 * showing, so a mark that is not set again is what says the widget has gone.
 * mapeditor_imgui.cpp reads it once a frame to decide whether SDL text input
 * should be running on the editor's window. */
static bool s_scriptViewDrew = false;

bool mapEditorImguiScriptViewDrew(void) {
    const bool drew  = s_scriptViewDrew;
    s_scriptViewDrew = false;
    return drew;
}

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

/* The most the issues list takes from the editor above it, and how wide the
 * completion popup opens. The popup is a popup rather than a column so it
 * costs the script none of its width at Deck size. */
static const float kIssuesMaxHeight = 150.0f;
static const float kCallsWidth      = 380.0f;
static const float kCallsListHeight = 220.0f;

/* The markers the widget draws, built from the issues that carry a line. A
 * problem the validator could not place stays out of here and is read off the
 * list below the editor instead. */
static void meScnApplyMarkers(TextEditor *editor, const MEScenarioCheck *chk) {
    std::map<int, std::string> markers;
    uint16_t                   i;

    for (i = 0; i < chk->result.count; i++) {
        const ScnValidateIssue *iss = &chk->result.issues[i];

        if (iss->line <= 0) {
            continue;
        }
        /* The widget holds one message a line, so two problems on one line are
           joined rather than one replacing the other. */
        std::map<int, std::string>::iterator at = markers.find(iss->line);
        if (at == markers.end()) {
            markers[iss->line] = iss->message;
        } else {
            at->second += "\n";
            at->second += iss->message;
        }
    }

    if (markers.empty()) {
        editor->ClearErrorMarkers();
    } else {
        editor->SetErrorMarkers(markers);
    }
}

/* What the last check found, under the editor. A row with a line moves the
 * caret to it and brings it into view; a row without one — a problem the
 * source does not spell in any one place — is still listed, because it is
 * still wrong.
 *
 * stale says whether to draw the line about the text having moved. It is
 * passed in rather than read off the check because the space that line takes
 * is reserved before the widget draws, and the widget is where an edit is
 * seen: drawing from the same answer the reservation used keeps the two in
 * step. */
static void meScnIssuesList(TextEditor *editor, const MEScenarioCheck *chk,
                            float height, bool stale) {
    char     row[SCN_VALIDATE_KEY_LEN + SCN_VALIDATE_MSG_LEN + 32];
    uint16_t i;

    /* The text has moved since these were found, so a row's line number is
       where the problem was rather than where it is now. The list is left
       standing: an author fixing one issue is still reading the others. */
    if (stale) {
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_CHECK_STALE));
    }

    ImGui::Text("%s (%u)", langGetText(STR_MAPEDIT_SCENARIO_ISSUES),
                (unsigned)chk->result.count);

    ImGui::BeginChild("##scenarioIssues", ImVec2(0.0f, height),
                      ImGuiChildFlags_Borders);
    {
        for (i = 0; i < chk->result.count; i++) {
            const ScnValidateIssue *iss = &chk->result.issues[i];

            if (iss->line > 0) {
                snprintf(row, sizeof(row), "%d  %s  %s", iss->line, iss->key,
                         iss->message);
            } else {
                snprintf(row, sizeof(row), "-  %s  %s", iss->key,
                         iss->message);
            }

            ImGui::PushID((int)i);
            if (ImGui::Selectable(row) && iss->line > 0) {
                /* An issue counts lines from 1 and the widget from 0. */
                editor->SetCursorPosition(iss->line - 1, 0);
                editor->SetViewAtLine(iss->line - 1,
                                      TextEditor::SetViewAtLineMode::Centered);
            }
            ImGui::PopID();
        }

        if (chk->result.dropped > 0) {
            meScnHint(langGetText(STR_MAPEDIT_SCENARIO_ISSUES_DROPPED));
        }
        /* The editor hands the validator no sim, so two of its checks did not
           run. Said here rather than nowhere. */
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_NO_SIM_CHECKS));
    }
    ImGui::EndChild();
}

/* The widget's text becomes the buffer's, and the last check is marked as
 * standing on text that has moved. Both ways the text changes — a typed edit
 * the undo index catches and the completion popup's insert — come through
 * here, so the issues list says so however it happened. */
static void meScnTextEdited(MEScenarioState *st, MEScenarioCheck *chk,
                            const char *text, size_t len) {
    meScenarioSetText(st, text, len);
    if (chk != NULL) {
        chk->stale = true;
    }
}

/* The game.* calls a script may make, from the binding registry itself. The
 * row is the name; the line under the list is what that row does. */
static void meScnCallsPopup(TextEditor *editor, MEScenarioState *st,
                            MEScenarioCheck *chk) {
    static char s_filter[64] = "";
    /* Which row the line under the list is describing. It survives a frame the
     * mouse is between rows, so the line does not blink out. */
    static int s_described = -1;

    const char *doc  = NULL;
    const char *name = NULL;
    size_t      count;
    size_t      i;

    if (!ImGui::BeginPopup("##scenarioCalls")) {
        return;
    }
    count = meScenarioCompletionCount();

    ImGui::SetNextItemWidth(kCallsWidth);
    ImGui::InputTextWithHint("##callFilter",
                             langGetText(STR_MAPEDIT_SCENARIO_FILTER),
                             s_filter, sizeof(s_filter));

    ImGui::BeginChild("##callList", ImVec2(kCallsWidth, kCallsListHeight),
                      ImGuiChildFlags_Borders);
    {
        for (i = 0; i < count; i++) {
            if (!meScenarioCompletionAt(i, &name, &doc)) {
                continue;
            }
            if (!meScnContains(name, s_filter)) {
                continue;
            }

            ImGui::PushID((int)i);
            if (ImGui::Selectable(name)) {
                editor->InsertTextAtCursor(name);
                /* The insert adds no undo record, so the watch on the undo
                   index in the body below will not see it. The text is read
                   back here instead; without this the buffer keeps what it
                   held before the insert and a save writes that. */
                const std::string text = editor->GetText();
                meScnTextEdited(st, chk, text.c_str(), text.size());
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered()) {
                s_described = (int)i;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    doc = NULL;
    if (s_described >= 0) {
        /* The name is not wanted here, only the line under the list. */
        (void)meScenarioCompletionAt((size_t)s_described, NULL, &doc);
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + kCallsWidth);
    meScnHint(doc != NULL ? doc : "");
    ImGui::PopTextWrapPos();

    ImGui::EndPopup();
}

/* What the functions view asked the script view to do, waiting for the next
 * frame the script view draws.
 *
 * The widget belongs to that view's own body and only one view draws per
 * frame, so the functions view cannot reach it. It leaves the action here
 * and switches the panel to the script view, which is where an author
 * pressing either button wants to end up: Add and Go to both mean show me
 * this in the script.
 *
 * An empty stub and a line of 0 mean there is nothing waiting. The line
 * counts from 1, as the scanner and the issues list count. */
static char s_pendingStub[ME_SCN_FN_STUB_MAX] = "";
static int  s_pendingLine                     = 0;

/* The two things the functions view can ask for. Each clears the other,
 * because only one of them can be what the author last pressed, and each
 * switches the panel to the view that carries it out. */
static void meScnAskInsert(size_t row, int *view) {
    meScnFnStub(row, s_pendingStub, sizeof(s_pendingStub));
    s_pendingLine = 0;
    *view         = ME_SCENARIO_VIEW_SCRIPT;
}

static void meScnAskGoTo(int line, int *view) {
    s_pendingStub[0] = '\0';
    s_pendingLine    = line;
    *view            = ME_SCENARIO_VIEW_SCRIPT;
}

/* Whatever the functions view left behind, applied to the widget.
 *
 * A stub goes in at the start of the line the cursor is on, so it takes
 * lines of its own and the line that was there is pushed down whole rather
 * than split around it. A blank line follows it, and the cursor is left at
 * the end of the function line, which is where the body gets typed.
 *
 * InsertTextAtCursor adds no undo record, so the watch on the undo index in
 * the body below will not see it. The text is read back here for the same
 * reason the completion popup reads it back: without that the buffer keeps
 * what it held before the insert and a save writes that. */
static void meScnApplyPending(TextEditor *editor, MEScenarioState *st,
                              MEScenarioCheck *chk) {
    if (s_pendingStub[0] != '\0') {
        const char *eol      = strchr(s_pendingStub, '\n');
        const int   firstLen = (eol != NULL) ? (int)(eol - s_pendingStub)
                                             : (int)strlen(s_pendingStub);
        int         line     = 0;
        int         column   = 0;
        char        text[ME_SCN_FN_STUB_MAX + 4];

        editor->GetCursorPosition(line, column);
        editor->SetCursorPosition(line, 0);
        snprintf(text, sizeof(text), "%s\n\n", s_pendingStub);
        editor->InsertTextAtCursor(text);
        s_pendingStub[0] = '\0';

        editor->SetCursorPosition(line, firstLen);
        editor->SetViewAtLine(line, TextEditor::SetViewAtLineMode::Centered);

        const std::string edited = editor->GetText();
        meScnTextEdited(st, chk, edited.c_str(), edited.size());
    }

    if (s_pendingLine > 0) {
        /* A definition counts lines from 1 and the widget from 0. The text
           can have moved since the list was drawn, so a line past the end
           of it lands on the last line rather than nowhere. */
        int line = s_pendingLine - 1;

        if (line >= editor->GetLineCount()) {
            line = editor->GetLineCount() - 1;
        }
        if (line < 0) {
            line = 0;
        }
        editor->SetCursorPosition(line, 0);
        editor->SetViewAtLine(line, TextEditor::SetViewAtLineMode::Centered);
        s_pendingLine = 0;
    }
}

static void meScnScriptBody(MEScenarioState *st, MEScenarioCheck *chk,
                            const char *mapPath, bool *wantSave,
                            bool *wantReload, bool *wantValidate) {
    /* One widget for the one view. It holds the text being edited and the
     * undo history that goes with it, so it outlives a frame. */
    static TextEditor s_editor;
    static bool       s_ready = false;
    /* Where the widget's undo history stood when the buffer below last
     * agreed with it. A typed edit adds an undo record, so a different index
     * means the text has moved on and is worth reading back. This is what
     * keeps GetText() off the per-frame path. The one mutation that adds no
     * record is the completion popup's insert, which is why that path reads
     * the text back itself rather than leaving it to this watch. */
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

    /* After the seeding above, so an insert made on the frame a script
     * arrived goes into that script rather than into the one it replaced. */
    meScnApplyPending(&s_editor, st, chk);

    /* The markers follow the last check the same way the text follows the last
     * read: applied once when they change, not rebuilt every frame. */
    if (chk != NULL && chk->pushToWidget) {
        meScnApplyMarkers(&s_editor, chk);
        chk->pushToWidget = false;
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
    /* Live whenever the pane knows where the script goes, rather than only
     * when one was found: a script written beside the map after the map was
     * opened is read by pressing this, and there is no other way to pick one
     * up short of closing the map and opening it again. A read that finds
     * nothing keeps the buffer and says so on the status line. */
    ImGui::BeginDisabled(st->scriptPath[0] == '\0');
    if (ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_RELOAD)) &&
        wantReload != NULL) {
        *wantReload = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", langGetText(STR_MAPEDIT_SCENARIO_RELOAD_TIP));
    }

    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_VALIDATE)) &&
        wantValidate != NULL) {
        *wantValidate = true;
    }

    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_CALLS))) {
        ImGui::OpenPopup("##scenarioCalls");
    }
    meScnCallsPopup(&s_editor, st, chk);

    /* A check that found nothing says so here rather than opening a list with
     * nothing in it. */
    if (chk != NULL && chk->hasRun && chk->result.count == 0) {
        ImGui::SameLine();
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_NO_ISSUES));
    }

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

    /* How much of the window the issues list takes, and so how much is left
     * for the editor. Nothing is reserved when there is nothing to list, which
     * leaves the pane exactly as it was before a check was run. */
    const bool haveIssues =
        (chk != NULL && chk->hasRun && chk->result.count > 0);
    float issuesHeight = 0.0f;
    if (haveIssues) {
        issuesHeight = ImGui::GetTextLineHeightWithSpacing() *
                       (float)(chk->result.count + 2);
        if (issuesHeight > kIssuesMaxHeight) {
            issuesHeight = kIssuesMaxHeight;
        }
    }

    /* The widget takes its characters from ImGui's character queue, which only
     * fills while SDL text input is running. Saying the view drew is what gets
     * it started; the editor's frame hook does the starting. */
    s_scriptViewDrew = true;

    /* Whether the list says the text has moved since the check ran, answered
     * before the widget draws because the line it takes comes out of the
     * editor's height. An edit made this frame is read back below, so it is
     * the frame after it that says so. */
    const bool staleLine = (haveIssues && chk->stale);

    /* What the list takes below the editor: the box, the line naming it, and
     * that line when there is one. */
    float belowHeight = 0.0f;
    if (haveIssues) {
        belowHeight = issuesHeight + ImGui::GetTextLineHeightWithSpacing();
        if (staleLine) {
            belowHeight += ImGui::GetTextLineHeightWithSpacing();
        }
    }

    /* The rest of the window is the editor, less whatever the list takes. */
    s_editor.Render("##scenarioScript", false,
                    ImVec2(0.0f, haveIssues ? -belowHeight : 0.0f), false);

    /* Read the text back only when the widget has moved on from what the
     * buffer holds. */
    const int undoIndex = s_editor.GetUndoIndex();
    if (undoIndex != s_seenUndoIndex) {
        const std::string text = s_editor.GetText();
        meScnTextEdited(st, chk, text.c_str(), text.size());
        s_seenUndoIndex = undoIndex;
    }

    if (haveIssues) {
        meScnIssuesList(&s_editor, chk, issuesHeight, staleLine);
    }
}

/* -------------------------------------------------------
 * Metadata
 * ------------------------------------------------------- */
/* The last component of a path, which is what a button says the scenario is
 * being packed into. */
static const char *meScnFileName(const char *path) {
    const char *at   = path;
    const char *last = path;

    if (path == NULL) {
        return "";
    }
    for (; *at != '\0'; at++) {
        if (*at == '/' || *at == '\\') {
            last = at + 1;
        }
    }
    return last;
}

/* Where the scenario is saved to: on to the map it was written for, or as a
 * file of its own that plays over any map. The two live under the metadata
 * form because that is where the package's identity is edited — its name, its
 * game type and whether it is built for this map at all. */
static void meScnSaveRow(const MEScenarioForm *f, MEScenarioState *st,
                         const char *mapPath, bool *wantPack,
                         bool *wantSaveMod) {
    const bool haveMap = (mapPath != NULL && mapPath[0] != '\0');
    const int  dropTags    = meScenarioFormTagCount(f);
    const int  dropRegions = meScenarioFormRegionCount(f);

    ImGui::Separator();

    ImGui::BeginDisabled(!haveMap);
    if (ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_PACK_MAP)) &&
        wantPack != NULL) {
        *wantPack = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (haveMap) {
        meScnHint(meScnFileName(mapPath));
    } else {
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_PACK_NO_MAP));
    }

    if (ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_SAVE_MOD)) &&
        wantSaveMod != NULL) {
        *wantSaveMod = true;
    }

    /* A mod is written from a copy with the tags and the regions cleared,
     * because it plays over a map it has never seen. Said here, beside the
     * button, so the author reads it before asking rather than after. */
    if (dropTags > 0 || dropRegions > 0) {
        MessageArgs args = {};
        args.number  = dropTags;
        args.number2 = dropRegions;
        ImGui::TextWrapped("%s",
                           langGetTextFmt(STR_MAPEDIT_SCENARIO_MOD_DROPS,
                                          &args));
    }

    /* What the last read or write did. One status line for the panel, which
     * the script view shows in its toolbar and this view shows here. */
    if (st != NULL && st->status[0] != '\0') {
        ImGui::TextWrapped("%s", st->status);
    }
}

static void meScnMetadataBody(MEScenarioForm *f, MEScenarioState *st,
                              const char *mapPath, bool *wantPack,
                              bool *wantSaveMod) {
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

    meScnSaveRow(f, st, mapPath, wantPack, wantSaveMod);
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
        /* A team number the scenario cannot be packed with, said here rather
         * than at the end of a pack that writes nothing. */
        switch (meScenarioFormTeamIdProblem(f, i)) {
            case ME_SCENARIO_TEAM_ID_RANGE: {
                MessageArgs args = {};
                args.number      = MAX_TANKS - 1;
                meScnHint(langGetTextFmt(STR_MAPEDIT_SCENARIO_TEAM_ID_RANGE,
                                         &args));
                break;
            }
            case ME_SCENARIO_TEAM_ID_TAKEN:
                meScnHint(langGetText(STR_MAPEDIT_SCENARIO_TEAM_ID_TAKEN));
                break;
            default:
                break;
        }
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
    /* What the author has typed into the Add Rule filter, and which row of the
     * list they are reading about. One panel, so one box and one selection,
     * and both survive a switch away and back. */
    static char s_filter[64] = "";
    static int  s_selected   = -1;

    ScenarioManifest *m = &f->manifest;
    int               i;
    int               removeAt    = -1;
    bool              addSelected = false;
    char              classic[64];
    char              phrase[128];
    char              range[128];
    char              detail[256];

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
            /* Clicking a row reads about the rule; the Add button under the
             * list is what puts it on the manifest. */
            if (ImGui::Selectable(name, r == s_selected)) {
                s_selected = r;
            }
        }
    }
    ImGui::EndChild();

    /* The selection is an index held across frames while the list under it
     * changes. A rule that has been added, or that the filter no longer
     * matches, is not in the list to be pointed at any more, so the block
     * below would describe a row the author cannot see and Add would take a
     * rule they did not pick. */
    if (s_selected >= 0 &&
        (s_selected >= simRulesRuleCount() ||
         meScenarioFormFindRule(f, s_selected) >= 0 ||
         !meScnContains(simRulesRuleName(s_selected), s_filter))) {
        s_selected = -1;
    }

    if (s_selected < 0) {
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_RULE_NO_SEL));
        return;
    }

    /* The selected rule: its name as the manifest spells it, what it governs,
     * and the two numbers an author needs before taking it. */
    ImGui::TextUnformatted(simRulesRuleName(s_selected));
    ImGui::SameLine();
    addSelected = ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_ADD));

    /* A description runs to about a hundred characters, so it wraps rather
     * than running off the right of the panel. */
    ImGui::TextWrapped("%s", simRulesRuleDescription(s_selected));

    simRulesRangePhrase(s_selected, range, sizeof(range));
    snprintf(detail, sizeof(detail), "%s %g   %s %s",
             langGetText(STR_MAPEDIT_SCENARIO_CLASSIC),
             simRulesClassicValue(s_selected),
             langGetText(STR_MAPEDIT_SCENARIO_RANGE), range);
    meScnHint(detail);

    if (addSelected) {
        /* A rule starts at the value the classic game plays it at, so the row
         * reads "unchanged" until the author moves it. It is on the manifest
         * now, so it leaves the list and the block is cleared with it. */
        meScenarioFormSetRule(f, s_selected, simRulesClassicValue(s_selected));
        s_selected = -1;
    }
}

/* -------------------------------------------------------
 * Tags and regions
 * ------------------------------------------------------- */

/* How wide an entity's row is before the box for its next tag, how wide that
 * box is, and how wide one of a region's four numbers is. The row is a fixed
 * width so the boxes down the list line up under each other. */
static const float kEntityRowWidth  = 200.0f;
static const float kTagBoxWidth     = 150.0f;
static const float kRegionNameWidth = 160.0f;
static const float kRegionNumWidth  = 60.0f;

/* The longest of the three entity lists, which is as many rows as one kind
 * can show. */
static const int kEntityRows = (MAX_PILLS >= MAX_BASES &&
                                MAX_PILLS >= MAX_STARTS)
                                   ? MAX_PILLS
                                   : (MAX_BASES >= MAX_STARTS ? MAX_BASES
                                                              : MAX_STARTS);

/* One of a region's four numbers, edited as an int and handed straight back to
 * the form. The clamping is the form's, so the boxes and Set from Selection
 * are held to the same rectangle. */
static bool meScnRegionField(langid label, const char *id, int *value) {
    bool changed;

    ImGui::PushID(id);
    ImGui::SetNextItemWidth(kRegionNumWidth);
    changed = ImGui::InputInt(langGetText(label), value, 0, 0);
    ImGui::PopID();
    return changed;
}

/* The named rectangles, under the entity lists. A region is drawn with the
 * selection tool the editor already has: select squares on the map, name them
 * and press Add. Each row can re-take the bounds from whatever is selected
 * now, which is how a region is moved without typing four numbers. */
static void meScnRegionRows(MEScenarioForm *f, const MEScenarioMapInfo *info) {
    /* The name being typed for the next region. One panel, so one box. */
    static char s_name[SCN_REGION_NAME_LEN] = "";

    ScenarioManifest *m       = &f->manifest;
    const bool        haveSel = (info != NULL && info->hasSelection);
    int               removeAt = -1;
    int               i;

    ImGui::Separator();
    ImGui::TextUnformatted(langGetText(STR_MAPEDIT_SCENARIO_REGIONS));
    meScnHint(langGetText(STR_MAPEDIT_SCENARIO_REGIONS_ON_MAP));

    if (m->numRegions == 0) {
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_NO_REGIONS));
    }

    for (i = 0; i < (int)m->numRegions; i++) {
        ScnManifestRegion *r = &m->regions[i];
        int                x = (int)r->x;
        int                y = (int)r->y;
        int                w = (int)r->w;
        int                h = (int)r->h;
        bool               moved = false;

        ImGui::PushID(i);
        ImGui::SetNextItemWidth(kRegionNameWidth);
        if (ImGui::InputTextWithHint("##name",
                                     langGetText(STR_MAPEDIT_SCENARIO_REGION_NAME),
                                     r->name, sizeof(r->name))) {
            f->dirty = true;
        }

        ImGui::SameLine();
        moved |= meScnRegionField(STR_MAPEDIT_SCENARIO_REGION_X, "x", &x);
        ImGui::SameLine();
        moved |= meScnRegionField(STR_MAPEDIT_SCENARIO_REGION_Y, "y", &y);
        ImGui::SameLine();
        moved |= meScnRegionField(STR_MAPEDIT_SCENARIO_REGION_W, "w", &w);
        ImGui::SameLine();
        moved |= meScnRegionField(STR_MAPEDIT_SCENARIO_REGION_H, "h", &h);
        if (moved) {
            meScenarioFormSetRegionRect(f, i, x, y, w, h);
        }

        /* The two buttons go under the numbers rather than after them: four
           boxes and two buttons on one line run off the right of the window at
           the size the panel opens at. */
        ImGui::Indent();
        ImGui::BeginDisabled(!haveSel);
        if (ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_REGION_FROM_SEL))) {
            meScenarioFormSetRegionRect(f, i, info->selX, info->selY,
                                        info->selW, info->selH);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_REMOVE))) {
            removeAt = i;
        }
        ImGui::Unindent();
        ImGui::PopID();
    }

    if (removeAt >= 0) {
        meScenarioFormRemoveRegion(f, removeAt);
    }

    ImGui::Separator();
    if (m->numRegions >= SCN_REGIONS_MAX) {
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_REGIONS_FULL));
        return;
    }
    if (!haveSel) {
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_REGION_NO_SEL));
        return;
    }

    ImGui::SetNextItemWidth(kRegionNameWidth);
    const bool entered = ImGui::InputTextWithHint(
        "##newRegion", langGetText(STR_MAPEDIT_SCENARIO_REGION_NAME), s_name,
        sizeof(s_name), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_ADD_REGION)) ||
         entered) &&
        meScenarioFormAddRegion(f, s_name, info->selX, info->selY, info->selW,
                                info->selH)) {
        s_name[0] = '\0';
    }
}

static void meScnTagsBody(MEScenarioForm *f, const MEScenarioMapInfo *info,
                          int selKind, int selIndex, int *clickedKind,
                          int *clickedIndex, int *panX, int *panY) {
    /* What is being typed for the next tag on each row. One buffer per row, so
       a name half typed against one pill does not appear against every other
       one. */
    static char s_typed[3][kEntityRows][SCN_TAG_LEN];

    /* The three entity lists, in the order the view draws them: which manifest
       array a tag goes in, what the header and the row say, and which of the
       editor's own selection kinds names it on the map. */
    struct TagKindRow {
        MEScenarioTagKind kind;
        langid            header;
        langid            row;
        int               selKind;
        int               count;
        const uint8_t    *xs;
        const uint8_t    *ys;
    };
    const TagKindRow kinds[3] = {
        {ME_SCENARIO_TAG_PILL, STR_MAPEDIT_SCENARIO_PILLS,
         STR_MAPEDIT_SCENARIO_PILL_ROW, ME_SEL_PILL,
         info != NULL ? info->numPills : 0, info != NULL ? info->pillX : NULL,
         info != NULL ? info->pillY : NULL},
        {ME_SCENARIO_TAG_BASE, STR_MAPEDIT_SCENARIO_BASES,
         STR_MAPEDIT_SCENARIO_BASE_ROW, ME_SEL_BASE,
         info != NULL ? info->numBases : 0, info != NULL ? info->baseX : NULL,
         info != NULL ? info->baseY : NULL},
        {ME_SCENARIO_TAG_START, STR_MAPEDIT_SCENARIO_STARTS,
         STR_MAPEDIT_SCENARIO_START_ROW, ME_SEL_START,
         info != NULL ? info->numStarts : 0, info != NULL ? info->startX : NULL,
         info != NULL ? info->startY : NULL},
    };

    /* A tag is dropped after the list has been drawn, so the row being read is
       not the row being changed. */
    MEScenarioTagKind removeKind   = ME_SCENARIO_TAG_PILL;
    int               removeEntity = -1;
    int               removeTag    = -1;
    int               k;

    for (k = 0; k < (int)(sizeof(kinds) / sizeof(kinds[0])); k++) {
        const TagKindRow *kd = &kinds[k];
        int               i;

        ImGui::PushID(k);
        if (ImGui::CollapsingHeader(langGetText(kd->header),
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            if (kd->count <= 0) {
                meScnHint(langGetText(STR_MAPEDIT_SCENARIO_NO_ENTITIES));
            }
            for (i = 0; i < kd->count && i < kEntityRows; i++) {
                const ScnManifestTags *tags =
                    meScenarioFormTags(f, kd->kind, i);
                MessageArgs args = {};
                int         t;

                if (tags == NULL) {
                    continue;   /* an entity the manifest has no entry for */
                }
                args.number  = i;
                args.number2 = (int)kd->xs[i];
                args.number3 = (int)kd->ys[i];

                ImGui::PushID(i);
                /* The row is marked when it is the object selected on the map,
                   and clicking it selects that object and pans to it. */
                if (ImGui::Selectable(langGetTextFmt(kd->row, &args),
                                      selKind == kd->selKind && selIndex == i,
                                      0, ImVec2(kEntityRowWidth, 0.0f))) {
                    if (clickedKind != NULL) {
                        *clickedKind = kd->selKind;
                    }
                    if (clickedIndex != NULL) {
                        *clickedIndex = i;
                    }
                    if (panX != NULL) {
                        *panX = (int)kd->xs[i];
                    }
                    if (panY != NULL) {
                        *panY = (int)kd->ys[i];
                    }
                }

                ImGui::SameLine();
                if (tags->count >= SCN_TAGS_PER_ENTITY) {
                    meScnHint(langGetText(STR_MAPEDIT_SCENARIO_TAGS_FULL));
                } else {
                    char *box = s_typed[k][i];

                    ImGui::SetNextItemWidth(kTagBoxWidth);
                    const bool entered = ImGui::InputTextWithHint(
                        "##tag", langGetText(STR_MAPEDIT_SCENARIO_TAG), box,
                        SCN_TAG_LEN, ImGuiInputTextFlags_EnterReturnsTrue);
                    ImGui::SameLine();
                    if ((ImGui::Button(
                             langGetText(STR_MAPEDIT_SCENARIO_ADD_TAG)) ||
                         entered) &&
                        meScenarioFormAddTag(f, kd->kind, i, box)) {
                        box[0] = '\0';
                    }
                }

                /* The tags themselves, under the row. Four at most, and each
                   one short, so they share a line. */
                if (tags->count > 0) {
                    ImGui::Indent();
                    for (t = 0; t < (int)tags->count; t++) {
                        ImGui::PushID(t);
                        ImGui::TextUnformatted(tags->tag[t]);
                        ImGui::SameLine();
                        if (ImGui::Button(
                                langGetText(STR_MAPEDIT_SCENARIO_REMOVE))) {
                            removeKind   = kd->kind;
                            removeEntity = i;
                            removeTag    = t;
                        }
                        ImGui::PopID();
                        if (t + 1 < (int)tags->count) {
                            ImGui::SameLine();
                        }
                    }
                    ImGui::Unindent();
                }
                ImGui::PopID();
            }
        }
        ImGui::PopID();
    }

    if (removeTag >= 0) {
        meScenarioFormRemoveTag(f, removeKind, removeEntity, removeTag);
    }

    meScnRegionRows(f, info);
}

/* -------------------------------------------------------
 * Functions
 * ------------------------------------------------------- */

/* How wide the column of Add and Go to buttons is, so the names line up
 * down the list whichever button a row carries, and how wide the chooser
 * under the list opens. */
static const float kFnButtonWidth  = 78.0f;
static const float kFnChooserWidth = 300.0f;

/* As many definitions as the view reads out of one script. The catalogue is
 * 35 rows and the rest of what a script defines is the author's own
 * helpers, so this is well above what a list has to show; a script with
 * more than this has the rest left off the list and off the chooser. */
#define ME_SCN_FOUND_MAX 128

static bool meScnFnIsNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* One line of the script, by the number the scanner gave it, cut where the
 * scanner cuts a line: at the first --, which is where the code stops. The
 * rule is written again here rather than reached for, because it belongs to
 * the scan and the scan answers names and lines, not text. NULL for a line
 * the text does not have. */
static const char *meScnFnLineAt(const char *text, int lineNo, size_t *len) {
    const char *p  = text;
    const char *eol;
    int         at = 1;
    size_t      i;

    if (text == NULL || lineNo < 1) {
        return NULL;
    }
    while (at < lineNo) {
        eol = strchr(p, '\n');
        if (eol == NULL) {
            return NULL;
        }
        p = eol + 1;
        at++;
    }
    eol  = strchr(p, '\n');
    *len = (eol != NULL) ? (size_t)(eol - p) : strlen(p);
    for (i = 0; i + 1 < *len; i++) {
        if (p[i] == '-' && p[i + 1] == '-') {
            *len = i;
            break;
        }
    }
    return p;
}

/* Whether the definition on that line is the colon form — the
 * function scenario:on_tick(tick) spelling.
 *
 * The scanner reports that line as defining on_tick, which is right: the
 * field is defined either way. What it does not report is which spelling
 * did it, and the difference is the whole of what this is for. A colon puts
 * an implicit self in front of the parameters, while the host calls the
 * field with the hook's own arguments, so self swallows the first of them
 * and every argument after it shifts. The author has a hook that is there
 * and behaves wrongly. So the line the scanner named is read again, for the
 * name with a colon in front of it and a parameter list after it. */
static bool meScnFnColonForm(const char *text, const char *name, int lineNo) {
    size_t      len     = 0;
    const char *line    = meScnFnLineAt(text, lineNo, &len);
    const size_t nameLen = strlen(name);
    size_t      i;

    if (line == NULL || nameLen == 0) {
        return false;
    }
    for (i = 0; i + nameLen <= len; i++) {
        size_t j;

        if (memcmp(line + i, name, nameLen) != 0) {
            continue;
        }
        /* The name a parameter list opens on, rather than one mentioned in
           passing or the tail of a longer one. */
        if (i + nameLen < len && meScnFnIsNameChar(line[i + nameLen])) {
            continue;
        }
        j = i + nameLen;
        while (j < len && (line[j] == ' ' || line[j] == '\t')) {
            j++;
        }
        if (j >= len || line[j] != '(') {
            continue;
        }
        /* And what stands in front of it, past whatever spaces. A colon
           there is the form; a dot or the keyword itself is not. */
        j = i;
        while (j > 0 && (line[j - 1] == ' ' || line[j - 1] == '\t')) {
            j--;
        }
        if (j > 0 && line[j - 1] == ':') {
            return true;
        }
    }
    return false;
}

/* How many of the definitions found are of that name, which line the first
 * of them is on, and whether any of them is the colon form. */
static int meScnFnDefinedAs(const char *text, const MEScnFoundFn *found,
                            size_t nFound, const char *name, int *firstLine,
                            bool *colon) {
    int    n = 0;
    size_t i;

    *firstLine = 0;
    *colon     = false;
    for (i = 0; i < nFound; i++) {
        if (strcmp(found[i].name, name) != 0) {
            continue;
        }
        if (n == 0) {
            *firstLine = found[i].line;
        }
        n++;
        if (meScnFnColonForm(text, name, found[i].line)) {
            *colon = true;
        }
    }
    return n;
}

/* Every function a scenario may define, what each is for, and which of them
 * this script has written. A row the script does not define offers a stub
 * to start from; one it does offers somewhere to go. Never both: a second
 * definition of a name is live Lua that silently replaces the first, so an
 * Add here would write a function the author would never see run.
 *
 * Neither button touches the script itself. Both leave the action for the
 * script view and switch to it — see meScnApplyPending. */
static void meScnFunctionsBody(MEScenarioState *st, int *view) {
    /* What the author has typed into the filter. One panel, so one box, and
     * it survives a switch away and back. */
    static char s_filter[64] = "";

    MEScnFoundFn found[ME_SCN_FOUND_MAX];
    const char  *text  = (st->script != NULL) ? st->script : "";
    const size_t count = meScnFnCount();
    size_t       nFound;
    size_t       i;
    char         params[ME_SCN_FN_STUB_MAX];

    /* What this script defines, read again every frame. It is a line scan
     * over a buffer already in memory, and the list has to follow an edit
     * made in the script view a moment ago. */
    nFound = meScnScanFunctions(text, found, ME_SCN_FOUND_MAX);

    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##fnFilter",
                             langGetText(STR_MAPEDIT_SCENARIO_FILTER),
                             s_filter, sizeof(s_filter));

    /* The chooser and its line keep the foot of the window; the list takes
     * everything above them. */
    const float below = ImGui::GetFrameHeightWithSpacing() +
                        ImGui::GetTextLineHeightWithSpacing();

    ImGui::BeginChild("##fnList", ImVec2(0.0f, -below),
                      ImGuiChildFlags_Borders);
    {
        for (i = 0; i < count; i++) {
            const char *name    = meScnFnName(i);
            const char *returns = meScnFnReturns(i);
            int         firstLine = 0;
            bool        colon     = false;
            int         defined;

            if (!meScnContains(name, s_filter)) {
                continue;
            }
            defined = meScnFnDefinedAs(text, found, nFound, name, &firstLine,
                                       &colon);

            ImGui::PushID((int)i);
            if (defined > 0) {
                if (ImGui::Button(
                        langGetText(STR_MAPEDIT_SCENARIO_FN_GOTO))) {
                    meScnAskGoTo(firstLine, view);
                }
            } else if (ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_ADD))) {
                meScnAskInsert(i, view);
            }

            /* The name and its parameters as an author writes them, so the
               row reads as the line the stub puts in the script. */
            ImGui::SameLine(kFnButtonWidth);
            meScnFnParamList(i, params, sizeof(params));
            ImGui::Text("%s%s", name, params);

            if (defined > 0) {
                MessageArgs args = {};
                args.number      = firstLine;
                ImGui::SameLine();
                meScnHint(langGetTextFmt(STR_MAPEDIT_SCENARIO_FN_IN_SCRIPT,
                                         &args));
            }

            ImGui::Indent(kFnButtonWidth);
            ImGui::PushTextWrapPos(0.0f);
            meScnHint(meScnFnDescription(i));

            /* A policy is a question, so its row says what the host does
               with the answer. A hook answers nothing and has no such
               line. */
            if (returns[0] != '\0') {
                MessageArgs args = {};
                meScnCopy(args.string1, sizeof(args.string1), returns);
                meScnHint(langGetTextFmt(STR_MAPEDIT_SCENARIO_FN_ANSWERS,
                                         &args));
            }

            /* The two ways a definition that is there can still be wrong.
               Drawn in the ordinary colour rather than the hint colour,
               because neither is a note about the function. */
            if (colon) {
                ImGui::TextUnformatted(
                    langGetText(STR_MAPEDIT_SCENARIO_FN_COLON));
            }
            if (defined > 1) {
                MessageArgs args = {};
                args.number      = defined;
                ImGui::TextUnformatted(
                    langGetTextFmt(STR_MAPEDIT_SCENARIO_FN_TWICE, &args));
            }
            ImGui::PopTextWrapPos();
            ImGui::Unindent(kFnButtonWidth);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    /* Somewhere to jump to, listing what the file defines in the order it
     * defines it — an author's own helpers with the hooks, since those are
     * what the file has. Only what is in the file: the list above is
     * already every function there is, and a definition that has not been
     * written has nowhere to go. */
    if (nFound == 0) {
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_FN_NONE_YET));
        return;
    }

    ImGui::SetNextItemWidth(kFnChooserWidth);
    if (ImGui::BeginCombo("##fnGoTo",
                          langGetText(STR_MAPEDIT_SCENARIO_FN_GOTO_ONE))) {
        for (i = 0; i < nFound; i++) {
            MessageArgs args = {};

            meScnCopy(args.string1, sizeof(args.string1), found[i].name);
            args.number = found[i].line;

            ImGui::PushID((int)i);
            if (ImGui::Selectable(
                    langGetTextFmt(STR_MAPEDIT_SCENARIO_FN_AT_LINE, &args))) {
                meScnAskGoTo(found[i].line, view);
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}

/* -------------------------------------------------------
 * The panel
 * ------------------------------------------------------- */
void mapEditorImguiScenarioPanel(MEScenarioState *st, MEScenarioForm *form,
                                 MEScenarioCheck *check, const char *mapPath,
                                 int *view, bool *p_open,
                                 const MEScenarioMapInfo *mapInfo, int selKind,
                                 int selIndex, int *clickedKind,
                                 int *clickedIndex, int *panX, int *panY,
                                 bool *wantSave, bool *wantReload,
                                 bool *wantValidate, bool *wantPack,
                                 bool *wantSaveMod) {
    /* Nothing clicked and nowhere to pan until a row below says otherwise, on
       every path out of here including the ones that draw nothing. */
    if (clickedKind != NULL) {
        *clickedKind = ME_SEL_NONE;
    }
    if (clickedIndex != NULL) {
        *clickedIndex = -1;
    }
    if (panX != NULL) {
        *panX = -1;
    }
    if (panY != NULL) {
        *panY = -1;
    }

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
    ImGui::SameLine();
    meScnViewButton(view, ME_SCENARIO_VIEW_TAGS,
                    STR_MAPEDIT_SCENARIO_VIEW_TAGS);
    ImGui::SameLine();
    meScnViewButton(view, ME_SCENARIO_VIEW_FUNCTIONS,
                    STR_MAPEDIT_SCENARIO_VIEW_FUNCTIONS);
    ImGui::Separator();

    switch (*view) {
        case ME_SCENARIO_VIEW_METADATA:
            meScnMetadataBody(form, st, mapPath, wantPack, wantSaveMod);
            break;
        case ME_SCENARIO_VIEW_LOBBY:
            meScnLobbyBody(form);
            break;
        case ME_SCENARIO_VIEW_RULES:
            meScnRulesBody(form);
            break;
        case ME_SCENARIO_VIEW_TAGS:
            meScnTagsBody(form, mapInfo, selKind, selIndex, clickedKind,
                          clickedIndex, panX, panY);
            break;
        case ME_SCENARIO_VIEW_FUNCTIONS:
            meScnFunctionsBody(st, view);
            break;
        default:
            meScnScriptBody(st, check, mapPath, wantSave, wantReload,
                            wantValidate);
            break;
    }

    ImGui::End();
}
