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
 *   Triggers lists the triggers the manifest declares and sets
 *   which hook each one runs on, off that same catalogue. The
 *   tests under a trigger are drawn there too: a field of the
 *   hook's payload, an operator the field can answer, and a
 *   value the field's own type says how to state. So are the
 *   actions: an op the registry offers or call, the line saying
 *   what that op does, and one widget per argument under the
 *   registry's own name and type for it.
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

#include <SDL3/SDL.h>  /* SDL_snprintf — a number written into UI text reads
                        * the same whatever locale the process is in */

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

/* What a value the rule will not take is written in. The same red the map
 * validator writes its errors in, so a problem reads as a problem wherever
 * the editor says one. */
static const ImVec4 kValueRefused(1.0f, 0.3f, 0.3f, 1.0f);

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
                SDL_snprintf(row, sizeof(row), "%d  %s  %s", iss->line,
                             iss->key, iss->message);
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
        /* The editor hands the validator no sim, so the tag check did not run.
           Said here rather than nowhere. */
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
                /* InsertTextAtCursor writes no undo record, because the
                   vendored widget keeps AddUndo private and nothing outside
                   it can add one. So the name cannot be taken back as a unit
                   — an undo from here steps through whatever the widget
                   recorded before it — and the watch on the undo index in the
                   body below does not see the insert either. The text is read
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
 * InsertTextAtCursor writes no undo record, because the vendored widget keeps
 * AddUndo private and nothing outside it can add one. So the stub cannot be
 * taken back as a unit — an undo from here steps through whatever the widget
 * recorded before it — and the watch on the undo index in the body below does
 * not see the insert either. The text is read back here for the same reason
 * the completion popup reads it back: without that the buffer keeps what it
 * held before the insert and a save writes that. Both inserts are the same
 * shape, and it is the widget's rather than this file's. */
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

    /* What the file may decide, and the word the manifest writes each one
     * as. Two rows and no None: a file that says nothing is a scenario, so
     * there is no third answer to offer. */
    static const struct {
        ScnManifestKind kind;
        langid          label;
    } kKinds[] = {
        {scnKindScenario, STR_MAPEDIT_SCENARIO_KIND_SCENARIO},
        {scnKindKeepsWinCondition, STR_MAPEDIT_SCENARIO_KIND_MOD},
    };
    const int kKindCount = (int)(sizeof(kKinds) / sizeof(kKinds[0]));

    ScenarioManifest *m = &f->manifest;
    const bool        isMod = meScenarioFormKeepsWinCondition(f);
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

    /* What the file may decide, ahead of the fields it decides them with,
     * because it is what says whether those fields are offered at all. */
    chosen = 0;
    for (i = 0; i < kKindCount; i++) {
        if (m->kind == kKinds[i].kind) {
            chosen = i;
            break;
        }
    }
    ImGui::SetNextItemWidth(360.0f);
    if (ImGui::BeginCombo(langGetText(STR_MAPEDIT_SCENARIO_KIND),
                          langGetText(kKinds[chosen].label))) {
        for (i = 0; i < kKindCount; i++) {
            if (ImGui::Selectable(langGetText(kKinds[i].label), i == chosen)) {
                meScenarioFormSetKind(f, kKinds[i].kind);
            }
        }
        ImGui::EndCombo();
    }
    meScnHint(langGetText(STR_MAPEDIT_SCENARIO_KIND_NOTE));

    /* The game type is win-deciding: open, tournament and strict end a round
     * on different things, and it is the only way a file picks between them.
     * So a mod is not shown the combo at all, rather than being shown one it
     * would fail the check on. Hidden and not disabled, because the rows here
     * are a plain stack and taking one out leaves no hole; a line in its
     * place says why it has gone.
     *
     * What the author already typed stays in the manifest either way, so
     * moving the kind back brings the answer back with it. */
    if (isMod) {
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_GAME_MOD));
    } else {
        /* A word the manifest already holds that is none of the four shows as
         * itself, so picking from the list is the only thing that replaces
         * it. */
        chosen = -1;
        for (i = 0; i < kGameCount; i++) {
            if (strcmp(m->game, kGames[i].word) == 0) {
                chosen = i;
                break;
            }
        }
        preview = (chosen >= 0) ? langGetText(kGames[chosen].label) : m->game;
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::BeginCombo(langGetText(STR_MAPEDIT_SCENARIO_GAME),
                              preview)) {
            for (i = 0; i < kGameCount; i++) {
                if (ImGui::Selectable(langGetText(kGames[i].label),
                                      i == chosen)) {
                    meScnCopy(m->game, sizeof(m->game), kGames[i].word);
                    f->dirty = true;
                }
            }
            ImGui::EndCombo();
        }
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

/* What the last check said about one rule, or NULL where it said nothing. The
 * validator keys a rule's problem "rules.<name>", which is the whole of what
 * ties a row here to a line in the list under the script. A check that has not
 * run has said nothing about anything. */
static const ScnValidateIssue *meScnRuleIssue(const MEScenarioCheck *chk,
                                              const char *name) {
    char     key[SCN_VALIDATE_KEY_LEN];
    uint16_t i;

    if (chk == NULL || !chk->hasRun || name == NULL || name[0] == '\0') {
        return NULL;
    }
    snprintf(key, sizeof(key), "rules.%s", name);
    for (i = 0; i < chk->result.count; i++) {
        if (strcmp(chk->result.issues[i].key, key) == 0) {
            return &chk->result.issues[i];
        }
    }
    return NULL;
}

/* What one rule is set to as the table stands: the manifest's own value where
 * it sets that rule, and the classic default where it does not. That is what
 * the check reads for a rule a scenario leaves alone, so a ceiling read off
 * this row is the ceiling the check will hold the table to. */
static double meScnRuleValueNow(const ScenarioManifest *m, int rule) {
    int i;

    for (i = 0; i < (int)m->numRules; i++) {
        if ((int)m->rules[i].rule == rule) {
            return m->rules[i].value;
        }
    }
    return simRulesClassicValue(rule);
}

