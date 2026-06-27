/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * braintest_panelwindow.cpp — ImGui tab strip backed by
 * the runtime panel registry.
 *********************************************************/

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

extern "C" {
#include "braintest_panelwindow.h"
#include "braintest_panel_registry.h"
#include "braintest_panel_types.h"
}

/* Built-in "text" renderer — plain TextUnformatted dump of
 * whatever string the brain returned. Exported (no static)
 * so braintest_main.c can register it as the type "text"
 * handler at startup. */
extern "C" void panelRenderText(int /*registry_idx*/, const char *body) {
    if (body && body[0]) {
        ImGui::TextUnformatted(body);
    } else {
        ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.6f, 1.0f),
            "(no data yet — polling...)");
    }
}

static bool          sInitialized = false;
static ImGuiContext *sCtx         = NULL;

/* Per-tab cache: most-recent body text + last-poll wallclock.
 * The window only repolls the *active* tab — switching tabs
 * triggers an immediate poll so the user sees fresh data
 * after a switch, not stale text from the prior session. */
struct TabCache {
    char  *text;
    Uint64 lastPollMs;
};
static TabCache sCache[PANEL_REG_MAX];
static int      sActiveTab    = 0;
static int      sLastActiveTab = -1;

static const Uint64 kPollIntervalMs = PANEL_POLL_INTERVAL_MS;

void panelWindowInit(SDL_Window *window, SDL_Renderer *renderer) {
    if (sInitialized || !window || !renderer) return;
    IMGUI_CHECKVERSION();
    ImGuiContext *prev = ImGui::GetCurrentContext();
    sCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(sCtx);
    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(6, 6);
    s.ItemSpacing   = ImVec2(6, 4);
    s.FramePadding  = ImVec2(6, 3);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    sInitialized = true;
    if (prev) ImGui::SetCurrentContext(prev);
}

void panelWindowShutdown(void) {
    if (!sInitialized) return;
    ImGuiContext *prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(sCtx);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(sCtx);
    sCtx = NULL;
    sInitialized = false;
    if (prev && prev != sCtx) ImGui::SetCurrentContext(prev);
    for (int i = 0; i < PANEL_REG_MAX; i++) {
        free(sCache[i].text);
        sCache[i].text = NULL;
        sCache[i].lastPollMs = 0;
    }
}

void panelWindowProcessEvent(SDL_Event *ev) {
    if (!sInitialized || !sCtx) return;
    ImGuiContext *prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(sCtx);
    ImGui_ImplSDL3_ProcessEvent(ev);
    if (prev) ImGui::SetCurrentContext(prev);
}

static int sLastFollowBot = -1;

static void clearAllCache(void) {
    for (int i = 0; i < PANEL_REG_MAX; i++) {
        free(sCache[i].text);
        sCache[i].text = NULL;
        sCache[i].lastPollMs = 0;
    }
}

