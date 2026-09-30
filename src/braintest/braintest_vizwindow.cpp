/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * braintest_vizwindow.cpp — ImGui table backed by the
 * runtime viz registry. See header for design.
 *********************************************************/

#include <SDL3/SDL.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "imgui.h"
#include "imgui_internal.h"   /* PushItemFlag + ImGuiItemFlags_MixedValue (tri-state checkbox) */
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

extern "C" {
#include "braintest_vizwindow.h"
#include "braintest_viz_registry.h"
}

static bool          sImGuiInitialized = false;
static ImGuiContext *sCtx              = NULL;

void vizWindowInit(SDL_Window *window, SDL_Renderer *renderer) {
    if (sImGuiInitialized || !window || !renderer) return;
    IMGUI_CHECKVERSION();
    ImGuiContext *prev = ImGui::GetCurrentContext();
    sCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(sCtx);
    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(6, 6);
    s.ItemSpacing   = ImVec2(6, 4);
    s.FramePadding  = ImVec2(6, 3);
    s.CellPadding   = ImVec2(6, 4);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    sImGuiInitialized = true;
    if (prev) ImGui::SetCurrentContext(prev);
}

void vizWindowShutdown(void) {
    if (!sImGuiInitialized) return;
    ImGuiContext *prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(sCtx);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(sCtx);
    sCtx = NULL;
    sImGuiInitialized = false;
    if (prev && prev != sCtx) ImGui::SetCurrentContext(prev);
}

void vizWindowProcessEvent(SDL_Event *ev) {
    if (!sImGuiInitialized || !sCtx) return;
    ImGuiContext *prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(sCtx);
    ImGui_ImplSDL3_ProcessEvent(ev);
    if (prev) ImGui::SetCurrentContext(prev);
}

bool vizWindowWantsTextInput(void) {
    if (!sImGuiInitialized || !sCtx) return false;
    ImGuiContext *prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(sCtx);
    bool want = ImGui::GetIO().WantTextInput;
    if (prev) ImGui::SetCurrentContext(prev);
    return want;
}

/* Drop keyboard focus from the filter/label inputs. Call when the V window is
 * hidden — the context isn't stepped while hidden, so an active InputText would
 * otherwise stay "focused" (capturing text input / re-focusing on reopen). */
void vizWindowClearFocus(void) {
    if (!sImGuiInitialized || !sCtx) return;
    ImGuiContext *prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(sCtx);
    ImGui::ClearActiveID();
    ImGui::GetIO().WantTextInput = false;
    ImGui::GetIO().WantCaptureKeyboard = false;
    if (prev) ImGui::SetCurrentContext(prev);
}

/* Build a sort-order vector each frame (registry insertion
 * order isn't necessarily readable — we sort by id/label). */
static int sort_order_buf[VIZ_REG_MAX];
static int build_sort_order(void) {
    int n = vizRegistryCount();
    for (int i = 0; i < n; i++) sort_order_buf[i] = i;
    for (int i = 1; i < n; i++) {
        int cur = sort_order_buf[i];
        const VizRegistryEntry *ce = vizRegistryGet(cur);
        const char *cs = ce->id[0] ? ce->id : ce->label;
        int j = i - 1;
        while (j >= 0) {
            const VizRegistryEntry *pe = vizRegistryGet(sort_order_buf[j]);
            const char *ps = pe->id[0] ? pe->id : pe->label;
            if (SDL_strcasecmp(ps, cs) <= 0) break;
            sort_order_buf[j + 1] = sort_order_buf[j];
            j--;
        }
        sort_order_buf[j + 1] = cur;
    }
    return n;
}

/* ──────────────────── Viz category + HUD/Map metadata ──────────────────
 * Display-only grouping for the V window. HUD/Map is the overlay's anchor
 * space (HUD = screen-anchored, drawn via viz.hud_text/hud_rect; Map =
 * world-anchored, drawn via viz.text/line/circle/rect). Category is a
 * functional bucket used for the group tri-state checkboxes at the top.
 *
 * Kept here (not in the brain registry) because it's purely a BrainTest
 * presentation concern — no engine/binding/Lua changes needed. New viz
 * ids that aren't listed fall back to category "Misc" and a HUD/Map guess
 * from the id prefix; add a row below to place them. */
enum {
    VCAT_HUD = 0, VCAT_CHARGE, VCAT_PILLTAKE, VCAT_SQUAD, VCAT_NAV,
    VCAT_LGM, VCAT_TANKCBT, VCAT_THREAT, VCAT_CIRCLES, VCAT_PLACEMENT,
    VCAT_ALLY, VCAT_SHELLS, VCAT_REPOSITION, VCAT_DECOY, VCAT_MISC, VCAT_COUNT
};

