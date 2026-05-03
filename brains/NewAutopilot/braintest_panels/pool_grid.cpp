/*********************************************************
 * brains/NewAutopilot/braintest_panels/pool_grid.cpp
 *
 * Bot-specific BrainTest panel renderer for NewAutopilot's
 * goal-pool data model. Compiled into BrainTest at build
 * time via brains/<bot>/braintest_panels/*.cpp glob.
 *
 * Visual layout matches the original optimize-branch
 * braintest_poolwindow.cpp pixel-for-pixel: 2x5 grid for
 * pools 1..10 + def_build / wait_for_lgm strips, per-pool
 * color coding, two-line rows (stats line + dim formula
 * line), winner / active-goal / flash backgrounds, click +
 * Ctrl+C copy, double-click detail popup.
 *
 * Input: JSON from goals.get_pool_breakdown_json (see brain
 * for the schema). Renderer is registered for panel type
 * "NewAutopilot:pool_grid" via static initializer below.
 *********************************************************/

#include <SDL3/SDL.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include "imgui.h"
#include "cJSON.h"
#include "braintest_panel_types.h"

namespace {

/* ── Term documentation (verbatim from optimize-branch poolwindow) ──
 * Looked up by the word immediately before "{value}" in a row's
 * formula — populates the "Meaning" column of the detail popup. */
struct TermDoc { const char *term; const char *desc; };
static const TermDoc kTermDocs[] = {
    {"A*",      "Path cost — Dijkstra/A* weighted grid distance to the target tile"},
    {"raw",     "Uncapped A* cost (shown when it was capped for distant/water targets)"},
    {"base",    "Fixed base constant added to every candidate of this goal type"},
    {"danger",  "Danger at destination × weight — hostile pills/tanks in firing range"},
    {"stale",   "Staleness penalty — target not seen recently; info may be wrong"},
    {"contest", "Contested penalty — an enemy tank is near this base"},
    {"hyst",    "Switch penalty — GOAL_SWITCH_PENALTY(30) or GOAL_TARGET_SWITCH_PENALTY(15) + commitment(ticks*0.5, cap 75); negative in Pool 1 means already on this target"},
    {"deplete", "Depletion penalty — base observed to have low shells or armour stock"},
    {"urgency", "Urgency discount (negative) — more pill damage = higher priority"},
    {"diff",    "Difficulty score — terrain around pill makes the attack harder"},
    {"spot",    "Best attack spot — path cost to the nearest good firing position"},
    {"anger",   "Anger wait cost — pill is riled up; penalty reflects waiting for it to calm"},
    {"xfire",   "Crossfire penalty — other hostile pills nearby will also fire at you"},
    {"intcpt",  "Intercept risk — enemy tank may reach this pill before you do"},
    {"hp",      "Health multiplier — lower pill HP = lower cost (easier kill)"},
    {"wound",   "Wounded discount — heavily damaged pill is a very high-value target"},
    {"self_dr", "Self-danger reduction (subtracted) — discount on the spot-path cost equal to the sum of the target pill's own contribution along that path. Applied regardless of pill HP so a fresh-but-targeted pill stops bullying its own approach corridor."},
    {"ammo",    "Shells-budget penalty — INF if shells < pill_hp (can't finish), otherwise 5 per shell that the take would leave us at below SHELLS_LOW (assuming exactly pill.health shots). Replaces the old has_shells gate that cleared the entire pool when shells dipped below SHELLS_LOW mid-take."},
    {"threat",  "Threat coverage × weight — hostile pill fire overlaps this base"},
    {"carry",   "Carry discount (negative) — you are already holding a pill to place"},
    {"mult",    "Multiplier — scales the entire bracketed cost sum"},
    {"cpill",   "Capture pill multiplier — dead pill pickup scales the path cost down"},
    {"aim",     "Aim bonus (negative) — tank is already in your sights; cheaper to engage"},
    {"wall",    "Wall obstruction penalty — blocks between you and target beyond 1; 2 blocks=+100, 5 blocks=+400"},
    {"loc",     "Strategic location multiplier — Phase 2; scales cost by terrain/position (e.g. deep_hostile). 1.0 = neutral."},
    {"dens",    "Density discount multiplier — Phase 4; lower when many friendly candidates of the same kind cluster nearby (n = neighbor count). 1.0 = no discount."},
    {"pickup",  "Pickup value — Phase 3; dead-pill bonus subtracted from travel cost when a pickup lies along the way."},
    {"wsim",    "Forward-sim damage cost — additive armour/ammo cost from simulating travel through danger fields; set in the wsim block."},
    {NULL, NULL}
};

/* ── cJSON helpers ─────────────────────────────────── */
double getNum(const cJSON *o, const char *k, double d) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return (v && cJSON_IsNumber(v)) ? v->valuedouble : d;
}
const char *getStr(const cJSON *o, const char *k, const char *d) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return (v && cJSON_IsString(v)) ? v->valuestring : d;
}
bool getBool(const cJSON *o, const char *k, bool d) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    if (!v) return d;
    if (cJSON_IsBool(v))   return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v)) return v->valuedouble != 0.0;
    return d;
}

