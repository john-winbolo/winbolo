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

/* Stable hue per section name → consistent color across renders. */
static ImU32 colorForName(const char *name) {
    unsigned hash = 5381;
    for (const char *p = name; p && *p; p++) hash = hash * 33u + (unsigned char)*p;
    float h = (float)(hash % 360);
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(h / 360.0f, 0.55f, 0.85f, r, g, b);
    return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255);
}

/* Lighten or darken a color by `delta` (positive lightens). */
static ImU32 shadeColor(ImU32 c, int delta) {
    int r = (c      ) & 0xFF;
    int g = (c >>  8) & 0xFF;
    int b = (c >> 16) & 0xFF;
    int a = (c >> 24) & 0xFF;
    auto clamp = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    return IM_COL32(clamp(r + delta), clamp(g + delta), clamp(b + delta), a);
}

/* Vertical stacked time bar. Main sections (world / threat / goals /
 * steer / etc.) become colored blocks sized proportional to their ms.
 * Sub-sections render INSIDE their parent block: indented from the left
 * with a colored stub bar + their own label, so the parent/child
 * relationship is visually obvious without a tooltip.
 *
 * Bar fills the available column height; bar scale honours target_ms so
 * under-budget ticks leave the bottom dark and over-budget ticks grow
 * the scale so a spike never clips. */