static void meScnRulesBody(MEScenarioForm *f, const MEScenarioCheck *chk) {
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
    SimRuleRange      rng;

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
        SDL_snprintf(classic, sizeof(classic), "%s %g",
                     langGetText(STR_MAPEDIT_SCENARIO_CLASSIC),
                     simRulesClassicValue(rule));
        meScnHint(classic);

        ImGui::SameLine();
        simRulesPhrase(rule, m->rules[i].value, phrase, sizeof(phrase));
        ImGui::TextUnformatted(phrase);

        /* Whether the value is one the rule holds at all, asked of the row
           itself every frame so the mark follows the typing rather than the
           last check. A row with a floor and no ceiling has nothing above it
           to be outside of. */
        {
            const char             *name = simRulesRuleName(rule);
            const ScnValidateIssue *iss  = NULL;
            bool                    bad  = false;

            if (simRulesRuleRange(rule, &rng)) {
                bad = m->rules[i].value < rng.lo ||
                      (rng.hasHi && m->rules[i].value > rng.hi);
                /* And the ceiling another rule carries, where one does. A
                   rule can have both that and a fixed ceiling, so it is asked
                   either way, and the partner's value is read as the table
                   stands rather than off the last check — so a row marks as
                   the author types into either of the two. */
                if (!bad && rng.cappedBy >= 0) {
                    bad = m->rules[i].value >
                          meScnRuleValueNow(m, rng.cappedBy);
                }
            }
            if (!bad) {
                /* Two rules that each sit inside their own bounds can still
                   break the pair they share, which takes the whole table to
                   see. That answer is the check's. */
                iss = meScnRuleIssue(chk, name);
            }

            if (bad) {
                simRulesRangePhrase(rule, range, sizeof(range));
                snprintf(detail, sizeof(detail), "%s %s",
                         langGetText(STR_MAPEDIT_SCENARIO_RULE_RANGE_BAD),
                         range);
                ImGui::SameLine();
                ImGui::TextColored(kValueRefused, "%s", detail);
            } else if (iss != NULL) {
                /* The rule's own bounds are not what is wrong here, so the
                   row says what the check said instead of naming them. */
                ImGui::SameLine();
                ImGui::TextColored(kValueRefused, "%s", iss->message);
            }
        }

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
    SDL_snprintf(detail, sizeof(detail), "%s %g   %s %s",
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

/* How many of the definitions found are of that name, which line the first
 * of them is on, and how that first one was written.
 *
 * One name written twice in two spellings is possible, and the spelling
 * answered is the first definition's — the same one firstLine names and Go
 * to jumps at, so the line the row draws is about the line the row points
 * at. That there is more than one is the next line's business. */
static int meScnFnDefinedAs(const MEScnFoundFn *found, size_t nFound,
                            const char *name, int *firstLine,
                            MEScnFnForm *form) {
    int    n = 0;
    size_t i;

    *firstLine = 0;
    *form      = ME_SCN_FORM_GLOBAL;
    for (i = 0; i < nFound; i++) {
        if (strcmp(found[i].name, name) != 0) {
            continue;
        }
        if (n == 0) {
            *firstLine = found[i].line;
            *form      = found[i].form;
        }
        n++;
    }
    return n;
}

/* What is wrong with a definition written that way, or NULL for one the
 * host resolves and calls with the arguments the author wrote.
 *
 * A colon puts an implicit self in front of the parameters while the host
 * calls the field with the hook's own arguments, so self swallows the first
 * of them and every argument after it shifts: the hook is there and behaves
 * wrongly. The other two are not there at all as far as the host is
 * concerned — it reads a hook off the globals and off the scenario table,
 * so a local and a field of another table are never found and never run. */
static const char *meScnFnFormNote(MEScnFnForm form) {
    switch (form) {
    case ME_SCN_FORM_COLON:
        return langGetText(STR_MAPEDIT_SCENARIO_FN_COLON);
    case ME_SCN_FORM_LOCAL:
        return langGetText(STR_MAPEDIT_SCENARIO_FN_LOCAL);
    case ME_SCN_FORM_TABLE:
        return langGetText(STR_MAPEDIT_SCENARIO_FN_TABLE);
    default:
        return NULL;
    }
}

/* Every function a scenario may define, what each is for, and which of them
 * this script has written. A row the script does not define offers a stub
 * to start from; one it does offers somewhere to go. Never both: a second
 * definition of a name is live Lua that silently replaces the first, so an
 * Add here would write a function the author would never see run.
 *
 * Neither button touches the script itself. Both leave the action for the
 * script view and switch to it — see meScnApplyPending.
 *
 * The form comes in for one question: whether the file says it is a mod,
 * which takes the round-deciding function out of what is offered. */
static void meScnFunctionsBody(MEScenarioState *st, const MEScenarioForm *f,
                               int *view) {
    /* What the author has typed into the filter. One panel, so one box, and
     * it survives a switch away and back. */
    static char s_filter[64] = "";

    MEScnFoundFn found[ME_SCN_FOUND_MAX];
    const char  *text  = (st->script != NULL) ? st->script : "";
    const size_t count = meScnFnCount();
    const bool   isMod = meScenarioFormKeepsWinCondition(f);
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
            MEScnFnForm form      = ME_SCN_FORM_GLOBAL;
            int         defined;

            if (!meScnContains(name, s_filter)) {
                continue;
            }
            defined = meScnFnDefinedAs(found, nFound, name, &firstLine,
                                       &form);

            /* A mod leaves the win condition alone, so the one function whose
               answer decides an ending is not offered to one: the row goes and
               the Add button with it, which is the only way into a script this
               view has. A line in its place says why, the way the metadata
               view's game type does, so an author looking for the function
               finds the reason rather than a gap.
               A file that has written it anyway keeps its row, the same way an
               action already naming a round-deciding op still shows in its
               combo: the Go to button is how the author reaches the thing the
               check is complaining about, and the row carries no Add. */
            if (isMod && defined == 0 && meScnFnDecidesRound(i)) {
                MessageArgs args = {};

                meScnCopy(args.string1, sizeof(args.string1), name);
                /* Where the name would have been, and wrapped at the window's
                   edge the way the descriptions under each row are: the line
                   is a sentence rather than a label. */
                ImGui::Indent(kFnButtonWidth);
                ImGui::PushTextWrapPos(0.0f);
                meScnHint(langGetTextFmt(STR_MAPEDIT_SCENARIO_FN_MOD, &args));
                ImGui::PopTextWrapPos();
                ImGui::Unindent(kFnButtonWidth);
                continue;
            }

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

            /* The ways a definition that is there can still be wrong: how
               it was written, and how many times. Drawn in the ordinary
               colour rather than the hint colour, because neither is a
               note about the function. */
            if (defined > 0) {
                const char *note = meScnFnFormNote(form);

                if (note != NULL) {
                    ImGui::TextUnformatted(note);
                }
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
 * Triggers
 * ------------------------------------------------------- */

/* How wide the hook combo opens: enough for the longest name the catalogue
 * carries with the arrow beside it, and narrow enough to leave the label and
 * the Remove button on the same line at the size the panel opens at. */
static const float kTriggerWhenWidth = 220.0f;

/* The first hook the catalogue carries, which is what Add Trigger seeds a new
 * trigger with. One is never made blank — the validator refuses a trigger that
 * names no hook, so a blank one would be born broken — and the row's combo is
 * where the author picks the one they meant. "" if the catalogue carries no
 * hook at all, which the form refuses. */
static const char *meScnFirstHook(void) {
    const size_t count = meScnFnCount();
    size_t       row;

    for (row = 0; row < count; row++) {
        if (meScnFnIsHook(row)) {
            return meScnFnName(row);
        }
    }
    return "";
}

/* Which hook one trigger runs on, picked off the catalogue.
 *
 * Hooks alone. A policy is a question the host asks and reads the answer to,
 * which a list of actions has none to give, so a trigger naming one would
 * never run and the validator refuses it.
 *
 * A name the manifest already holds that the list does not offer shows as
 * itself, the way the metadata view's game type does, so picking from the list
 * is the only thing that replaces it. */
static void meScnTriggerWhen(MEScenarioForm *f, int index, const char *when) {
    const size_t count = meScnFnCount();
    size_t       row;

    ImGui::SetNextItemWidth(kTriggerWhenWidth);
    if (!ImGui::BeginCombo(langGetText(STR_MAPEDIT_SCENARIO_TRIGGER_WHEN),
                           when)) {
        return;
    }
    for (row = 0; row < count; row++) {
        const char *name = meScnFnName(row);

        if (!meScnFnIsHook(row)) {
            continue;
        }
        ImGui::PushID((int)row);
        if (ImGui::Selectable(name, strcmp(name, when) == 0)) {
            meScenarioFormSetTriggerWhen(f, index, name);
        }
        ImGui::PopID();
    }
    ImGui::EndCombo();
}

/* How wide a test's three widgets open. A test is two lines — the field and
 * the operator on the first, the value, the box beside it and Remove on the
 * second — and these keep both of those lines inside the window at the size
 * the panel opens at. */
static const float kCondFieldWidth = 170.0f;
static const float kCondOpWidth    = 90.0f;
static const float kCondValueWidth = 190.0f;

/* The catalogue row a hook name sits on, and meScnFnCount() for a name the
 * catalogue has not got. A trigger naming one of those has no fields to offer,
 * and the tests it already holds are drawn with the field combo empty rather
 * than left out of the view. */
static size_t meScnCatalogueRow(const char *when) {
    const size_t count = meScnFnCount();
    size_t       row;

    for (row = 0; row < count; row++) {
        if (strcmp(meScnFnName(row), when) == 0) {
            return row;
        }
    }
    return count;
}

/* What the field a test names holds, off the trigger's own hook. NONE and not
 * derived for a name the hook has no field for, which is what a row written by
 * hand against the wrong hook is drawn as. */
static void meScnCondFieldType(size_t row, const char *field,
                               MEScnParamType *type, bool *derived) {
    const size_t count = meScnFnFieldCount(row);
    size_t       at;

    for (at = 0; at < count; at++) {
        char name[ME_SCN_FIELD_NAME_LEN];

        if (meScnFnFieldAt(row, at, name, sizeof(name), type, derived) &&
            strcmp(name, field) == 0) {
            return;
        }
    }
    *type    = ME_SCN_PARAM_NONE;
    *derived = false;
}

/* The two writes behind every value widget below. Writing one kind clears what
 * the other kind left behind — a number leaves no text and a string leaves no
 * number — so a row never carries half of the value it used to hold.
 *
 * inText is cleared either way. That flag says a string is on the action's own
 * text because this slot is too small for it, and only an action has a text to
 * put one on: a test's value is never inText. */
static void meScnCondSetNum(ScnTrigValue *v, ScnTrigValueKind kind,
                            double num) {
    v->kind    = kind;
    v->inText  = false;
    v->num     = num;
    v->text[0] = '\0';
}

static void meScnCondSetText(ScnTrigValue *v, ScnTrigValueKind kind,
                             const char *text) {
    v->kind   = kind;
    v->inText = false;
    v->num    = 0.0;
    meScnCopy(v->text, sizeof(v->text), text);
}

/* The kind of value a field of this type is tested against. What an empty
 * value is made as when the field under a row changes, so a number left behind
 * by the old field is not read as a name under the new one. */
static ScnTrigValueKind meScnCondKindFor(MEScnParamType type) {
    switch (type) {
        case ME_SCN_PARAM_BOOL:
            return SCN_TRIG_VAL_BOOL;
        case ME_SCN_PARAM_WORD:
        case ME_SCN_PARAM_STRING:
        case ME_SCN_PARAM_TAG:
        case ME_SCN_PARAM_REGION:
            return SCN_TRIG_VAL_STRING;
        case ME_SCN_PARAM_TARGET:
            /* Either kind is right, so an empty one is the seat: a target
               written as a number is the common case, and the chooser beside
               the widget is how the author asks for the other. */
            return SCN_TRIG_VAL_NUMBER;
        case ME_SCN_PARAM_COLOUR:
            /* The number, for the same reason and one more: an empty text
               box is a word the palette has not got and the binding would
               raise on it, while 0 is a palette entry. An author who wants
               the word picks Text and types it. */
            return SCN_TRIG_VAL_NUMBER;
        default:
            return SCN_TRIG_VAL_NUMBER;
    }
}

static void meScnCondEmptyValue(ScnTrigValue *v, MEScnParamType type) {
    memset(v, 0, sizeof(*v));
    v->kind = meScnCondKindFor(type);
}

/* A whole number, written back as a NUMBER. The widget behind the fields that
 * hold a count or an index, and the fall-back under the two pickers that can
 * find nothing to offer. */
static bool meScnCondNumber(const char *label, ScnTrigValue *v) {
    int n = (int)v->num;

    ImGui::SetNextItemWidth(kCondValueWidth);
    if (!ImGui::InputInt(label, &n, 0, 0)) {
        return false;
    }
    meScnCondSetNum(v, SCN_TRIG_VAL_NUMBER, (double)n);
    return true;
}

/* Every distinct tag the manifest carries, over all three entity kinds. That
 * is the same set scnTagCarried holds a test against, so a tag picked here is
 * one the validator accepts. Sorted, and one row per name however many
 * entities carry it. */
static bool meScnTagPicker(const MEScenarioForm *f, const char *label,
                           ScnTrigValue *v) {
    const MEScenarioTagKind kinds[3] = {ME_SCENARIO_TAG_PILL,
                                        ME_SCENARIO_TAG_BASE,
                                        ME_SCENARIO_TAG_START};
    std::map<std::string, bool> names;
    bool                        changed = false;
    int                         k;

    for (k = 0; k < (int)(sizeof(kinds) / sizeof(kinds[0])); k++) {
        const int cap = meScenarioFormEntityCap(kinds[k]);
        int       i;

        for (i = 0; i < cap; i++) {
            const ScnManifestTags *tags = meScenarioFormTags(f, kinds[k], i);
            int                    t;

            if (tags == NULL) {
                continue;
            }
            for (t = 0; t < (int)tags->count && t < SCN_TAGS_PER_ENTITY; t++) {
                names[tags->tag[t]] = true;
            }
        }
    }

    ImGui::SetNextItemWidth(kCondValueWidth);
    if (!ImGui::BeginCombo(label, v->text)) {
        return false;
    }
    if (names.empty()) {
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_NO_TAGS_YET));
    }
    for (std::map<std::string, bool>::const_iterator at = names.begin();
         at != names.end(); ++at) {
        const char *name = at->first.c_str();

        if (ImGui::Selectable(name, strcmp(name, v->text) == 0)) {
            meScnCondSetText(v, SCN_TRIG_VAL_STRING, name);
            changed = true;
        }
    }
    ImGui::EndCombo();
    return changed;
}

/* The regions the manifest names, which are not every region a round has.
 * game.define_region names one while the round runs, so a name this list does
 * not offer is not a name that is wrong: a stored one the list has no row for
 * shows as itself, the way the hook combo's does. */
static bool meScnRegionPicker(const MEScenarioForm *f, const char *label,
                              ScnTrigValue *v) {
    const ScenarioManifest *m       = &f->manifest;
    bool                    changed = false;
    int                     i;

    ImGui::SetNextItemWidth(kCondValueWidth);
    if (!ImGui::BeginCombo(label, v->text)) {
        return false;
    }
    if (m->numRegions == 0) {
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_NO_REGIONS_YET));
    }
    for (i = 0; i < (int)m->numRegions; i++) {
        const char *name = m->regions[i].name;

        ImGui::PushID(i);
        if (ImGui::Selectable(name, strcmp(name, v->text) == 0)) {
            meScnCondSetText(v, SCN_TRIG_VAL_STRING, name);
            changed = true;
        }
        ImGui::PopID();
    }
    ImGui::EndCombo();
    return changed;
}

