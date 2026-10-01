/*
 * imgui_game_info.cpp - ImGui game information window for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "imgui_game_info.h"
#include "imgui_main_menu.h"
#include "imgui_context.h"
#include "imgui_comments.h"
#include "imgui.h"
#include "../../gui/lang.h"
/* Plain C, no bolo headers — see the note at the top of that header. */
#include "../game_settings_blob.h"
/* The same for the scripts and rule changes a recording carries. */
#include "../lv_scripts.h"
/* And for the scenario's scores and the server's tick. */
#include "../lv_presentation.h"
#include "../../gui/sim_rules_phrase.h"

#include <SDL3/SDL_stdinc.h>   /* SDL_utf8strlcpy */

#include <cstdio>
#include <cstring>
#include <ctime>

/* External functions from backend - using C types directly */
extern "C" {
    unsigned char lv_screenGetNumPlayers(void);
    int32_t lv_screenGetGameTimeLeft(void);
    int32_t lv_screenGetGameStartDelay(void);
    void lv_screenGetMapName(char *buffer);
    void lv_screenGetPlayerName(char *buffer, unsigned char player, size_t destSize);
    int lv_screenGetGameSettings(unsigned char *out, int maxLen);
    bool lv_playersIsInUse(unsigned char playerNumber);
    bool lv_screenGetLoggedPlayerName(unsigned char slot, char *dest, size_t destSize);
}

/* Game info state */
static char s_map_name[256] = "";
static char s_version[32] = "";
static char s_game_type[32] = "";
static char s_hidden_mines[16] = "";
static char s_computer_tanks[32] = "";
static char s_time_limit[64] = "";
static char s_start_delay[64] = "";
static char s_wbn_key[64] = "";
static char s_start_time[64] = "";
static int s_num_players = 0;
/* TRUE while s_time_limit holds the replayed world's running clock rather
 * than a recorded limit: imgui_game_info_update overwrites it every frame the
 * round has time left on it, and that beats either recorded source. */
static bool s_time_limit_is_live = false;

/* Game type enum from backend.h, which numbers it from 1. The .wbv header
 * and the settings event both carry these values. */
enum {
    gameOpen = 1,
    gameTournament,
    gameStrictTournament,
    gameScripted
};

/* AI type enum */
enum {
    aiNone = 0,
    aiYes = 1,
    aiYesAdvantage = 2,
    aiFull = 3
};

/* View policy, as the settings payload packs it. Matches ViewPolicy in
 * src/bolo/public/view_policy.h; spelled out here because this panel
 * deliberately includes no bolo headers, bar sim_rules_names.h for the
 * rule names. */
enum {
    viewPolicyAlways = 0,
    viewPolicyKey = 1,
    viewPolicyDecay = 2,
    viewPolicyOff = 3
};

/* Which block of squares the map overview keeps live round the player's
 * own tank. Matches OverviewWindow in src/bolo/public/view_policy.h,
 * spelled out here for the same reason as the policies above. Line of
 * sight needs no mirror: the payload carries it as one bit. */
enum {
    overviewWindowExpanded = 0,
    overviewWindowClassic = 1,
    overviewWindowNone = 2
};

/* Most bytes the event can carry — it frames the payload behind a single
 * length byte. Matches LV_GAME_SETTINGS_MAX in the log viewer's backend.h,
 * which this panel does not include. */
#define GAME_SETTINGS_MAX_LEN 255

/* Lobby settings the recording carries, decoded from that payload. Kept
 * apart from the strings above: those come from the .wbv header, written
 * when the lobby opened, and the two arrive independently. */
static bool s_have_settings = false;
static int  s_view_policy[3] = {0, 0, 0};
static int  s_view_decay[3] = {0, 0, 0};
static bool s_classic_mode = false;
static bool s_allies_in_trees = false;
/* A recording without the settings-flags byte reads as off. */
static bool s_positional_sound = false;
/* A log with no settings record, or one written before these two bits
 * existed, has both clear — which is the expanded window with nothing
 * blocking sight, what the game did before the settings existed. */
