/*
 * android_gamesetup.cpp — Touch-friendly ImGui game setup dialog for Android.
 *
 * Based on src/gui/sdl3/dialogs/imgui_gamesetup.cpp but adapted for
 * fullscreen Android display with scaled fonts and touch-sized controls.
 * Runs its own blocking SDL event loop.
 *
 * On Android, map selection uses a list of bundled maps from the assets
 * directory instead of a native file picker.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <SDL3/SDL.h>

#include "../common/wb_log.h"

#include "imgui.h"
#include "../gui/imgui_theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

extern "C" {
#include "../gui/sdl3/sdl3draw.h"
#include "../gui/sdl3/bg_game.h"
#include "../gui/gamefront.h"
#include "../gui/lang.h"
#include "global.h"
#include "client_mapload.h"
#include "client_sim.h"

/* The desktop imgui_gamesetup screen has been removed in favor of the
 * lobby flow. The Android port still uses its own setup dialog (this
 * file) — declare the entry point inline so we don't have to keep
 * around a one-symbol header for it. */
int imguiGameSetupShow(struct ClientSim *cs);
}

#define NUM_SECONDS 60

/* Load font data from Android assets via SDL_IOFromFile */
static unsigned char *loadFontFromAssets(const char *path, int *outSize) {
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) return nullptr;
    Sint64 size = SDL_GetIOSize(io);
    if (size <= 0) { SDL_CloseIO(io); return nullptr; }
    unsigned char *buf = (unsigned char *)IM_ALLOC((size_t)size);
    if (!buf) { SDL_CloseIO(io); return nullptr; }
    size_t read = SDL_ReadIO(io, buf, (size_t)size);
    SDL_CloseIO(io);
    if ((Sint64)read != size) { IM_FREE(buf); return nullptr; }
    *outSize = (int)size;
    return buf;
}

/* Scan for .map files in data/maps/ assets directory */
#define MAX_MAP_FILES 32
static char mapFiles[MAX_MAP_FILES][256];
static const char *mapLabels[MAX_MAP_FILES + 1]; /* +1 for inbuilt */
static int mapCount = 0;

static void scanBundledMaps(void) {
    mapCount = 0;

    /* Always include the inbuilt map as first option */
    SDL_strlcpy(mapFiles[0], "", sizeof(mapFiles[0]));
    mapLabels[0] = langGetText(STR_MAPCHOOSER_EVERARD);
    mapCount = 1;

    /* Try to enumerate map files from the assets */
    char **files = SDL_GlobDirectory("data/maps", "*.map", SDL_GLOB_CASEINSENSITIVE, NULL);
    if (files) {
        for (int i = 0; files[i] && mapCount < MAX_MAP_FILES; i++) {
            /* files[i] is relative to the glob base, e.g., "Everard Island.map" */
            SDL_snprintf(mapFiles[mapCount], sizeof(mapFiles[mapCount]),
                         "data/maps/%s", files[i]);

            /* Use just the filename without extension as label */
            const char *name = files[i];
            size_t len = SDL_strlen(name);
            if (len > 4) {
                char label[256];
                SDL_strlcpy(label, name, sizeof(label));
                /* Strip .map extension */
                size_t labelLen = SDL_strlen(label);
                if (labelLen > 4 && SDL_strcasecmp(label + labelLen - 4, ".map") == 0) {
                    label[labelLen - 4] = '\0';
                }
                /* Skip if it's "Everard Island" since we already have the inbuilt */
                if (SDL_strcasecmp(label, "Everard Island") != 0) {
                    SDL_strlcpy(mapFiles[mapCount], mapFiles[mapCount], sizeof(mapFiles[mapCount]));
                    /* Store the label — use the mapFiles buffer area after the path */
                    static char storedLabels[MAX_MAP_FILES][256];
                    SDL_strlcpy(storedLabels[mapCount], label, sizeof(storedLabels[mapCount]));
                    mapLabels[mapCount] = storedLabels[mapCount];
                    mapCount++;
                }
            }
        }
        SDL_free(files);
    }
}