/* ── Per-pool color (matches optimize branch verbatim) ── */
ImVec4 poolColorFor(int idx) {
    if (idx == 0)  return ImVec4(1.0f, 0.4f,  1.0f,  1);   /* override: magenta */
    if (idx == 1)  return ImVec4(0.4f, 1.0f,  0.4f,  1);   /* refuel: green */
    if (idx == 2)  return ImVec4(1.0f, 0.85f, 0.3f,  1);   /* defend_pill: gold */
    if (idx == 3)  return ImVec4(0.55f,0.8f,  1.0f,  1);   /* capture_base: sky blue */
    if (idx == 7)  return ImVec4(0.25f,0.45f, 0.95f, 1);   /* attack_base: deep blue */
    if (idx == 4)  return ImVec4(1.0f, 0.7f,  0.75f, 1);   /* capture_pill: light pink */
    if (idx == 5)  return ImVec4(1.0f, 0.5f,  0.45f, 1);   /* repair_pill: salmon */
    if (idx == 6)  return ImVec4(0.95f,0.25f, 0.25f, 1);   /* attack_pill: deep red */
    if (idx == 8)  return ImVec4(0.4f, 0.9f,  0.9f,  1);   /* place_strategic: cyan */
    if (idx == 9)  return ImVec4(1.0f, 0.65f, 0.2f,  1);   /* attack_tank: orange */
    if (idx == 10) return ImVec4(1.0f, 1.0f,  1.0f,  1);   /* winners: white */
    if (idx == 11) return ImVec4(0.7f, 0.5f,  1.0f,  1);   /* def_build: violet */
    if (idx == 12) return ImVec4(0.55f,0.85f, 0.55f, 1);   /* wait_for_lgm: sage */
    return ImVec4(0.8f, 0.8f, 0.8f, 1);
}

/* ── Parsed sections / rows ─────────────────────────── */
struct Row {
    int    id;
    int    src_pool;
    int    mx, my;
    float  cost, weighted;
    bool   winner;
    bool   activeGoal;
    bool   isOverride;
    int    staleTicks;
    float  flashAlpha;
    /* Bumped from 512 → 2048 to fit pool-6's multi-segment detail map
     * (hp / anger / stale / finish_other / self_dr / ammo / spot —
     * the last carries the realized path tiles which alone can be
     * 300+ chars). Truncation here cuts off the trailing segments
     * and the parser then prints "?" in the affected rows. */
    char   formula[2048];
    /* Reject info (capture_pill: dead pill that exists on the map but
     * can't be picked this tick — in_tank / blocked / stale). When
     * non-empty, the row renders dimmed with a colored chip. */
    char   reject[24];
    int    rejectRemaining;
};

struct Section {
    int   idx;
    char  name[32];
    int   count;
    int   winner_id;
    float phase_weight;
    Row  *rows;
    int   nrows;
};

static const float FLASH_DURATION_MS = 1000.0f;

/* All persistent per-window state lives here. Keyed by the panel
 * registry idx the host passes to render() so that opening pool
 * windows for two different bots doesn't bleed selection / detail-
 * popup / rank-flash state between them. */
struct DetailRow {
    bool  open, justOpened;
    int   sectionIdx;
    int   srcPool;
    char  poolName[32];
    int   rowId, mx, my;
    float cost, weighted;
    bool  winner;
    /* Match Row::formula's 2048 — without this, the double-click
     * handler truncates the formula again on its way into the popup
     * and the trailing detail segments (e.g. spot path) get lost. */
    char  formula[2048];
};
struct PanelState {
    /* Per-section rank tracking for flash animation, indexed by
     * section idx (1..12). map<row_id → rank/flash-start>. */
    std::unordered_map<int,int>    prevRank[13];
    std::unordered_map<int,Uint64> flashStart[13];

    /* Click selection + Ctrl+C copy buffer. */
    int  selectedSection = -1;
    int  selectedRowId   = -1;
    char copyBuf[512]    = {0};
    bool anyRowClickedThisFrame = false;

