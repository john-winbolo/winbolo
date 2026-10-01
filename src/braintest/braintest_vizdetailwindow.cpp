/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * braintest_vizdetailwindow.cpp — ImGui inspector for the
 * C-side viz-detail registry.
 *
 *   D dialog (toggled by the D key)         — index list,
 *                                              one row per entry,
 *                                              click an entry to
 *                                              open its detail
 *                                              window.
 *   Detail windows (one per opened entry)   — read-only body in
 *                                              an InputTextMultiline
 *                                              so the user can copy
 *                                              the contents. Close
 *                                              independently. Stay
 *                                              open if the index
 *                                              dialog is closed.
 *
 * Bidirectional sync with the map: clicking on the map opens the
 * matching entry's detail window; selecting a row in the index
 * sets the highlight target which the renderer outlines on the map.
 *********************************************************/

#include "imgui.h"
#include <cstdio>
#include <cstring>
#include <cfloat>

extern "C" {
#include "braintest_vizdetailwindow.h"
#include "braintest_vizdetail_registry.h"
}

static bool sVisible      = false;     /* index dialog visibility */
static char sSelectedID[VIZDETAIL_ID_MAX] = {0};
static bool sScrollPending = false;
/* Hover (mouse over a row in the index dialog). Cleared every
 * render frame at the top of renderIndexDialog and set when a
 * row's IsItemHovered fires. Renderer uses this for a secondary
 * cyan highlight that lets the user preview without clicking. */
static char sHoveredID[VIZDETAIL_ID_MAX] = {0};

/* Up to N detail windows can sit open simultaneously. Each holds
 * the id of the entry it shows + a bool ImGui can flip via the [X]
 * close button. We never reuse a slot until its window is closed —
 * keeps things simple at the cost of a small fixed cap. */
#define VIZDETAIL_MAX_OPEN_WINDOWS 16
struct OpenDetailWindow {
    char id[VIZDETAIL_ID_MAX];
    bool open;
};
static OpenDetailWindow sOpenDetails[VIZDETAIL_MAX_OPEN_WINDOWS];
static int              sOpenDetailCount = 0;

/* Single-window mode (default): clicking a new row replaces the
 * one open detail window instead of opening another. The window
 * uses a fixed ImGui id so position/size are preserved across
 * id swaps. Toggle off via the index-dialog checkbox to get the
 * old per-id-window behavior. */
static bool sSingleWindowMode = true;

static void requestOpenDetail(const char *id) {
    if (!id || !id[0]) return;
    if (sSingleWindowMode) {
        /* Reuse slot 0; close any other slots that linger from a
         * previous multi-window session. */
        for (int i = 1; i < sOpenDetailCount; i++) {
            sOpenDetails[i].open = false;
        }
        snprintf(sOpenDetails[0].id, VIZDETAIL_ID_MAX, "%s", id);
        sOpenDetails[0].open = true;
        if (sOpenDetailCount < 1) sOpenDetailCount = 1;
        return;
    }
    for (int i = 0; i < sOpenDetailCount; i++) {
        if (strncmp(sOpenDetails[i].id, id, VIZDETAIL_ID_MAX) == 0) {
            sOpenDetails[i].open = true;
            return;
        }
    }
    if (sOpenDetailCount >= VIZDETAIL_MAX_OPEN_WINDOWS) {
        /* Evict the oldest if full — newest open wins. */
        for (int i = 1; i < sOpenDetailCount; i++) {
            sOpenDetails[i - 1] = sOpenDetails[i];
        }
        sOpenDetailCount--;
    }
    snprintf(sOpenDetails[sOpenDetailCount].id, VIZDETAIL_ID_MAX, "%s", id);
    sOpenDetails[sOpenDetailCount].open = true;
    sOpenDetailCount++;
}

static void compactClosedDetails(void) {
    int dst = 0;
    for (int src = 0; src < sOpenDetailCount; src++) {
        if (sOpenDetails[src].open) {
            if (dst != src) sOpenDetails[dst] = sOpenDetails[src];
            dst++;
        }
    }
    sOpenDetailCount = dst;
}