/* The team numbers the lobby template seats. A template that seats none is not
 * a round with no teams in it — the host fills the lobby from its own settings
 * — so the fall-back is a box to type a number in rather than a list with
 * nothing in it. */
static bool meScnTeamPicker(const MEScenarioForm *f, const char *label,
                            ScnTrigValue *v) {
    const ScnManifestLobby *lobby   = &f->manifest.lobby;
    char                    shown[16];
    bool                    changed = false;
    int                     i;

    if (lobby->numTeams == 0) {
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_NO_TEAMS_YET));
        return meScnCondNumber(label, v);
    }

    SDL_snprintf(shown, sizeof(shown), "%d", (int)v->num);
    ImGui::SetNextItemWidth(kCondValueWidth);
    if (!ImGui::BeginCombo(label, shown)) {
        return false;
    }
    for (i = 0; i < (int)lobby->numTeams; i++) {
        const int id = (int)lobby->teams[i].id;
        char      idText[16];

        SDL_snprintf(idText, sizeof(idText), "%d", id);
        ImGui::PushID(i);
        if (ImGui::Selectable(idText, (int)v->num == id)) {
            meScnCondSetNum(v, SCN_TRIG_VAL_NUMBER, (double)id);
            changed = true;
        }
        ImGui::PopID();
    }
    ImGui::EndCombo();
    return changed;
}