static const char *kVizCatLabel[VCAT_COUNT] = {
    "Core HUD", "Charge", "Pill take", "Squad/Blitz", "Nav",
    "LGM", "Tank combat", "Threat", "Circles", "Placement",
    "Ally/Comms", "Shells/Hitbox", "Reposition", "Decoy", "Misc",
};

struct VizMeta { const char *id; unsigned char cat; bool hud; };
static const VizMeta kVizMeta[] = {
    /* Shells / hitbox */
    {"shot_tile_grid", VCAT_SHELLS, false}, {"lgm_tile_grid", VCAT_SHELLS, false},
    {"shell_hitbox", VCAT_SHELLS, false},   {"shell_hit_dot", VCAT_SHELLS, false},
    {"lgm_hitbox", VCAT_SHELLS, false},     {"hitbox_logs", VCAT_SHELLS, false},
    {"shot_tracker_viz", VCAT_SHELLS, false},
    /* Core HUD */
    {"hud_budget", VCAT_HUD, true},         {"hud_manual_control", VCAT_HUD, true},
    {"hud_resources", VCAT_HUD, true},      {"hud_tick_info", VCAT_HUD, true},
    {"hud_replan", VCAT_HUD, true},         {"hud_goal", VCAT_HUD, true},
    {"hud_goal_candidates", VCAT_HUD, true},{"hud_attack_status", VCAT_HUD, true},
    {"hud_base_killer", VCAT_HUD, true},    {"hud_stuck_counter", VCAT_HUD, true},
    {"hud_compass", VCAT_HUD, true},        {"hud_emergency_drop", VCAT_HUD, true},
    {"hud_version", VCAT_HUD, true},        {"tank_position", VCAT_HUD, true},
    {"tank_angle", VCAT_HUD, true},
    /* Charge / approach */
    {"approach_dist", VCAT_CHARGE, false},  {"charge_status", VCAT_CHARGE, true},
    {"charge_stop_pred", VCAT_CHARGE, false},{"approach_stop_pred", VCAT_CHARGE, false},
    {"detree_progress", VCAT_CHARGE, false},
    /* Pill take / shield / attack planning */
    {"pill_range_circle", VCAT_PILLTAKE, false}, {"pill_id_label", VCAT_PILLTAKE, false},
    {"plan_trace", VCAT_PILLTAKE, false},        {"shield_scan_candidates", VCAT_PILLTAKE, false},
    {"shield_scan_blockers", VCAT_PILLTAKE, false}, {"shield_blocker_union", VCAT_PILLTAKE, false},
    {"shield_scan_trajectory", VCAT_PILLTAKE, false}, {"shield_scan_legend", VCAT_PILLTAKE, false},
    {"wall_build_queue", VCAT_PILLTAKE, false},  {"build_status", VCAT_PILLTAKE, false},
    {"build_decision_banner", VCAT_PILLTAKE, false}, {"wall_skip_reason", VCAT_PILLTAKE, false},
    {"pill_take_target", VCAT_PILLTAKE, false},  {"base_shield_viz", VCAT_PILLTAKE, false},
    {"inspect_pill", VCAT_PILLTAKE, false},      {"wounded_pill_marker", VCAT_PILLTAKE, false},
    {"pool6_self_dr", VCAT_PILLTAKE, false},     {"wsim_paths", VCAT_PILLTAKE, false},
    {"swerve_dir_choice", VCAT_PILLTAKE, false}, {"bpc_cover_samples", VCAT_PILLTAKE, false},
    {"plan_position_progress", VCAT_PILLTAKE, false}, {"pill_eval_progress", VCAT_PILLTAKE, false},
    {"attack_bullet_counter", VCAT_PILLTAKE, false}, {"attack_base_marker", VCAT_PILLTAKE, false},
    {"forest_path_tiles", VCAT_PILLTAKE, false}, {"hud_kill_attempt", VCAT_PILLTAKE, true},
    {"hud_shoot_pill_progress", VCAT_PILLTAKE, true}, {"pill_shot_count", VCAT_PILLTAKE, false},
    {"hardline_path", VCAT_PILLTAKE, false},     {"hud_swerve_debug", VCAT_PILLTAKE, true},
    {"attack_scan_spots", VCAT_PILLTAKE, false}, {"attack_scan_spots_all_pills", VCAT_PILLTAKE, false},
    {"attack_clear_reason", VCAT_PILLTAKE, true},{"attack_chosen_standoff_marker", VCAT_PILLTAKE, false},
    {"attack_pill_pickup_path", VCAT_PILLTAKE, false},
    {"fast_approach", VCAT_PILLTAKE, false},
    /* Squad / blitz */
    {"blitz_comm_lines", VCAT_SQUAD, false}, {"blitz_joinable", VCAT_SQUAD, true},
    {"squad_roster", VCAT_SQUAD, true},      {"blitz_call", VCAT_SQUAD, false},
    {"blitz_roster", VCAT_SQUAD, false},     {"blitz_wait_timeout", VCAT_SQUAD, true},
    {"blitz_join_hl", VCAT_SQUAD, false},    {"help_range", VCAT_SQUAD, false},
    {"squad_blitz", VCAT_SQUAD, false},      {"blitz_negotiate_scan", VCAT_SQUAD, false},
    {"squad_labels", VCAT_SQUAD, false},     {"role_live", VCAT_SQUAD, false},
    {"hard_take_pills", VCAT_SQUAD, false},
    /* Navigation / steering */
    {"steering_text", VCAT_NAV, false},      {"cliff_safety", VCAT_NAV, false},
    {"pf_destination", VCAT_NAV, false},     {"pf_path_lines", VCAT_NAV, false},
    {"blocked_tiles", VCAT_NAV, false},      {"nav_veer", VCAT_NAV, false},
    {"nav_lookahead_marker", VCAT_NAV, false}, {"wall_shoot_precond", VCAT_NAV, false},
    {"facing_away_brake", VCAT_NAV, false},  {"hud_throttle", VCAT_NAV, true},
    {"adjacent_tiles", VCAT_NAV, false},
    /* LGM */
    {"lgm_stranded", VCAT_LGM, false},       {"lgm_destination", VCAT_LGM, false},
    {"ally_lgm_marker", VCAT_LGM, false},    {"hud_lgm_status", VCAT_LGM, true},
    {"hud_lgm_blocked", VCAT_LGM, true},     {"hud_enemy_lgm_dead", VCAT_LGM, true},
    {"enemy_lgm_marker", VCAT_LGM, false},   {"kill_lgm_status", VCAT_LGM, false},
    {"lgm_registry_hud", VCAT_LGM, true},    {"lgm_registry_map", VCAT_LGM, false},
    {"kill_lgm_engage", VCAT_LGM, false},    {"kill_lgm_predict", VCAT_LGM, false},
    {"kill_lgm_sim_path", VCAT_LGM, false},
    {"builder_pool_shell_gate", VCAT_LGM, false},
    /* Tank combat */
    {"tank_hitbox", VCAT_TANKCBT, false},    {"tank_aim_marker", VCAT_TANKCBT, false},
    {"tank_combat_viz", VCAT_TANKCBT, false},{"ghost_tank", VCAT_TANKCBT, false},
    {"tank_combat_standoff_scan", VCAT_TANKCBT, false},
    /* Threat / coverage */
    {"coverage_grid", VCAT_THREAT, false},   {"pill_threat_overlay", VCAT_THREAT, false},
    {"friendly_pill_shield", VCAT_THREAT, false},
    {"demine_scan", VCAT_THREAT, false},     {"trepair_scan", VCAT_THREAT, false},
    /* Influence / circles */
    {"circles", VCAT_CIRCLES, false},        {"circles_hud", VCAT_CIRCLES, true},
    {"circle_trend", VCAT_CIRCLES, false},   {"reinforce_link", VCAT_CIRCLES, false},
    {"circle_poi", VCAT_CIRCLES, false},     {"circle_history", VCAT_CIRCLES, false},
    {"circle_warning", VCAT_CIRCLES, false}, {"front_band", VCAT_CIRCLES, false},
    /* Pill placement / portfolio */
    {"repair_pill_viz", VCAT_PLACEMENT, false}, {"bait_pill_marker", VCAT_PLACEMENT, false},
    {"pill_portfolio", VCAT_PLACEMENT, true},
    {"pill_best_spots_back", VCAT_PLACEMENT, false}, {"pill_best_spots_aggro", VCAT_PLACEMENT, false},
    {"panic_build", VCAT_PLACEMENT, false},  {"blocker_pills", VCAT_PLACEMENT, false},
    {"pill_roles", VCAT_PLACEMENT, false},
    /* Reposition (move-pill voting + scoring) */
    {"pill_reposition_marker", VCAT_REPOSITION, false}, {"repos_claims", VCAT_REPOSITION, false},
    {"reposition_scores", VCAT_REPOSITION, false}, {"reposition_vote", VCAT_REPOSITION, true},
    {"reposition_scores_hud", VCAT_REPOSITION, true}, {"reposition_votes", VCAT_REPOSITION, true},
    /* Ally / comms state */
    {"ally_state_overlay", VCAT_ALLY, true}, {"chat_log_overlay", VCAT_ALLY, true},
    {"hud_refuel_ally_check", VCAT_ALLY, true}, {"ally_claimed_marker", VCAT_ALLY, false},
    {"ally_avoid_overlay", VCAT_ALLY, false},
    /* Decoy hold getaway (decoy_getaway.lua) */
    {"decoy_chain", VCAT_DECOY, false},      {"decoy_scan_cells", VCAT_DECOY, false},
    {"decoy_score_terms", VCAT_DECOY, false},{"decoy_pill_lines", VCAT_DECOY, false},
    {"decoy_blocker_count", VCAT_DECOY, false}, {"decoy_status", VCAT_DECOY, false},
    /* Misc / meta / test */
    {"label_overlays", VCAT_MISC, false},    {"label_hud_overlays", VCAT_MISC, true},
    {"test_lgm_target", VCAT_MISC, false},   {"test_victim_marker", VCAT_MISC, false},
};

