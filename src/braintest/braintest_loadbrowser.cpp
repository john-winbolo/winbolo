/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * BrainTest "Load Session" browser — ImGui rendering.
 * See braintest_loadbrowser.h. Drawn inside the main window's
 * ImGui frame (mainImGuiBeginFrame / EndFrame).
 *********************************************************/
#include <SDL3/SDL.h>
#include "imgui.h"

#include "braintest_loadbrowser.h"

/* The path-tail compare (loadBrowserSameSession) and the segment lookup
 * (loadBrowserFindSegment) live in braintest_loadbrowser_segment.c; the name
 * grammar and the rename planner (loadBrowserSplitName,
 * loadBrowserRenameFamily, loadBrowserApplyRenames) in
 * braintest_loadbrowser_rename.c — both plain C, so tests/unit can link them
 * and every decision below is testable without ImGui. */

/* ---- Rename editor state ----------------------------------------------
 * One row at a time owns the editor. Keyed by row index (the popup ID lives
 * under the row's PushID) AND by name, so a listing that changed under us
 * closes the editor instead of renaming the wrong session. */
static int  s_renameRow = -1;
static char s_renameName[LOADBROWSER_MAX_NAME] = "";
static char s_renameLabel[LOADBROWSER_LABEL_MAX + 1] = "";
static char s_renameOrig[LOADBROWSER_LABEL_MAX + 1] = "";  /* label on open */
static bool s_renameFocus = false;
static char s_renamePlan[LOADBROWSER_PLAN_MAX][2][300];

/* Outcome of the LAST apply, shown as a banner in the window itself rather
 * than in the editor: the editor closes on apply (and the rescan behind it
 * renames the very rows it was anchored to), so a message living in the popup
 * would vanish before it could be read. The plan's own refusals are different
 * — those are recomputed every frame and shown inline in the editor. */
static char s_renameBanner[400] = "";
static bool s_renameBannerBad  = false;

static void renameEditorClose() {
    s_renameRow = -1;
    s_renameName[0] = '\0';
}

/* The per-row "Rename..." editor, drawn as a popup anchored under the button.
 * Returns true when renames were applied and the caller must rescan. */