    /* Double-click detail popup. */
    DetailRow detail = {};
};

/* Lookup-by-idx so each (registry_idx → PanelState) pair survives
 * across frames. unordered_map for sparse keying — most idxs never
 * have a pool_grid renderer attached. */
static std::unordered_map<int, PanelState> g_states;

static PanelState &stateFor(int registry_idx) {
    /* operator[] default-constructs on first access; subsequent calls
     * return the same instance. -1 (no idx) gets its own slot. */
    return g_states[registry_idx];
}

/* ── Section list parsing from cJSON ─────────────────── */
static void parseRow(cJSON *jrow, Row *r, int section_idx, bool is_winners) {
    memset(r, 0, sizeof(*r));
    r->id        = (int)getNum(jrow, "id", 0);
    r->src_pool  = is_winners ? (int)getNum(jrow, "src_pool", section_idx)
                              : section_idx;
    r->mx        = (int)getNum(jrow, "mx", 0);
    r->my        = (int)getNum(jrow, "my", 0);
    r->cost      = (float)getNum(jrow, "cost", -1);
    r->weighted  = (float)getNum(jrow, "weighted", -1);
    r->winner    = getBool(jrow, "is_winner", false);
    r->activeGoal= getBool(jrow, "active_goal", false);
    r->isOverride= getBool(jrow, "is_override", false);
    r->staleTicks= (int)getNum(jrow, "stale", -1);
    const char *f = getStr(jrow, "formula", "");
    SDL_strlcpy(r->formula, f, sizeof(r->formula));
    const char *rej = getStr(jrow, "reject", "");
    SDL_strlcpy(r->reject, rej, sizeof(r->reject));
    r->rejectRemaining = (int)getNum(jrow, "reject_remaining", 0);
}

static int parseSections(cJSON *root, Section *out, int outMax) {
    cJSON *jsecs = cJSON_GetObjectItem(root, "sections");
    if (!jsecs || !cJSON_IsArray(jsecs)) return 0;
    int n = cJSON_GetArraySize(jsecs);
    if (n > outMax) n = outMax;
    for (int i = 0; i < n; i++) {
        cJSON *js = cJSON_GetArrayItem(jsecs, i);
        Section *s = &out[i];
        memset(s, 0, sizeof(*s));
        s->idx          = (int)getNum(js, "idx", i + 1);
        SDL_strlcpy(s->name, getStr(js, "name", "?"), sizeof(s->name));
        s->phase_weight = (float)getNum(js, "weight", 1.0);
        s->winner_id    = (int)getNum(js, "winner_id", -1);
        cJSON *jrows = cJSON_GetObjectItem(js, "rows");
        int rn = (jrows && cJSON_IsArray(jrows)) ? cJSON_GetArraySize(jrows) : 0;
        s->nrows = rn;
        s->count = rn;
        if (rn > 0) {
            s->rows = (Row *)calloc(rn, sizeof(Row));
            bool is_winners = (strcmp(s->name, "WINNERS") == 0);
            for (int ri = 0; ri < rn; ri++) {
                parseRow(cJSON_GetArrayItem(jrows, ri),
                         &s->rows[ri], s->idx, is_winners);
            }
        }
    }
    return n;
}

static void freeSections(Section *s, int n) {
    for (int i = 0; i < n; i++) free(s[i].rows);
}

/* ── Row rendering — verbatim from optimize branch ──
 * `st` is the per-window state; selection / detail / copy buffer
 * all live there so two windows for different bots don't share. */