/* Resolve an entry's category + HUD/Map. Unlisted ids → Misc, with the
 * HUD/Map guessed from the id ("hud_" prefix) or the short-desc ("HUD:"). */
static int vizMetaFor(const VizRegistryEntry *e, bool *isHud) {
    if (e && e->id[0]) {
        for (size_t i = 0; i < sizeof(kVizMeta) / sizeof(kVizMeta[0]); i++) {
            if (strcmp(kVizMeta[i].id, e->id) == 0) {
                if (isHud) *isHud = kVizMeta[i].hud;
                return kVizMeta[i].cat;
            }
        }
    }
    if (isHud) {
        bool h = false;
        if (e) {
            if (!SDL_strncasecmp(e->id, "hud_", 4)) h = true;
            else if (!SDL_strncmp(e->short_desc, "HUD:", 4)) h = true;
        }
        *isHud = h;
    }
    return VCAT_MISC;
}

/* Turn every entry in a category on/off (group tri-state checkbox). */
static void vizSetCategory(int cat, bool on, void (*onToggle)(int)) {
    int n = vizRegistryCount();
    for (int i = 0; i < n; i++) {
        VizRegistryEntry *m = vizRegistryGetMutable(i);
        if (!m) continue;
        if (vizMetaFor(m, NULL) == cat) m->is_on = on;
    }
    if (onToggle) onToggle(0);
}