static int  s_overview_window = overviewWindowExpanded;
static bool s_line_of_sight = false;
static bool s_ranked = false;
/* Recorded and decoded, but lobby administrivia the panel does not draw. */
static bool s_auto_lock = false;
static bool s_password_set = false;
static bool s_allow_new_players = false;
static uint32_t s_lobby_locks = 0;
/* "Allow smart pings" as the round was played. A recording written before
 * the settings-flags byte existed has none, and that reads as allowed. */
static bool s_smart_pings_allowed = true;
/* The four rows the header carries too. The event is what the round was
 * actually played under, so where it is present these win. */
static char s_ev_game_type[32] = "";
static char s_ev_hidden_mines[16] = "";
static char s_ev_computer_tanks[32] = "";
static char s_ev_time_limit[64] = "";

void lv_imgui_game_info_init(void) {
    /* Clear all fields initially */
    lv_imgui_game_info_clear();
}

void lv_imgui_game_info_clear(void) {
    s_map_name[0] = '\0';
    s_version[0] = '\0';
    s_game_type[0] = '\0';
    s_hidden_mines[0] = '\0';
    s_computer_tanks[0] = '\0';
    s_time_limit[0] = '\0';
    s_start_delay[0] = '\0';
    s_wbn_key[0] = '\0';
    s_start_time[0] = '\0';
    s_num_players = 0;
    s_time_limit_is_live = false;
    s_have_settings = false;
    s_smart_pings_allowed = true;
}

/* Called from lv_frontEndSetGameInformation in main.c */
void lv_imgui_game_info_set(int clear, unsigned char versionMajor, unsigned char versionMinor, unsigned char versionRevision,
                         char *mapName, unsigned char gameType, int hiddenMines, unsigned char aiType,
                         int32_t startDelay, int32_t timeLimit, unsigned char *wbnKey, int32_t startTime) {
    if (clear) {
        lv_imgui_game_info_clear();
        return;
    }
    
    /* Version string */
    snprintf(s_version, sizeof(s_version), "%d.%d%d", versionMajor, versionMinor, versionRevision);
    
    /* Map name */
    strncpy(s_map_name, mapName, sizeof(s_map_name) - 1);
    s_map_name[sizeof(s_map_name) - 1] = '\0';
    
    /* Game type — copy from the localized string table. */
    {
        langid id;
        switch (gameType) {
            case gameOpen:             id = STR_DLGGAMEINFO_OPEN;   break;
            case gameTournament:       id = STR_DLGGAMEINFO_TOURN;  break;
            case gameStrictTournament: id = STR_DLGGAMEINFO_STRICT; break;
            case gameScripted:         id = STR_DLGGAMEINFO_SCRIPTED; break;
            default:                   id = STR_UNKNOWN;            break;
        }
        snprintf(s_game_type, sizeof(s_game_type), "%s", langGetText(id));
    }

    /* Hidden mines */
    snprintf(s_hidden_mines, sizeof(s_hidden_mines), "%s",
             langGetText(hiddenMines ? STR_YES : STR_NO));

    /* Computer tanks */
    {
        langid id;
        switch (aiType) {
            case aiNone:         id = STR_NO;                  break;
            case aiYes:          id = STR_YES;                 break;
            case aiYesAdvantage: id = STR_DLGGAMEINFO_AIADV;   break;
            case aiFull:         id = STR_DLGGAMEINFO_FULLADV; break;
            default:             id = STR_UNKNOWN;             break;
        }
        snprintf(s_computer_tanks, sizeof(s_computer_tanks), "%s", langGetText(id));
    }

    /* Time limit */
    if (timeLimit == -1) { /* UNLIMITED_GAME_TIME */
        snprintf(s_time_limit, sizeof(s_time_limit), "%s",
                 langGetText(STR_DLGGAMEINFO_UNLIMITED));
    } else {
        /* Convert ticks to minutes */
        int32_t minutes = timeLimit / 60 / 50; /* 50 ticks per second */
        MessageArgs args = {};
        args.number = (int)(minutes + 1);
        snprintf(s_time_limit, sizeof(s_time_limit), "%s",
                 langGetTextFmt(STR_DLGGAMEINFO_TIMEREMAINING, &args));
    }

    /* Start delay */
    if (startDelay > 0) {
        int32_t seconds = startDelay / 50; /* 50 ticks per second */
        MessageArgs args = {};
        args.number = (int)seconds;
        snprintf(s_start_delay, sizeof(s_start_delay), "%s",
                 langGetTextFmt(STR_LV_SECONDS, &args));
    } else {
        s_start_delay[0] = '\0';
    }
    
    /* WBN key */
    if (wbnKey && wbnKey[0] != '\0') {
        strncpy(s_wbn_key, (char*)wbnKey, 32);
        s_wbn_key[32] = '\0';
    } else {
        s_wbn_key[0] = '\0';
    }
    /* Push to the comments panel so it can fetch when the user opens it */
    lv_imgui_comments_set_key(s_wbn_key);
    
    /* Start time */
    if (startTime > 0) {
        time_t t = startTime;
        struct tm tm_buf;
#ifdef _WIN32
        if (gmtime_s(&tm_buf, &t) == 0) {
            strftime(s_start_time, sizeof(s_start_time), "%a %b %d %H:%M:%S %Y GMT", &tm_buf);
        }
#else
        if (gmtime_r(&t, &tm_buf) != NULL) {
            strftime(s_start_time, sizeof(s_start_time), "%a %b %d %H:%M:%S %Y GMT", &tm_buf);
        }
#endif
    } else {
        s_start_time[0] = '\0';
    }
    
    s_num_players = 0;
}