static void renderRow(PanelState &st, const Section *s, int i, Row *r) {
    ImVec4 rowCol = poolColorFor(r->src_pool);
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const int lines = (r->formula[0] ? 2 : 1);
    /* Rejected rows (e.g., capture_pill row that exists but cannot be
     * picked this tick — in_tank / blocked / stale) render dimmed with
     * a colored chip explaining why. The hit-button stays full-alpha so
     * the row remains clickable / inspectable in the detail popup. */
    const bool isRej = (r->reject[0] != '\0');

    ImVec2 hitCursorStart = ImGui::GetCursorPos();
    char hitId[32];
    SDL_snprintf(hitId, sizeof(hitId), "##hit%d_%d", s->idx, i);
    if (ImGui::InvisibleButton(hitId,
            ImVec2(ImGui::GetContentRegionAvail().x, lineH * lines))) {
        st.selectedSection        = s->idx;
        st.selectedRowId          = r->id;
        st.anyRowClickedThisFrame = true;
        if (r->cost >= 1e9f)
            SDL_snprintf(st.copyBuf, sizeof(st.copyBuf),
                "#%d (%d,%d) cost=INF wt=%.0f  %s",
                r->id, r->mx, r->my, r->weighted, r->formula);
        else
            SDL_snprintf(st.copyBuf, sizeof(st.copyBuf),
                "#%d (%d,%d) cost=%.0f wt=%.0f  %s",
                r->id, r->mx, r->my, r->cost, r->weighted, r->formula);
    }
    if (ImGui::IsItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        st.detail.open       = true;
        st.detail.justOpened = true;
        st.detail.sectionIdx = s->idx;
        st.detail.srcPool    = r->src_pool;
        SDL_strlcpy(st.detail.poolName, s->name, sizeof(st.detail.poolName));
        st.detail.rowId    = r->id;
        st.detail.mx       = r->mx;
        st.detail.my       = r->my;
        st.detail.cost     = r->cost;
        st.detail.weighted = r->weighted;
        st.detail.winner   = r->winner;
        SDL_strlcpy(st.detail.formula, r->formula, sizeof(st.detail.formula));
    }
    bool isSelected = (st.selectedSection == s->idx
                       && st.selectedRowId == r->id);
    ImGui::SetCursorPos(hitCursorStart);

    ImVec2 rowStart = ImGui::GetCursorScreenPos();
    float rectW = rowStart.x + ImGui::GetContentRegionAvail().x;
    ImVec2 a = ImVec2(rowStart.x - 2, rowStart.y - 1);
    ImVec2 b = ImVec2(rectW, rowStart.y + lineH * lines + 1);
    if (isSelected) {
        ImGui::GetWindowDrawList()->AddRectFilled(a, b,
            IM_COL32(80, 120, 200, 90), 3.0f);
    }
    if (r->winner) {
        ImU32 bg = IM_COL32(
            (int)(rowCol.x * 90),
            (int)(rowCol.y * 90),
            (int)(rowCol.z * 90),
            160);
        ImGui::GetWindowDrawList()->AddRectFilled(a, b, bg, 3.0f);
    }
    if (r->activeGoal) {
        ImGui::GetWindowDrawList()->AddRectFilled(a, b,
            IM_COL32(80, 220, 80, 55), 3.0f);
        ImGui::GetWindowDrawList()->AddRect(a, b,
            IM_COL32(120, 255, 120, 255), 3.0f, 0, 2.0f);
    }
    if (r->flashAlpha > 0.0f) {
        ImU32 flash = IM_COL32(255, 255, 200,
                               (int)(r->flashAlpha * 120));
        ImGui::GetWindowDrawList()->AddRectFilled(a, b, flash, 3.0f);
    }

    /* Dim the entire row when rejected. Pop'd at the end of the row.
     * Hit area was already drawn above with full alpha. */
    if (isRej) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                            ImGui::GetStyle().Alpha * 0.45f);
    }

    /* Line 1: marker / id / pos / cost / weighted / staleness */
    if (r->activeGoal) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1), "\xe2\x96\xb6");
    } else if (r->winner) {
        ImGui::TextColored(ImVec4(1, 1, 0.4f, 1), "*");
    } else {
        ImGui::TextColored(ImVec4(0.3f, 0.3f, 0.3f, 1), " ");
    }
    ImGui::SameLine();
    if (r->isOverride) {
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.1f, 1.0f), "\xe2\x9a\xa1");
        ImGui::SameLine();
    }
    ImGui::TextColored(rowCol, "#%-3d", r->id);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1),
        "(%3d,%3d)", r->mx, r->my);
    ImGui::SameLine();
    if (r->cost < 0) {
        ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1), "cost=   ?   ");
    } else if (r->cost >= 1e9f) {
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "cost=  INF  ");
    } else {
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1, 1),
            "cost= %-6.0f", r->cost);
    }
    if (r->weighted >= 0 && r->weighted < 1e9f) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 1, 0.4f, 1),
            "wt= %-6.0f", r->weighted);
    }
    ImGui::SameLine();
    if (r->staleTicks < 0) {
        ImGui::TextColored(ImVec4(0.3f, 0.3f, 0.3f, 1), "~");
    } else if (r->staleTicks < 10) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1), "%dt", r->staleTicks);
    } else if (r->staleTicks < 50) {
        float t = (float)(r->staleTicks - 10) / 40.0f;
        ImGui::TextColored(ImVec4(1.0f, 1.0f - t * 0.5f, 0.3f, 1),
                           "%dt", r->staleTicks);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1), "%dt", r->staleTicks);
    }

    /* Reject chip — appears on the same line as cost/wt/stale. Pop'd
     * the dim alpha briefly so the chip itself reads at full opacity. */
    if (isRej) {
        ImGui::SameLine();
        ImGui::PopStyleVar();
        ImVec4 chipCol;
        if      (!strcmp(r->reject, "in_tank")) chipCol = ImVec4(0.95f,0.85f,0.20f,1);
        else if (!strcmp(r->reject, "blocked")) chipCol = ImVec4(1.00f,0.40f,0.40f,1);
        else if (!strcmp(r->reject, "stale"))   chipCol = ImVec4(0.70f,0.70f,0.70f,1);
        else                                     chipCol = ImVec4(0.85f,0.85f,0.85f,1);
        if ((!strcmp(r->reject, "blocked") || !strcmp(r->reject, "stale"))
            && r->rejectRemaining > 0) {
            ImGui::TextColored(chipCol, "[%s %dt]", r->reject, r->rejectRemaining);
        } else {
            ImGui::TextColored(chipCol, "[%s]", r->reject);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                            ImGui::GetStyle().Alpha * 0.45f);
    }

    /* Line 2: dim formula (truncate at "||" detail separator) */
    if (r->formula[0]) {
        ImVec4 fcol = ImVec4(rowCol.x * 0.7f, rowCol.y * 0.7f,
                             rowCol.z * 0.7f, 1);
        const char *sep = strstr(r->formula, "||");
        if (sep) {
            int dispLen = (int)(sep - r->formula);
            char dispBuf[512];
            if (dispLen >= (int)sizeof(dispBuf)) dispLen = (int)sizeof(dispBuf) - 1;
            memcpy(dispBuf, r->formula, dispLen);
            dispBuf[dispLen] = '\0';
            ImGui::TextColored(fcol, "    %s", dispBuf);
        } else {
            ImGui::TextColored(fcol, "    %s", r->formula);
        }
    }
    if (isRej) ImGui::PopStyleVar();
    ImGui::Spacing();
}