/* ───────────────────────── Viz "sets" ─────────────────────────────────
 * Four named slots that capture/restore which overlays are enabled, plus
 * Select-all / Clear-all. A set stores the id (or label, for native rows)
 * of every currently-on entry; Load turns those on and everything else off.
 * Persisted to BrainTestVizSets.ini next to BrainTestViz.ini so slots and
 * their labels survive restarts. */
#define VIZ_SET_COUNT      4
#define VIZ_SET_LABEL_MAX  64
#define VIZ_SET_KEY_MAX    (VIZ_REG_ID_MAX > VIZ_REG_LABEL_MAX ? VIZ_REG_ID_MAX : VIZ_REG_LABEL_MAX)

struct VizSet {
    char label[VIZ_SET_LABEL_MAX];
    char keys[VIZ_REG_MAX][VIZ_SET_KEY_MAX];  /* ids/labels of on entries */
    int  keyCount;
    bool saved;
};
static VizSet sVizSets[VIZ_SET_COUNT];
static bool   sVizSetsLoaded = false;

/* Collection mode for replay recording (the V-window radio). Defined here so the
 * VizSets ini save/load below can persist it alongside the slots. Values:
 *   0 = only ON layers, followed tank only
 *   1 = ALL layers (even off) for the viewed/followed tank only
 *   2 = ALL layers (even off) for ALL tanks
 *   3 = only ON layers, for ALL tanks  */
static int sCollectMode = 0;

static const char *vizEntryKey(const VizRegistryEntry *e) {
    if (!e) return "";
    return e->id[0] ? e->id : e->label;
}

static void vizSetsPath(char *out, size_t n) {
    char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
    if (prefDir && prefDir[0]) SDL_snprintf(out, n, "%sBrainTestVizSets.ini", prefDir);
    else                       SDL_snprintf(out, n, "BrainTestVizSets.ini");
    SDL_free(prefDir);
}

static void vizSetsSave(void) {
    char path[FILENAME_MAX];
    vizSetsPath(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "[Collect]\nmode=%d\n", sCollectMode);
    for (int s = 0; s < VIZ_SET_COUNT; s++) {
        fprintf(f, "[Set%d]\n", s);
        fprintf(f, "label=%s\n", sVizSets[s].label);
        fprintf(f, "saved=%d\n", sVizSets[s].saved ? 1 : 0);
        for (int k = 0; k < sVizSets[s].keyCount; k++)
            fprintf(f, "on=%s\n", sVizSets[s].keys[k]);
    }
    fclose(f);
}