/* The map's pills or bases, under the numbers the editor shows them by.
 *
 * A pill and a base are counted from 1 in a payload, the way a script counts
 * them, and the editor's own lists are counted from 0. So editor entity i is
 * shown as i and stored as i + 1, and a stored n reads back as editor entity
 * n - 1. That sum is written here and nowhere else in this file.
 *
 * With no map info, or a map holding none of the kind, there is nothing to
 * list and the number is typed instead. */
static bool meScnEntityPicker(const MEScenarioMapInfo *info,
                              MEScnParamType type, const char *label,
                              ScnTrigValue *v) {
    const bool     pills = (type == ME_SCN_PARAM_PILL);
    const langid   rowId = pills ? STR_MAPEDIT_SCENARIO_PILL_ROW
                                 : STR_MAPEDIT_SCENARIO_BASE_ROW;
    const uint8_t *xs    = NULL;
    const uint8_t *ys    = NULL;
    const int      cap   = pills ? MAX_PILLS : MAX_BASES;
    int            count   = 0;
    int            shown;
    char           preview[128];
    MessageArgs    args    = {};
    bool           changed = false;
    int            i;

    if (info != NULL) {
        count = pills ? info->numPills : info->numBases;
        xs    = pills ? info->pillX : info->baseX;
        ys    = pills ? info->pillY : info->baseY;
    }
    if (count > cap) {
        count = cap;
    }
    if (count <= 0) {
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_NO_ENTITIES));
        return meScnCondNumber(label, v);
    }

    shown = (int)v->num - 1;
    if (shown >= 0 && shown < count) {
        args.number  = shown;
        args.number2 = (int)xs[shown];
        args.number3 = (int)ys[shown];
        meScnCopy(preview, sizeof(preview), langGetTextFmt(rowId, &args));
    } else {
        /* A number the map has no entity for: the number itself, so the author
           can see what the row is holding. */
        SDL_snprintf(preview, sizeof(preview), "%d", (int)v->num);
    }

    ImGui::SetNextItemWidth(kCondValueWidth);
    if (!ImGui::BeginCombo(label, preview)) {
        return false;
    }
    for (i = 0; i < count; i++) {
        MessageArgs item = {};

        item.number  = i;
        item.number2 = (int)xs[i];
        item.number3 = (int)ys[i];

        ImGui::PushID(i);
        if (ImGui::Selectable(langGetTextFmt(rowId, &item), i == shown)) {
            meScnCondSetNum(v, SCN_TRIG_VAL_NUMBER, (double)(i + 1));
            changed = true;
        }
        ImGui::PopID();
    }
    ImGui::EndCombo();
    return changed;
}

/* The fields of the same hook, as a value. What a test holds when it reads its
 * right-hand side off the payload the trigger fired on rather than off a name
 * the author wrote down. */
static bool meScnCondRefCombo(size_t row, const char *label,
                              ScnTrigValue *v) {
    const size_t count   = meScnFnFieldCount(row);
    bool         changed = false;
    size_t       at;

    ImGui::SetNextItemWidth(kCondValueWidth);
    if (!ImGui::BeginCombo(label, v->text)) {
        return false;
    }
    for (at = 0; at < count; at++) {
        char name[ME_SCN_FIELD_NAME_LEN];

        if (!meScnFnFieldAt(row, at, name, sizeof(name), NULL, NULL)) {
            continue;
        }
        ImGui::PushID((int)at);
        if (ImGui::Selectable(name, strcmp(name, v->text) == 0)) {
            meScnCondSetText(v, SCN_TRIG_VAL_FIELD, name);
            changed = true;
        }
        ImGui::PopID();
    }
    ImGui::EndCombo();
    return changed;
}

/* Whether another argument of the same action is already holding its text.
 * Only one can: an action carries one long line. */
static bool meScnActTextTaken(const ScnTrigAct *act, const ScnTrigValue *v) {
    int k;

    for (k = 0; k < (int)act->numArgs && k < SCN_TRIGGER_ARGS_MAX; k++) {
        if (&act->args[k] != v && act->args[k].inText) {
            return true;
        }
    }
    return false;
}

/* A string, written where it fits.
 *
 * act is the action the value belongs to and NULL for a test's value, which
 * is the same pair of cases mjTrigValue reads a file's string under. A test
 * has nowhere to put a long one, so it is held to the slot's own length; an
 * action has its text, and a string too long for the slot goes there with
 * inText saying so.
 *
 * An action carries one such line, so the second argument to want it is held
 * to the slot instead and told why. No op the registry carries takes two
 * strings — message, say, log, score, announce and end_round have one each —
 * so that only happens under call, whose arguments the author types the kind
 * of.
 *
 * The box is seeded from wherever the string is living, so a long one keeps
 * its whole length across frames rather than being read back cut. */
static bool meScnCondString(const char *label, ScnTrigAct *act,
                            ScnTrigValue *v) {
    char       buf[SCN_TRIGGER_TEXT_LEN];
    const bool taken = (act != NULL) && meScnActTextTaken(act, v);
    const int  room  = (act == NULL || taken) ? SCN_TRIGGER_NAME_LEN
                                              : SCN_TRIGGER_TEXT_LEN;

    if (taken) {
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_TEXT_ONE_LONG));
    }

    meScnCopy(buf, (size_t)room,
              (act != NULL && v->inText) ? act->text : v->text);

    ImGui::SetNextItemWidth(kCondValueWidth);
    if (!ImGui::InputText(label, buf, (size_t)room)) {
        return false;
    }
    if (act != NULL && strlen(buf) >= SCN_TRIGGER_NAME_LEN) {
        meScnCopy(act->text, sizeof(act->text), buf);
        v->kind    = SCN_TRIG_VAL_STRING;
        v->inText  = true;
        v->num     = 0.0;
        v->text[0] = '\0';
        return true;
    }
    if (act != NULL && v->inText) {
        act->text[0] = '\0';   /* this argument was the one holding it */
    }
    meScnCondSetText(v, SCN_TRIG_VAL_STRING, buf);
    return true;
}

/* Which of the two forms an argument takes where its parameter takes either:
 * a target, which is a seat number or one of the words the surface names for
 * a wider audience, and a colour, which is the palette's word or the number
 * behind it. Both spellings are right, so the author says which, the same way
 * one of call's arguments is chosen between.
 *
 * One chooser rather than one per type, because the question and the answer
 * are the same either way: the widget below follows what the value holds, and
 * nothing here needs to know which parameter asked.
 *
 * The words themselves are not offered. Which of them an argument may name is
 * the op's own — "all" everywhere the shared target reader is used, "all" or
 * "team" on say, the palette for a colour — and the registry does not carry
 * those sets, so a word is typed the way any other word on this panel is.
 *
 * A value read off the payload is the third form, and the box the value
 * widget already draws is what says so; the chooser stands down while it is
 * ticked. */