/* Called every frame from the window body with whatever the decoder has
 * collected. Every multi-byte value in the payload is big-endian. */
void lv_imgui_game_info_set_settings(const unsigned char *payload, int len) {
    LvGameSettings s;

    if (!lvGameSettingsDecode(payload, len, &s)) {
        s_have_settings = false;
        return;
    }

    s_view_policy[0] = s.viewPolicy[0];
    s_view_policy[1] = s.viewPolicy[1];
    s_view_policy[2] = s.viewPolicy[2];
    s_classic_mode = s.classicMode;
    s_allies_in_trees = s.alliesInTrees;

    s_view_decay[0] = s.viewDecay[0];
    s_view_decay[1] = s.viewDecay[1];
    s_view_decay[2] = s.viewDecay[2];

    /* Game type */
    {
        langid id;
        switch (s.gameType) {
            case gameOpen:             id = STR_DLGGAMEINFO_OPEN;   break;
            case gameTournament:       id = STR_DLGGAMEINFO_TOURN;  break;
            case gameStrictTournament: id = STR_DLGGAMEINFO_STRICT; break;
            case gameScripted:         id = STR_DLGGAMEINFO_SCRIPTED; break;
            default:                   id = STR_UNKNOWN;            break;
        }
        snprintf(s_ev_game_type, sizeof(s_ev_game_type), "%s", langGetText(id));
    }

    /* Computer tanks */
    {
        langid id;
        switch (s.aiType) {
            case aiNone:         id = STR_NO;                  break;
            case aiYes:          id = STR_YES;                 break;
            case aiYesAdvantage: id = STR_DLGGAMEINFO_AIADV;   break;
            case aiFull:         id = STR_DLGGAMEINFO_FULLADV; break;
            default:             id = STR_UNKNOWN;             break;
        }
        snprintf(s_ev_computer_tanks, sizeof(s_ev_computer_tanks), "%s", langGetText(id));
    }

    /* Byte 9 flags */
    snprintf(s_ev_hidden_mines, sizeof(s_ev_hidden_mines), "%s",
             langGetText((s.flags & 0x01) ? STR_YES : STR_NO));
    s_auto_lock = (s.flags & 0x04) != 0;
    s_ranked = (s.flags & 0x08) != 0;
    s_password_set = (s.flags & 0x10) != 0;
    s_allow_new_players = (s.flags & 0x20) != 0;
    /* One bit for three values. Classic mode forces None, and classic
     * mode has its own bit, so the pair reads back exactly except for a
     * host who chose None without classic mode: that logs as Classic. */
    s_overview_window = (s.flags & 0x40)
                            ? (s.classicMode ? overviewWindowNone
                                             : overviewWindowClassic)
                            : overviewWindowExpanded;
    s_line_of_sight   = (s.flags & 0x80) != 0;

    /* Time limit — recorded as whole minutes behind an enabled bit, where
     * the header carries ticks. The minutes are meaningless with the bit
     * clear, so that case says unlimited rather than printing a number. */
    if ((s.flags & 0x02) == 0) {
        snprintf(s_ev_time_limit, sizeof(s_ev_time_limit), "%s",
                 langGetText(STR_DLGGAMEINFO_UNLIMITED));
    } else {
        MessageArgs args = {};
        args.number = s.timeMinutes;
        snprintf(s_ev_time_limit, sizeof(s_ev_time_limit), "%s",
                 langGetTextFmt(STR_DLGGAMEINFO_TIMEREMAINING, &args));
    }

    s_lobby_locks = s.lobbyLocks;
    /* The label is "Allow smart pings", so the row says yes when the host
     * left them on. A recording with no settings-flags byte decodes as
     * allowed, which is what those servers did. */
    s_smart_pings_allowed = !s.smartPingsOff;
    s_positional_sound = s.positionalSound;
    s_have_settings = true;
}

