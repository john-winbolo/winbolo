/*********************************************************
 * tier_control.cpp — capacity tier panel for BrainTest.
 *
 * Renders the followed bot's current capacity tier (1..10),
 * the lever values that hash to it, and a ↑/↓ control that
 * pushes _G._BT_TIER_OVERRIDE back into the bot's Lua state
 * via botManagerExecLua so the user can lock the tier for
 * testing. "Auto" clears the override and lets the dynamic
 * algorithm resume.
 *
 * Body schema (from brain.get_capacity_state_json()):
 *   {
 *     "tier":      <int 1..10>,
 *     "override":  <int 1..10> | null,
 *     "last_ms":   <float>,
 *     "target_ms": <float>,
 *     "ratio_ewma":<float>,
 *     "levels": [
 *       { "tier":1, "dij_short":..., "dij_long":..., "scan_step":...,
 *         "pp_spread":..., "sb_spread":..., "ttl_mult":...,
 *         "eval_iv":..., "wsim": <int|null|false>, "place_r":...,
 *         "tank_step":..., "ms": <float|null> },
 *       ... (10 entries)
 *     ]
 *   }
 *********************************************************/

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "cJSON.h"
#include "../../../src/braintest/braintest_panel_types.h"
#include "../../../src/braintest/braintest_panel_registry.h"

extern "C" {
#include "../../../src/bolo/bot_manager.h"
}

namespace {

double getNum(const cJSON *o, const char *k, double d) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return (v && cJSON_IsNumber(v)) ? v->valuedouble : d;
}

/* Push a tier override (or clear it) to the bot that owns this panel.
 * Uses botManagerExecLua, the same mechanism BrainTest uses to mirror
 * V-dialog state back into bot Lua. */
void pushTierOverride(int registry_idx, int tier_or_zero) {
    if (registry_idx < 0) return;
    const PanelRegistryEntry *e = panelRegistryGet(registry_idx);
    if (!e) return;
    int bot = e->bot_owner;
    if (bot < 0 || !botManagerIsBot((BYTE)bot)) return;

    char src[64];
    if (tier_or_zero >= 1 && tier_or_zero <= 10) {
        SDL_snprintf(src, sizeof(src), "_G._BT_TIER_OVERRIDE=%d", tier_or_zero);
    } else {
        SDL_snprintf(src, sizeof(src), "_G._BT_TIER_OVERRIDE=nil");
    }
    botManagerExecLua((BYTE)bot, src);
}

