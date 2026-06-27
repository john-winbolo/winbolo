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

int loadBrowserRender(bool *open, const LoadSessionEntry *list, int count) {
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

            for (int i = 0; i < count; i++) {
                const LoadSessionEntry *e = &list[i];
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                if (e->loadable) ImGui::TextUnformatted(e->name);
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
