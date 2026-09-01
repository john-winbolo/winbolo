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

/* Path-tail compare: the browser stores "debug_sessions/<name>" while
 * -loadsession may carry any prefix or separator style. Two dirs are the
 * same session iff their basenames match. */
static bool sameSessionDir(const char *a, const char *b) {
    if (!a || !b || !a[0] || !b[0]) return false;
    const char *ba = a, *bb = b;
    for (const char *c = a; *c; c++) if (*c == '/' || *c == '\\') ba = c + 1;
    for (const char *c = b; *c; c++) if (*c == '/' || *c == '\\') bb = c + 1;
    return SDL_strcmp(ba, bb) == 0;
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
                const bool isLoaded = sameSessionDir(e->dir, loadedDir);
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