void renderTierControl(int registry_idx, const char *body) {
    if (!body || !*body) {
        ImGui::TextDisabled("(no data — first tick not yet rendered)");
        return;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1),
                           "JSON parse failed");
        return;
    }

    int    tier      = (int)getNum(root, "tier", 10);
    bool   overrideActive = false;
    int    overrideTier   = tier;
    {
        cJSON *o = cJSON_GetObjectItem(root, "override");
        if (o && cJSON_IsNumber(o)) {
            overrideActive = true;
            overrideTier   = (int)o->valuedouble;
        }
    }
    double lastMs    = getNum(root, "last_ms",    0.0);
    double targetMs  = getNum(root, "target_ms",  0.0);
    double ratioEwma = getNum(root, "ratio_ewma", 0.0);

    /* ── Header: current tier + ratio ──────────────────────────────── */
    {
        ImVec4 col(0.85f, 0.95f, 0.85f, 1.0f);
        if      (tier <= 3) col = ImVec4(1.00f, 0.40f, 0.40f, 1);
        else if (tier <= 5) col = ImVec4(1.00f, 0.65f, 0.30f, 1);
        else if (tier <= 7) col = ImVec4(0.95f, 0.90f, 0.40f, 1);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::Text("Capacity tier: %d / 10", tier);
        ImGui::PopStyleColor();
    }
    ImGui::Text("ratio %.2f   last %.2f ms / target %.2f ms",
                ratioEwma, lastMs, targetMs);
    if (overrideActive) {
        ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.0f, 1.0f),
                           "(manual override active: tier %d)", overrideTier);
    } else {
        ImGui::TextDisabled("(dynamic mode — no override)");
    }

    /* ── ↑ / ↓ / Auto controls ─────────────────────────────────────── */
    ImGui::Separator();
    int controlTier = overrideActive ? overrideTier : tier;

    ImGui::PushButtonRepeat(true);
    if (ImGui::ArrowButton("##tier_up", ImGuiDir_Up)) {
        int next = controlTier + 1;
        if (next > 10) next = 10;
        pushTierOverride(registry_idx, next);
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##tier_dn", ImGuiDir_Down)) {
        int next = controlTier - 1;
        if (next < 1) next = 1;
        pushTierOverride(registry_idx, next);
    }
    ImGui::PopButtonRepeat();
    ImGui::SameLine();
    ImGui::Text("set tier");
    ImGui::SameLine(0.0f, 24.0f);
    if (ImGui::Button("Auto (clear override)")) {
        pushTierOverride(registry_idx, 0);
    }

    /* ── Active tier's lever values ────────────────────────────────── */
    ImGui::Separator();
    ImGui::Text("Active levers (tier %d):", tier);
    cJSON *levels = cJSON_GetObjectItem(root, "levels");
    cJSON *active = nullptr;
    if (levels && cJSON_IsArray(levels)) {
        int n = cJSON_GetArraySize(levels);
        for (int i = 0; i < n; i++) {
            cJSON *L = cJSON_GetArrayItem(levels, i);
            if (L && (int)getNum(L, "tier", -1) == tier) { active = L; break; }
        }
    }
    if (active && ImGui::BeginTable("active_levers", 2,
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_RowBg)) {
        auto row = [&](const char *k, const char *fmt, double v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k);
            ImGui::TableNextColumn(); ImGui::Text(fmt, v);
        };
        auto rowStr = [&](const char *k, const char *s) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k);
            ImGui::TableNextColumn(); ImGui::Text("%s", s);
        };
        row("dij_short",  "%.0f", getNum(active, "dij_short", 0));
        row("dij_long",   "%.0f", getNum(active, "dij_long",  0));
        row("scan_step",  "%.0f°", getNum(active, "scan_step", 0));
        row("pp_spread",  "%.0f", getNum(active, "pp_spread", 1));
        row("sb_spread",  "%.0f", getNum(active, "sb_spread", 1));
        row("ttl_mult",   "%.2fx", getNum(active, "ttl_mult", 1));
        row("eval_iv",    "every %.0f tick(s)", getNum(active, "eval_iv", 1));
        {
            cJSON *w = cJSON_GetObjectItem(active, "wsim");
            if (!w || cJSON_IsNull(w))      rowStr("wsim", "all (no cap)");
            else if (cJSON_IsBool(w) && !cJSON_IsTrue(w)) rowStr("wsim", "false (skip)");
            else if (cJSON_IsNumber(w)) {
                char b[32]; SDL_snprintf(b, sizeof(b), "top %d", (int)w->valuedouble);
                rowStr("wsim", b);
            } else rowStr("wsim", "?");
        }
        row("place_r",    "%.0f", getNum(active, "place_r",  0));
        row("tank_step",  "%.0f°", getNum(active, "tank_step", 0));
        ImGui::EndTable();
    }

    /* ── Per-tier ms history ───────────────────────────────────────── */
    ImGui::Separator();
    ImGui::Text("Per-tier ms history (corroboration data):");
    if (levels && cJSON_IsArray(levels)
        && ImGui::BeginTable("tier_ms", 11,
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("");
        for (int t = 10; t >= 1; t--) {
            char h[8]; SDL_snprintf(h, sizeof(h), "%d", t);
            ImGui::TableSetupColumn(h);
        }
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("ms");
        for (int t = 10; t >= 1; t--) {
            ImGui::TableNextColumn();
            cJSON *L = nullptr;
            int n = cJSON_GetArraySize(levels);
            for (int i = 0; i < n; i++) {
                cJSON *e = cJSON_GetArrayItem(levels, i);
                if (e && (int)getNum(e, "tier", -1) == t) { L = e; break; }
            }
            if (L) {
                cJSON *m = cJSON_GetObjectItem(L, "ms");
                if (m && cJSON_IsNumber(m)) {
                    if (t == tier) {
                        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1),
                                           "*%.2f", m->valuedouble);
                    } else {
                        ImGui::Text("%.2f", m->valuedouble);
                    }
                } else {
                    if (t == tier) ImGui::TextColored(
                        ImVec4(0.4f, 1.0f, 0.4f, 1), "*--");
                    else           ImGui::TextDisabled("--");
                }
            }
        }
        ImGui::EndTable();
    }

    /* ── Legend ────────────────────────────────────────────────────── */
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Lever legend")) {
        ImGui::TextWrapped(
            "dij_short: SHORT-slate Dijkstra nodes/tick (default 500). "
            "Powers steering nav + close spot lookups.");
        ImGui::TextWrapped(
            "dij_long: LONG-slate Dijkstra nodes/tick (default ~560). "
            "Powers map-wide candidate scoring.");
        ImGui::TextWrapped(
            "scan_step: eval_pill_difficulty step in degrees (5/10/20/45). "
            "5° = 72 angles (full); 45° = 8 angles (precomputed stamps).");
        ImGui::TextWrapped(
            "pp_spread: plan_position scan spread (currently coarsens to "
            "45° when > 1, peak ~9ms → ~1ms).");
        ImGui::TextWrapped(
            "sb_spread: shield-blocker scan spread (currently a no-op; "
            "kept in table for future use).");
        ImGui::TextWrapped(
            "ttl_mult: multiplier on pool-6 diff_cache distance-tier TTLs "
            "(50/150/500 base).");
        ImGui::TextWrapped(
            "eval_iv: ticks between step_eval_queue pops (1 = every tick).");
        ImGui::TextWrapped(
            "wsim: top-N of the merged pool[] gets death-prediction "
            "simulated. nil = all, integer = top N, false = skip.");
        ImGui::TextWrapped(
            "place_r: STRATEGIC_PLACE_SEARCH_RADIUS for pool-8 heatmap. "
            "(2R+1)² tiles get scored.");
        ImGui::TextWrapped(
            "tank_step: tank_combat_steer CLOSE-substate standoff scan "
            "step in degrees (default 5).");
    }

    cJSON_Delete(root);
}

/* Static initializer: registers our renderer when this translation unit
 * is loaded. The brain side registers the panel itself with type
 * "tier_control" via braintest_panel_register; both halves must agree. */
struct TierControlAutoRegister {
    TierControlAutoRegister() {
        /* Type name "tier_ctrl" matches the brain's
         * braintest_panel_register("Capacity tiers", "tier_ctrl", …). The
         * registry namespaces it to "NewAutopilot:tier_ctrl" before our
         * renderer is dispatched. PANEL_REG_TYPE_MAX is 24, so the type
         * suffix has to fit in (24 - len("NewAutopilot:") - 1) = 10 chars
         * to avoid silent truncation. */
        panelTypeRegister("NewAutopilot:tier_ctrl", renderTierControl);
        panelTypeRegister("tier_ctrl",              renderTierControl);
    }
};
static TierControlAutoRegister s_tierControlAutoRegister;

} // namespace
