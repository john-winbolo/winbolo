/*********************************************************
 * braintest_botwindow.cpp
 *
 * One ImGui-on-SDL window per panel-with-shortcut. Mirrors
 * braintest_vizwindow / braintest_panelwindow — separate
 * ImGuiContext per window so each can be activated, ticked,
 * and rendered independently of the others and the main
 * BrainTest window.
 *********************************************************/

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

extern "C" {
#include "braintest_botwindow.h"
#include "braintest_panel_registry.h"
#include "braintest_panel_types.h"
}

/* One slot per registry idx. SDL window/renderer/ImGui ctx
 * are NULL until the panel is first toggled visible. After
 * that they're kept alive (just hidden via SDL_HideWindow)
 * so subsequent toggles are O(1). */
struct BotWindowState {
    SDL_Window    *window;
    SDL_Renderer  *renderer;
    ImGuiContext  *ctx;
    char          *cachedText;
    Uint64         lastPollMs;
    /* `visible` tracks the actual SDL_Window state (shown vs hidden);
     * `userWanted` tracks "should this window be open when the user
     * returns to its bot." Switching followBot away auto-hides the
     * window but leaves userWanted alone — switching back restores it.
     * Toggling via shortcut key flips both. */
    bool           visible;
    bool           userWanted;
    bool           initialized;
};
static BotWindowState g_slots[PANEL_REG_MAX];

static const Uint64 kPollIntervalMs = PANEL_POLL_INTERVAL_MS;

static bool ensureCreated(int idx, const PanelRegistryEntry *e) {
    BotWindowState *s = &g_slots[idx];
    if (s->initialized) return s->window != NULL;
    s->initialized = true;
    char title[160];
    SDL_snprintf(title, sizeof(title),
                 "BrainTest – %s (bot %d)  [%s]",
                 e->name, e->bot_owner, e->shortcut);
    s->window = SDL_CreateWindow(title, 900, 600,
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!s->window) {
        SDL_Log("WARN: bot window create failed for '%s' (%s)",
                e->name, SDL_GetError());
        return false;
    }
    s->renderer = SDL_CreateRenderer(s->window, NULL);
    if (!s->renderer) {
        SDL_DestroyWindow(s->window); s->window = NULL;
        return false;
    }
    /* Independent ImGui context — same pattern as V/P. */
    IMGUI_CHECKVERSION();
    ImGuiContext *prev = ImGui::GetCurrentContext();
    s->ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(s->ctx);
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowPadding = ImVec2(6, 6);
    st.ItemSpacing   = ImVec2(6, 4);
    st.FramePadding  = ImVec2(6, 3);
    ImGui_ImplSDL3_InitForSDLRenderer(s->window, s->renderer);
    ImGui_ImplSDLRenderer3_Init(s->renderer);
    if (prev) ImGui::SetCurrentContext(prev);
    return true;
}

bool botWindowToggle(int registry_idx) {
    if (registry_idx < 0 || registry_idx >= PANEL_REG_MAX) return false;
    const PanelRegistryEntry *e = panelRegistryGet(registry_idx);
    if (!e || !e->shortcut[0]) return false;
    if (!ensureCreated(registry_idx, e)) return false;
    BotWindowState *s = &g_slots[registry_idx];
    if (s->visible) {
        SDL_HideWindow(s->window);
        s->visible    = false;
        s->userWanted = false;
    } else {
        SDL_ShowWindow(s->window);
        SDL_RaiseWindow(s->window);
        s->visible    = true;
        s->userWanted = true;
    }
    return true;
}

