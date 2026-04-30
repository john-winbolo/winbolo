/*********************************************************
 * braintest_vizwindow.cpp — ImGui table backed by the
 * runtime viz registry. See header for design.
 *********************************************************/

#include <SDL3/SDL.h>
#include <cctype>
#include <cstdio>
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

    if (ImGui::BeginTable("##viz", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("On",      ImGuiTableColumnFlags_WidthFixed,   50.0f);
        ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthFixed,  200.0f);
        ImGui::TableSetupColumn("Short",   ImGuiTableColumnFlags_WidthFixed,  340.0f);
        ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
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
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(
                e->is_on ? ImVec4(0.40f, 0.95f, 0.40f, 1.0f)
                         : ImVec4(0.55f, 0.55f, 0.55f, 1.0f),
                e->is_on ? "[X]" : "[ ]");

            ImGui::TableSetColumnIndex(1);
            const char *nameSrc = e->id[0] ? e->id : e->label;
            char nameBuf[128];
            SDL_snprintf(nameBuf, sizeof(nameBuf), "%s  (%s)",
                         nameSrc, e->key_hint);
            ImVec4 col = e->is_on ? ImVec4(0.85f, 1.0f, 0.85f, 1.0f)
                                  : ImVec4(0.85f, 0.85f, 0.95f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            char selId[160];
            SDL_snprintf(selId, sizeof(selId), "%s##nm%d", nameBuf, idx);
            if (ImGui::Selectable(selId, false,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                VizRegistryEntry *m = vizRegistryGetMutable(idx);
                if (m) m->is_on = !m->is_on;
                if (onToggle) onToggle(idx);
            }
            ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(2);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.85f, 1.0f));
            ImGui::TextWrapped("%s", e->short_desc);
            ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(3);
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