static void renderSection(PanelState &st, Section *s) {
    ImVec4 col = poolColorFor(s->idx);
    bool isWinners = (strcmp(s->name, "WINNERS") == 0);
    if (!isWinners) {
        ImGui::SetWindowFontScale(1.5f);
        ImGui::TextColored(col, "%d.", s->idx);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::SameLine();
    }
    ImGui::TextColored(col, "%s", s->name);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1),
        "(%d  x%.1f)", s->count, s->phase_weight);
    ImGui::Separator();

    if (s->nrows == 0) {
        ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1), "(empty)");
        return;
    }
    char childId[32];
    SDL_snprintf(childId, sizeof(childId), "##sec%d", s->idx);
    ImGui::BeginChild(childId, ImVec2(0, 0), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (int i = 0; i < s->nrows; i++) {
        renderRow(st, s, i, &s->rows[i]);
    }
    ImGui::EndChild();
}

/* Detail popup — verbatim port from optimize-branch poolwindow.
 * Splits the formula at "||" into a display half and a computation
 * half; parses the display half into name{value} terms; cross-
 * references each term with kTermDocs (meaning) and the computation
 * half (how-computed); renders everything in a 3- or 4-column table. */
static void renderDetailPopup(PanelState &st, int winW, int winH) {
    DetailRow &sDetail = st.detail;
    if (!sDetail.open) return;

    if (sDetail.justOpened) {
        ImGui::SetNextWindowFocus();
        sDetail.justOpened = false;
    }
    ImGui::SetNextWindowSize(ImVec2(560, 460), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2((float)winW * 0.5f - 280, (float)winH * 0.5f - 230),
        ImGuiCond_FirstUseEver);

    char title[80];
    ImVec4 poolCol = poolColorFor(sDetail.srcPool > 0
                                  ? sDetail.srcPool
                                  : sDetail.sectionIdx);
    SDL_snprintf(title, sizeof(title),
                 "Pool %d (%s)  —  Row #%d###detail",
                 sDetail.sectionIdx, sDetail.poolName, sDetail.rowId);

    bool open = sDetail.open;
    ImGui::Begin(title, &open,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

    if (ImGui::IsWindowFocused() &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        open = false;

    ImGui::TextColored(poolCol, "Pool %d: %s",
                       sDetail.sectionIdx, sDetail.poolName);
    if (sDetail.winner) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f),
                           "  \xe2\x98\x85 WINNER");
    }
    ImGui::Separator();

    ImGui::Text("ID: #%d    Location: (%d, %d)",
                sDetail.rowId, sDetail.mx, sDetail.my);
    /* ASCII dashes (not em-dash) so the glyph always renders in the
     * default ImGui font. The em-dash showed as "?" otherwise. */
    if (sDetail.cost < 0)
        ImGui::Text("Cost: (pending)    Weighted: --");
    else if (sDetail.cost >= 1e9f)
        ImGui::Text("Cost: INF    Weighted: INF");
    else
        ImGui::Text("Cost: %.1f    Weighted (xphase): %.1f",
                    sDetail.cost, sDetail.weighted);
    ImGui::Separator();

    /* Split the formula at the "||" detail separator. */
    const char *detailSep = strstr(sDetail.formula, "||");
    /* dispFormula only holds the half before "||"; that side is short
     * (~150 chars). 512 is plenty here — the long part is the detail
     * map after "||" which the parser walks directly off sDetail.formula. */
    char dispFormula[512] = {0};
    if (detailSep) {
        int dlen = (int)(detailSep - sDetail.formula);
        if (dlen >= (int)sizeof(dispFormula)) dlen = (int)sizeof(dispFormula) - 1;
        memcpy(dispFormula, sDetail.formula, dlen);
    } else {
        SDL_strlcpy(dispFormula, sDetail.formula, sizeof(dispFormula));
    }

    /* Parse the per-term computation map from the || section.
     * Format: "name:computation|name:computation|..." */
    struct TermCompute { char name[32]; char compute[400]; };
    TermCompute computes[24];
    int nComputes = 0;
    if (detailSep) {
        const char *dp = detailSep + 2;
        while (*dp && nComputes < 24) {
            const char *segEnd = strchr(dp, '|');
            if (!segEnd) segEnd = dp + strlen(dp);
            const char *colon = (const char *)memchr(dp, ':', segEnd - dp);
            if (colon) {
                int nlen = (int)(colon - dp);
                int clen = (int)(segEnd - colon - 1);
                if (nlen > 0 && nlen < 32 && clen > 0 && clen < 400) {
                    SDL_strlcpy(computes[nComputes].name,    dp,        nlen + 1);
                    SDL_strlcpy(computes[nComputes].compute, colon + 1, clen + 1);
                    nComputes++;
                }
            }
            dp = (*segEnd == '|') ? segEnd + 1 : segEnd;
        }
    }

    /* Display-portion formula in yellow. */
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Formula:");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.65f, 1.0f));
    ImGui::TextWrapped("  %s", dispFormula[0] ? dispFormula : "(none)");
    ImGui::PopStyleColor();
    ImGui::Separator();

    /* Parse word{value} terms out of the display formula. */
    struct ParsedTerm { char name[32]; char value[32]; };
    ParsedTerm terms[32];
    int nTerms = 0;
    const char *p = dispFormula;
    while (*p && nTerms < 32) {
        const char *ob = strchr(p, '{');
        if (!ob) break;
        const char *cb = strchr(ob + 1, '}');
        if (!cb) break;
        const char *ns = ob - 1;
        while (ns >= dispFormula &&
               (isalnum((unsigned char)*ns) || *ns == '*' || *ns == '_'))
            ns--;
        ns++;
        int nlen = (int)(ob - ns);
        int vlen = (int)(cb - ob - 1);
        if (nlen > 0 && nlen < 32 && vlen < 32) {
            SDL_strlcpy(terms[nTerms].name,  ns,     nlen + 1);
            SDL_strlcpy(terms[nTerms].value, ob + 1, vlen + 1);
            nTerms++;
        }
        p = cb + 1;
    }

    bool hasComputed = (nComputes > 0);
    if (nTerms > 0) {
        ImGui::Text("Term Breakdown:");
        ImGui::Spacing();
        int nCols = hasComputed ? 4 : 3;
        if (ImGui::BeginTable("##termtbl", nCols,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Term",    ImGuiTableColumnFlags_WidthFixed,  68.0f);
            ImGui::TableSetupColumn("Value",   ImGuiTableColumnFlags_WidthFixed,  52.0f);
            ImGui::TableSetupColumn("Meaning", ImGuiTableColumnFlags_WidthStretch);
            if (hasComputed)
                ImGui::TableSetupColumn("How computed",
                                        ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (int ti = 0; ti < nTerms; ti++) {
                const char *docStr = NULL;
                for (int di = 0; kTermDocs[di].term; di++) {
                    if (strcmp(terms[ti].name, kTermDocs[di].term) == 0) {
                        docStr = kTermDocs[di].desc;
                        break;
                    }
                }
                const char *compStr = NULL;
                for (int ci = 0; ci < nComputes; ci++) {
                    if (strcmp(terms[ti].name, computes[ci].name) == 0) {
                        compStr = computes[ci].compute;
                        break;
                    }
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.5f, 1.0f),
                                   "%s", terms[ti].name);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f),
                                   "%s", terms[ti].value);
                ImGui::TableSetColumnIndex(2);
                if (docStr) ImGui::TextWrapped("%s", docStr);
                else        ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1),
                                               "(no description)");
                if (hasComputed) {
                    ImGui::TableSetColumnIndex(3);
                    if (compStr)
                        ImGui::TextWrapped("%s", compStr);
                    else
                        ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1), "—");
                }
            }
            ImGui::EndTable();
        }
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                           "No formula terms to explain.");
    }

    ImGui::End();
    sDetail.open = open;
}