void botWindowProcessEvent(SDL_Event *ev) {
    /* Forward to every live ImGui context. Cheap — usually 0 or 1
     * windows are open at any one time. */
    for (int i = 0; i < PANEL_REG_MAX; i++) {
        BotWindowState *s = &g_slots[i];
        if (!s->ctx) continue;
        ImGuiContext *prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(s->ctx);
        ImGui_ImplSDL3_ProcessEvent(ev);
        if (prev) ImGui::SetCurrentContext(prev);
    }
    /* Also handle close-window-requested for bot windows so the X
     * button hides instead of leaking the resource. */
    if (ev->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        SDL_Window *w = SDL_GetWindowFromEvent(ev);
        for (int i = 0; i < PANEL_REG_MAX; i++) {
            BotWindowState *s = &g_slots[i];
            if (s->window == w && s->visible) {
                SDL_HideWindow(s->window);
                s->visible    = false;
                s->userWanted = false; /* explicit close = stay closed */
                break;
            }
        }
    }
}

bool botWindowWantsTextInput(void) {
    /* No input widgets yet, but keep the symmetry with V. */
    return false;
}

void botWindowRenderAll(int followBot,
                        char *(*onPollPanel)(int registry_idx)) {
    /* First pass: reconcile visibility against followBot.
     *  - Owner mismatch + currently visible → hide but keep userWanted
     *    so we restore on switch-back.
     *  - Owner match + userWanted + currently hidden → re-show.
     * This runs over every initialized slot so swap-state is consistent
     * even for windows the user hasn't pressed the shortcut for this
     * frame. */
    for (int i = 0; i < PANEL_REG_MAX; i++) {
        BotWindowState *s = &g_slots[i];
        if (!s->initialized || !s->window) continue;
        const PanelRegistryEntry *e = panelRegistryGet(i);
        if (!e) continue;
        if (e->bot_owner != followBot) {
            if (s->visible) {
                SDL_HideWindow(s->window);
                s->visible = false; /* userWanted preserved */
            }
        } else if (s->userWanted && !s->visible) {
            SDL_ShowWindow(s->window);
            SDL_RaiseWindow(s->window);
            s->visible = true;
        }
    }

    for (int i = 0; i < PANEL_REG_MAX; i++) {
        BotWindowState *s = &g_slots[i];
        if (!s->visible || !s->window || !s->renderer || !s->ctx) continue;
        const PanelRegistryEntry *e = panelRegistryGet(i);
        if (!e) continue;
        if (e->bot_owner != followBot) continue; /* belt + suspenders */

        /* Poll the panel's data on the same 10Hz cadence as P. */
        Uint64 now = SDL_GetTicks();
        if (onPollPanel
            && now - s->lastPollMs >= kPollIntervalMs) {
            char *fresh = onPollPanel(i);
            if (fresh) {
                free(s->cachedText);
                s->cachedText = fresh;
            }
            s->lastPollMs = now;
        }

        int winW, winH;
        SDL_GetWindowSize(s->window, &winW, &winH);

        ImGuiContext *prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(s->ctx);
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
        char title[160];
        SDL_snprintf(title, sizeof(title), "%s###bw%d", e->name, i);
        ImGui::Begin(title, NULL,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse);

        PanelRenderFn fn = panelTypeFind(e->type);
        if (!fn) fn = panelTypeFind("text");
        if (fn) fn(i, s->cachedText);

        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(s->renderer, 22, 24, 32, 255);
        SDL_RenderClear(s->renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),
                                              s->renderer);
        SDL_RenderPresent(s->renderer);

        if (prev) ImGui::SetCurrentContext(prev);
    }
}

void botWindowShutdownAll(void) {
    for (int i = 0; i < PANEL_REG_MAX; i++) {
        BotWindowState *s = &g_slots[i];
        if (!s->initialized) continue;
        if (s->ctx) {
            ImGuiContext *prev = ImGui::GetCurrentContext();
            ImGui::SetCurrentContext(s->ctx);
            ImGui_ImplSDLRenderer3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext(s->ctx);
            if (prev && prev != s->ctx) ImGui::SetCurrentContext(prev);
            s->ctx = NULL;
        }
        if (s->renderer) { SDL_DestroyRenderer(s->renderer); s->renderer = NULL; }
        if (s->window)   { SDL_DestroyWindow(s->window);     s->window   = NULL; }
        free(s->cachedText); s->cachedText = NULL;
        s->initialized = false;
        s->visible     = false;
    }
}
