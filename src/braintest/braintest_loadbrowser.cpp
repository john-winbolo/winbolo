/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
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
 * (loadBrowserFindSegment) live in braintest_loadbrowser_segment.c — plain C,
 * so tests/unit can link them. */

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

int loadBrowserRender(bool *open, const LoadSessionEntry *list, int count,
                      const char *loadedDir) {
    if (!open || !*open) return -1;
    int chosen = -1;

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

        if (count <= 0) {
            ImGui::TextDisabled("No debug_sessions/ recordings found.");
        } else if (ImGui::BeginTable("sessions", 6,
                       ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Session",  ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Map",      ImGuiTableColumnFlags_WidthStretch, 1.5f);
            ImGui::TableSetupColumn("Players",  ImGuiTableColumnFlags_WidthFixed,   60.0f);
            ImGui::TableSetupColumn("Length",   ImGuiTableColumnFlags_WidthFixed,   64.0f);
            ImGui::TableSetupColumn("Size",     ImGuiTableColumnFlags_WidthFixed,   72.0f);
            ImGui::TableSetupColumn("",         ImGuiTableColumnFlags_WidthFixed,   60.0f);
            ImGui::TableHeadersRow();

            const bool appearing = ImGui::IsWindowAppearing();
            for (int i = 0; i < count; i++) {
                const LoadSessionEntry *e = &list[i];
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
    return chosen;
}