static void vizSetsLoad(void) {
    sCollectMode = 0;
    for (int s = 0; s < VIZ_SET_COUNT; s++) {
        sVizSets[s].keyCount = 0;
        sVizSets[s].saved    = false;
        SDL_snprintf(sVizSets[s].label, sizeof(sVizSets[s].label), "Set %d", s + 1);
    }
    char path[FILENAME_MAX];
    vizSetsPath(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    int  cur = -1;       /* current [Set#] index, or -1 */
    bool inCollect = false;
    while (fgets(line, sizeof(line), f)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = '\0';
        if (line[0] == '[') {
            int idx;
            if (sscanf(line, "[Set%d]", &idx) == 1 && idx >= 0 && idx < VIZ_SET_COUNT) {
                cur = idx; inCollect = false;
            } else if (!strcmp(line, "[Collect]")) {
                cur = -1; inCollect = true;
            } else {
                cur = -1; inCollect = false;
            }
        } else if (inCollect) {
            if (!strncmp(line, "mode=", 5)) {
                int m = atoi(line + 5);
                if (m >= 0 && m <= 3) sCollectMode = m;
            }
        } else if (cur >= 0) {
            if (!strncmp(line, "label=", 6)) {
                SDL_snprintf(sVizSets[cur].label, sizeof(sVizSets[cur].label), "%s", line + 6);
            } else if (!strncmp(line, "saved=", 6)) {
                sVizSets[cur].saved = atoi(line + 6) != 0;
            } else if (!strncmp(line, "on=", 3) && sVizSets[cur].keyCount < VIZ_REG_MAX) {
                SDL_snprintf(sVizSets[cur].keys[sVizSets[cur].keyCount], VIZ_SET_KEY_MAX, "%s", line + 3);
                sVizSets[cur].keyCount++;
            }
        }
    }
    fclose(f);
}

/* Capture the current on-set into a slot. */
static void vizSetCapture(int slot) {
    VizSet *S = &sVizSets[slot];
    S->keyCount = 0;
    int n = vizRegistryCount();
    for (int i = 0; i < n && S->keyCount < VIZ_REG_MAX; i++) {
        const VizRegistryEntry *e = vizRegistryGet(i);
        if (e && e->is_on) {
            SDL_snprintf(S->keys[S->keyCount], VIZ_SET_KEY_MAX, "%s", vizEntryKey(e));
            S->keyCount++;
        }
    }
    S->saved = true;
    vizSetsSave();
}

/* Turn on exactly the slot's saved entries (everything else off). */
static void vizSetApply(int slot, void (*onToggle)(int)) {
    VizSet *S = &sVizSets[slot];
    if (!S->saved) return;
    int n = vizRegistryCount();
    for (int i = 0; i < n; i++) {
        VizRegistryEntry *m = vizRegistryGetMutable(i);
        if (!m) continue;
        const char *key = vizEntryKey(m);
        bool want = false;
        for (int k = 0; k < S->keyCount; k++) {
            if (!strcmp(S->keys[k], key)) { want = true; break; }
        }
        m->is_on = want;
    }
    if (onToggle) onToggle(0);  /* persist registry ini (idx ignored) */
}

static void vizSetAll(bool on, void (*onToggle)(int)) {
    int n = vizRegistryCount();
    for (int i = 0; i < n; i++) {
        VizRegistryEntry *m = vizRegistryGetMutable(i);
        if (m) m->is_on = on;
    }
    if (onToggle) onToggle(0);
}

/* sCollectMode (the V-window "Collect for replay" radio) is defined above so the
 * VizSets ini persists it. It's independent of the per-row toggles: it controls
 * what each brain EMITS (and thus what gets recorded), so a replay can carry
 * layers that were toggled off / other tanks. */
int vizWindowCollectMode(void) {
    /* Load the persisted collect mode on first access, NOT just when the V
     * window is first rendered. The recording path (pushVizStateToBots) reads
     * this every tick; if the user starts a sim without ever opening the V
     * window, the ini value (e.g. "ALL tanks") would otherwise never load and
     * we'd silently record at the static default 0 (followed-tank-only),
     * leaving every non-followed bot with no overlays in playback. */
    if (!sVizSetsLoaded) { vizSetsLoad(); sVizSetsLoaded = true; }
    return sCollectMode;
}

void vizWindowRender(SDL_Renderer *renderer, int winW, int winH,
                     void (*onToggle)(int idx)) {
    if (!sImGuiInitialized || !renderer) return;

    ImGuiContext *prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(sCtx);
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
    ImGui::Begin("Visualizations", NULL,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse);

    ImGui::TextColored(ImVec4(0.6f, 0.65f, 0.75f, 1.0f),
        "Click a row to toggle. Saved to BrainTestViz.ini.");

    /* Load slots + collect-mode from BrainTestVizSets.ini before drawing the
     * radio so it reflects the persisted value on the first frame. */
    if (!sVizSetsLoaded) { vizSetsLoad(); sVizSetsLoaded = true; }

    /* Collection mode for replay: what the brains EMIT/record, independent of
     * the per-row toggles below. Persisted to BrainTestVizSets.ini on change. */
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.7f, 0.75f, 0.85f, 1.0f), "Collect for replay:");
    bool collectChanged = false;
    collectChanged |= ImGui::RadioButton("ON layers, followed tank only",        &sCollectMode, 0);
    ImGui::SameLine();
    collectChanged |= ImGui::RadioButton("ALL layers, viewed tank (even if off)", &sCollectMode, 1);
    ImGui::SameLine();
    collectChanged |= ImGui::RadioButton("ALL layers, ALL tanks (even if off)",   &sCollectMode, 2);
    ImGui::SameLine();
    collectChanged |= ImGui::RadioButton("ON layers, ALL tanks",                  &sCollectMode, 3);
    if (collectChanged) vizSetsSave();

    /* ── Sets (left) + Category toggles (right) ──
     * Two side-by-side groups so the category checkboxes fill the wide
     * empty area to the right of the Save/Load slots instead of stacking
     * underneath them. */
    ImGui::Separator();

    /* Left group: 4 save/load slots + select/clear all. */
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.7f, 0.75f, 0.85f, 1.0f), "Sets");
    ImGui::SameLine();
    if (ImGui::Button("Select all")) vizSetAll(true,  onToggle);
    ImGui::SameLine();
    if (ImGui::Button("Clear all"))  vizSetAll(false, onToggle);
    for (int s = 0; s < VIZ_SET_COUNT; s++) {
        ImGui::PushID(s);
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::InputTextWithHint("##setlabel", "label",
                                     sVizSets[s].label, sizeof(sVizSets[s].label))) {
            vizSetsSave();  /* persist label edits even without re-Save */
        }
        ImGui::SameLine();
        if (ImGui::Button("Save")) vizSetCapture(s);
        ImGui::SameLine();
        if (!sVizSets[s].saved) ImGui::BeginDisabled();
        if (ImGui::Button("Load")) vizSetApply(s, onToggle);
        if (!sVizSets[s].saved) ImGui::EndDisabled();
        ImGui::SameLine();
        if (sVizSets[s].saved)
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "(%d on)", sVizSets[s].keyCount);
        else
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(empty)");
        ImGui::PopID();
    }
    ImGui::EndGroup();

    /* Right group: one tri-state checkbox per category, wrapped to fill
     * the remaining width. Checked = all on, empty = none on, dash (mixed)
     * = some on. Clicking a full box turns the whole category off; an
     * empty/mixed box turns it all on. [#/total] count rides in the label. */
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::BeginGroup();
    {
        int catTotal[VCAT_COUNT] = {0};
        int catOn[VCAT_COUNT]    = {0};
        int n = vizRegistryCount();
        for (int i = 0; i < n; i++) {
            const VizRegistryEntry *e = vizRegistryGet(i);
            if (!e) continue;
            int c = vizMetaFor(e, NULL);
            catTotal[c]++;
            if (e->is_on) catOn[c]++;
        }
        ImGui::TextColored(ImVec4(0.7f, 0.75f, 0.85f, 1.0f), "Categories");

        const ImGuiStyle &st = ImGui::GetStyle();
        float avail   = ImGui::GetContentRegionAvail().x;
        float boxW    = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x; /* the square + gap to text */
        float lineW   = 0.0f;
        bool  lineNew = true;
        for (int c = 0; c < VCAT_COUNT; c++) {
            if (catTotal[c] == 0) continue;
            bool allOn = (catOn[c] == catTotal[c]);
            bool mixed = (catOn[c] > 0 && !allOn);
            char disp[48], lbl[64];
            SDL_snprintf(disp, sizeof(disp), "%s [%d/%d]",
                         kVizCatLabel[c], catOn[c], catTotal[c]);
            SDL_snprintf(lbl, sizeof(lbl), "%s##cat%d", disp, c);
            float itemW = boxW + ImGui::CalcTextSize(disp).x;
            /* Wrap to a new line when this box would overflow the panel. */
            if (!lineNew && lineW + st.ItemSpacing.x + itemW > avail) lineNew = true;
            if (!lineNew) ImGui::SameLine();
            if (mixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
            bool val = allOn;
            if (ImGui::Checkbox(lbl, &val)) vizSetCategory(c, !allOn, onToggle);
            if (mixed) ImGui::PopItemFlag();
            lineW   = lineNew ? itemW : lineW + st.ItemSpacing.x + itemW;
            lineNew = false;
        }
    }
    ImGui::EndGroup();

    /* Filter (case-insensitive substring across id, label, descs). */
    static char sFilter[128] = {0};
    ImGui::SetNextItemWidth(300.0f);
    ImGui::InputTextWithHint("##vizfilter", "Filter (substring, any column)",
                             sFilter, sizeof(sFilter));
    ImGui::SameLine();
    if (ImGui::Button("Clear")) sFilter[0] = '\0';

    /* Type filters: HUD-only / Map-only toggle pills. Drawn below on the
     * "Show:" row, just right of All/None. Press one to show ONLY that type;
     * press both — or neither (the default) — to show both. So HUD pressed =
     * HUD ONLY, but with HUD unpressed you still see HUD rows; it just isn't
     * the only thing. */
    static bool sShowHud = false;
    static bool sShowMap = false;

    /* Category filter is a plain allow-list: a row shows iff its category tag
     * is pressed in. Starts with every tag on (all rows show). "All" presses
     * every tag in (show all), "None" clears every tag (show none). */
    static bool sCatFilter[VCAT_COUNT];
    static bool sCatFilterInit = false;
    if (!sCatFilterInit) {
        for (int c = 0; c < VCAT_COUNT; c++) sCatFilter[c] = true;
        sCatFilterInit = true;
    }
    {
        const ImGuiStyle &st = ImGui::GetStyle();
        ImGui::TextColored(ImVec4(0.6f, 0.65f, 0.75f, 1.0f), "Show:");
        ImGui::SameLine();
        if (ImGui::SmallButton("All##catall")) {
            for (int c = 0; c < VCAT_COUNT; c++) sCatFilter[c] = true;  /* show all categories */
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("None##catnone")) {
            for (int c = 0; c < VCAT_COUNT; c++) sCatFilter[c] = false; /* show no categories */
        }
        /* HUD / Map type pills, right of All/None: press one for X-ONLY. */
        {
            bool *typeFlags[2]      = { &sShowHud, &sShowMap };
            const char *typeLbls[2] = { "HUD##typehud", "Map##typemap" };
            for (int t = 0; t < 2; t++) {
                ImGui::SameLine();
                if (*typeFlags[t]) {
                    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.66f, 0.38f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.30f, 0.72f, 0.42f, 1.0f));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.19f, 0.24f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.27f, 0.33f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.30f, 0.31f, 0.38f, 1.0f));
                }
                if (ImGui::SmallButton(typeLbls[t])) *typeFlags[t] = !*typeFlags[t];
                ImGui::PopStyleColor(3);
            }
        }
        /* Per-category toggle tags, wrapped to the window width (official
         * ImGui button-wrap idiom: after each button, peek whether the next
         * one still fits before the right edge). */
        float rightX = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        for (int c = 0; c < VCAT_COUNT; c++) {
            bool active = sCatFilter[c];
            char lbl[48];
            SDL_snprintf(lbl, sizeof(lbl), "%s##tag%d", kVizCatLabel[c], c);
            if (c == 0) ImGui::SameLine();  /* join the "Show: All" line */
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.66f, 0.38f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.30f, 0.72f, 0.42f, 1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.19f, 0.24f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.27f, 0.33f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.30f, 0.31f, 0.38f, 1.0f));
            }
            if (ImGui::SmallButton(lbl)) sCatFilter[c] = !sCatFilter[c];
            ImGui::PopStyleColor(3);
            /* Keep the next tag on this line only if it fits before the edge. */
            if (c + 1 < VCAT_COUNT) {
                float nextW = ImGui::CalcTextSize(kVizCatLabel[c + 1]).x + st.FramePadding.x * 2.0f;
                float lastX = ImGui::GetItemRectMax().x;
                if (lastX + st.ItemSpacing.x + nextW < rightX) ImGui::SameLine();
            }
        }
    }

    char filterLower[128];
    int  filterLen = 0;
    for (int j = 0; sFilter[j] && j < (int)sizeof(filterLower) - 1; j++) {
        filterLower[j] = (char)tolower((unsigned char)sFilter[j]);
        filterLen      = j + 1;
    }
    filterLower[filterLen] = '\0';

    auto matches = [&](const char *s) -> bool {
        if (filterLen == 0) return true;
        if (!s) return false;
        size_t slen = strlen(s);
        if ((int)slen < filterLen) return false;
        for (size_t k = 0; k + filterLen <= slen; k++) {
            int j = 0;
            for (; j < filterLen; j++) {
                if (tolower((unsigned char)s[k + j]) != filterLower[j]) break;
            }
            if (j == filterLen) return true;
        }
        return false;
    };

    ImGui::Separator();

    if (ImGui::BeginTable("##viz", 7,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("On",       ImGuiTableColumnFlags_WidthFixed,   40.0f);
        ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthFixed,   70.0f);
        ImGui::TableSetupColumn("Type",     ImGuiTableColumnFlags_WidthFixed,   46.0f);
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed,  100.0f);
        ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthFixed,  200.0f);
        ImGui::TableSetupColumn("Short",    ImGuiTableColumnFlags_WidthFixed,  340.0f);
        ImGui::TableSetupColumn("Details",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        int n = build_sort_order();
        for (int k = 0; k < n; k++) {
            int idx = sort_order_buf[k];
            const VizRegistryEntry *e = vizRegistryGet(idx);
            if (!e) continue;
            bool isHud = false;
            int  catIdx = vizMetaFor(e, &isHud);
            const char *typeStr = isHud ? "HUD" : "Map";
            const char *catStr  = kVizCatLabel[catIdx];
            /* Type toggles: skip the row whose type is unchecked. Both-off
             * is treated as both-on so the list never goes fully blank. */
            if (sShowHud != sShowMap && (isHud ? !sShowHud : !sShowMap)) continue;
            /* Category allow-list: show the row iff its category tag is on.
             * "All" turns every tag on (show all), "None" turns them off
             * (show none); individual tags filter to exactly those checked. */
            if (!sCatFilter[catIdx]) continue;
            if (filterLen > 0 &&
                !matches(e->id) && !matches(e->label) &&
                !matches(e->short_desc) && !matches(e->long_desc) &&
                !matches(typeStr) && !matches(catStr)) {
                continue;
            }
            ImGui::TableNextRow();

            /* Highlight a frequently-toggled meta-row (label_overlays
             * is the "tag every shape with its viz_id" debug aid)
             * with a green tint so the user can spot it at a glance
             * even when the list is filtered or scrolled. */
            if (e->id[0] && strcmp(e->id, "label_overlays") == 0) {
                ImU32 bgCol = ImGui::GetColorU32(
                    ImVec4(0.15f, 0.45f, 0.15f, 0.55f));
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, bgCol);
            }

            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(
                e->is_on ? ImVec4(0.40f, 0.95f, 0.40f, 1.0f)
                         : ImVec4(0.55f, 0.55f, 0.55f, 1.0f),
                e->is_on ? "[X]" : "[ ]");

            /* Shortcut column. Only show the hint when it's a real
             * binding ("-" means "no key"). */
            ImGui::TableSetColumnIndex(1);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.55f, 0.85f, 0.95f, 1.0f));
            ImGui::TextUnformatted(
                (e->key_hint[0] && strcmp(e->key_hint, "-") != 0)
                    ? e->key_hint : "");
            ImGui::PopStyleColor();

            /* Type: HUD (screen-anchored) vs Map (world-anchored). */
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(isHud ? ImVec4(0.95f, 0.80f, 0.45f, 1.0f)
                                     : ImVec4(0.55f, 0.80f, 0.95f, 1.0f),
                               "%s", typeStr);

            ImGui::TableSetColumnIndex(3);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.78f, 0.86f, 1.0f));
            ImGui::TextUnformatted(catStr);
            ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(4);
            const char *nameSrc = e->id[0] ? e->id : e->label;
            ImVec4 col = e->is_on ? ImVec4(0.85f, 1.0f, 0.85f, 1.0f)
                                  : ImVec4(0.85f, 0.85f, 0.95f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            char selId[160];
            SDL_snprintf(selId, sizeof(selId), "%s##nm%d", nameSrc, idx);
            if (ImGui::Selectable(selId, false,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                VizRegistryEntry *m = vizRegistryGetMutable(idx);
                if (m) m->is_on = !m->is_on;
                if (onToggle) onToggle(idx);
            }
            ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(5);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.85f, 1.0f));
            ImGui::TextWrapped("%s", e->short_desc);
            ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(6);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.72f, 0.8f, 1.0f));
            ImGui::TextWrapped("%s", e->long_desc);
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }

    ImGui::End();
    ImGui::Render();
    SDL_SetRenderDrawColor(renderer, 22, 24, 32, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);

    if (prev) ImGui::SetCurrentContext(prev);
}
