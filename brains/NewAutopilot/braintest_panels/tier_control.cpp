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
    /* Running tick offset (ms since think start) so the hover tooltip
     * can show each section as "start - end (duration)" rather than
     * a bare ms number. Sections are emitted in execution order and
     * represent contiguous wall-clock spans, so summing prior ms gives
     * the start offset of the current section within the tick. */
    double running_ms = 0.0;
    /* Capture mouse position once per frame so per-row hit-testing in
     * the tooltip can highlight whichever sub/subsub block the cursor
     * is actually over (vs. just the parent). The renderer loop below
     * sets these indices when it finds a block whose rect contains the
     * mouse; the tooltip then colors the matching row orange. */
    const ImVec2 mouse_pos = ImGui::GetIO().MousePos;
    for (int i = 0; i < n; i++) {
        cJSON *s = cJSON_GetArrayItem(sections, i);
        if (!s) continue;
        const char *name = "?";
        cJSON *jn = cJSON_GetObjectItem(s, "name");
        if (jn && cJSON_IsString(jn)) name = jn->valuestring;
        double ms = getNum(s, "ms", 0.0);
        if (ms <= 0.001) continue;
        double parent_start_ms = running_ms;
        running_ms += ms;
        /* Per-main hover tracking. -1 means "no row hit at this level".
         * Set by the sub/subsub loops below when their rect covers the
         * mouse; the tooltip uses these to highlight the matching row. */
        int hovered_sub_idx     = -1;  // index into this main's subs[]
        int hovered_ss_sub_idx  = -1;  // sub index whose subsubs were hit
        int hovered_ss_idx      = -1;  // index into that sub's subs[]

        float h = (float)(ms / scale_ms) * (bar_h - 2.0f);
        if (h < 2.0f) h = 2.0f;
        if (yCursor + h > origin.y + bar_h - 1.0f) {
            h = (origin.y + bar_h - 1.0f) - yCursor;
            if (h < 1.0f) break;
        }
        ImU32 col = colorForName(name);
        ImVec2 p0(origin.x + kBlockX,         yCursor);
        ImVec2 p1(origin.x + bar_w - kBlockX, yCursor + h);
        /* Double-click any block to put a one-line summary on the
         * clipboard, so the user can paste timing into a chat. The
         * deepest level wins because the per-rect checks below run in
         * order: subsub overwrites sub overwrites main. */
        bool main_hit = mouse_pos.x >= p0.x && mouse_pos.x <= p1.x &&
                        mouse_pos.y >= p0.y && mouse_pos.y <= p1.y;
        if (main_hit && ImGui::IsMouseDoubleClicked(0)) {
            char buf[256];
            SDL_snprintf(buf, sizeof(buf),
                         "%s: %.3f - %.3f (%.3f) ms",
                         name, parent_start_ms, parent_start_ms + ms, ms);
            ImGui::SetClipboardText(buf);
        }
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

                /* Track the running offset within the tick so the
                 * double-click clipboard summary can include start/end
                 * timestamps for the row, matching the tooltip format. */
                double sub_start_offset = parent_start_ms;
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
                    bool sub_hit = mouse_pos.x >= bp0.x && mouse_pos.x <= bp1.x &&
                                   mouse_pos.y >= bp0.y && mouse_pos.y <= bp1.y;
                    if (sub_hit) {
                        hovered_sub_idx = si;
                        if (ImGui::IsMouseDoubleClicked(0)) {
                            char buf[256];
                            SDL_snprintf(buf, sizeof(buf),
                                         "%s: %.3f - %.3f (%.3f) ms",
                                         snm, sub_start_offset,
                                         sub_start_offset + sms, sms);
                            ImGui::SetClipboardText(buf);
                        }
                    }
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

                    /* Third level: subsubs nested inside this sub block.
                     * Same pattern as the main->sub loop above, just
                     * indented further with `kSubIndent` added once
                     * more. Only attempted when the sub block is tall
                     * enough to host children at all. */
                    cJSON *ssubs = cJSON_GetObjectItem(sb, "subs");
                    int ssn = (ssubs && cJSON_IsArray(ssubs))
                                ? cJSON_GetArraySize(ssubs) : 0;
                    if (ssn > 0 && sh > 14.0f) {
                        const float kSubSubIndent = 18.0f;
                        float ssAreaY0 = bp0.y + 1.0f;
                        float ssAreaY1 = bp1.y - 1.0f;
                        float ssAreaH  = ssAreaY1 - ssAreaY0;
                        if (ssAreaH > 3.0f) {
                            double ss_scale = sms;
                            if (ss_scale < 0.001) ss_scale = 1.0;
                            float ssYCursor = ssAreaY0;
                            /* Subsub running offset for clipboard summary. */
                            double ssub_start_offset = sub_start_offset;
                            const float kSSTrunkX = bp0.x + 6.0f;
                            ImU32 ss_trunk_col = shadeColor(sub_col, -70);
                            dl->AddLine(ImVec2(kSSTrunkX, ssAreaY0),
                                        ImVec2(kSSTrunkX, ssAreaY1),
                                        ss_trunk_col, 1.0f);
                            for (int ssi = 0; ssi < ssn; ssi++) {
                                cJSON *ssb = cJSON_GetArrayItem(ssubs, ssi);
                                if (!ssb) continue;
                                const char *ssnm = "?";
                                cJSON *ssjn = cJSON_GetObjectItem(ssb, "name");
                                if (ssjn && cJSON_IsString(ssjn)) ssnm = ssjn->valuestring;
                                double ssms = getNum(ssb, "ms", 0.0);
                                if (ssms <= 0.001) continue;
                                float ssh = (float)(ssms / ss_scale) * ssAreaH;
                                if (ssh < 1.0f) ssh = 1.0f;
                                if (ssYCursor + ssh > ssAreaY1) {
                                    ssh = ssAreaY1 - ssYCursor;
                                    if (ssh < 0.5f) break;
                                }
                                ImU32 ss_col = shadeColor(colorForName(ssnm), -45);
                                float ssMidY = ssYCursor + ssh * 0.5f;
                                dl->AddLine(ImVec2(kSSTrunkX, ssMidY),
                                            ImVec2(bp0.x + kSubSubIndent - 1, ssMidY),
                                            ss_trunk_col, 1.0f);
                                ImVec2 ssp0(bp0.x + kSubSubIndent, ssYCursor);
                                ImVec2 ssp1(bp1.x - 2,             ssYCursor + ssh);
                                bool ss_hit = mouse_pos.x >= ssp0.x && mouse_pos.x <= ssp1.x &&
                                              mouse_pos.y >= ssp0.y && mouse_pos.y <= ssp1.y;
                                if (ss_hit) {
                                    hovered_ss_sub_idx = si;
                                    hovered_ss_idx     = ssi;
                                    if (ImGui::IsMouseDoubleClicked(0)) {
                                        char buf[256];
                                        /* Subsub names from the brain are
                                         * bare ("misc", "search (...)") —
                                         * prepend the parent sub's name so
                                         * the clipboard line carries the
                                         * full hierarchy ("steer/nav-
                                         * dispatch/path/misc"). */
                                        SDL_snprintf(buf, sizeof(buf),
                                                     "%s/%s: %.3f - %.3f (%.3f) ms",
                                                     snm, ssnm,
                                                     ssub_start_offset,
                                                     ssub_start_offset + ssms, ssms);
                                        ImGui::SetClipboardText(buf);
                                    }
                                }
                                dl->AddRectFilled(ssp0, ssp1, ss_col, 1.0f);
                                dl->AddRect(ssp0, ssp1, shadeColor(ss_col, -40), 1.0f);
                                char sslbl[64];
                                SDL_snprintf(sslbl, sizeof(sslbl), "%s %.2fms", ssnm, ssms);
                                ImVec2 ssts = ImGui::CalcTextSize(sslbl);
                                if (ssh >= ssts.y + 1) {
                                    dl->AddText(
                                        ImVec2(ssp0.x + 3,
                                               ssp0.y + (ssh - ssts.y) * 0.5f),
                                        IM_COL32(20, 20, 30, 255), sslbl);
                                } else if (ssh >= 7) {
                                    dl->AddText(
                                        ImVec2(ssp0.x + 3, ssp0.y),
                                        IM_COL32(20, 20, 30, 255), ssnm);
                                }
                                ssub_start_offset += ssms;
                                ssYCursor += ssh + 1.0f;
                            }
                        }
                    }

                    sub_start_offset += sms;
                    subYCursor += sh + 2.0f;
                }
            }
        }

        /* Hover tooltip on the parent block: full breakdown for cases
         * where the sub-blocks are too small to read in-line. */
        if (ImGui::IsMouseHoveringRect(p0, p1)) {
            ImGui::BeginTooltip();
            /* Layout: "<start> ms  <indented_name>  (<duration> ms)".
             * Left column is the absolute start offset within the tick,
             * so rows naturally read top-to-bottom in time order. The
             * duration is appended after the name in parens so the eye
             * can scan either column independently. Fixed-width left
             * column lines names up across all 3 nesting levels. */
            ImGui::Text("%7.3f ms  %s  (%.3f ms)",
                        parent_start_ms, name, ms);
            if (sn > 0) {
                ImGui::Separator();
                /* Subs are emitted in execution order too, so we can
                 * walk them with the same accumulator pattern starting
                 * at the parent's start offset. */
                /* Highlight color for the row matching the cursor —
                 * orange so it pops against the dimmed default. */
                const ImVec4 kOrange(1.00f, 0.55f, 0.10f, 1.0f);
                double sub_acc = parent_start_ms;
                for (int si = 0; si < sn; si++) {
                    cJSON *sb = cJSON_GetArrayItem(subs, si);
                    if (!sb) continue;
                    const char *snm = "?";
                    cJSON *sjn = cJSON_GetObjectItem(sb, "name");
                    if (sjn && cJSON_IsString(sjn)) snm = sjn->valuestring;
                    double sms = getNum(sb, "ms", 0.0);
                    /* The sub row is highlighted only when the cursor
                     * is inside the sub's own rect AND not inside one
                     * of its subsubs (so a hover deep into a subsub
                     * doesn't double-highlight its parent sub). */
                    bool sub_hit = (si == hovered_sub_idx) &&
                                   (hovered_ss_sub_idx != si);
                    if (sub_hit) {
                        ImGui::TextColored(kOrange,
                                           "%7.3f ms    %s  (%.3f ms)",
                                           sub_acc, snm, sms);
                    } else {
                        ImGui::TextDisabled("%7.3f ms    %s  (%.3f ms)",
                                            sub_acc, snm, sms);
                    }
                    /* Third level (subsubs) — emitters writing 4-space
                     * indents land here. Display nested under their
                     * parent sub so the tooltip mirrors the visual
                     * hierarchy in the bar. */
                    cJSON *ssubs = cJSON_GetObjectItem(sb, "subs");
                    int ssn = (ssubs && cJSON_IsArray(ssubs))
                                ? cJSON_GetArraySize(ssubs) : 0;
                    double ssub_acc = sub_acc;
                    for (int ssi = 0; ssi < ssn; ssi++) {
                        cJSON *ssb = cJSON_GetArrayItem(ssubs, ssi);
                        if (!ssb) continue;
                        const char *ssnm = "?";
                        cJSON *ssjn = cJSON_GetObjectItem(ssb, "name");
                        if (ssjn && cJSON_IsString(ssjn)) ssnm = ssjn->valuestring;
                        double ssms = getNum(ssb, "ms", 0.0);
                        bool ss_hit = (si == hovered_ss_sub_idx)
                                   && (ssi == hovered_ss_idx);
                        if (ss_hit) {
                            ImGui::TextColored(kOrange,
                                               "%7.3f ms        %s  (%.3f ms)",
                                               ssub_acc, ssnm, ssms);
                        } else {
                            ImGui::TextDisabled("%7.3f ms        %s  (%.3f ms)",
                                                ssub_acc, ssnm, ssms);
                        }
                        ssub_acc += ssms;
                    }
                    sub_acc += sms;
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
    /* 50/50 split: left = stacked time bar; right = tier controls.
     * Was hard-coded 200 px which clamped the bar narrower than half
     * the panel and let the right column's wider tables crowd in. */
    const float kBarColW = ImGui::GetContentRegionAvail().x * 0.5f;
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
        /* Two timing references shown so you can verify alignment:
         *   Think  — brain-measured wall-clock between the two
         *            PERFORMANCE MARKER clocks bracketing Brain.think.
         *            Same timer (clock_us) the section "done" emits use,
         *            so sum-of-sections == Think.
         *   host   — brain.lastThinkMs (PREVIOUS tick), the host's
         *            SDL_GetPerformanceCounter delta around
         *            luaBrainInstanceTick. The top-left HUD reads this
         *            same value. Slightly larger than Think because it
         *            includes Lua dispatch + return marshal overhead.
         * The two should track each other within ~50-200 µs. If they
         * diverge meaningfully, something between the markers is hiding
         * from the section breakdown. */
        double thinkTotal = getNum(root, "think_total_ms", -1.0);
        if (thinkTotal >= 0.0) {
            ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1),
                "Think: %.2f ms (host: %.2f, target %.2f)",
                thinkTotal, lastMs, targetMs);
        } else {
            ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1),
                "host: %.2f / target %.2f ms (--perf-log off)",
                lastMs, targetMs);
        }
        if (total_ms > 0.001) {
            ImGui::TextDisabled("  sections sum: %.2f ms", total_ms);
        }
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