void vizDetailWindowToggle(void) { sVisible = !sVisible; }
bool vizDetailWindowIsVisible(void) { return sVisible; }

void vizDetailWindowSelectAndScroll(const char *id) {
    if (!id) return;
    snprintf(sSelectedID, sizeof(sSelectedID), "%s", id);
    sScrollPending = true;
    sVisible = true;
    requestOpenDetail(id);
}

const char *vizDetailWindowGetSelected(void) { return sSelectedID; }
const char *vizDetailWindowGetHovered(void)  { return sHoveredID; }

static const char *kKindNames[] = { "rect", "circle", "text" };

static void renderIndexDialog(int follow_bot) {
    /* Reset hover regardless of whether the dialog is visible —
     * if the user closed it, the previous hover should NOT persist. */
    sHoveredID[0] = '\0';
    if (!sVisible) return;
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(380, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Viz details (D)", &open)) {
        ImGui::End();
        if (!open) sVisible = false;
        return;
    }

    int n = vizDetailCount();
    int shown = 0;
    for (int i = 0; i < n; i++) {
        const VizDetailEntry *e = vizDetailGet(i);
        if (e && e->bot_owner == follow_bot) shown++;
    }
    ImGui::Text("%d entries this tick", shown);
    ImGui::SameLine();
    ImGui::Checkbox("single window", &sSingleWindowMode);
    if (sSelectedID[0]) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f),
                           "  selected: %s", sSelectedID);
        ImGui::SameLine();
        if (ImGui::SmallButton("clear##sel")) sSelectedID[0] = '\0';
    }
    ImGui::Separator();

    if (shown == 0) {
        ImGui::TextDisabled("(brain hasn't registered any this tick)");
    }

    for (int i = 0; i < n; i++) {
        const VizDetailEntry *e = vizDetailGet(i);
        if (!e) continue;
        if (e->bot_owner != follow_bot) continue;
        const bool is_selected = (sSelectedID[0]
            && strncmp(sSelectedID, e->id, VIZDETAIL_ID_MAX) == 0);

        if (is_selected && sScrollPending) {
            ImGui::SetScrollHereY(0.25f);
            sScrollPending = false;
        }

        const char *kind_str = (e->kind >= 0 && (int)e->kind < 3)
            ? kKindNames[e->kind] : "?";
        char row[256];
        if (e->label[0]) {
            snprintf(row, sizeof(row), "[%s] %s — %s",
                     kind_str, e->id, e->label);
        } else {
            snprintf(row, sizeof(row), "[%s] %s", kind_str, e->id);
        }
        ImGui::PushID(i);
        if (is_selected) ImGui::PushStyleColor(ImGuiCol_Header,
            ImVec4(0.20f, 0.55f, 0.25f, 1.0f));
        if (ImGui::Selectable(row, is_selected, ImGuiSelectableFlags_AllowDoubleClick)) {
            snprintf(sSelectedID, sizeof(sSelectedID), "%s", e->id);
            requestOpenDetail(e->id);
        }
        if (ImGui::IsItemHovered()) {
            /* Drives the cyan secondary highlight on the map. Last
             * row hovered this frame wins (which is fine, only one
             * row is hovered at a time). */
            snprintf(sHoveredID, sizeof(sHoveredID), "%s", e->id);
        }
        if (is_selected) ImGui::PopStyleColor();
        ImGui::PopID();
    }

    ImGui::End();
    if (!open) sVisible = false;
}