/* Update dynamic fields (called periodically) */
void imgui_game_info_update(void) {
    s_num_players = lv_screenGetNumPlayers();
    
    int32_t timeLeft = lv_screenGetGameTimeLeft();
    s_time_limit_is_live = (timeLeft != -1);
    if (timeLeft != -1) { /* UNLIMITED_GAME_TIME */
        int32_t minutes = timeLeft / 60 / 50;
        MessageArgs args = {};
        args.number = (int)(minutes + 1);
        snprintf(s_time_limit, sizeof(s_time_limit), "%s",
                 langGetTextFmt(STR_DLGGAMEINFO_TIMEREMAINING, &args));
    }

    int32_t startDelay = lv_screenGetGameStartDelay();
    if (startDelay > 0) {
        int32_t seconds = startDelay / 50;
        MessageArgs args = {};
        args.number = (int)seconds;
        snprintf(s_start_delay, sizeof(s_start_delay), "%s",
                 langGetTextFmt(STR_LV_SECONDS, &args));
    } else {
        s_start_delay[0] = '\0';
    }
}

/* A rule's value as the lobby's rules popup writes it: whole numbers without
 * a decimal point, anything else to two places with the trailing zeros
 * trimmed, so it agrees with the multiple simRulesPhrase prints beside it. */
static void game_info_rule_number(double value, char *buf, size_t bufLen) {
    size_t len;

    snprintf(buf, bufLen, "%.2f", value);
    if (strchr(buf, '.') == NULL) return;
    len = strlen(buf);
    while (len > 0 && buf[len - 1] == '0') buf[--len] = '\0';
    if (len > 0 && buf[len - 1] == '.') buf[--len] = '\0';
}

/* What the recording's scripts.json says the round ran: the scenario and mod
 * count, the scripts, the rules at the playhead and each rule change the
 * playhead has passed. Draws nothing for a plain recording or a closed log. */