static bool renameEditor(const LoadSessionEntry *list, int count,
                         const LoadSessionEntry *e, const char *loadedDir) {
    bool applied = false;
    if (!ImGui::BeginPopup("renameeditor")) return false;

    /* The list was rebuilt under the open editor — bail rather than act on a
     * row that may now be a different session. */
    if (!loadBrowserNameEq(s_renameName, e->name)) {
        renameEditorClose();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return false;
    }

    ImGui::Text("Rename %s", e->name);
    ImGui::TextDisabled("The label is shared by every 15-minute block and split");
    ImGui::TextDisabled("part of this game, so they all move together.");
    ImGui::Separator();

    ImGui::SetNextItemWidth(300.0f);
    if (s_renameFocus) { ImGui::SetKeyboardFocusHere(); s_renameFocus = false; }
    const bool enter = ImGui::InputText("Label", s_renameLabel, sizeof s_renameLabel,
                                        ImGuiInputTextFlags_EnterReturnsTrue |
                                        ImGuiInputTextFlags_CharsNoBlank);
    ImGui::SameLine();
    ImGui::TextDisabled("(A-Z a-z 0-9 _ -; empty drops it)");

    /* Recomputed every frame: what the user types IS the preview, and the
     * refusal (bad character, name taken, loaded session) is the same check
     * OK would make. */
    char err[400] = "";
    const int n = loadBrowserRenameFamily(list, count, e->dir, s_renameLabel,
                                          loadedDir, s_renamePlan,
                                          LOADBROWSER_PLAN_MAX, err, sizeof err);
    if (n > 0) {
        ImGui::Text("%d dir%s:", n, n == 1 ? "" : "s");
        if (ImGui::BeginChild("##renameplan", ImVec2(560.0f, 110.0f),
                              ImGuiChildFlags_Borders)) {
            for (int i = 0; i < n; i++) {
                char oldb[LOADBROWSER_MAX_NAME], newb[LOADBROWSER_MAX_NAME];
                loadBrowserBaseName(s_renamePlan[i][0], oldb, sizeof oldb);
                loadBrowserBaseName(s_renamePlan[i][1], newb, sizeof newb);
                ImGui::Text("%s  ->  %s", oldb, newb);
            }
        }
        ImGui::EndChild();
    } else if (SDL_strcmp(s_renameLabel, s_renameOrig) == 0) {
        /* Nothing typed yet — the planner's "already labelled" is the truth,
         * but on a freshly opened editor it is a prompt, not an error. */
        ImGui::TextDisabled("Type a new label.");
    } else {
        ImGui::PushTextWrapPos(560.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", err);
        ImGui::PopTextWrapPos();
    }

    ImGui::Separator();
    if (n <= 0) ImGui::BeginDisabled();
    const bool ok = ImGui::Button("OK") || enter;
    if (n <= 0) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        renameEditorClose();
        ImGui::CloseCurrentPopup();
    } else if (ok && n > 0) {
        char aerr[400] = "";
        /* Explicit cast: C forbids the implicit char(*)[2][300] ->
         * const char(*)[2][300] conversion at this nesting depth. */
        const int done = loadBrowserApplyRenames((const char (*)[2][300])s_renamePlan,
                                                 n, aerr, sizeof aerr);
        if (done >= 0) {
            SDL_snprintf(s_renameBanner, sizeof s_renameBanner,
                         "Renamed %d dir%s to \"%s\".", done, done == 1 ? "" : "s",
                         s_renameLabel[0] ? s_renameLabel : "(no label)");
            s_renameBannerBad = false;
        } else {
            /* No rollback — the message says how many renames stand. */
            SDL_strlcpy(s_renameBanner, aerr, sizeof s_renameBanner);
            s_renameBannerBad = true;
        }
        /* Rescan either way: on a partial failure the dirs that DID move must
         * show their new names, or the next plan would be built from stale
         * rows. */
        applied = true;
        renameEditorClose();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return applied;
}

/* One segment-step button. `dir` is +1 (next) / -1 (prev); when there is no
 * such segment the button is drawn disabled with the reason in its label, so
 * "am I on the last part?" is answered without reading dir names.
 * Returns the list index to load, or -1. */
static int segmentButton(const LoadSessionEntry *list, int count,
                         const char *loadedDir, int dir) {
    int idx = loadBrowserFindSegment(list, count, loadedDir, dir);
    char label[160];
    if (idx >= 0) {
        if (dir > 0) SDL_snprintf(label, sizeof label, "Next segment: %s  >>", list[idx].name);
        else         SDL_snprintf(label, sizeof label, "<<  Prev segment: %s", list[idx].name);
    } else {
        if (dir > 0) SDL_strlcpy(label, "Next segment (last segment)  >>", sizeof label);
        else         SDL_strlcpy(label, "<<  Prev segment (first segment)", sizeof label);
    }

    if (idx < 0) ImGui::BeginDisabled();
    bool hit = ImGui::Button(label);
    if (idx < 0) ImGui::EndDisabled();
    return (hit && idx >= 0) ? idx : -1;
}

/* ---- Filter ------------------------------------------------------------
 * A substring typed into the box above the table hides every row that does
 * not contain it (case-insensitive) in its session name, map name or
 * not-loadable note. Purely a view: row indices handed back to the caller are
 * still indices into the full `list`, the loaded-row highlight and the [ / ]
 * segment walk are untouched, and clearing the box shows everything again.
 * Kept across window open/close so a narrowed list stays narrowed. */
static char s_filter[64] = "";

static bool filterMatches(const LoadSessionEntry *e, const char *filter) {
    if (!filter || !filter[0]) return true;
    const char *fields[3] = { e->name, e->map, e->note };
    for (int f = 0; f < 3; f++) {
        const char *hay = fields[f];
        if (!hay || !hay[0]) continue;
        size_t n = SDL_strlen(filter);
        for (const char *h = hay; *h; h++) {
            size_t k = 0;
            while (k < n && h[k] &&
                   SDL_tolower((unsigned char)h[k]) ==
                   SDL_tolower((unsigned char)filter[k])) k++;
            if (k == n) return true;
        }
    }
    return false;
}

int loadBrowserRender(bool *open, const LoadSessionEntry *list, int count,
                      const char *loadedDir, bool *needsRescan) {
    if (!open || !*open) return -1;
    int chosen = -1;
    bool renamed = false;

    ImGui::SetNextWindowSize(ImVec2(660, 440), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Load Session", open)) {
        ImGui::TextUnformatted(
            "Replay a winbolods recording. Loading ABANDONS the current game "
            "and relaunches BrainTest on the recorded map.");
        ImGui::Separator();

        /* Segment strip: walking a long game is [ and ] (or these two
         * buttons), never a hunt through the table below for the dir whose
         * name differs by one digit. */
        if (loadedDir && loadedDir[0] && count > 0) {
            char base[LOADBROWSER_MAX_NAME];
            loadBrowserBaseName(loadedDir, base, sizeof base);
            ImGui::Text("Loaded: %s", base);
            int sel = segmentButton(list, count, loadedDir, -1);
            if (sel >= 0) chosen = sel;
            ImGui::SameLine();
            sel = segmentButton(list, count, loadedDir, +1);
            if (sel >= 0) chosen = sel;
            ImGui::SameLine();
            ImGui::TextDisabled("([ / ])");
            ImGui::Separator();
        }

        /* Result of the last rename, until dismissed. */
        if (s_renameBanner[0]) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(s_renameBannerBad ? ImVec4(1.0f, 0.55f, 0.45f, 1.0f)
                                                 : ImVec4(0.55f, 1.0f, 0.6f, 1.0f),
                               "%s", s_renameBanner);
            ImGui::PopTextWrapPos();
            if (ImGui::SmallButton("Dismiss")) s_renameBanner[0] = '\0';
            ImGui::Separator();
        }

        /* Filter box. Typing narrows the table on every keystroke; the "x"
         * (or emptying the box) shows the whole list again. */
        int shown = 0;
        if (count > 0) {
            ImGui::SetNextItemWidth(260.0f);
            ImGui::InputTextWithHint("##sessfilter", "filter: name, map or note",
                                     s_filter, sizeof s_filter);
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) s_filter[0] = '\0';
            for (int i = 0; i < count; i++) {
                if (filterMatches(&list[i], s_filter)) shown++;
            }
            ImGui::SameLine();
            if (s_filter[0]) ImGui::TextDisabled("%d of %d", shown, count);
            else             ImGui::TextDisabled("%d session(s)", count);
        }

        if (count <= 0) {
            ImGui::TextDisabled("No debug_sessions/ recordings found.");
        } else if (shown <= 0) {
            ImGui::TextDisabled("No session matches \"%s\".", s_filter);
        } else if (ImGui::BeginTable("sessions", 6,
                       ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Session",  ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Map",      ImGuiTableColumnFlags_WidthStretch, 1.5f);
            ImGui::TableSetupColumn("Players",  ImGuiTableColumnFlags_WidthFixed,   60.0f);
            ImGui::TableSetupColumn("Length",   ImGuiTableColumnFlags_WidthFixed,   64.0f);
            ImGui::TableSetupColumn("Size",     ImGuiTableColumnFlags_WidthFixed,   72.0f);
            ImGui::TableSetupColumn("",         ImGuiTableColumnFlags_WidthFixed,  140.0f);
            ImGui::TableHeadersRow();

            const bool appearing = ImGui::IsWindowAppearing();
            for (int i = 0; i < count; i++) {
                const LoadSessionEntry *e = &list[i];
                if (!filterMatches(e, s_filter)) continue;   /* hidden by the filter */
                /* Highlight the CURRENTLY LOADED session's row (and scroll it
                 * into view when the window opens): "which part am I on?" is
                 * one glance, and the next part is the neighbouring row. */
                const bool isLoaded = loadBrowserSameSession(e->dir, loadedDir);
                ImGui::TableNextRow();
                if (isLoaded) {
                    ImU32 hl = ImGui::GetColorU32(ImVec4(0.18f, 0.42f, 0.22f, 0.65f));
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, hl);
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, hl);
                }

                ImGui::TableNextColumn();
                if (isLoaded && appearing) ImGui::SetScrollHereY(0.4f);
                /* Invisible full-row selectable purely for the HOVER style:
                 * the whole row lights up under the mouse (ImGuiCol_Header*),
                 * which makes scanning a long list much easier. AllowOverlap
                 * keeps the Load button (drawn later, on top) clickable. */
                if (e->loadable) {
                    ImGui::PushID(i);
                    ImGui::Selectable("##rowhover", false,
                        ImGuiSelectableFlags_SpanAllColumns |
                        ImGuiSelectableFlags_AllowOverlap);
                    ImGui::PopID();
                    ImGui::SameLine(0.0f, 0.0f);
                }
                if (isLoaded) {
                    ImGui::TextColored(ImVec4(0.55f, 1.0f, 0.6f, 1.0f), "%s", e->name);
                    ImGui::SameLine(); ImGui::TextDisabled("(loaded)");
                } else if (e->loadable) ImGui::TextUnformatted(e->name);
                else             ImGui::TextDisabled("%s", e->name);

                if (!e->loadable) {
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", e->note[0] ? e->note : "not loadable");
                    for (int c = 0; c < 3; c++) { ImGui::TableNextColumn(); ImGui::TextDisabled("-"); }
                    ImGui::TableNextColumn(); /* no Load button */
                    continue;
                }

                ImGui::TableNextColumn(); ImGui::TextUnformatted(e->map[0] ? e->map : "?");
                ImGui::TableNextColumn(); ImGui::Text("%d", e->bots);
                ImGui::TableNextColumn();
                if (e->durationSec >= 0) ImGui::Text("%d:%02d", e->durationSec / 60, e->durationSec % 60);
                else                     ImGui::TextUnformatted("?");
                ImGui::TableNextColumn(); ImGui::Text("%.0f MB", e->sizeMB);

                ImGui::TableNextColumn();
                ImGui::PushID(i);
                if (ImGui::Button("Load")) chosen = i;
                ImGui::SameLine();
                /* The dir name carries the label the game was STARTED with
                 * (WINBOLO_BRAINDBG_LABEL); this is how it gets corrected
                 * afterwards, for the whole game at once. */
                if (ImGui::Button("Rename...")) {
                    char ts[16], label[LOADBROWSER_MAX_NAME];
                    int blk = 0, px = 0, pn = 0;
                    s_renameRow = i;
                    SDL_strlcpy(s_renameName, e->name, sizeof s_renameName);
                    s_renameLabel[0] = '\0';
                    if (loadBrowserSplitName(e->name, ts, sizeof ts, &blk,
                                             label, sizeof label, &px, &pn))
                        SDL_strlcpy(s_renameLabel, label, sizeof s_renameLabel);
                    SDL_strlcpy(s_renameOrig, s_renameLabel, sizeof s_renameOrig);
                    s_renameBanner[0] = '\0';
                    s_renameFocus = true;
                    ImGui::OpenPopup("renameeditor");
                }
                if (s_renameRow == i && renameEditor(list, count, e, loadedDir))
                    renamed = true;
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::Separator();
        if (ImGui::Button("Cancel")) *open = false;
        ImGui::SameLine();
        ImGui::TextDisabled("(press O to toggle this window)");
    }
    ImGui::End();
    if (needsRescan && renamed) *needsRescan = true;
    /* A frame that renamed dirs never also loads one: the indices the caller
     * would use are about to be rebuilt. */
    return renamed ? -1 : chosen;
}