/* ── Top-level renderer registered for "NewAutopilot:pool_grid" ── */
void renderPoolGrid(int registry_idx, const char *body) {
    PanelState &st = stateFor(registry_idx);
    if (!body || !body[0]) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.4f, 1.0f),
                           "(no data yet — polling brain.get_pool_breakdown_json)");
        return;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                           "JSON parse error");
        ImGui::TextWrapped("Body: %.200s%s", body,
                           strlen(body) > 200 ? "..." : "");
        return;
    }

    const int   kSchemaVersion = 1;
    int  schemaV     = (int)getNum(root, "schema_version", 0);
    const char *phase = getStr(root, "phase", "?");
    int  tickIn      = (int)getNum(root, "tick", -1);
    int  replanIn    = (int)getNum(root, "replan_left", -1);
    bool isReplanTick = (replanIn == 0);
    int  followBot    = (int)getNum(root, "bot", -1);

    /* Schema-version banner — render but don't bail. The renderer
     * may still produce a useful (if degraded) view if the brain
     * upgraded fields without breaking shape. */
    if (schemaV != kSchemaVersion) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
            "WARN: pool_grid schema_version=%d, renderer expects %d",
            schemaV, kSchemaVersion);
    }

    /* Index sections by idx for grid placement (1..12). */
    enum { MAX_SECS = 32 };
    Section sections[MAX_SECS];
    int nSections = parseSections(root, sections, MAX_SECS);

    Section *byIdx[13] = {0};
    for (int i = 0; i < nSections; i++) {
        if (sections[i].idx >= 1 && sections[i].idx <= 12) {
            byIdx[sections[i].idx] = &sections[i];
        }
    }

    /* Flash animation: any row whose rank changed this frame gets a
     * fresh flash timestamp; flashAlpha then decays over
     * FLASH_DURATION_MS. */
    Uint64 now = SDL_GetTicks();
    for (int si = 1; si <= 12; si++) {
        Section *sec = byIdx[si];
        if (!sec) continue;
        auto &prev  = st.prevRank[si];
        auto &flash = st.flashStart[si];
        for (int ri = 0; ri < sec->nrows; ri++) {
            Row *r = &sec->rows[ri];
            /* Use a composite key to distinguish items with the same ID
             * from different source pools (common in the WINNERS pool).
             * Without this, two winners sharing an ID would clash in
             * flashStart and prevRank, causing one to always think its
             * rank changed and getting stuck in the white flash state. */
            int key = (r->src_pool << 16) | r->id;
            auto it = prev.find(key);
            bool rankChanged = (it == prev.end()) || (it->second != ri);
            if (rankChanged) flash[key] = now;
            auto fit = flash.find(key);
            if (fit != flash.end()) {
                float elapsed = (float)(now - fit->second);
                r->flashAlpha = 1.0f - elapsed / FLASH_DURATION_MS;
                if (r->flashAlpha < 0.0f) r->flashAlpha = 0.0f;
            }
        }
        prev.clear();
        for (int ri = 0; ri < sec->nrows; ri++) {
            int key = (sec->rows[ri].src_pool << 16) | sec->rows[ri].id;
            prev[key] = ri;
        }
    }

    /* Top-of-window header. The host has already opened a Begin() —
     * we render the inner content. */
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 1.0f, 1),
                       "TANK %d", followBot);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::SameLine();
    if (tickIn >= 0)
        ImGui::Text("  Tick %d  Phase: %s", tickIn, phase);
    else
        ImGui::Text("  Phase: %s", phase);
    ImGui::SameLine();
    if (replanIn == 0) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1),
            "    >>> REPLAN THIS TICK <<<");
    } else if (replanIn > 0 && replanIn <= 5) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1),
            "    Replan: %d", replanIn);
    } else if (replanIn > 0) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1),
            "    Replan: %d", replanIn);
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1),
        "    *=winner per pool   wt=cost*phase_weight");
    ImGui::Separator();

    /* 2x5 grid for sections 1..10. */
    ImVec2 avail = ImGui::GetContentRegionAvail();
    /* Reserve room at the bottom for the def_build (11) +
     * wait_for_lgm (12) strips when those sections exist. */
    float reservedH = 0.0f;
    if (byIdx[11]) reservedH += 56.0f;
    if (byIdx[12]) reservedH += 56.0f;
    const float gap = 4.0f;
    float gridH = avail.y - reservedH;
    if (gridH < 100.0f) gridH = 100.0f;
    float rowH = (gridH - gap) * 0.5f;
    float colW = (avail.x - gap * 4.0f) / 5.0f;

    if (isReplanTick) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                              IM_COL32(48, 48, 52, 255));
    }
    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < 5; col++) {
            if (col > 0) ImGui::SameLine(0.0f, gap);
            int sidx = row * 5 + col + 1;
            char cellId[24];
            SDL_snprintf(cellId, sizeof(cellId), "##cell%d", sidx);
            ImGui::BeginChild(cellId, ImVec2(colW, rowH), true);
            if (byIdx[sidx]) {
                renderSection(st, byIdx[sidx]);
            } else {
                ImGui::TextColored(poolColorFor(sidx),
                                   "%d. (no data)", sidx);
            }
            ImGui::EndChild();
        }
    }
    if (isReplanTick) ImGui::PopStyleColor();

    /* def_build (11) — full-width strip below the grid. */
    if (byIdx[11]) {
        ImVec2 a11 = ImGui::GetContentRegionAvail();
        ImGui::BeginChild("##cell11", ImVec2(a11.x, 52.0f), true);
        renderSection(st, byIdx[11]);
        ImGui::EndChild();
    }
    /* wait_for_lgm (12) — single-row strip. */
    if (byIdx[12]) {
        ImVec2 a12 = ImGui::GetContentRegionAvail();
        ImGui::BeginChild("##cell12", ImVec2(a12.x, 52.0f), true);
        renderSection(st, byIdx[12]);
        ImGui::EndChild();
    }

    /* Click on empty space deselects. */
    if (ImGui::IsMouseClicked(0) && !st.anyRowClickedThisFrame) {
        st.selectedSection = -1;
        st.selectedRowId   = -1;
        st.copyBuf[0]      = '\0';
    }
    st.anyRowClickedThisFrame = false;

    /* Ctrl+C copies the selected row to the system clipboard. */
    if (st.selectedRowId >= 0 && st.copyBuf[0] &&
            ImGui::GetIO().KeyCtrl &&
            ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        ImGui::SetClipboardText(st.copyBuf);
    }

    /* Refresh the detail window from this frame's freshly-parsed
     * sections so playback scrubbing keeps it in sync. Match by
     * (sectionIdx, rowId). If the row isn't present this frame
     * (e.g. the candidate fell out of the queue), leave sDetail
     * as-is so the user keeps the last-known state. */
    if (st.detail.open && !st.detail.justOpened) {
        for (int si = 0; si < nSections; si++) {
            if (sections[si].idx != st.detail.sectionIdx) continue;
            for (int ri = 0; ri < sections[si].nrows; ri++) {
                Row *r = &sections[si].rows[ri];
                if (r->id != st.detail.rowId) continue;
                st.detail.srcPool  = r->src_pool;
                st.detail.mx       = r->mx;
                st.detail.my       = r->my;
                st.detail.cost     = r->cost;
                st.detail.weighted = r->weighted;
                st.detail.winner   = r->winner;
                SDL_strlcpy(st.detail.formula, r->formula, sizeof(st.detail.formula));
                break;
            }
            break;
        }
    }

    int winW = (int)ImGui::GetWindowWidth();
    int winH = (int)ImGui::GetWindowHeight();
    renderDetailPopup(st, winW, winH);

    freeSections(sections, nSections);
    cJSON_Delete(root);
}

/* Self-register at static-init time. */
struct AutoRegister {
    AutoRegister() {
        panelTypeRegister("NewAutopilot:pool_grid", &renderPoolGrid);
    }
};
static AutoRegister _auto;

} /* namespace */