static void renderDetailWindow(int slot, int follow_bot) {
    OpenDetailWindow *w = &sOpenDetails[slot];
    if (!w->open) return;
    int idx = vizDetailFindByID(w->id, follow_bot);
    const VizDetailEntry *e = (idx >= 0) ? vizDetailGet(idx) : NULL;

    char title[VIZDETAIL_ID_MAX + 32];
    /* In single-window mode use a fixed ImGui id so position/size
     * persist as the user clicks between rows. In multi mode the id
     * is per-entry so each window remembers its own placement. */
    if (sSingleWindowMode) {
        snprintf(title, sizeof(title), "Detail: %s###vd_w_single", w->id);
    } else {
        snprintf(title, sizeof(title), "Detail: %s###vd_w_%s", w->id, w->id);
    }
    ImGui::SetNextWindowSize(ImVec2(460, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &w->open)) {
        ImGui::End();
        return;
    }
    /* Focused detail window drives the primary (red) highlight on
     * the map. Lets the user click between detail windows to switch
     * which entry is highlighted without going back to the index. */
    if (ImGui::IsWindowFocused()) {
        snprintf(sSelectedID, sizeof(sSelectedID), "%s", w->id);
    }

    if (!e) {
        /* The id isn't in the current registry view (brain stopped
         * emitting it this tick / playback frame doesn't have it).
         * Keep the window open so the user can scrub back to a
         * frame that has it without losing their place. */
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                           "Entry not present in current frame.");
        ImGui::TextDisabled("id: %s", w->id);
        if (ImGui::Button("Close")) w->open = false;
        ImGui::SameLine();
        if (ImGui::Button("Highlight")) {
            snprintf(sSelectedID, sizeof(sSelectedID), "%s", w->id);
        }
        ImGui::End();
        return;
    }

    /* Header summary */
    const char *kind_str = (e->kind >= 0 && (int)e->kind < 3)
        ? kKindNames[e->kind] : "?";
    ImGui::TextColored(ImVec4(0.7f, 0.95f, 1.0f, 1.0f), "id:    %s", e->id);
    ImGui::Text("kind:  %s", kind_str);
    if (e->label[0]) ImGui::Text("label: %s", e->label);

    /* Geometry summary */
    char geom[128];
    switch (e->kind) {
    case VIZDETAIL_KIND_RECT:
        snprintf(geom, sizeof(geom), "rect (%.3f,%.3f)-(%.3f,%.3f)",
                 e->x1, e->y1, e->x2, e->y2);
        break;
    case VIZDETAIL_KIND_CIRCLE:
        snprintf(geom, sizeof(geom), "circle center (%.3f,%.3f) r=%.3f",
                 e->x1, e->y1, e->x2);
        break;
    case VIZDETAIL_KIND_TEXT:
        snprintf(geom, sizeof(geom), "text anchor (%.3f,%.3f)", e->x1, e->y1);
        break;
    default:
        snprintf(geom, sizeof(geom), "?");
        break;
    }
    ImGui::TextDisabled("geom:  %s", geom);

    /* Highlight + close buttons */
    if (ImGui::Button("Highlight on map")) {
        snprintf(sSelectedID, sizeof(sSelectedID), "%s", e->id);
    }
    ImGui::SameLine();
    if (ImGui::Button("Close##det")) w->open = false;
    ImGui::Separator();

    /* Body — InputTextMultiline so it's selectable/copyable. */
    if (e->body_line_count > 0) {
        char buf[VIZDETAIL_BODY_LINES_MAX * VIZDETAIL_BODY_LINE_MAX];
        int  off = 0;
        for (int li = 0; li < e->body_line_count; li++) {
            int rem = (int)sizeof(buf) - off - 2;
            if (rem <= 0) break;
            int wlen = snprintf(buf + off, rem, "%s\n", e->body[li]);
            if (wlen < 0) break;
            off += wlen;
        }
        if (off > 0 && buf[off - 1] == '\n') buf[off - 1] = '\0';
        ImGui::InputTextMultiline("##body", buf, sizeof(buf),
            ImVec2(-FLT_MIN, -FLT_MIN),
            ImGuiInputTextFlags_ReadOnly);
    } else {
        ImGui::TextDisabled("(no body lines)");
    }

    ImGui::End();
}

void vizDetailWindowRender(int follow_bot) {
    /* Index dialog */
    renderIndexDialog(follow_bot);
    /* Per-id detail windows. Render even when the index dialog is
     * closed — they're independent, so the user can keep a few
     * entries pinned open while toggling D off. */
    for (int i = 0; i < sOpenDetailCount; i++) renderDetailWindow(i, follow_bot);
    compactClosedDetails();
}
