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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include "imgui.h"
#include "cJSON.h"
#include "braintest_panel_types.h"

namespace {

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
    char   formula[512];
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

/* ── Per-section rank tracking for flash animation ── */
static std::unordered_map<int,int>    sPrevRank[13];
static std::unordered_map<int,Uint64> sFlashStart[13];
static const float FLASH_DURATION_MS = 1000.0f;

/* ── Selection / clipboard / detail popup state ─────── */
static int  sSelectedSection       = -1;
static int  sSelectedRowId         = -1;
static char sCopyBuf[512]          = {0};
static bool sAnyRowClickedThisFrame = false;

struct DetailRow {
    bool  open, justOpened;
    int   sectionIdx;
    int   srcPool;
    char  poolName[32];
    int   rowId, mx, my;
    float cost, weighted;
    bool  winner;
    char  formula[512];
};
static DetailRow sDetail = {};

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

/* ── Row rendering — verbatim from optimize branch ── */
static void renderRow(const Section *s, int i, Row *r) {
    ImVec4 rowCol = poolColorFor(r->src_pool);
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const int lines = (r->formula[0] ? 2 : 1);

    ImVec2 hitCursorStart = ImGui::GetCursorPos();
    char hitId[32];
    SDL_snprintf(hitId, sizeof(hitId), "##hit%d_%d", s->idx, i);
    if (ImGui::InvisibleButton(hitId,
            ImVec2(ImGui::GetContentRegionAvail().x, lineH * lines))) {
        sSelectedSection        = s->idx;
        sSelectedRowId          = r->id;
        sAnyRowClickedThisFrame = true;
        if (r->cost >= 1e9f)
            SDL_snprintf(sCopyBuf, sizeof(sCopyBuf),
                "#%d (%d,%d) cost=INF wt=%.0f  %s",
                r->id, r->mx, r->my, r->weighted, r->formula);
        else
            SDL_snprintf(sCopyBuf, sizeof(sCopyBuf),
                "#%d (%d,%d) cost=%.0f wt=%.0f  %s",
                r->id, r->mx, r->my, r->cost, r->weighted, r->formula);
    }
    if (ImGui::IsItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        sDetail.open       = true;
        sDetail.justOpened = true;
        sDetail.sectionIdx = s->idx;
        sDetail.srcPool    = r->src_pool;
        SDL_strlcpy(sDetail.poolName, s->name, sizeof(sDetail.poolName));
        sDetail.rowId    = r->id;
        sDetail.mx       = r->mx;
        sDetail.my       = r->my;
        sDetail.cost     = r->cost;
        sDetail.weighted = r->weighted;
        sDetail.winner   = r->winner;
        SDL_strlcpy(sDetail.formula, r->formula, sizeof(sDetail.formula));
    }
    bool isSelected = (sSelectedSection == s->idx && sSelectedRowId == r->id);
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
    ImGui::Spacing();
}

static void renderSection(const Section *s) {
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
        renderRow(s, i, &((Section *)s)->rows[i]);
    }
    ImGui::EndChild();
}

static void renderDetailPopup(int winW, int winH) {
    if (!sDetail.open) return;
    if (sDetail.justOpened) {
        ImGui::SetNextWindowPos(
            ImVec2(winW * 0.5f - 280, winH * 0.5f - 200),
            ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(560, 400), ImGuiCond_Always);
        sDetail.justOpened = false;
    }
    char title[96];
    SDL_snprintf(title, sizeof(title),
                 "Row #%d  —  %s###detail",
                 sDetail.rowId, sDetail.poolName);
    bool open = sDetail.open;
    if (ImGui::Begin(title, &open,
                     ImGuiWindowFlags_NoSavedSettings)) {
        ImVec4 pc = poolColorFor(sDetail.srcPool > 0
                                 ? sDetail.srcPool
                                 : sDetail.sectionIdx);
        ImGui::TextColored(pc, "Pool: %s", sDetail.poolName);
        ImGui::Text("ID: #%d   Pos: (%d, %d)",
                    sDetail.rowId, sDetail.mx, sDetail.my);
        if (sDetail.cost >= 1e9f)
            ImGui::Text("Cost: INF   Weighted: %.0f", sDetail.weighted);
        else
            ImGui::Text("Cost: %.0f   Weighted: %.0f",
                        sDetail.cost, sDetail.weighted);
        ImGui::Separator();
        ImGui::TextWrapped("Formula:");
        ImGui::TextWrapped("%s", sDetail.formula);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) open = false;
    }
    ImGui::End();
    if (!open) sDetail.open = false;
}

/* ── Top-level renderer registered for "NewAutopilot:pool_grid" ── */
void renderPoolGrid(const char *body) {
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

    const char *phase = getStr(root, "phase", "?");
    int  tickIn      = (int)getNum(root, "tick", -1);
    int  replanIn    = (int)getNum(root, "replan_left", -1);
    bool isReplanTick = (replanIn == 0);
    int  followBot    = (int)getNum(root, "bot", -1);

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
        auto &prev  = sPrevRank[si];
        auto &flash = sFlashStart[si];
        for (int ri = 0; ri < sec->nrows; ri++) {
            Row *r = &sec->rows[ri];
            auto it = prev.find(r->id);
            bool rankChanged = (it == prev.end()) || (it->second != ri);
            if (rankChanged) flash[r->id] = now;
            auto fit = flash.find(r->id);
            if (fit != flash.end()) {
                float elapsed = (float)(now - fit->second);
                r->flashAlpha = 1.0f - elapsed / FLASH_DURATION_MS;
                if (r->flashAlpha < 0.0f) r->flashAlpha = 0.0f;
            }
        }
        prev.clear();
        for (int ri = 0; ri < sec->nrows; ri++) {
            prev[sec->rows[ri].id] = ri;
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
                renderSection(byIdx[sidx]);
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
        renderSection(byIdx[11]);
        ImGui::EndChild();
    }
    /* wait_for_lgm (12) — single-row strip. */
    if (byIdx[12]) {
        ImVec2 a12 = ImGui::GetContentRegionAvail();
        ImGui::BeginChild("##cell12", ImVec2(a12.x, 52.0f), true);
        renderSection(byIdx[12]);
        ImGui::EndChild();
    }

    /* Click on empty space deselects. */
    if (ImGui::IsMouseClicked(0) && !sAnyRowClickedThisFrame) {
        sSelectedSection = -1;
        sSelectedRowId   = -1;
        sCopyBuf[0]      = '\0';
    }
    sAnyRowClickedThisFrame = false;

    /* Ctrl+C copies the selected row to the system clipboard. */
    if (sSelectedRowId >= 0 && sCopyBuf[0] &&
            ImGui::GetIO().KeyCtrl &&
            ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        ImGui::SetClipboardText(sCopyBuf);
    }

    int winW = (int)ImGui::GetWindowWidth();
    int winH = (int)ImGui::GetWindowHeight();
    renderDetailPopup(winW, winH);

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