static void game_info_draw_scripts(void) {
    const LvScripts *sc = lv_screenGetScripts();
    if (sc == NULL || !sc->present) {
        return;
    }

    int count = sc->count;
    if (count < 0) count = 0;
    if (count > LV_SCRIPTS_MAX) count = LV_SCRIPTS_MAX;

    /* The scenario line. The first scenario row names it; a round that ran
     * only mods is named after its map. */
    {
        const char *scenario = sc->map;
        int mods = 0;
        bool found = false;
        for (int i = 0; i < count; i++) {
            const LvScriptRow *row = &sc->scripts[i];
            if (strcmp(row->kind, "mod") == 0) {
                mods++;
            } else if (!found && strcmp(row->kind, "scenario") == 0) {
                scenario = row->name[0] != '\0' ? row->name : row->file;
                found = true;
            }
        }
        if (mods == 0) {
            ImGui::TextUnformatted(scenario);
        } else {
            MessageArgs args = {};
            /* A script's name is the script's own text, and a byte count
               can end inside a character: cut it where one ends. */
            SDL_utf8strlcpy(args.string1, scenario, sizeof(args.string1));
            args.number = mods;
            ImGui::TextUnformatted(langGetTextFmt(
                mods == 1 ? STR_LV_INFO_SCENARIO_MODS_1 : STR_LV_INFO_SCENARIO_MODS_N,
                &args));
        }
    }

    /* The scripts, in load order: file, kind and where it came from. */
    if (count > 0) {
        ImGui::SeparatorText(langGetText(STR_LV_INFO_SCRIPTS));
        for (int i = 0; i < count; i++) {
            const LvScriptRow *row = &sc->scripts[i];
            const char *kind = row->kind;
            const char *source = row->source;
            if (strcmp(kind, "scenario") == 0) {
                kind = langGetText(STR_DLGLOBBY_SCENARIO_TAG_SCENARIO);
            } else if (strcmp(kind, "mod") == 0) {
                kind = langGetText(STR_DLGLOBBY_SCENARIO_TAG_MOD);
            }
            if (strcmp(source, "map") == 0) {
                source = langGetText(STR_LV_INFO_SCRIPT_SOURCE_MAP);
            } else if (strcmp(source, "server") == 0) {
                source = langGetText(STR_LV_INFO_SCRIPT_SOURCE_SERVER);
            }
            ImGui::BeginGroup();
            ImGui::TextUnformatted(row->file);
            ImGui::SameLine();
            ImGui::TextDisabled("[%s]", kind);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", source);
            ImGui::EndGroup();
            if (row->description[0] != '\0' && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", row->description);
            }
        }
    }

    /* Every rule the round opened on or changed later, once each, in index
     * order. */
    const LvRuleChange *changes = NULL;
    int changeCount = lv_screenGetRuleChanges(&changes);
    if (changes == NULL || changeCount < 0) changeCount = 0;

    bool listed[SIM_RULE_COUNT] = {};
    bool any = false;
    {
        int ruleCount = sc->ruleCount;
        if (ruleCount < 0) ruleCount = 0;
        if (ruleCount > SIM_RULE_COUNT) ruleCount = SIM_RULE_COUNT;
        for (int i = 0; i < ruleCount; i++) {
            int index = sc->rules[i].index;
            if (index >= 0 && index < SIM_RULE_COUNT) {
                listed[index] = true;
                any = true;
            }
        }
        for (int i = 0; i < changeCount; i++) {
            int index = changes[i].index;
            if (index >= 0 && index < SIM_RULE_COUNT) {
                listed[index] = true;
                any = true;
            }
        }
    }
    if (!any) {
        return;
    }

    uint32_t playhead = lv_screenGetTimeRunning();

    ImGui::SeparatorText(langGetText(STR_DLGLOBBY_SCENARIO_RULES));
    if (ImGui::BeginTable("##gi_rules", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_RULES_COL_RULE),
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_RULES_COL_CLASSIC),
                                ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("00000").x);
        ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_RULES_COL_SCENARIO),
                                ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("00000").x);
        ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_RULES_COL_CHANGE),
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int rule = 0; rule < SIM_RULE_COUNT; rule++) {
            if (!listed[rule]) continue;

            double value = lv_screenRuleValueAt(rule, playhead);
            char   classicText[32];
            char   valueText[32];
            char   phrase[96];
            game_info_rule_number(simRulesClassicValue(rule),
                                  classicText, sizeof(classicText));
            game_info_rule_number(value, valueText, sizeof(valueText));
            simRulesPhrase(rule, value, phrase, sizeof(phrase));

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(simRulesRuleName(rule));
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(classicText);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(valueText);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(phrase);
        }
        ImGui::EndTable();
    }

    /* The changes the playhead has passed, in the order the recording holds
     * them, each at the server's game tick, the clock a script counts in. */
    for (int i = 0; i < changeCount; i++) {
        const LvRuleChange *ch = &changes[i];
        if (ch->ms > playhead) continue;
        if (ch->index < 0 || ch->index >= SIM_RULE_COUNT) continue;

        char phrase[96];
        simRulesPhrase(ch->index, ch->value, phrase, sizeof(phrase));

        MessageArgs args = {};
        args.number = (int)lv_screenServerTickAt(ch->ms);
        /* The phrase comes from the translation table, so it can hold
           characters of more than one byte: cut both where a character
           ends. */
        SDL_utf8strlcpy(args.string1, simRulesRuleName(ch->index),
                        sizeof(args.string1));
        SDL_utf8strlcpy(args.string2, phrase, sizeof(args.string2));
        ImGui::TextUnformatted(langGetTextFmt(STR_LV_INFO_RULE_CHANGE, &args));
    }
}