static void renderTimeBar(cJSON *sections, float bar_w, float bar_h,
                          float total_budget_ms) {
    if (!sections || !cJSON_IsArray(sections)
        || cJSON_GetArraySize(sections) == 0) {
        ImGui::TextDisabled("(no section data —");
        ImGui::TextDisabled("pass --perf-log)");
        ImGui::Dummy(ImVec2(bar_w, bar_h));
        return;
    }
    int n = cJSON_GetArraySize(sections);
    double total_ms = 0.0;
    for (int i = 0; i < n; i++) {
        cJSON *s = cJSON_GetArrayItem(sections, i);
        if (s) total_ms += getNum(s, "ms", 0.0);
    }
    /* Auto-fit: blocks always fill the bar to the bottom. The target_ms
     * is shown as a horizontal marker line so you can still see budget
     * compliance — anything above the line means total_ms exceeds target. */
    double scale_ms = total_ms;
    if (scale_ms < 0.001) scale_ms = 1.0;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();

    /* Bar background frame. Most of it gets painted over by section
     * blocks now (auto-fit), but the frame itself remains as a border. */
    dl->AddRectFilled(origin,
                      ImVec2(origin.x + bar_w, origin.y + bar_h),
                      IM_COL32(40, 40, 50, 255), 4.0f);
    dl->AddRect(origin,
                ImVec2(origin.x + bar_w, origin.y + bar_h),
                IM_COL32(120, 120, 140, 255), 4.0f);

    const float kPadY     = 1.5f;
    const float kBlockX   = 4.0f;
    /* Sub blocks are dramatically indented + start AFTER a tree-style
     * "├─" marker drawn per sub. The visible distance plus the connector
     * makes the parent/child relationship read at a glance even on a
     * narrow bar. */
    const float kSubIndent = 32.0f;
    const float kSubBarW   = 5.0f;

    float yCursor = origin.y + 1.0f;
    for (int i = 0; i < n; i++) {
        cJSON *s = cJSON_GetArrayItem(sections, i);
        if (!s) continue;
        const char *name = "?";
        cJSON *jn = cJSON_GetObjectItem(s, "name");
        if (jn && cJSON_IsString(jn)) name = jn->valuestring;
        double ms = getNum(s, "ms", 0.0);
        if (ms <= 0.001) continue;

        float h = (float)(ms / scale_ms) * (bar_h - 2.0f);
        if (h < 2.0f) h = 2.0f;
        if (yCursor + h > origin.y + bar_h - 1.0f) {
            h = (origin.y + bar_h - 1.0f) - yCursor;
            if (h < 1.0f) break;
        }
        ImU32 col = colorForName(name);
        ImVec2 p0(origin.x + kBlockX,         yCursor);
        ImVec2 p1(origin.x + bar_w - kBlockX, yCursor + h);
        dl->AddRectFilled(p0, p1, col, 2.0f);
        /* Outline so sub-blocks inside don't blend into the parent. */
        dl->AddRect(p0, p1, shadeColor(col, -50), 2.0f);

        /* Header strip across the top of the parent block, painted
         * darker so it visually separates the "label area" from the
         * "child sub-block area" below. The strip's width spans the
         * full parent block. */
        const float kHeaderStripH = 18.0f;
        if (h >= kHeaderStripH + 4.0f) {
            ImVec2 hp0(p0.x, p0.y);
            ImVec2 hp1(p1.x, p0.y + kHeaderStripH);
            dl->AddRectFilled(hp0, hp1, shadeColor(col, -30), 2.0f);
        }

        /* Parent label in the header strip. */
        char lbl[64];
        SDL_snprintf(lbl, sizeof(lbl), "%s %.2fms", name, ms);
        ImVec2 ts = ImGui::CalcTextSize(lbl);
        if (h >= ts.y + 2) {
            dl->AddText(ImVec2(p0.x + 4, p0.y + 2),
                        IM_COL32(20, 20, 30, 255), lbl);
        } else if (h >= 8) {
            dl->AddText(ImVec2(p0.x + 4, p0.y),
                        IM_COL32(20, 20, 30, 255), name);
        }

        /* Sub-sections rendered INSIDE the parent block. Each sub's
         * height is proportional to (sub.ms / parent.ms) * parent_h.
         * They start below the parent label strip (~16px) and stack
         * downward. Visual cue: indented from parent's left edge, with
         * a darker color stub + a thin "tab" running up to the parent
         * label so the hierarchy reads at a glance. */
        cJSON *subs = cJSON_GetObjectItem(s, "subs");
        int sn = (subs && cJSON_IsArray(subs)) ? cJSON_GetArraySize(subs) : 0;
        if (sn > 0 && h > 22.0f) {
            float subAreaY0 = p0.y + kHeaderStripH;
            float subAreaY1 = p1.y - 1.0f;
            float subAreaH  = subAreaY1 - subAreaY0;
            if (subAreaH > 4.0f) {
                /* Distribute the parent block's available sub-area
                 * among subs proportional to ms. If subs sum to less
                 * than parent.ms, the leftover stays in parent color
                 * underneath (un-attributed parent work). */
                double sub_scale = ms;
                if (sub_scale < 0.001) sub_scale = 1.0;
                float subYCursor = subAreaY0;

                /* Vertical "trunk" line running down the parent's left
                 * edge, threading through each sub. ImGui doesn't have a
                 * tree-control style for raw draws, so we synthesize it:
                 * a vertical line at fixed x, plus a horizontal stub
                 * branching out at each sub's mid-y. Visual cue: classic
                 * "├─" tree marker. */
                const float kTrunkX = p0.x + 8.0f;
                ImU32 trunk_col = shadeColor(col, -70);
                dl->AddLine(ImVec2(kTrunkX, subAreaY0),
                            ImVec2(kTrunkX, subAreaY1),
                            trunk_col, 1.5f);

                for (int si = 0; si < sn; si++) {
                    cJSON *sb = cJSON_GetArrayItem(subs, si);
                    if (!sb) continue;
                    const char *snm = "?";
                    cJSON *sjn = cJSON_GetObjectItem(sb, "name");
                    if (sjn && cJSON_IsString(sjn)) snm = sjn->valuestring;
                    double sms = getNum(sb, "ms", 0.0);
                    if (sms <= 0.001) continue;
                    float sh = (float)(sms / sub_scale) * subAreaH;
                    if (sh < 1.5f) sh = 1.5f;
                    if (subYCursor + sh > subAreaY1) {
                        sh = subAreaY1 - subYCursor;
                        if (sh < 1.0f) break;
                    }
                    ImU32 sub_col = shadeColor(colorForName(snm), -25);
                    /* Horizontal connector branching from trunk to the
                     * sub block — drawn at sub mid-y. */
                    float midY = subYCursor + sh * 0.5f;
                    dl->AddLine(ImVec2(kTrunkX,             midY),
                                ImVec2(p0.x + kSubIndent - 2, midY),
                                trunk_col, 1.5f);
                    /* Sub block, deeply indented. */
                    ImVec2 bp0(p0.x + kSubIndent, subYCursor);
                    ImVec2 bp1(p1.x - 4,           subYCursor + sh);
                    dl->AddRectFilled(bp0, bp1, sub_col, 2.0f);
                    dl->AddRect(bp0, bp1, shadeColor(sub_col, -40), 2.0f);
                    /* Sub label. */
                    char slbl[64];
                    SDL_snprintf(slbl, sizeof(slbl), "%s %.2fms", snm, sms);
                    ImVec2 sts = ImGui::CalcTextSize(slbl);
                    if (sh >= sts.y + 1) {
                        dl->AddText(
                            ImVec2(bp0.x + 4,
                                   bp0.y + (sh - sts.y) * 0.5f),
                            IM_COL32(15, 15, 25, 255), slbl);
                    } else if (sh >= 7) {
                        dl->AddText(
                            ImVec2(bp0.x + 4, bp0.y),
                            IM_COL32(15, 15, 25, 255), snm);
                    }
                    subYCursor += sh + 2.0f;
                }
            }
        }

        /* Hover tooltip on the parent block: full breakdown for cases
         * where the sub-blocks are too small to read in-line. */
        if (ImGui::IsMouseHoveringRect(p0, p1)) {
            ImGui::BeginTooltip();
            ImGui::Text("%s: %.3f ms", name, ms);
            if (sn > 0) {
                ImGui::Separator();
                for (int si = 0; si < sn; si++) {
                    cJSON *sb = cJSON_GetArrayItem(subs, si);
                    if (!sb) continue;
                    const char *snm = "?";
                    cJSON *sjn = cJSON_GetObjectItem(sb, "name");
                    if (sjn && cJSON_IsString(sjn)) snm = sjn->valuestring;
                    ImGui::TextDisabled("  %s: %.3f ms", snm,
                                        getNum(sb, "ms", 0.0));
                }
            }
            ImGui::EndTooltip();
        }

        yCursor += h + kPadY;
    }

    /* Budget line: a dashed horizontal marker at (target_ms / total_ms)
     * of the bar height. Below the line = within budget; anything above
     * the line means we're over. Skip when target is unset / total is
     * tiny. */
    if (total_budget_ms > 0.05f && total_ms > 0.001) {
        float frac = (float)(total_budget_ms / total_ms);
        if (frac < 1.0f) {
            float ly = origin.y + (bar_h * frac);
            /* Dashed line via short segments. */
            const float dash = 6.0f, gap = 4.0f;
            float xstart = origin.x + 2.0f, xend = origin.x + bar_w - 2.0f;
            ImU32 line_col = IM_COL32(255, 230, 80, 220);
            for (float xx = xstart; xx < xend; xx += dash + gap) {
                float xx2 = xx + dash;
                if (xx2 > xend) xx2 = xend;
                dl->AddLine(ImVec2(xx, ly), ImVec2(xx2, ly), line_col, 1.5f);
            }
            /* Label "target" floating just above the line. */
            char tlbl[32];
            SDL_snprintf(tlbl, sizeof(tlbl), "target %.2fms", total_budget_ms);
            ImVec2 tts = ImGui::CalcTextSize(tlbl);
            float tlx = origin.x + bar_w - tts.x - 6.0f;
            float tly = ly - tts.y - 1.0f;
            if (tly < origin.y + 2) tly = ly + 2.0f;
            dl->AddText(ImVec2(tlx, tly), line_col, tlbl);
        }
    }

    ImGui::Dummy(ImVec2(bar_w, bar_h));
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

    /* ── Two columns: left = stacked time bar, right = tier controls. ── */
    const float kBarColW = 200.0f;
    ImGui::Columns(2, "tier_ctrl_cols", true);
    ImGui::SetColumnWidth(0, kBarColW);

    /* Left column: stacked time bar. ContentRegionAvail.y in a column
     * is sized to the column's prior content, not the window — so we
     * compute the bar height directly from the window's remaining space
     * (window content max - current cursor y). The right column has
     * lots of widgets, but we want the LEFT bar to fill the same total
     * window height regardless. */
    {
        cJSON *secs = cJSON_GetObjectItem(root, "sections");
        double total_ms = 0.0;
        if (secs && cJSON_IsArray(secs)) {
            int n = cJSON_GetArraySize(secs);
            for (int i = 0; i < n; i++) {
                cJSON *s = cJSON_GetArrayItem(secs, i);
                if (s) total_ms += getNum(s, "ms", 0.0);
            }
        }
        ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1),
                           "Tick: %.2f / %.2f ms", total_ms, targetMs);
        /* Window-relative height: window inner height minus current
         * y-cursor inside the window minus a small bottom margin. This
         * pulls the bar all the way to the bottom of the panel. */
        float winH       = ImGui::GetWindowContentRegionMax().y;
        float curY       = ImGui::GetCursorPosY();
        float bar_h      = winH - curY - 6.0f;
        if (bar_h < 50.0f) bar_h = 50.0f;
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float bar_w  = avail.x - 6.0f;
        renderTimeBar(secs, bar_w, bar_h, (float)targetMs);
    }
    ImGui::NextColumn();

    /* Right column: existing tier controls. ── */
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

    ImGui::Columns(1);  /* end the left/right split */
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