void panelWindowRender(SDL_Renderer *renderer, int winW, int winH,
                       int followBot,
                       char *(*onPollPanel)(int panel_idx)) {
    if (!sInitialized || !renderer) return;

    /* On followBot change, drop every cached panel body — the data
     * we have was for the previous bot and re-rendering it under a
     * different brain's renderer would either misparse (different
     * JSON schema) or just confuse the user. Forces a fresh poll. */
    if (followBot != sLastFollowBot) {
        clearAllCache();
        sLastFollowBot = followBot;
        sActiveTab     = 0;
        sLastActiveTab = -1;
    }

    /* Build the visible-tab index list = registry entries owned by
     * the current followBot AND without a shortcut (shortcut'd panels
     * get their own SDL window via braintest_botwindow). We translate
     * "active tab within the visible list" → absolute registry idx
     * for polling/dispatch. */
    int totalN = panelRegistryCount();
    int visible[PANEL_REG_MAX];
    int n = 0;
    for (int i = 0; i < totalN && n < PANEL_REG_MAX; i++) {
        const PanelRegistryEntry *e = panelRegistryGet(i);
        if (e && e->bot_owner == followBot && !e->shortcut[0]) {
            visible[n++] = i;
        }
    }
    if (sActiveTab >= n) sActiveTab = 0;

    /* Poll the active tab if it's due (or the user just switched).
     * We poll under the absolute registry idx so the host's poll
     * callback can read the right Lua expression. */
    if (n > 0 && onPollPanel) {
        Uint64 nowMs = SDL_GetTicks();
        bool   tabChanged = (sActiveTab != sLastActiveTab);
        int    absIdx     = visible[sActiveTab];
        if (tabChanged ||
            nowMs - sCache[absIdx].lastPollMs >= kPollIntervalMs) {
            char *fresh = onPollPanel(absIdx);
            if (fresh) {
                free(sCache[absIdx].text);
                sCache[absIdx].text = fresh;
            }
            sCache[absIdx].lastPollMs = nowMs;
        }
        sLastActiveTab = sActiveTab;
    }

    ImGuiContext *prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(sCtx);
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
    char title[96];
    SDL_snprintf(title, sizeof(title),
        "Brain panels — bot %d (%d registered)###panels",
        followBot, n);
    ImGui::Begin(title, NULL,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse);

    if (n == 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f),
            "No panels registered for bot %d.", followBot);
        ImGui::TextWrapped(
            "The bot's brain hasn't called braintest_panel_register("
            "\"name\", \"type\", \"lua_expr\"). Add a call in your "
            "brain's open() — see brains/GoalHunter/init.lua for "
            "an example.");
        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 22, 24, 32, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        if (prev) ImGui::SetCurrentContext(prev);
        return;
    }

    if (ImGui::BeginTabBar("##panels", ImGuiTabBarFlags_None)) {
        for (int v = 0; v < n; v++) {
            int absIdx = visible[v];
            const PanelRegistryEntry *e = panelRegistryGet(absIdx);
            if (!e) continue;
            if (ImGui::BeginTabItem(e->name)) {
                sActiveTab = v;
                ImGui::BeginChild("##panelbody",
                    ImVec2(0, 0), false,
                    ImGuiWindowFlags_HorizontalScrollbar);
                const char *body = sCache[absIdx].text;
                /* Route the body to this panel's registered renderer. If the
                 * type has NO renderer, DON'T silently dump raw JSON via a
                 * "text" fallback — print + log the mismatch (once per type)
                 * and show it in-panel, so a missing/mis-registered renderer
                 * is obvious instead of looking like brain garbage. */
                PanelRenderFn fn = panelTypeFind(e->type);
                if (fn) {
                    fn(absIdx, body);
                } else {
                    static char sLastWarned[64] = "";
                    if (SDL_strcmp(sLastWarned, e->type) != 0) {
                        SDL_strlcpy(sLastWarned, e->type, sizeof sLastWarned);
                        char reg[512]; reg[0] = '\0';
                        int nt = panelTypeCount();
                        for (int ti = 0; ti < nt; ti++) {
                            const char *nm = panelTypeNameAt(ti);
                            if (!nm) continue;
                            size_t l = SDL_strlen(reg);
                            SDL_snprintf(reg + l, sizeof(reg) - l,
                                         "%s%s", l ? ", " : "", nm);
                        }
                        fprintf(stderr,
                            "PANEL ERROR: no renderer registered for type '%s' "
                            "(panel '%s'). Registered: [%s]\n",
                            e->type, e->name, reg);
                    }
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                        "No renderer for panel type \"%s\".", e->type);
                    ImGui::TextWrapped(
                        "Its panelTypeRegister() didn't run or used a different "
                        "name (check the 1.0/1.5 split). See stderr for the "
                        "registered type list. Raw JSON suppressed.");
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
    ImGui::Render();
    SDL_SetRenderDrawColor(renderer, 22, 24, 32, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);

    if (prev) ImGui::SetCurrentContext(prev);
}