extern "C" int imguiGameSetupShow(ClientSim *cs) {
    SDL_Window *window = sdl3DrawGetWindow();
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (!window || !renderer) return 0;

    /* Save and disable logical presentation so ImGui input matches rendering */
    int savedLogW = 0, savedLogH = 0;
    SDL_RendererLogicalPresentation savedLogMode = SDL_LOGICAL_PRESENTATION_DISABLED;
    SDL_GetRenderLogicalPresentation(renderer, &savedLogW, &savedLogH, &savedLogMode);
    SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);

    int screenW, screenH;
    SDL_GetWindowSize(window, &screenW, &screenH);
    if (screenW <= 0 || screenH <= 0) {
        screenW = 1920;
        screenH = 1080;
    }

    SDL_ShowWindow(window);

    /* Set up ImGui context */
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    /* Compute UI scale based on screen height (base: 540px) */
    float uiScale = (float)screenH / 540.0f;
    if (uiScale < 1.0f) uiScale = 1.0f;
    float fontSize = 20.0f * uiScale;

    {
        int fontDataSize = 0;
        unsigned char *fontData = loadFontFromAssets("data/fonts/InterVariable.ttf", &fontDataSize);
        if (!fontData) {
            fontData = loadFontFromAssets("fonts/InterVariable.ttf", &fontDataSize);
        }
        if (fontData) {
            WB_LOG_DEBUG(WB_LOG_CAT_ASSET, "[GameSetup] Loaded font, %d bytes, size=%.0f", fontDataSize, fontSize);
            io.Fonts->AddFontFromMemoryTTF(fontData, fontDataSize, fontSize);
        } else {
            WB_LOG_WARN(WB_LOG_CAT_ASSET, "[GameSetup] Font load failed, using default font scaled");
            ImFontConfig config;
            config.SizePixels = fontSize;
            config.OversampleH = 2;
            config.OversampleV = 2;
            io.Fonts->AddFontDefault(&config);
        }
    }

    /* Touch-friendly style — make the dialog panel semi-transparent */
    {
        ImGuiStyle &style = ImGui::GetStyle();
        style.TouchExtraPadding = ImVec2(8.0f, 8.0f);
        style.ScaleAllSizes(uiScale);
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.12f, 0.85f);
    }

    /* Load current game options */
    char password[MAP_STR_SIZE];
    gameType gt;
    bool hm;
    aiType ai;
    int32_t sd;
    int32_t tlimit;

    password[0] = '\0';
    gameFrontGetGameOptions(password, &gt, &hm, &ai, &sd, &tlimit);

    int gameTypeIdx = (gt == gameOpen) ? 0 : (gt == gameTournament) ? 1 : 2;
    bool hiddenMines = hm;
    int aiIdx = (ai == aiNone) ? 0 : (ai == aiYes) ? 1 : (ai == aiYesAdvantage) ? 2 : 3;

    bool usePassword = (password[0] != '\0');
    char passBuf[MAP_STR_SIZE];
    SDL_strlcpy(passBuf, password, MAP_STR_SIZE);

    bool useStartDelay = (sd > 0);
    int startDelaySec = (sd > 0) ? (sd / (1000 / 20)) : 0;

    bool useTimeLimit = (tlimit != UNLIMITED_GAME_TIME);
    int timeLimitMin = 0;
    if (tlimit != UNLIMITED_GAME_TIME) {
        timeLimitMin = tlimit / (1000 / 20) / NUM_SECONDS;
    }

    /* Scan for bundled maps */
    scanBundledMaps();
    int selectedMap = 0; /* 0 = Everard Island (inbuilt) */

    /* Use shared background game (live bots playing) */
    BgGame *bg = bgGameGetShared();
    bool hasBg = (bg != nullptr);
    Uint64 lastTickTime = SDL_GetTicks();

    float dlgW = (float)screenW;
    float dlgH = (float)screenH;

    int result = 0;
    bool running = true;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }

        /* Tick the background game at fixed rate */
        if (hasBg) {
            bgGameTickFixed(bg, &lastTickTime);
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        /* Full-screen scrollable ImGui window */
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(dlgW, dlgH));
        ImGui::Begin("##GameSetup", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse);

        /* Center content horizontally with padding */
        float contentW = dlgW * 0.7f;
        if (contentW < 400.0f * uiScale) contentW = 400.0f * uiScale;
        if (contentW > dlgW - 40.0f) contentW = dlgW - 40.0f;
        float padX = (dlgW - contentW) * 0.5f;

        ImGui::SetCursorPosX(padX);
        ImGui::BeginGroup();

        ImGui::SetWindowFontScale(1.3f);
        ImGui::TextUnformatted(langGetText(STR_DLGGAMESETUP_TITLE));
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Spacing();

        /* --- Map selection via combo --- */
        ImGui::TextUnformatted(langGetText(STR_DLGGAMESETUP_CHOOSEMAP));
        ImGui::SetNextItemWidth(contentW * 0.8f);
        if (ImGui::BeginCombo("##MapSelect", mapLabels[selectedMap])) {
            for (int i = 0; i < mapCount; i++) {
                bool isSelected = (selectedMap == i);
                if (ImGui::Selectable(mapLabels[i], isSelected)) {
                    selectedMap = i;
                    if (i == 0) {
                        /* Inbuilt map */
                        gameFrontSetFileName((char *)"");
                    } else {
                        /* Try to load the selected map to validate it */
                        bool ok = clientLoadMap(cs, (char *)mapFiles[i], gameOpen, FALSE, 0,
                                              UNLIMITED_GAME_TIME, (char *)"Me", TRUE);
                        if (ok) {
                            gameFrontSetFileName((char *)mapFiles[i]);
                        } else {
                            selectedMap = 0;
                            gameFrontSetFileName((char *)"");
                        }
                    }
                }
                if (isSelected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* --- Game Type --- */
        ImGui::TextUnformatted(langGetText(STR_DLGGAMESETUP_GAMETYPE_LBL));
        ImGui::RadioButton(langGetText(STR_DLGGAMESETUP_RADIO1), &gameTypeIdx, 0);
        ImGui::RadioButton(langGetText(STR_DLGGAMESETUP_RADIO2), &gameTypeIdx, 1);
        ImGui::RadioButton(langGetText(STR_DLGGAMESETUP_RADIO3), &gameTypeIdx, 2);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* --- Options --- */
        ImGui::Checkbox(langGetText(STR_DLGGAMESETUP_HIDDENMINES), &hiddenMines);
        ImGui::Spacing();

        ImGui::TextUnformatted(langGetText(STR_DLGGAMESETUP_AICOMPPLAYERS));
        ImGui::Indent();
        ImGui::RadioButton(langGetText(STR_DLGGAMESETUP_NOAI), &aiIdx, 0);
        ImGui::RadioButton(langGetText(STR_DLGGAMESETUP_ALLOWAI), &aiIdx, 1);
        ImGui::RadioButton(langGetText(STR_DLGGAMESETUP_ALLOWADV), &aiIdx, 2);
        ImGui::RadioButton(langGetText(STR_DLGGAMESETUP_ALLOWFULL), &aiIdx, 3);
        ImGui::Unindent();

        ImGui::Spacing();

        ImGui::Checkbox(langGetText(STR_DLGGAMESETUP_PASSWORD), &usePassword);
        if (usePassword) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160.0f * uiScale);
            ImGui::InputText("##password", passBuf, MAP_STR_SIZE,
                             ImGuiInputTextFlags_Password);
        }

        ImGui::Checkbox(langGetText(STR_DLGGAMESETUP_STARTDELAY), &useStartDelay);
        if (useStartDelay) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f * uiScale);
            char secLabel[64];
            SDL_snprintf(secLabel, sizeof(secLabel), "%s##sd",
                         langGetText(STR_DLGGAMESETUP_SECONDS));
            ImGui::InputInt(secLabel, &startDelaySec, 0, 0);
            if (startDelaySec < 0) startDelaySec = 0;
        }

        ImGui::Checkbox(langGetText(STR_DLGGAMESETUP_TIMELIMIT), &useTimeLimit);
        if (useTimeLimit) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f * uiScale);
            char minLabel[64];
            SDL_snprintf(minLabel, sizeof(minLabel), "%s##tl",
                         langGetText(STR_DLGGAMESETUP_MINUTES));
            ImGui::InputInt(minLabel, &timeLimitMin, 0, 0);
            if (timeLimitMin < 0) timeLimitMin = 0;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* --- OK / Cancel --- */
        {
            float btnW = 140.0f * uiScale;
            float btnH = 50.0f * uiScale;
            float totalW = btnW * 2 + 16.0f * uiScale;
            ImGui::SetCursorPosX(padX + (contentW - totalW) * 0.5f);

            if (ImGui::Button(langGetText(STR_OK), ImVec2(btnW, btnH))) {
                gameType newGt = (gameTypeIdx == 0) ? gameOpen :
                                 (gameTypeIdx == 1) ? gameTournament : gameStrictTournament;
                aiType newAi = (aiIdx == 0) ? aiNone :
                               (aiIdx == 1) ? aiYes :
                               (aiIdx == 2) ? aiYesAdvantage : aiFull;

                int32_t newSd = 0;
                if (useStartDelay && startDelaySec > 0) {
                    newSd = startDelaySec * (1000 / 20);
                }

                int32_t newTl = UNLIMITED_GAME_TIME;
                if (useTimeLimit && timeLimitMin > 0) {
                    newTl = timeLimitMin * NUM_SECONDS * (1000 / 20);
                }

                char newPass[MAP_STR_SIZE];
                newPass[0] = '\0';
                if (usePassword) {
                    SDL_strlcpy(newPass, passBuf, MAP_STR_SIZE);
                }

                gameFrontSetGameOptions(newPass, newGt, hiddenMines, newAi, newSd, newTl, FALSE);

                result = 1;
                running = false;
            }

            ImGui::SameLine(0.0f, 16.0f * uiScale);

            if (ImGui::Button(langGetText(STR_CANCEL), ImVec2(btnW, btnH))) {
                running = false;
            }
        }

        ImGui::EndGroup();
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);

        /* Draw live background game and semi-transparent overlay */
        if (hasBg) {
            bgGameRenderWithOverlay(bg, renderer, screenW, screenH);
        }

        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    /* Tear down ImGui */
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    /* Restore the logical presentation for the game renderer */
    SDL_SetRenderLogicalPresentation(renderer, savedLogW, savedLogH, savedLogMode);

    /* Flush any quit events */
    SDL_FlushEvent(SDL_EVENT_QUIT);

    return result;
}