static bool meScnNumberOrTextArg(ScnTrigValue *v) {
    bool changed = false;

    if (v->kind == SCN_TRIG_VAL_FIELD) {
        return false;
    }
    if (ImGui::RadioButton(langGetText(STR_MAPEDIT_SCENARIO_ARG_NUMBER),
                           v->kind != SCN_TRIG_VAL_STRING) &&
        v->kind != SCN_TRIG_VAL_NUMBER) {
        meScnCondEmptyValue(v, ME_SCN_PARAM_NUMBER);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(langGetText(STR_MAPEDIT_SCENARIO_ARG_TEXT),
                           v->kind == SCN_TRIG_VAL_STRING) &&
        v->kind != SCN_TRIG_VAL_STRING) {
        meScnCondEmptyValue(v, ME_SCN_PARAM_STRING);
        changed = true;
    }
    return changed;
}

/* The right-hand side of one test: the widget the field's own type calls for,
 * and the box beside it that swaps the whole side for a reference to another
 * field of the same payload. row is the catalogue row of the trigger's hook,
 * which that reference lists the fields of.
 *
 * label is what the widget is drawn under: a test says Value, and an action's
 * argument says the name the registry gives it. act is the action the value
 * belongs to and NULL for a test's value, which is what decides where a long
 * string can live.
 *
 * Answers whether the author moved anything. */
static bool meScnCondValue(const MEScenarioForm *f,
                           const MEScenarioMapInfo *info, size_t row,
                           MEScnParamType type, bool derived,
                           const char *label, ScnTrigAct *act,
                           ScnTrigValue *v) {
    bool changed = false;

    /* The widget follows the type. Whether the field answers a set of names or
       one value decides the operators beside it and not the widget: in asks
       whether a set holds a name, and that name is stated the same way. */
    (void)derived;

    if (v->kind == SCN_TRIG_VAL_FIELD) {
        changed = meScnCondRefCombo(row, label, v);
    } else {
        switch (type) {
            case ME_SCN_PARAM_BOOL: {
                bool on = (v->num != 0.0);

                if (ImGui::Checkbox(label, &on)) {
                    meScnCondSetNum(v, SCN_TRIG_VAL_BOOL, on ? 1.0 : 0.0);
                    changed = true;
                }
                break;
            }
            case ME_SCN_PARAM_TAG:
                changed = meScnTagPicker(f, label, v);
                break;
            case ME_SCN_PARAM_REGION:
                changed = meScnRegionPicker(f, label, v);
                break;
            case ME_SCN_PARAM_TEAM:
                changed = meScnTeamPicker(f, label, v);
                break;
            case ME_SCN_PARAM_PILL:
            case ME_SCN_PARAM_BASE:
                changed = meScnEntityPicker(info, type, label, v);
                break;
            case ME_SCN_PARAM_WORD:
            case ME_SCN_PARAM_STRING:
                /* A word is one of a fixed set of strings the surface names,
                   and the catalogue does not carry that set, so there is no
                   list to offer and the word is typed. */
                changed = meScnCondString(label, act, v);
                break;
            case ME_SCN_PARAM_TARGET:
            case ME_SCN_PARAM_COLOUR: {
                /* A seat or a word, a palette word or its number, and the
                   chooser above says which. The widget follows what the
                   value holds, so the box drawn is the box that writes the
                   kind the author asked for. */
                const bool picked = meScnNumberOrTextArg(v);

                changed = (v->kind == SCN_TRIG_VAL_STRING)
                              ? meScnCondString(label, act, v)
                              : meScnCondNumber(label, v);
                changed = changed || picked;
                break;
            }
            case ME_SCN_PARAM_SLOT:
            case ME_SCN_PARAM_OWNER:
            case ME_SCN_PARAM_ITEM:
            case ME_SCN_PARAM_SQUARE_X:
            case ME_SCN_PARAM_SQUARE_Y:
                changed = meScnCondNumber(label, v);
                break;
            case ME_SCN_PARAM_NUMBER: {
                double n = v->num;

                ImGui::SetNextItemWidth(kCondValueWidth);
                if (ImGui::InputDouble(label, &n, 0.0, 0.0, "%g")) {
                    meScnCondSetNum(v, SCN_TRIG_VAL_NUMBER, n);
                    changed = true;
                }
                break;
            }
            default:
                /* A field the hook has not got, and one holding a type this
                   build has no widget for: the label alone, so the row still
                   draws and its field combo and Remove still work. The value
                   is left exactly as it was found. */
                ImGui::TextUnformatted(label);
                break;
        }
    }

    /* A hook handed nothing has no field to name, so there is nothing to refer
       to and no box to tick. */
    if (meScnFnFieldCount(row) == 0) {
        return changed;
    }

    bool asRef = (v->kind == SCN_TRIG_VAL_FIELD);

    ImGui::SameLine();
    if (ImGui::Checkbox(langGetText(STR_MAPEDIT_SCENARIO_VALUE_FROM_PAYLOAD),
                        &asRef)) {
        if (asRef) {
            meScnCondSetText(v, SCN_TRIG_VAL_FIELD, "");
        } else {
            meScnCondEmptyValue(v, type);
        }
        changed = true;
    }
    return changed;
}

/* Whether a field can answer this operator, which is the pair of refusals
 * scnCheckTrigCond writes: a set of names answers in and ne and nothing else,
 * and one value answers everything but in. The combo offers what this says yes
 * to, so it cannot make a row the validator turns down. */
static bool meScnCondOpFits(ScnTrigCompare op, MEScnParamType type,
                            bool derived) {
    if (op == SCN_TRIG_CMP_UNKNOWN) {
        return false;   /* a row that named no operator; nothing answers it */
    }
    if (meScenarioFormFieldIsSet(type, derived)) {
        return op == SCN_TRIG_CMP_IN || op == SCN_TRIG_CMP_NE;
    }
    return op != SCN_TRIG_CMP_IN;
}

/* The fields of the trigger's own hook. Picking a different one writes the new
 * name, empties the value to the new field's kind and moves the operator to
 * one the new field can answer where the current one cannot — which is what
 * meScenarioFormAddCond does when it makes a row. A value left behind by the
 * old field would turn a row the validator passed into one it refuses. */
static bool meScnCondField(size_t row, ScnTrigCond *c) {
    const size_t count   = meScnFnFieldCount(row);
    bool         changed = false;
    size_t       at;

    ImGui::SetNextItemWidth(kCondFieldWidth);
    if (!ImGui::BeginCombo(langGetText(STR_MAPEDIT_SCENARIO_TEST_FIELD),
                           c->field)) {
        return false;
    }
    for (at = 0; at < count; at++) {
        char           name[ME_SCN_FIELD_NAME_LEN];
        MEScnParamType type    = ME_SCN_PARAM_NONE;
        bool           derived = false;
        bool           holding;

        if (!meScnFnFieldAt(row, at, name, sizeof(name), &type, &derived)) {
            continue;
        }
        holding = (strcmp(name, c->field) == 0);

        ImGui::PushID((int)at);
        if (ImGui::Selectable(name, holding) && !holding) {
            meScnCopy(c->field, sizeof(c->field), name);
            meScnCondEmptyValue(&c->value, type);
            if (!meScnCondOpFits(c->op, type, derived)) {
                c->op = meScenarioFormFieldIsSet(type, derived)
                            ? SCN_TRIG_CMP_IN
                            : SCN_TRIG_CMP_EQ;
            }
            changed = true;
        }
        ImGui::PopID();
    }
    ImGui::EndCombo();
    return changed;
}

