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

/* ───────────────────────── Viz "sets" ─────────────────────────────────
 * Three named slots that capture/restore which overlays are enabled, plus
 * Select-all / Clear-all. A set stores the id (or label, for native rows)
 * of every currently-on entry; Load turns those on and everything else off.
 * Persisted to BrainTestVizSets.ini next to BrainTestViz.ini so slots and
 * their labels survive restarts. */
#define VIZ_SET_COUNT      3
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
    int  cur = -1;
    while (fgets(line, sizeof(line), f)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = '\0';
        if (line[0] == '[') {
            int idx;
            if (sscanf(line, "[Set%d]", &idx) == 1 && idx >= 0 && idx < VIZ_SET_COUNT) cur = idx;
            else cur = -1;
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

    /* ── Sets: 3 save/load slots + select/clear all ── */
    if (!sVizSetsLoaded) { vizSetsLoad(); sVizSetsLoaded = true; }
    ImGui::Separator();
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

    /* Filter (case-insensitive substring across id, label, descs). */
    static char sFilter[128] = {0};
    ImGui::SetNextItemWidth(300.0f);
    ImGui::InputTextWithHint("##vizfilter", "Filter (substring, any column)",
                             sFilter, sizeof(sFilter));
    ImGui::SameLine();
    if (ImGui::Button("Clear")) sFilter[0] = '\0';

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

    if (ImGui::BeginTable("##viz", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("On",       ImGuiTableColumnFlags_WidthFixed,   40.0f);
        ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthFixed,   70.0f);
        ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthFixed,  200.0f);
        ImGui::TableSetupColumn("Short",    ImGuiTableColumnFlags_WidthFixed,  340.0f);
        ImGui::TableSetupColumn("Details",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        int n = build_sort_order();
        for (int k = 0; k < n; k++) {
            int idx = sort_order_buf[k];
            const VizRegistryEntry *e = vizRegistryGet(idx);
            if (!e) continue;
            if (filterLen > 0 &&
                !matches(e->id) && !matches(e->label) &&
                !matches(e->short_desc) && !matches(e->long_desc)) {
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

            ImGui::TableSetColumnIndex(2);
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

            ImGui::TableSetColumnIndex(3);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.85f, 1.0f));
            ImGui::TextWrapped("%s", e->short_desc);
            ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(4);
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
