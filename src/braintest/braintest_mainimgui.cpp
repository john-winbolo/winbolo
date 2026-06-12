#include <SDL3/SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

extern "C" {
#include "braintest_mainimgui.h"
}

static bool          sInitialized = false;
static ImGuiContext *sCtx         = NULL;
static SDL_Window   *sWindow      = NULL;

void mainImGuiInit(SDL_Window *window, SDL_Renderer *renderer) {
    if (sInitialized || !window || !renderer) return;
    IMGUI_CHECKVERSION();
    ImGuiContext *prev = ImGui::GetCurrentContext();
    sCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(sCtx);
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowPadding = ImVec2(8, 8);
    st.ItemSpacing   = ImVec2(6, 4);
    st.FramePadding  = ImVec2(6, 3);
    /* Slightly more transparent backgrounds since we sit on top of
     * the live map and full-opacity panels would obscure too much. */
    ImVec4 *col = st.Colors;
    col[ImGuiCol_WindowBg].w = 0.92f;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    sInitialized = true;
    sWindow      = window;
    if (prev) ImGui::SetCurrentContext(prev);
}

void mainImGuiShutdown(void) {
    if (!sInitialized) return;
    ImGuiContext *prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(sCtx);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(sCtx);
    sCtx = NULL;
    sInitialized = false;
    if (prev && prev != sCtx) ImGui::SetCurrentContext(prev);
}

void mainImGuiProcessEvent(SDL_Event *ev) {
    if (!sInitialized || !sCtx) return;
    /* Only forward events delivered to the main window — keeps our
     * ImGui IO clean of clicks/keys aimed at the V/panel/pool
     * sub-windows. */
    Uint32 wid = 0;
    switch (ev->type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:    wid = ev->button.windowID; break;
        case SDL_EVENT_MOUSE_MOTION:       wid = ev->motion.windowID; break;
        case SDL_EVENT_MOUSE_WHEEL:        wid = ev->wheel.windowID;  break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:             wid = ev->key.windowID;    break;
        case SDL_EVENT_TEXT_INPUT:         wid = ev->text.windowID;   break;
        case SDL_EVENT_TEXT_EDITING:       wid = ev->edit.windowID;   break;
        default: wid = SDL_GetWindowID(sWindow); break;
    }
    if (wid != 0 && wid != SDL_GetWindowID(sWindow)) return;
    ImGuiContext *prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(sCtx);
    ImGui_ImplSDL3_ProcessEvent(ev);
    if (prev) ImGui::SetCurrentContext(prev);
}

void mainImGuiBeginFrame(void) {
    if (!sInitialized || !sCtx) return;
    ImGui::SetCurrentContext(sCtx);
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void mainImGuiEndFrame(SDL_Renderer *renderer) {
    if (!sInitialized || !sCtx) return;
    ImGui::SetCurrentContext(sCtx);
    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
}

bool mainImGuiWantsMouse(void) {
    if (!sInitialized || !sCtx) return false;
    ImGuiContext *prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(sCtx);
    bool want = ImGui::GetIO().WantCaptureMouse;
    if (prev) ImGui::SetCurrentContext(prev);
    return want;
}

bool mainImGuiWantsTextInput(void) {
    if (!sInitialized || !sCtx) return false;
    ImGuiContext *prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(sCtx);
    bool want = ImGui::GetIO().WantTextInput;
    if (prev) ImGui::SetCurrentContext(prev);
    return want;
}

void mainImGuiRenderShortcuts(bool *visible) {
    if (!visible || !*visible) return;
    if (!sInitialized || !sCtx) return;
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(440, 540), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("BrainTest – Shortcuts (F1)", &open,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        if (!open) *visible = false;
        return;
    }

    /* Static list — keep in lockstep with the SDL_EVENT_KEY_DOWN
     * switch in braintest_main.c. Two columns: key, description. */
    struct Row { const char *key; const char *desc; };
    static const Row kPlayback[] = {
        {"Space",   "Pause / resume"},
        {",",       "Step back one frame (auto-pause + playback)"},
        {".",       "Step forward one frame (steps live past end)"},
        {"Home",    "Jump to start of recording (playback)"},
        {"End",     "Jump back to live (latest tick)"},
        {NULL,      NULL},
    };
    static const Row kView[] = {
        {"Tab",     "Cycle to next active bot"},
        {"F",       "Free camera toggle"},
        {"H",       "Top HUD toggle"},
        {"M",       "Manual control toggle"},
        {"X",       "Bare-screen mode (suppress all overlays except HUD)"},
        {"= / +",   "Zoom in"},
        {"- / _",   "Zoom out"},
        {"V",       "Toggle V dialog (visualization filter)"},
        {"S",       "Toggle Shot Simulator panel"},
        {"D",       "Toggle viz-detail inspector (clickable map primitives + body text)"},
        {"L",       "HUD edit: unlock/lock overlay drag (lock saves layout)"},
        {"Shift+L", "HUD edit: reset overlay positions to defaults (confirm)"},
        {"F1",      "Toggle this shortcut list"},
        {"Esc",     "Quit"},
        {NULL,      NULL},
    };
    static const Row kOverlays[] = {
        {"1",       "Influence overlay"},
        {"2",       "Danger overlay"},
        {"3",       "Front-line overlay"},
        {"4",       "A* / Dijkstra path overlay"},
        {"5",       "Cost-to heatmap (Shift = lower danger weight)"},
        {"6",       "Fog of war"},
        {"7",       "Dijkstra heatmap (Shift = cycle slate)"},
        {"9",       "Values overlay"},
        {"0",       "Clear all native overlays"},
        {NULL,      NULL},
    };

    auto drawSection = [](const char *title, const Row *rows) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.30f, 1.0f), "%s", title);
        ImGui::Separator();
        if (ImGui::BeginTable(title, 2,
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
            for (const Row *r = rows; r->key; r++) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ImVec4(0.7f, 0.95f, 1.0f, 1.0f), "%s", r->key);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextWrapped("%s", r->desc);
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
    };

    drawSection("Playback / time",     kPlayback);
    drawSection("View / windows",      kView);
    drawSection("Overlays (numeric)",  kOverlays);

    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                       "Bot-registered panel keys (lowercase a-z) are added");
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                       "dynamically when the brain registers them.");

    ImGui::End();
    if (!open) *visible = false;
}