/* What the test asks with, out of the operators the field can answer. */
static bool meScnCondOp(MEScnParamType type, bool derived, ScnTrigCond *c) {
    const int count   = meScenarioFormCompareCount();
    bool      changed = false;
    int       i;

    ImGui::SetNextItemWidth(kCondOpWidth);
    if (!ImGui::BeginCombo(langGetText(STR_MAPEDIT_SCENARIO_TEST_OP),
                           meScenarioFormCompareName(c->op))) {
        return false;
    }
    for (i = 0; i < count; i++) {
        const ScnTrigCompare op = meScenarioFormCompareAt(i);

        if (!meScnCondOpFits(op, type, derived)) {
            continue;
        }
        ImGui::PushID(i);
        if (ImGui::Selectable(meScenarioFormCompareName(op), op == c->op)) {
            c->op   = op;
            changed = true;
        }
        ImGui::PopID();
    }
    ImGui::EndCombo();
    return changed;
}

/* How wide the op combo opens, and the box call states a function name in.
 * Both leave the label and what follows on the same line at the size the
 * panel opens at. */
static const float kActionOpWidth = 170.0f;
static const float kCallNameWidth = 190.0f;

/* Whether an action names call, which is no row of the game table. */
static bool meScnActionIsCall(const ScnTrigAct *a) {
    return strcmp(a->op, ME_SCENARIO_CALL_OP) == 0;
}

/* The registry index an op name sits on, and meScenarioCompletionCount() for
 * a name the registry has not got — call, and anything a file wrote that the
 * surface does not carry. */
static size_t meScnOpRow(const char *op) {
    const size_t count = meScenarioCompletionCount();
    size_t       i;

    for (i = 0; i < count; i++) {
        const char *name = NULL;

        if (meScenarioCompletionAt(i, &name, NULL) && name != NULL &&
            strcmp(name, op) == 0) {
            return i;
        }
    }
    return count;
}

/* The last argument an op insists on, counted as a position rather than as a
 * number of them: an optional argument written before a required one still
 * leaves the required one counted, which is how scnCheckTrigAct reads it. 0
 * for a row the registry has not got. */
static int meScnOpRequired(size_t row) {
    const size_t count    = meScenarioOpParamCount(row);
    int          required = 0;
    size_t       p;

    for (p = 0; p < count; p++) {
        bool optional = false;

        if (meScenarioOpParamAt(row, p, NULL, NULL, &optional) && !optional) {
            required = (int)p + 1;
        }
    }
    return required;
}

/* What an argument of this op holds. NUMBER for one the registry has no
 * parameter for, which is every argument of call: nothing types those. */
static MEScnParamType meScnArgType(size_t row, int at, bool isCall) {
    MEScnParamType type = ME_SCN_PARAM_NUMBER;

    if (!isCall) {
        (void)meScenarioOpParamAt(row, (size_t)at, NULL, &type, NULL);
    }
    return type;
}

/* Another argument on the end, emptied to a value of its parameter's kind. */
static void meScnActionAddArg(ScnTrigAct *a, size_t row, bool isCall) {
    if (a->numArgs >= SCN_TRIGGER_ARGS_MAX) {
        return;
    }
    meScnCondEmptyValue(&a->args[a->numArgs],
                        meScnArgType(row, (int)a->numArgs, isCall));
    a->numArgs++;
}

/* The last argument off the end. The long line goes with it when it was the
 * argument holding it, so an action never carries text nothing refers to. */
static void meScnActionDropArg(ScnTrigAct *a) {
    if (a->numArgs == 0) {
        return;
    }
    a->numArgs--;
    if (a->args[a->numArgs].inText) {
        a->text[0] = '\0';
    }
    memset(&a->args[a->numArgs], 0, sizeof(a->args[0]));
}

/* A different op, and the arguments go with it. numArgs comes down to what
 * the new op insists on, every argument is emptied to a value of its own
 * parameter's kind, and the long line is cleared with them: an argument left
 * behind by the op before would turn a row the validator passed into one it
 * refuses. call is held to one argument, the function's name. */
static void meScnActionSetOp(ScnTrigAct *a, const char *op) {
    size_t row;
    bool   isCall;
    int    want;
    int    p;

    meScnCopy(a->op, sizeof(a->op), op);
    a->text[0] = '\0';
    memset(a->args, 0, sizeof(a->args));
    a->numArgs = 0;

    isCall = meScnActionIsCall(a);
    row    = meScnOpRow(a->op);
    want   = isCall ? 1 : meScnOpRequired(row);
    if (want > SCN_TRIGGER_ARGS_MAX) {
        want = SCN_TRIGGER_ARGS_MAX;
    }
    for (p = 0; p < want; p++) {
        meScnCondEmptyValue(&a->args[p], meScnArgType(row, p, isCall));
    }
    a->numArgs = (uint8_t)want;
}

/* What the action does: the rows an action can use, and call.
 *
 * meScenarioOpIsAction is the narrower of the registry's two flags. It leaves
 * out the ops that take a table or a function, which an action cannot state,
 * and the read accessors, which run and answer a value nobody is there to
 * read. Neither is an op to offer here.
 *
 * isMod leaves out one more set: the ops that decide the round, which a file
 * that says it is a mod may not call. Not offered rather than offered and
 * refused later, because the host will not start a round for a mod whose
 * trigger names one, and an author should not be able to pick a thing the
 * round will not take. An action that already names one stays in the row and
 * shows in the box, which is how the author sees what has to change; the
 * check says so in the issues list. */
static bool meScnActionOp(ScnTrigAct *a, bool isMod) {
    const size_t count   = meScenarioCompletionCount();
    bool         changed = false;
    bool         holding;
    size_t       i;

    ImGui::SetNextItemWidth(kActionOpWidth);
    if (!ImGui::BeginCombo(langGetText(STR_MAPEDIT_SCENARIO_ACTION_OP),
                           a->op)) {
        return false;
    }
    for (i = 0; i < count; i++) {
        const char *name = NULL;

        if (!meScenarioOpIsAction(i) ||
            (isMod && meScenarioOpDecidesRound(i)) ||
            !meScenarioCompletionAt(i, &name, NULL) || name == NULL) {
            continue;
        }
        holding = (strcmp(name, a->op) == 0);

        ImGui::PushID((int)i);
        if (ImGui::Selectable(name, holding) && !holding) {
            meScnActionSetOp(a, name);
            changed = true;
        }
        ImGui::PopID();
    }

    /* call runs a top-level function of the author's own script rather than a
       row of the game table, so the list is those rows plus this word. */
    holding = meScnActionIsCall(a);
    if (ImGui::Selectable(ME_SCENARIO_CALL_OP, holding) && !holding) {
        meScnActionSetOp(a, ME_SCENARIO_CALL_OP);
        changed = true;
    }
    ImGui::EndCombo();
    return changed;
}

/* Whether call can reach a definition written that way. The router looks a
 * call's name up with rawget on the globals table and nowhere else, so a
 * global is the only spelling it ever finds — see act() in
 * scenario_triggers.lua.
 *
 * This is narrower than what the host resolves for a hook, which is read off
 * the globals and then off the scenario table. A field of the scenario table
 * is a hook the host runs and a name call reaches nothing with, so the two
 * questions cannot share an answer. */