/* A scenario's scores at the playhead: every slot with a valid score, then
 * every team. Draws nothing when no score is valid, which is every plain
 * recording. */
static void game_info_draw_scores(void) {
    bool any = false;

    for (int i = 0; i < MAX_TANKS && !any; i++) {
        const LvPresScore *s = lv_screenGetScore(SCN_SCORE_KIND_PLAYER, (BYTE)i);
        any = (s != NULL && s->valid);
    }
    for (int t = 1; t < LV_PRES_TEAMS && !any; t++) {
        const LvPresScore *s = lv_screenGetScore(SCN_SCORE_KIND_TEAM, (BYTE)t);
        any = (s != NULL && s->valid);
    }
    if (!any) {
        return;
    }

    ImGui::SeparatorText(langGetText(STR_LV_INFO_SCORES));
    if (!ImGui::BeginTable("##gi_scores", 3,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        return;
    }
    ImGui::TableSetupColumn("##who", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##score", ImGuiTableColumnFlags_WidthFixed,
                            ImGui::CalcTextSize("-00000000").x);

    for (int i = 0; i < MAX_TANKS; i++) {
        const LvPresScore *s = lv_screenGetScore(SCN_SCORE_KIND_PLAYER, (BYTE)i);
        if (s == NULL || !s->valid) continue;

        /* The name the slot plays under now, else the one the recording gave
         * it, else its number. */
        char name[64] = "";
        if (lv_playersIsInUse((unsigned char)i)) {
            lv_screenGetPlayerName(name, (unsigned char)i, sizeof(name));
        } else {
            lv_screenGetLoggedPlayerName((unsigned char)i, name, sizeof(name));
        }
        if (name[0] == '\0') {
            MessageArgs args = {};
            args.number = i;
            snprintf(name, sizeof(name), "%s",
                     langGetTextFmt(STR_LV_INFO_SCORE_SLOT, &args));
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(name);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(s->label);
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%d", (int)s->score);
    }
    for (int t = 1; t < LV_PRES_TEAMS; t++) {
        const LvPresScore *s = lv_screenGetScore(SCN_SCORE_KIND_TEAM, (BYTE)t);
        if (s == NULL || !s->valid) continue;

        MessageArgs args = {};
        args.number = t;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(langGetTextFmt(STR_DLGLOBBY_TEAM_HEADER, &args));
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(s->label);
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%d", (int)s->score);
    }
    ImGui::EndTable();
}

void lv_imgui_game_info_window(void) {
    if (!lv_g_show_game_info_window) {
        return;
    }
    
    {
        ImGuiCond cond = lv_g_reset_window_positions ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
        ImVec2 vp = ImGui::GetMainViewport()->Size;
        /* Tall enough for the settings rows a recorded log adds below the
         * header ones; the user's own size wins once they set one. */
        ImGui::SetNextWindowSize(ImVec2(280, 300), cond);
        ImGui::SetNextWindowPos(ImVec2(vp.x - 280 - 10, 30), cond);
    }

    char gi_title[128];
    snprintf(gi_title, sizeof(gi_title), "%s###gameinfo", langGetText(STR_LV_WIN_GAMEINFO));
    if (ImGui::Begin(gi_title, &lv_g_show_game_info_window, ImGuiWindowFlags_NoCollapse)) {
        /* Reposition window relative to right/bottom edge when viewport is resized */
        float dx, dy;
        if (lv_imgui_context_get_resize_delta(&dx, &dy)) {
            ImVec2 pos = ImGui::GetWindowPos();
            float old_vp_x = ImGui::GetMainViewport()->Size.x - dx;
            float old_vp_y = ImGui::GetMainViewport()->Size.y - dy;
            ImVec2 size = ImGui::GetWindowSize();
            if (pos.x + size.x * 0.5f > old_vp_x * 0.5f) ImGui::SetWindowPos(ImVec2(pos.x + dx, pos.y + dy));
            else if (pos.y + size.y * 0.5f > old_vp_y * 0.5f) ImGui::SetWindowPos(ImVec2(pos.x, pos.y + dy));
        }
        /* Always clamp to keep fully on screen */
        {
            ImVec2 pos = ImGui::GetWindowPos();
            ImVec2 size = ImGui::GetWindowSize();
            ImVec2 vp = ImGui::GetMainViewport()->Size;
            ImVec2 new_pos = pos;
            if (new_pos.x + size.x > vp.x) new_pos.x = vp.x - size.x;
            if (new_pos.y + size.y > vp.y) new_pos.y = vp.y - size.y;
            if (new_pos.x < 0) new_pos.x = 0;
            if (new_pos.y < 0) new_pos.y = 0;
            if (new_pos.x != pos.x || new_pos.y != pos.y) ImGui::SetWindowPos(new_pos);
        }

        /* The lobby settings the recording carries. A loaded file has them
         * from the load-time walk and a live feed picks them up as they
         * arrive, so they are read here each frame rather than pushed once. */
        {
            unsigned char settings[GAME_SETTINGS_MAX_LEN];
            int len = lv_screenGetGameSettings(settings, (int)sizeof(settings));
            lv_imgui_game_info_set_settings(settings, len);
        }

        /* Static fields — use a single MessageArgs reused across rows
         * since each ImGui::Text call consumes the rendered string
         * before the next overwrites the format buffer. */
        {
            MessageArgs args = {};
            strncpy(args.string1, s_map_name, sizeof(args.string1) - 1);
            ImGui::TextUnformatted(langGetTextFmt(STR_LV_INFO_MAP, &args));
        }
        {
            MessageArgs args = {};
            strncpy(args.string1, s_version, sizeof(args.string1) - 1);
            ImGui::TextUnformatted(langGetTextFmt(STR_LV_VERSION_FMT, &args));
        }
        /* These four rows are in the header and in the settings event alike.
         * The header is what the lobby opened with; the event is what the
         * round was played under, so it wins wherever the log has one. */
        {
            MessageArgs args = {};
            strncpy(args.string1, s_have_settings ? s_ev_game_type : s_game_type,
                    sizeof(args.string1) - 1);
            ImGui::TextUnformatted(langGetTextFmt(STR_LV_INFO_GAMETYPE, &args));
        }
        {
            MessageArgs args = {};
            strncpy(args.string1, s_have_settings ? s_ev_hidden_mines : s_hidden_mines,
                    sizeof(args.string1) - 1);
            ImGui::TextUnformatted(langGetTextFmt(STR_LV_INFO_HIDDENMINES, &args));
        }
        {
            MessageArgs args = {};
            strncpy(args.string1, s_have_settings ? s_ev_computer_tanks : s_computer_tanks,
                    sizeof(args.string1) - 1);
            ImGui::TextUnformatted(langGetTextFmt(STR_LV_INFO_COMPUTER_TANKS, &args));
        }

        /* Dynamic fields - update periodically */
        imgui_game_info_update();

        {
            /* A replayed round with time on the clock counts down in
             * s_time_limit, which beats both recorded values; with no clock
             * running the event's limit stands in for the header's. */
            MessageArgs args = {};
            const char *limit = (s_have_settings && !s_time_limit_is_live)
                                    ? s_ev_time_limit : s_time_limit;
            strncpy(args.string1, limit, sizeof(args.string1) - 1);
            ImGui::TextUnformatted(langGetTextFmt(STR_LV_INFO_TIME_LIMIT, &args));
        }
        {
            MessageArgs args = {};
            strncpy(args.string1, s_start_delay, sizeof(args.string1) - 1);
            ImGui::TextUnformatted(langGetTextFmt(STR_LV_INFO_START_DELAY, &args));
        }
        {
            MessageArgs args = {};
            args.number = s_num_players;
            ImGui::TextUnformatted(langGetTextFmt(STR_DLGGAMEINFO_NUMPLAYERS, &args));
        }

        /* WBN key - clickable link */
        if (s_wbn_key[0] != '\0' && strlen(s_wbn_key) == 32) {
            ImGui::TextUnformatted(langGetText(STR_LV_INFO_WBN_KEY));
            ImGui::SameLine();
            char url[128];
            snprintf(url, sizeof(url), "https://www.winbolo.net/gamelog/%s", s_wbn_key);
            ImGui::TextLinkOpenURL(s_wbn_key, url);
        }

        {
            MessageArgs args = {};
            strncpy(args.string1, s_start_time, sizeof(args.string1) - 1);
            ImGui::TextUnformatted(langGetTextFmt(STR_LV_INFO_START_TIME, &args));
        }

        game_info_draw_scripts();
        game_info_draw_scores();

        /* Server visibility rules and the two mode flags, from the settings
         * event. The row labels come from the lobby form and carry no
         * punctuation, so the colon is supplied here — the same shape the
         * live client's Game Information panel uses. */
        if (s_have_settings) {
            static const langid viewLabels[3] = {
                STR_DLGLOBBY_VIEW_PILL,
                STR_DLGLOBBY_VIEW_BASE,
                STR_DLGLOBBY_VIEW_ALLY,
            };
            const char *modes[] = {
                langGetText(STR_DLGLOBBY_VIEW_ALWAYS),
                langGetText(STR_DLGLOBBY_VIEW_KEY),
                langGetText(STR_DLGLOBBY_VIEW_DECAY),
                langGetText(STR_DLGLOBBY_VIEW_OFF),
            };
            for (int r = 0; r < 3; r++) {
                /* Two bits cannot hold an invalid policy today, but the
                 * payload is untrusted input and the field could widen:
                 * fall back to the wire default rather than index past
                 * modes[], as the live client's panel does. */
                int policy = s_view_policy[r];
                if (policy < viewPolicyAlways || policy > viewPolicyOff) {
                    policy = viewPolicyAlways;
                }
                const char *label = langGetText(viewLabels[r]);
                if (policy == viewPolicyDecay) {
                    ImGui::Text("%s: %s %d %s", label, modes[policy], s_view_decay[r],
                                langGetText(STR_DLGLOBBY_VIEW_DECAY_SECS));
                } else {
                    ImGui::Text("%s: %s", label, modes[policy]);
                }
            }
            ImGui::Text("%s: %s", langGetText(STR_DLGLOBBY_CLASSIC_MODE_CB),
                        langGetText(s_classic_mode ? STR_YES : STR_NO));
            ImGui::Text("%s: %s", langGetText(STR_DLGLOBBY_ALLIES_TREES_CB),
                        langGetText(s_allies_in_trees ? STR_YES : STR_NO));
            ImGui::Text("%s: %s", langGetText(STR_DLGLOBBY_POSITIONAL_SOUND_CB),
                        langGetText(s_positional_sound ? STR_YES : STR_NO));
            ImGui::Text("%s: %s", langGetText(STR_DLGLOBBY_OVERVIEW_WINDOW),
                        langGetText(s_overview_window == overviewWindowNone
                                        ? STR_DLGLOBBY_WINDOW_NONE
                                    : s_overview_window == overviewWindowClassic
                                        ? STR_DLGLOBBY_WINDOW_CLASSIC
                                        : STR_DLGLOBBY_WINDOW_EXPANDED));
            ImGui::Text("%s: %s", langGetText(STR_DLGLOBBY_LINE_OF_SIGHT_CB),
                        langGetText(s_line_of_sight ? STR_YES : STR_NO));
            ImGui::Text("%s: %s", langGetText(STR_DLGLOBBY_SMART_PINGS_CB),
                        langGetText(s_smart_pings_allowed ? STR_YES : STR_NO));
            ImGui::Text("%s: %s", langGetText(STR_DLGLOBBY_RANKED),
                        langGetText(s_ranked ? STR_YES : STR_NO));
        }
    }
    ImGui::End();
}