static bool meScnCallReaches(const MEScnFoundFn *fn) {
    return fn->form == ME_SCN_FORM_GLOBAL;
}

/* call's first argument: the name of a top-level function of the script. The
 * arrow beside the box lists what the script defines, scanned again every
 * frame the way the functions view scans it, so the list follows an edit made
 * in the script pane a moment ago.
 *
 * Only the names call can reach. A local, a field of the scenario table and a
 * field of any other table are all definitions the author can see in their
 * own script and the router will never find, so offering one is offering an
 * action that does nothing. The box beside the arrow still takes any name
 * typed into it: the list is what the editor vouches for, not what it
 * allows. */
static bool meScnCallName(const char *text, ScnTrigValue *v) {
    MEScnFoundFn found[ME_SCN_FOUND_MAX];
    size_t       nFound;
    size_t       nReach = 0;
    bool         changed = false;
    size_t       i;

    ImGui::SetNextItemWidth(kCallNameWidth);
    if (ImGui::InputText(langGetText(STR_MAPEDIT_SCENARIO_CALL_FUNCTION),
                         v->text, SCN_TRIGGER_NAME_LEN)) {
        v->kind   = SCN_TRIG_VAL_STRING;
        v->inText = false;
        v->num    = 0.0;
        changed   = true;
    }

    nFound = meScnScanFunctions(text, found, ME_SCN_FOUND_MAX);
    for (i = 0; i < nFound; i++) {
        if (meScnCallReaches(&found[i])) {
            nReach++;
        }
    }
    ImGui::SameLine();
    /* A script whose every definition is one call cannot reach has nothing to
       put in the list, and lands on the same line as a script that has
       defined nothing yet: there is no name here to pick. */
    if (nReach == 0) {
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_CALL_NO_FUNCTIONS));
        return changed;
    }
    if (ImGui::BeginCombo("##pick", "", ImGuiComboFlags_NoPreview)) {
        for (i = 0; i < nFound; i++) {
            if (!meScnCallReaches(&found[i])) {
                continue;
            }
            ImGui::PushID((int)i);
            if (ImGui::Selectable(found[i].name,
                                  strcmp(found[i].name, v->text) == 0)) {
                meScnCondSetText(v, SCN_TRIG_VAL_STRING, found[i].name);
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

/* Which of the two forms one of call's later arguments takes. Nothing types
 * them — the script's own function is what receives them, and scenario_
 * triggers.lua hands over everything past the name — so the author says.
 *
 * A value read off the payload is the third form, and the box the value
 * widget already draws is what says so; the chooser stands down while it is
 * ticked rather than offering a third answer to the same question. */
static bool meScnCallArgKind(ScnTrigValue *v) {
    bool changed = false;

    if (v->kind == SCN_TRIG_VAL_FIELD) {
        return false;
    }
    if (ImGui::RadioButton(langGetText(STR_MAPEDIT_SCENARIO_ARG_NUMBER),
                           v->kind != SCN_TRIG_VAL_STRING) &&
        v->kind != SCN_TRIG_VAL_NUMBER) {
        meScnCondEmptyValue(v, ME_SCN_PARAM_NUMBER);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(langGetText(STR_MAPEDIT_SCENARIO_ARG_TEXT),
                           v->kind == SCN_TRIG_VAL_STRING) &&
        v->kind != SCN_TRIG_VAL_STRING) {
        meScnCondEmptyValue(v, ME_SCN_PARAM_STRING);
        changed = true;
    }
    return changed;
}

/* The triggers the scenario declares: one row each, saying where it sits, what
 * it listens on and how much it carries, with the tests under it.
 *
 * The actions themselves are not drawn here. The count is, so an author can
 * see that a trigger has something in it before pressing the button that
 * throws it away. */
static void meScnTriggersBody(MEScenarioState *st, MEScenarioForm *f,
                              const MEScenarioMapInfo *info) {
    ScenarioManifest *m           = &f->manifest;
    /* What the script defines, for the function call names itself. */
    const char       *text        = (st->script != NULL) ? st->script : "";
    int               removeAt    = -1;
    /* A test and an action are dropped after the list has been drawn, so the
       row being read is never the row being changed. */
    int               condTrigger = -1;
    int               condAt      = -1;
    int               actTrigger  = -1;
    int               actAt       = -1;
    int               i;

    if (m->numTriggers == 0) {
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_NO_TRIGGERS));
    }

    for (i = 0; i < (int)m->numTriggers; i++) {
        const ScnTrigger *t       = &m->triggers[i];
        const size_t      hookRow = meScnCatalogueRow(t->when);
        const size_t      fields  = meScnFnFieldCount(hookRow);
        MessageArgs       args    = {};
        int               w;
        int               a;

        ImGui::PushID(i);
        ImGui::Separator();

        /* Where the trigger sits, spelled the way the validator keys it, so a
           problem read off the issues list under the script names the row the
           author is looking at. */
        ImGui::Text("triggers[%d]", i);

        ImGui::SameLine();
        args.number  = (int)t->numWhere;
        args.number2 = (int)t->numActions;
        meScnHint(langGetTextFmt(STR_MAPEDIT_SCENARIO_TRIGGER_ROWS, &args));

        /* The combo and Remove go under that line rather than after it: the
           four of them on one line run off the right of the window at the size
           the panel opens at. */
        ImGui::Indent();
        meScnTriggerWhen(f, i, t->when);
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_REMOVE))) {
            removeAt = i;
        }

        /* The tests: all of them have to hold for the actions to run, so a
           trigger carrying none runs every time its hook does. */
        ImGui::TextUnformatted(langGetText(STR_MAPEDIT_SCENARIO_TESTS));
        if (t->numWhere == 0) {
            meScnHint(langGetText(STR_MAPEDIT_SCENARIO_NO_TESTS));
        }

        for (w = 0; w < (int)t->numWhere; w++) {
            /* The row is read out whole, edited as a local and written back
               whole, which is the contract on the cond accessors: nothing here
               reaches into the manifest to write a part of one. */
            ScnTrigCond    cond    = t->where[w];
            MEScnParamType type    = ME_SCN_PARAM_NONE;
            bool           derived = false;
            bool           moved   = false;

            ImGui::PushID(w);
            moved |= meScnCondField(hookRow, &cond);

            /* Read after the combo, which can have just moved the row on to a
               different field: the operator and the value below are that
               field's. */
            meScnCondFieldType(hookRow, cond.field, &type, &derived);

            ImGui::SameLine();
            moved |= meScnCondOp(type, derived, &cond);

            /* The value and Remove go under the field and the operator for the
               reason the combo above goes under its own line: four widgets and
               a button on one line run off the right of the window. */
            ImGui::Indent();
            moved |= meScnCondValue(f, info, hookRow, type, derived,
                                    langGetText(STR_MAPEDIT_SCENARIO_TEST_VALUE),
                                    NULL, &cond.value);
            ImGui::SameLine();
            if (ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_REMOVE))) {
                condTrigger = i;
                condAt      = w;
            }
            ImGui::Unindent();
            ImGui::PopID();

            if (moved) {
                meScenarioFormSetCond(f, i, w, &cond);
            }
        }

        if (fields == 0) {
            /* on_setup, on_start and on_end are handed no payload, so there is
               no field to name and meScenarioFormAddCond refuses. */
            meScnHint(langGetText(STR_MAPEDIT_SCENARIO_HOOK_NO_FIELDS));
        } else if (t->numWhere >= SCN_TRIGGER_CONDS_MAX) {
            meScnHint(langGetText(STR_MAPEDIT_SCENARIO_TESTS_FULL));
        } else if (ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_ADD_TEST))) {
            meScenarioFormAddCond(f, i);
        }

        /* The actions: what the trigger does once every test above holds. */
        ImGui::TextUnformatted(langGetText(STR_MAPEDIT_SCENARIO_ACTIONS));
        if (t->numActions == 0) {
            meScnHint(langGetText(STR_MAPEDIT_SCENARIO_NO_ACTIONS));
        }

        /* An id of their own, so an action's widgets and the test's above them
           do not share one where both sit at the same index. */
        ImGui::PushID("actions");
        for (a = 0; a < (int)t->numActions; a++) {
            /* Read out whole, edited as a local and written back whole, the
               way a test is. */
            ScnTrigAct act      = t->actions[a];
            bool       moved    = false;
            bool       isCall;
            size_t     opRow;
            int        takes;
            int        required;
            int        p;

            ImGui::PushID(a);
            moved |= meScnActionOp(&act, meScenarioFormKeepsWinCondition(f));

            /* Everything the op says about itself is read after the combo,
               which can have just named a different one. */
            isCall   = meScnActionIsCall(&act);
            opRow    = meScnOpRow(act.op);
            required = isCall ? 1 : meScnOpRequired(opRow);
            takes    = isCall ? SCN_TRIGGER_ARGS_MAX
                              : (int)meScenarioOpParamCount(opRow);
            if (takes > SCN_TRIGGER_ARGS_MAX) {
                takes = SCN_TRIGGER_ARGS_MAX;
            }

            ImGui::SameLine();
            if (ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_REMOVE))) {
                actTrigger = i;
                actAt      = a;
            }

            ImGui::Indent();
            /* What the op does, so an author reads it without leaving the
               view. The registry's documents run to a couple of hundred
               characters, so the line is wrapped at the window's edge rather
               than left to run past it. call has no registry row to carry
               one. */
            ImGui::PushTextWrapPos(0.0f);
            if (isCall) {
                meScnHint(langGetText(STR_MAPEDIT_SCENARIO_CALL_RUNS_SCRIPT));
            } else {
                const char *doc = NULL;

                if (meScenarioCompletionAt(opRow, NULL, &doc) && doc != NULL) {
                    meScnHint(doc);
                }
            }
            ImGui::PopTextWrapPos();

            /* The arguments the action states. They are positional, so these
               are the first numArgs of what the op takes. */
            for (p = 0; p < (int)act.numArgs && p < takes; p++) {
                const char    *argName = NULL;
                MEScnParamType type    = ME_SCN_PARAM_NUMBER;

                ImGui::PushID(p);
                if (isCall && p == 0) {
                    /* The function's name. scnCheckTrigAct holds call to
                       stating one and says nothing else about it: what the
                       arguments below mean is the script's own business. */
                    moved |= meScnCallName(text, &act.args[0]);
                } else if (isCall) {
                    moved |= meScnCallArgKind(&act.args[p]);
                    type = (act.args[p].kind == SCN_TRIG_VAL_STRING)
                               ? ME_SCN_PARAM_STRING
                               : ME_SCN_PARAM_NUMBER;
                    moved |= meScnCondValue(f, info, hookRow, type, false,
                                            "##arg", &act, &act.args[p]);
                } else if (meScenarioOpParamAt(opRow, (size_t)p, &argName,
                                               &type, NULL)) {
                    moved |= meScnCondValue(f, info, hookRow, type, false,
                                            argName, &act, &act.args[p]);
                }
                ImGui::PopID();
            }

            /* One more while the op takes one, and one less while the op does
               not insist on the last of them. */
            if ((int)act.numArgs < takes) {
                if (ImGui::Button(
                        langGetText(STR_MAPEDIT_SCENARIO_ADD_ARG))) {
                    meScnActionAddArg(&act, opRow, isCall);
                    moved = true;
                }
                if ((int)act.numArgs > required) {
                    ImGui::SameLine();
                }
            }
            if ((int)act.numArgs > required) {
                if (ImGui::Button(
                        langGetText(STR_MAPEDIT_SCENARIO_DROP_ARG))) {
                    meScnActionDropArg(&act);
                    moved = true;
                }
            }
            ImGui::Unindent();
            ImGui::PopID();

            if (moved) {
                meScenarioFormSetAction(f, i, a, &act);
            }
        }
        ImGui::PopID();

        if (t->numActions >= SCN_TRIGGER_ACTIONS_MAX) {
            meScnHint(langGetText(STR_MAPEDIT_SCENARIO_ACTIONS_FULL));
        } else if (ImGui::Button(
                       langGetText(STR_MAPEDIT_SCENARIO_ADD_ACTION))) {
            meScenarioFormAddAction(f, i);
        }

        ImGui::Unindent();
        ImGui::PopID();
    }

    if (actAt >= 0) {
        meScenarioFormRemoveAction(f, actTrigger, actAt);
    }
    if (condAt >= 0) {
        meScenarioFormRemoveCond(f, condTrigger, condAt);
    }
    if (removeAt >= 0) {
        meScenarioFormRemoveTrigger(f, removeAt);
    }

    ImGui::Separator();
    if (m->numTriggers >= SCN_TRIGGERS_MAX) {
        meScnHint(langGetText(STR_MAPEDIT_SCENARIO_TRIGGERS_FULL));
        return;
    }
    if (ImGui::Button(langGetText(STR_MAPEDIT_SCENARIO_ADD_TRIGGER))) {
        meScenarioFormAddTrigger(f, meScnFirstHook());
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
    ImGui::SameLine();
    meScnViewButton(view, ME_SCENARIO_VIEW_TRIGGERS,
                    STR_MAPEDIT_SCENARIO_VIEW_TRIGGERS);
    ImGui::Separator();

    /* A change to the table since the check ran leaves the issues describing
       a table that is gone, the way an edit to the text leaves them naming
       lines that have moved. Every form's change counts up form->edits, so
       one comparison here covers the rules, the tags and the triggers alike:
       the count is taken when a run is first seen, and a count that has moved
       on since is an edit the check did not see. */
    {
        static unsigned s_runSeen    = 0;
        static unsigned s_editsAtRun = 0;

        if (check->runs != s_runSeen) {
            s_runSeen    = check->runs;
            s_editsAtRun = form->edits;
        } else if (check->hasRun && form->edits != s_editsAtRun) {
            check->stale = true;
        }
    }

    switch (*view) {
        case ME_SCENARIO_VIEW_METADATA:
            meScnMetadataBody(form, st, mapPath, wantPack, wantSaveMod);
            break;
        case ME_SCENARIO_VIEW_LOBBY:
            meScnLobbyBody(form);
            break;
        case ME_SCENARIO_VIEW_RULES:
            meScnRulesBody(form, check);
            break;
        case ME_SCENARIO_VIEW_TAGS:
            meScnTagsBody(form, mapInfo, selKind, selIndex, clickedKind,
                          clickedIndex, panX, panY);
            break;
        case ME_SCENARIO_VIEW_FUNCTIONS:
            meScnFunctionsBody(st, form, view);
            break;
        case ME_SCENARIO_VIEW_TRIGGERS:
            meScnTriggersBody(st, form, mapInfo);
            break;
        default:
            meScnScriptBody(st, check, mapPath, wantSave, wantReload,
                            wantValidate);
            break;
    }

    ImGui::End();
}
