/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 * Name:          lobby_commands.cpp
 * Purpose:       The lobby's outbound command dispatch: the
 *                ready toggle, add bot with its debounce and
 *                remove bot, team assignment, team clearing
 *                and the naming-pool change that re-rolls a
 *                team's bot names, per-bot configuration and
 *                brain selection, and game-setting edits.
 *                Each helper forks between the single-player
 *                host, which drives the in-process ServerSim
 *                directly, and multiplayer, which goes out
 *                over the wire wrappers. Also the per-brain
 *                about.txt metadata cache and the tagline
 *                renderer the bot picker and its gear
 *                tooltip draw from, plus the host's sticky
 *                brain pick that new bots inherit.
 *********************************************************/

#include <cstdio>   /* snprintf — "Bot N" fallback name */
#include <cstring>  /* strncmp — Easy./Medium./Hard. tagline token */

#include <SDL3/SDL.h>

#include "imgui.h"
#include "../../wb_theme.h"  /* g_theme — the BOT pill's colours, the name tag's fallback */
#include "lobby_internal.h"
extern "C" {
#include "client_sim.h"      /* ClientSim + lobby getters; MAX_TANKS, ClientLobbySlot, aiType */
#include "client_net.h"      /* clientSimNetSend* — ready, add/remove bot, team, settings */
#include "brain_list.h"      /* BrainList / BrainListEntry, BRAIN_LIST_MAX, brainListLoadMeta */
#include "server_sim.h"      /* serverSim* T1 wrappers for the SP-host paths */
#include "lobby_bot_pools.h" /* lobbyBotPoolCount / Pick */
#include "playername_validate.h"  /* playerNameValidate / playerNameCompare */
#include "../../../../bolo/public/wire_limits.h"  /* PACKET_MAX_PLAYER_NAME, LST_*, LOBBY_TIME_MINUTES_* */
#include "../../../../server/threads.h"  /* threadsWaitForMutex / Release — SP-host server calls */
#include "../../../../common/mp_diag_log.h"  /* mpDiagLog — settings dispatch trace */
#include "../../../sound.h"      /* soundPlayEffect — the local ready cue */
#include "../../../gamefront.h"  /* gameFrontGetSinglePlayerServerSim */
#include "../../../lang.h"       /* langGetText / langGetTextFmt / MessageArgs / STR_DLGLOBBY_* */
}

/* Lobby command-dispatch state: the per-slot bot-name override flags and
 * the add-bot debounce. All of it belongs to one lobby session, so
 * lobbyCommandReset() clears it on teardown. */
typedef struct LobbyCommandState {
    /* Per-slot tracking of whether the bot's name was manually overridden
     * by the host typing into the name input field.  Cleared when a bot
     * is added or its name is rerolled (those are pool-driven names);
     * set when the user types a name in the AiConfig sub-panel.  Used by
     * the team-naming-pool dropdown to decide which bots to auto-rename
     * when the pool changes — overridden names stay, pool-driven names
     * get a fresh pick from the new pool.
     *
     * UI-side state only (no server propagation). For multiplayer the
     * server picks names so this array would never gate anything; for
     * single-player it's the source of truth. */
    bool   botNameOverridden[MAX_TANKS] = {0};

    /* Add-bot debounce — disables the Add Bot button while a previously-sent
     * request is in flight. Counts current connected lobby slots at click
     * time, locks the button until either (a) the connected count grows to
     * the expected value (server acked) or (b) a 2 s timeout elapses (lost
     * packet / SP path that already returned). Prevents spam-clicks from
     * pushing past MAX_TANKS or otherwise racing the server.
     *
     * Belt-and-suspenders against the lobby-spam crash:
     *   - addBotFrame: ImGui frame number of the last successful send.
     *     Hard-blocks any second add-bot in the same frame, even if it
     *     comes from a different button (per-team header + per-row in the
     *     player table) before the debounce-disabled state can propagate.
     *   - All loops over MAX_TANKS guard the slot accessor against NULL
     *     and treat NULL as "not connected" (the SP path can briefly hold
     *     a partially-initialized slot while serverSimCreateBot is
     *     allocating the ClientSim). */
    Uint32 addBotSentMs                 = 0;
    int    addBotExpectedConn           = 0;
    int    addBotFrame                  = -1;
} LobbyCommandState;

static LobbyCommandState s_commands = {};

/* Players sets the flag when the host types a bot name and clears it when the
 * slot is reused. Indexed by lobby slot, MAX_TANKS entries. */
bool *lobbyCommandBotNameOverridden(void) {
    return s_commands.botNameOverridden;
}

void lobbyCommandReset(void) {
    s_commands = LobbyCommandState{};
}

/* ── Single-player vs multiplayer command dispatch ─────────────────
 * The lobby UI was originally written against the UDP transport — it
 * sends PACKET_LOBBY_* commands and relies on the server broadcasting
 * resulting state changes back via PACKET_LOBBY_STATE.  The single-
 * player path runs the server in-process and has no packet flow, so
 * commands route directly to the local spServerSim and the result is
 * the matching transportUdpServerBroadcast*Chg function fires —
 * those publish ControlEvents on the in-process subscriber bus so
 * the local humanSim sees the new state via clientSimApplyControl.
 *
 * Each helper takes both the ClientSim (to detect single-player) and
 * the Transport pointer (used by the multiplayer path).  When the
 * Transport is null and we're not single-player, the call is a no-op.
 */
void lobbySendReadyToggle(ClientSim *cs, bool ready) {
    /* Both single-player and multiplayer now go through the same
     * ready toggle. The server's all-ready detector trips the
     * lobby→running transition (synchronously via StartGameInPlace
     * when worldPreLoaded, via countdown+StartGame otherwise).
     * Rich presence is driven by the lobby/game loops' throttled
     * gameFrontTickSteamPresence* refreshers, so there's nothing to
     * push from here. */
    clientSimNetSendReady(cs, ready);
    /* Emit the cue locally for our own toggle. The remote echo path
     * (CTRL_LOBBY_SLOT) is gated on pn != myPN, so self never sounds
     * from the network — without this the local player hears nothing. */
    soundPlayEffect(ready ? lobbyReady : lobbyUnready);
}

/* The brain a new bot gets. Only one brain ships, so this is normally its
 * single entry; the name match survives for a dev tree that has frozen a
 * second one alongside it. What the player picks per bot is the DIFFICULTY,
 * not the brain — see lobbyBotDifficulty* below. */
static int lobbyDefaultBrainIdx(const BrainList *bl) {
    if (!bl || bl->count <= 0) return -1;
    for (int i = 0; i < bl->count; i++) {
        if (SDL_strcasecmp(bl->entries[i].name, "GoalHunter_1.7") == 0) return i;
    }
    return 0;  /* sorted newest-first → entry 0 is the newest GoalHunter */
}

/* Bot-brain state that outlives any one lobby session: the host's sticky
 * brain pick and the about.txt metadata cache. Deliberately has no reset
 * function — imguiLobbyFrameReset runs on every lobby→game edge, so
 * clearing this would drop the host's brain choice and re-read the
 * brains/ tree from disk once a round. */
typedef struct LobbyBrainCache {
    /* Sticky "last picked brain" catalogue index. ADD BOT uses this
     * when in range so new bots inherit whatever brain the host last
     * selected (via the per-bot AiConfig "Bot Code" dropdown), instead
     * of always falling back to the server's default brain. 0xFF means
     * "no sticky yet — use the server default"; valid values index into
     * the lobby brain catalogue. Process-scoped. */
    uint8_t        lastChosenBrainIdx = 0xFF;

    LobbyBrainMeta meta[BRAIN_LIST_MAX];
    int            metaCount          = 0;
} LobbyBrainCache;

static LobbyBrainCache s_brains = {};

/* Players writes the host's sticky brain pick from the per-bot Bot Code
 * dropdown. 0xFF, not 0, is the "no sticky yet" value. */
uint8_t *lobbyBrainLastChosenIdx(void) {
    return &s_brains.lastChosenBrainIdx;
}

const LobbyBrainMeta *lobbyBrainMetaFor(const char *name) {
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < s_brains.metaCount; i++) {
        if (SDL_strcasecmp(s_brains.meta[i].name, name) == 0) return &s_brains.meta[i];
    }
    if (s_brains.metaCount >= BRAIN_LIST_MAX) return NULL;
    LobbyBrainMeta *m = &s_brains.meta[s_brains.metaCount++];
    SDL_strlcpy(m->name, name, sizeof(m->name));
    brainListLoadMeta(name, m->tagline, sizeof(m->tagline),
                      m->desc, sizeof(m->desc));
    /* The brain's modes.txt, read once here alongside about.txt. A brain
     * that ships no manifest gets the synthesized single "default" mode,
     * so every caller below can treat the list as always present. */
    brainListLoadModes(name, &m->modes);
    /* The bot's tag colour, decided once per catalogue entry:
     *   1. the brain's own "color: #RRGGBB" line in about.txt (wins, and is
     *      the same on every machine that has the brain files);
     *   2. else the colour this machine remembered for the bot's base name
     *      ("GoalHunter", version dropped, so a new version keeps it);
     *   3. else one derived from that name — a hue hashed from the letters
     *      at a fixed, readable saturation and brightness — and written to
     *      the prefs so it is remembered from then on. */
    char base[BRAIN_LIST_NAME_LEN];
    brainListSplitVersion(name, base, sizeof(base));
    uint32_t rgb = 0;
    if (!brainListLoadColor(name, &rgb) && !gameFrontGetBotTagColor(base, &rgb)) {
        uint32_t h = 2166136261u;                     /* FNV-1a over the name */
        for (const char *c = base; *c; c++) {
            h ^= (uint32_t)SDL_tolower((unsigned char)*c);
            h *= 16777619u;
        }
        float hue = (float)(h % 360u);                /* 0..359 degrees */
        float sat = 0.55f, val = 0.85f;
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(hue / 360.0f, sat, val, r, g, b);
        rgb = ((uint32_t)(r * 255.0f + 0.5f) << 16) |
              ((uint32_t)(g * 255.0f + 0.5f) << 8)  |
               (uint32_t)(b * 255.0f + 0.5f);
        gameFrontSetBotTagColor(base, rgb);
    }
    m->color = rgb;
    return m;
}

void lobbyBotBrainTagColors(ClientSim *cs, int slot,
                            ImU32 *bg, ImU32 *fg, ImU32 *border) {
    /* Fall back to the BOT pill's colours when the catalogue has nothing. */
    ImU32 oBg = g_theme->botTagBg, oFg = g_theme->botTagText, oBd = g_theme->botTagBorder;
    const BrainList *bl = clientSimGetLobbyBrainList(cs);
    if (bl && bl->count > 0) {
        uint8_t cur = clientSimGetLobbyBotBrain(cs, (BYTE)slot);
        if (cur == 0xFF || cur >= bl->count) cur = 0;
        const LobbyBrainMeta *m = lobbyBrainMetaFor(bl->entries[cur].name);
        if (m) {
            /* Border is the colour itself, dimmed a little; the text is the
             * same colour lifted toward white so it reads on the dark pill;
             * the pill background matches the other row badges. */
            int r = (int)((m->color >> 16) & 0xFF);
            int g = (int)((m->color >> 8) & 0xFF);
            int b = (int)(m->color & 0xFF);
            oBd = IM_COL32(r * 72 / 100, g * 72 / 100, b * 72 / 100, 255);
            oFg = IM_COL32(r + (255 - r) * 45 / 100,
                           g + (255 - g) * 45 / 100,
                           b + (255 - b) * 45 / 100, 255);
        }
    }
    if (bg) *bg = oBg;
    if (fg) *fg = oFg;
    if (border) *border = oBd;
}

/* ── Bot modes ───────────────────────────────────────────────────────
 * Which modes a bot can be run in, and which difficulty levels each mode
 * offers, is the BRAIN's answer, not the lobby's: it comes from the
 * brain's modes.txt, which ships with the brain and is therefore readable
 * on every client before the game starts. The lobby only picks an index
 * into that list and sends it.
 *
 * A slot still on the 0xFF "server default" brain resolves to catalogue
 * entry 0, the same rule the row's brain name uses. */
const BrainModes *lobbyBotModesFor(ClientSim *cs, int slot) {
    const BrainList *bl = clientSimGetLobbyBrainList(cs);
    if (!bl || bl->count <= 0) return NULL;
    uint8_t cur = clientSimGetLobbyBotBrain(cs, (BYTE)slot);
    if (cur == 0xFF || cur >= bl->count) cur = 0;
    const LobbyBrainMeta *m = lobbyBrainMetaFor(bl->entries[cur].name);
    return m ? &m->modes : NULL;
}

void lobbyBotModeAndLevel(ClientSim *cs, int slot,
                          int *outMode, int *outLevel) {
    int mode = clientSimGetLobbyBotMode(cs, (BYTE)slot);
    int level = clientSimGetLobbyBotDifficulty(cs, (BYTE)slot);
    const BrainModes *modes = lobbyBotModesFor(cs, slot);
    if (modes) {
        if (mode < 0 || mode >= modes->modeCount) mode = 0;
        const BrainMode *m = &modes->modes[mode];
        if (level < 0 || level >= m->levelCount) level = m->defaultLevel;
    } else {
        /* No catalogue yet: fall back to the pre-manifest shape so the row
         * still renders something honest rather than an empty tag. */
        if (mode != 0) mode = 0;
        if (level < 0 || level > BOT_DIFFICULTY_MAX) level = BOT_DIFFICULTY_HARD;
    }
    if (outMode)  *outMode  = mode;
    if (outLevel) *outLevel = level;
}

/* Is this the brain's DEFAULT mode — the one every ordinary game uses, and
 * the only one whose three levels have hand-written lang strings? Judged by
 * the mode's key rather than its index so a brain whose first section is
 * named something else does not borrow the Easy/Medium/Hard wording. */
bool lobbyBotModeIsDefault(const BrainModes *modes, int mode) {
    if (!modes || mode < 0 || mode >= modes->modeCount) return true;
    return SDL_strcasecmp(modes->modes[mode].key, "default") == 0;
}

/* May this mode's levels be WORDED from the lang strings? Only when it is
 * the default mode AND its levels are still exactly easy / medium / hard in
 * that order — the three STR_BOT_DIFF_* blurbs describe those and nothing
 * else. A modes.txt that renames or extends the default mode's levels gets
 * its own labels shown instead of three strings that would quietly lie. */
bool lobbyBotModeUsesLangLevels(const BrainModes *modes, int mode) {
    if (!lobbyBotModeIsDefault(modes, mode)) return false;
    if (!modes || mode < 0 || mode >= modes->modeCount) return true;
    const BrainMode *m = &modes->modes[mode];
    if (m->levelCount != BOT_DIFFICULTY_MAX + 1) return false;
    for (int i = 0; i <= BOT_DIFFICULTY_MAX; i++) {
        if (SDL_strcasecmp(m->levels[i].key, botDifficultyName((uint8_t)i)) != 0) {
            return false;
        }
    }
    return true;
}

/* ── Bot difficulty presentation ─────────────────────────────────────
 * One brain plays every difficulty, so the difficulty owns the wording
 * (lang.h STR_BOT_DIFF_*) and the brain's about.txt only describes the
 * brain. Out-of-range difficulties read as Hard, matching
 * botDifficultyName. These lang strings describe the DEFAULT mode's three
 * levels only; a manifest-defined mode's levels are data and show their
 * own label. */

unsigned int lobbyBotDifficultyLabelId(uint8_t difficulty) {
    switch (difficulty) {
        case BOT_DIFFICULTY_EASY:   return STR_DLGLOBBY_BOTCFG_EASY;
        case BOT_DIFFICULTY_MEDIUM: return STR_DLGLOBBY_BOTCFG_MEDIUM;
        default:                    return STR_DLGLOBBY_BOTCFG_HARD;
    }
}

unsigned int lobbyBotDifficultyTaglineId(uint8_t difficulty) {
    switch (difficulty) {
        case BOT_DIFFICULTY_EASY:   return STR_BOT_DIFF_TAG_EASY;
        case BOT_DIFFICULTY_MEDIUM: return STR_BOT_DIFF_TAG_MEDIUM;
        default:                    return STR_BOT_DIFF_TAG_HARD;
    }
}

unsigned int lobbyBotDifficultyDescId(uint8_t difficulty) {
    switch (difficulty) {
        case BOT_DIFFICULTY_EASY:   return STR_BOT_DIFF_DESC_EASY;
        case BOT_DIFFICULTY_MEDIUM: return STR_BOT_DIFF_DESC_MEDIUM;
        default:                    return STR_BOT_DIFF_DESC_HARD;
    }
}

/* The bot's brain name without its version suffix — "GoalHunter_1.7" reads
 * as "GoalHunter" on a lobby row, because the version is a build detail and
 * the row has no space for it. A slot still on the 0xFF "server default"
 * sentinel is shown as the catalogue's first (newest) entry, which is what
 * the server default resolves to in every shipping build. Writes "" when
 * there is no catalogue yet (a client whose brain list hasn't arrived). */
void lobbyBotBrainBaseName(ClientSim *cs, int slot, char *out, size_t outSz) {
    if (!out || outSz == 0) return;
    out[0] = '\0';
    const BrainList *bl = clientSimGetLobbyBrainList(cs);
    if (!bl || bl->count <= 0) return;
    uint8_t cur = clientSimGetLobbyBotBrain(cs, (BYTE)slot);
    if (cur == 0xFF || cur >= bl->count) cur = 0;
    brainListSplitVersion(bl->entries[cur].name, out, outSz);
}

/* Render a one-line tagline. When `difficulty` is a BOT_DIFFICULTY_*
 * value, the leading token (up to and including the first full stop) is
 * coloured green / amber / red for that difficulty; the colour comes from
 * the index, never from the words, so a translated tagline keeps it. Pass
 * -1 for a free-form tagline (a brain's about.txt) to draw it flat.
 * wrapPosX > 0 wraps the remainder at that window-local x. */
void lobbyDrawTagline(const char *tag, float wrapPosX, int difficulty) {
    if (!tag || !tag[0]) return;
    const char *rest = tag;
    if (difficulty >= 0) {
        ImVec4 col;
        switch (difficulty) {
            case BOT_DIFFICULTY_EASY:   col = ImVec4(0.40f, 0.82f, 0.45f, 1.0f); break;
            case BOT_DIFFICULTY_MEDIUM: col = ImVec4(0.93f, 0.75f, 0.35f, 1.0f); break;
            default:                    col = ImVec4(0.95f, 0.52f, 0.38f, 1.0f); break;
        }
        const char *dot = strchr(tag, '.');
        if (dot != NULL && dot != tag) {
            ImGui::TextColored(col, "%.*s", (int)(dot + 1 - tag), tag);
            ImGui::SameLine(0.0f, 4.0f);
            rest = dot + 1;
            while (*rest == ' ') rest++;
        }
    }
    if (wrapPosX > 0.0f) ImGui::PushTextWrapPos(wrapPosX);
    ImGui::TextUnformatted(rest);
    if (wrapPosX > 0.0f) ImGui::PopTextWrapPos();
}

/* Gear hover tooltip: "Configure" plus a "Currently:" line naming the bot,
 * its mode when that is not the default one, and its difficulty
 * ("GoalHunter · Survival Scenario · Hard"). The default mode adds that
 * difficulty's short tagline underneath with the Easy./Medium./Hard. token
 * coloured; another mode's levels have no such blurb (they are data), so
 * the line above says it all. */
void lobbyGearTooltip(ClientSim *cs, int slot, float s) {
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_TOOLTIP_CONFIG));
    char brainName[BRAIN_LIST_NAME_LEN];
    lobbyBotBrainBaseName(cs, slot, brainName, sizeof(brainName));
    if (brainName[0]) {
        const BrainModes *modes = lobbyBotModesFor(cs, slot);
        int mode = 0, level = 0;
        lobbyBotModeAndLevel(cs, slot, &mode, &level);
        bool isDefaultMode = lobbyBotModeIsDefault(modes, mode);
        bool langLevels = (modes == NULL) || lobbyBotModeUsesLangLevels(modes, mode);
        const char *levelText = langLevels
            ? langGetText(lobbyBotDifficultyLabelId((uint8_t)level))
            : modes->modes[mode].levels[level].label;
        ImGui::Separator();
        MessageArgs cargs = {};
        if (isDefaultMode || !modes) {
            SDL_snprintf(cargs.string1, sizeof(cargs.string1), "%s \xC2\xB7 %s",
                         brainName, levelText);
        } else {
            SDL_snprintf(cargs.string1, sizeof(cargs.string1),
                         "%s \xC2\xB7 %s \xC2\xB7 %s",
                         brainName, modes->modes[mode].label, levelText);
        }
        ImGui::Text("%s", langGetTextFmt(STR_DLGLOBBY_BOTCFG_CURRENTLY, &cargs));
        if (langLevels) {
            lobbyDrawTagline(langGetText(lobbyBotDifficultyTaglineId((uint8_t)level)),
                             320.0f * s, level);
        }
    }
    ImGui::EndTooltip();
}

/* Add Bot. namingPool < 0 means "use the slot's team pool" (multiplayer
 * server already picks based on team membership). namingPool >= 0
 * forces a specific pool — used by per-team header "+ Bot" buttons
 * which have their own pool dropdown.  teamNumber > 0 assigns the
 * new bot to that team after creation; teamNumber == 0 leaves it on
 * whatever default team the server picked.  Single-player honors both
 * overrides locally; multiplayer currently ignores them (server-side
 * picks the name and default team — TODO: extend the protocol with a
 * teamId-aware add-bot packet). */
static void lobbySendAddBot(ClientSim *cs,
                            int namingPool, uint8_t teamNumber) {
    /* Validate the sticky brain pick against the current catalogue:
     * an out-of-range sticky (e.g. catalogue shrunk between picks)
     * falls back to the server-default sentinel. */
    /* First add: default to GoalHunter_1.7 (newest), not the server CLI default;
     * then stay sticky (the per-bot Bot Code dropdown updates
     * s_brains.lastChosenBrainIdx). */
    if (s_brains.lastChosenBrainIdx == 0xFF && cs) {
        int def = lobbyDefaultBrainIdx(clientSimGetLobbyBrainList(cs));
        if (def >= 0) s_brains.lastChosenBrainIdx = (uint8_t)def;
    }
    uint8_t stickyBrainIdx = s_brains.lastChosenBrainIdx;
    if (stickyBrainIdx != 0xFF && cs) {
        const BrainList *bl = clientSimGetLobbyBrainList(cs);
        if (!bl || stickyBrainIdx >= bl->count) stickyBrainIdx = 0xFF;
    }
    if (cs && clientSimIsSinglePlayer(cs)) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim) return;
        if (serverSimGetState(sim) != serverStateLobby) return;
        /* Find first free slot */
        BYTE slot;
        for (slot = 1; slot < MAX_TANKS; slot++) {
            if (!serverSimIsPlayerConnected(sim, slot)) break;
        }
        if (slot >= MAX_TANKS) return;
        if (serverSimGetBotBrainPath(sim)[0] == '\0') return;

        /* Pick a name. If a pool override is supplied we use it; else
         * fall back to "Bot N". Build the used-names list from current
         * lobby slots so the picker doesn't collide with existing bots. */
        char botName[32];
        if (namingPool >= 0 && namingPool < lobbyBotPoolCount()) {
            const char *usedNames[MAX_TANKS];
            int usedCount = 0;
            for (int i = 0; i < MAX_TANKS; i++) {
                if (serverSimIsPlayerConnected(sim, i)) {
                    usedNames[usedCount++] = clientSimGetLobbySlot(cs, (BYTE)(i))->playerName;
                }
            }
            lobbyBotPoolPick(namingPool, usedNames, usedCount, botName, sizeof(botName));
        } else {
            snprintf(botName, sizeof(botName), "Bot %d", slot);
        }

        /* Mirror the server's bot-name validator before applying.  The
         * picked name is normally a pool / "Bot N" pick — server-trusted
         * by construction — but the gate is consistent with the wire rule
         * (transport_udp_server.c rejects invalid names) and stops a pool
         * definition that smuggled in a control byte, reserved leading
         * '*', etc. from landing in the local sim. */
        if (botName[0] != '\0') {
            char validated[PACKET_MAX_PLAYER_NAME];
            PlayerNameValidationError nameErr = PLAYER_NAME_OK;
            if (!playerNameValidate(botName, validated,
                                    sizeof(validated), &nameErr)) {
                return;
            }
            SDL_strlcpy(botName, validated, sizeof(botName));
        }

        /* Serialise against the SDL timer thread's serverInstanceTick.
         * The publish flag inside serverSim is single-thread; without the
         * mutex the lobby heartbeat in serverInstanceTick can re-enter
         * publishControl while this thread is mid-publish. */
        threadsWaitForMutex();
        /* Difficulty first: it has to be in the slot's config before the
         * bot's brain is created, because that is where bot_manager reads it
         * to build the brain's difficulty= token. Single-player uses the
         * player's own chosen difficulty (or the skill guess), the same rule
         * the auto-seeded bots follow, so a bot the player adds by hand is
         * no harder than one the game gave them. */
        {
          const char *spBrain = serverSimGetBotBrainPath(sim);
          uint8_t spMode = gameFrontSpBotMode(spBrain);
          serverSimSetBotConfig(sim, slot, spMode,
                                gameFrontSpBotLevel(spBrain, spMode),
                                0 /* personality: normal */, NULL);
        }
        serverSimCreateBot(sim, slot, serverSimGetBotBrainPath(sim), botName,
                           (aiType)serverSimGetBotAiType(sim),
                           (gameType)clientSimGetLobbyGameType(cs),
                           clientSimIsLobbyHiddenMines(cs), 0, NULL);
        /* Mirror the per-bot brain selection so the AiConfig combo
         * reflects "this bot's brain" rather than a global default.
         * stickyBrainIdx == 0xFF picks up the server's CLI-configured
         * default brain; any other value swaps in the catalogue entry
         * the host last picked. */
        if (stickyBrainIdx != 0xFF) {
            serverSimSwitchBotBrain(sim, slot, stickyBrainIdx);
        } else {
            serverSimSetBotBrainIdxFor(sim, slot, 0xFF);
        }
        /* Now the bot exists and runs its final brain, give it the mode and
         * difficulty every new lobby bot gets (serverSimResolveNewBotConfig):
         * the player's own chosen level as the base, then what the map
         * requires for the bot's side, then what the player last picked by
         * hand. Before this, single player's add wrote only the base and
         * skipped the map and the manual pick altogether — which is why a bot
         * added to the Survival horde came up at the player's skill guess
         * instead of Hard. The team is the header's when it named one (that
         * is applied asynchronously below), else the one the add just gave
         * the slot. The brain reloads from this config at round start, so
         * setting it after the create is enough. */
        {
            const char *botBrain = (stickyBrainIdx != 0xFF)
                ? serverSimGetBrainPathForIdx(sim, stickyBrainIdx)
                : serverSimGetBotBrainPath(sim);
            const LobbyPlayer *lp = serverSimGetLobbyPlayer(sim, slot);
            int team = (teamNumber > 0 && teamNumber < MAX_TANKS)
                ? (int)teamNumber : (lp ? (int)lp->teamNumber : 0);
            if (botBrain != NULL && botBrain[0] != '\0') {
                uint8_t mode  = gameFrontSpBotMode(botBrain);
                uint8_t level = gameFrontSpBotLevel(botBrain, mode);
                if (serverSimResolveNewBotConfig(sim, team, botBrain, true,
                                                 &mode, &level)) {
                    serverSimSetBotConfig(sim, slot, mode, level,
                                          0 /* personality: normal */, NULL);
                }
            }
        }
        /* Publish the slot's new state. serverSimSetBotBrainIdxFor
         * (called via either branch above) publishes the bot-brain
         * event itself. */
        serverSimPublishLobbySlot(sim, slot);
        threadsReleaseMutex();
        /* Bot's name came from the pool — not an override. */
        s_commands.botNameOverridden[slot] = false;
        if (teamNumber > 0 && teamNumber < MAX_TANKS) {
            clientSimNetSendTeamSet(cs, slot, teamNumber);
        }
        return;
    }
    /* MP path */
    {
        /* Pick the bot's name from the chosen pool right here on the
         * client — server doesn't know pool contents (see
         * lobby_bot_pools.h). Empty string falls back to "Bot N". */
        char botName[32];
        botName[0] = '\0';
        if (cs && namingPool >= 0 && namingPool < lobbyBotPoolCount()) {
            const char *usedNames[MAX_TANKS];
            int usedCount = 0;
            for (int i = 0; i < MAX_TANKS; i++) {
                if (clientSimGetLobbySlot(cs, (BYTE)(i))->connected &&
                    clientSimGetLobbySlot(cs, (BYTE)(i))->playerName[0]) {
                    usedNames[usedCount++] = clientSimGetLobbySlot(cs, (BYTE)(i))->playerName;
                }
            }
            lobbyBotPoolPick(namingPool, usedNames, usedCount,
                             botName, sizeof(botName));
        }
        /* Same defensive gate as the SP branch.  Empty botName is the
         * legitimate "let the server pick a default" signal and must
         * pass through unchanged. */
        if (botName[0] != '\0') {
            char validated[PACKET_MAX_PLAYER_NAME];
            PlayerNameValidationError nameErr = PLAYER_NAME_OK;
            if (!playerNameValidate(botName, validated,
                                    sizeof(validated), &nameErr)) {
                return;
            }
            SDL_strlcpy(botName, validated, sizeof(botName));
        }
        /* Pass the sticky brain index for API symmetry with the SP path,
         * but be aware: PACKET_LOBBY_ADD_BOT does not propagate it to the
         * server today (the wire payload's brain bytes are vestigial and
         * the parser ignores them). The AiConfig combo issues a follow-up
         * SET_BOT_BRAIN once the new slot lands when a non-default brain
         * is desired. */
        clientSimNetSendAddBotConfigured(cs, teamNumber, stickyBrainIdx, botName);
    }
}

static int lobbyCountConnectedSlots(ClientSim *cs) {
    int n = 0;
    if (!cs) return 0;
    for (int i = 0; i < MAX_TANKS; i++) {
        const ClientLobbySlot *lp = clientSimGetLobbySlot(cs, (BYTE)i);
        if (lp && lp->connected) n++;
    }
    return n;
}

bool lobbyAddBotPending(ClientSim *cs) {
    if (!cs) return false;
    /* In the same ImGui frame as the last send, refuse another no matter
     * what — multiple buttons render before any one's click can update
     * the disabled state of the others. */
    if (s_commands.addBotFrame == ImGui::GetFrameCount()) return true;
    if (s_commands.addBotSentMs == 0) return false;
    Uint32 now = SDL_GetTicks();
    if (now - s_commands.addBotSentMs > 2000) {
        s_commands.addBotSentMs = 0;
        return false;
    }
    if (lobbyCountConnectedSlots(cs) >= s_commands.addBotExpectedConn) {
        s_commands.addBotSentMs = 0;
        return false;
    }
    return true;
}

void lobbySendAddBotDebounced(ClientSim *cs,
                                     int namingPool, uint8_t teamNumber) {
    if (!cs) return;
    /* Drop the call entirely if anything already added a bot this
     * frame — the visible button was probably stale-clicked. */
    if (s_commands.addBotFrame == ImGui::GetFrameCount()) return;
    /* Refuse to push past the 16-slot ceiling regardless of how the
     * server would otherwise handle it. Keeps the bot pool / subscriber
     * registry from being torched if MAX_TANKS slots are already in use
     * and a queued click slips through. */
    if (lobbyCountConnectedSlots(cs) >= MAX_TANKS) return;
    s_commands.addBotFrame = ImGui::GetFrameCount();
    s_commands.addBotSentMs = SDL_GetTicks();
    s_commands.addBotExpectedConn = lobbyCountConnectedSlots(cs) + 1;
    lobbySendAddBot(cs, namingPool, teamNumber);
}

void lobbySendRemoveBot(ClientSim *cs, uint8_t slot) {
    if (cs && clientSimIsSinglePlayer(cs)) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim) return;
        if (serverSimGetState(sim) != serverStateLobby) return;
        threadsWaitForMutex();
        serverSimRemoveBot(sim, slot);
        serverSimPublishLobbySlot(sim, slot);
        threadsReleaseMutex();
        if (slot < MAX_TANKS) s_commands.botNameOverridden[slot] = false;
        return;
    }
    clientSimNetSendRemoveBot(cs, slot);
}

/* Move `targetSlot` to `teamNumber`. SP-host applies via the
 * wrapper's local-transport branch; over UDP the server allows any
 * client to change its own team and gates other-target moves on
 * host / admin / openHost (matches the drag-and-drop UI gate). */
void lobbySendTeamSet(ClientSim *cs,
                             uint8_t targetSlot, uint8_t teamNumber) {
    clientSimNetSendTeamSet(cs, targetSlot, teamNumber);
}

/* Update a bot's per-slot config (difficulty / personality / name
 * override). Pre-validates the name on the client (UX courtesy — the
 * UI can drop bad input before sending) and hands off to the wire
 * wrapper, whose local-transport branch re-runs the canonical
 * validate-and-apply path for SP-host. */
void lobbySendBotConfig(ClientSim *cs,
                               uint8_t slot,
                               uint8_t mode, uint8_t difficulty,
                               uint8_t personality,
                               const char *name) {
    /* Pre-send validation for the bot name.  Empty name is the
     * legitimate "leave name unchanged; difficulty/personality still
     * apply" signal — pass through.  Non-empty must clear the same
     * validator the server applies (controls, reserved leading '*',
     * mixed scripts, length), then uniqueness against the client's
     * lobby-slot mirror — bots and humans both register as connected
     * here, so this catches bot-vs-bot collisions in addition to
     * bot-vs-human.  Server is authoritative
     * (transport_udp_server.c); this is the client mirror. */
    char validated[PACKET_MAX_PLAYER_NAME];
    const char *effectiveName = name;
    if (cs && name && name[0] != '\0') {
        PlayerNameValidationError nameErr = PLAYER_NAME_OK;
        if (!playerNameValidate(name, validated,
                                sizeof(validated), &nameErr)) {
            return;
        }
        for (BYTE j = 0; j < MAX_TANKS; j++) {
            if (j == slot) continue;
            const ClientLobbySlot *other = clientSimGetLobbySlot(cs, j);
            if (!other || !other->connected) continue;
            if (playerNameCompare(other->playerName, validated) == 0) {
                return;
            }
        }
        effectiveName = validated;
    }

    clientSimNetSendLobbyBotConfig(cs, slot, mode, difficulty, personality,
                                   effectiveName);
}

/* Change which Lua brain script a lobby bot uses. SP path mutates the
 * server sim directly; MP path goes through PACKET_LOBBY_SET_BOT_BRAIN.
 * brainIdx == 0xFF means "use the server's CLI-configured default
 * brain"; any other value indexes into the lobby brain catalogue. */
void lobbySendSetBotBrain(ClientSim *cs,
                                 uint8_t slot, uint8_t brainIdx) {
    if (cs && clientSimIsSinglePlayer(cs)) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim || slot >= MAX_TANKS) return;
        if (serverSimGetState(sim) != serverStateLobby) return;
        if (!serverSimGetLobbyPlayer(sim, slot)->isBot) return;
        /* serverSimSwitchBotBrain calls serverSimSetBotBrainIdxFor,
         * which publishes CTRL_LOBBY_BOT_BRAIN itself. */
        threadsWaitForMutex();
        serverSimSwitchBotBrain(sim, slot, brainIdx);
        threadsReleaseMutex();
        return;
    }
    clientSimNetSendLobbySetBotBrain(cs, slot, brainIdx);
}

/* Clear a team's metadata (color/name/pool back to defaults).
 * Mirrors PACKET_LOBBY_TEAM_CLEAR. Only called when the team is
 * empty — caller already gates on memberCount == 0. The wrapper's
 * local-transport branch handles SP-host. */
void lobbySendTeamClear(ClientSim *cs, uint8_t teamId) {
    clientSimNetSendLobbyTeamClear(cs, teamId);
}

/* Update a team's naming pool. The team-meta write goes through the
 * wire wrapper (its local-transport branch handles SP-host). After
 * the new pool is published we re-roll every bot on the team whose
 * name wasn't manually overridden so the new pool's vibe applies
 * immediately — the SP and MP branches diverge only in which set of
 * accessors they iterate (server-sim vs client-sim mirror); both
 * land on the same wire wrapper for the per-bot rename sends. */
void lobbySendTeamPool(ClientSim *cs,
                              uint8_t teamId, uint8_t namingPool,
                              const char *teamName) {
    if (cs && clientSimIsSinglePlayer(cs)) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim || teamId == 0 || teamId >= MAX_TANKS) return;
        /* Route the team-meta write through the wire wrapper. The
         * local-transport branch in client_net.c applies the same
         * fields (and pool-uniqueness rewrite) under the threads
         * mutex. Read the current color and start side so the wrapper's
         * required args don't clobber them — fresh teams fall back to a
         * per-teamId default color, same as the MP branch below. */
        uint8_t color = clientSimGetLobbyTeamColor(cs, (BYTE)(teamId));
        if (!clientSimGetLobbyTeamInUse(cs, (BYTE)(teamId))) {
            color = (uint8_t)((teamId - 1) & 7);
        }
        uint8_t startSide = clientSimGetLobbyTeamStartSide(cs, (BYTE)(teamId));
        clientSimNetSendLobbyTeamMeta(cs, teamId, color, namingPool, startSide, teamName);

        /* Rename bots on this team whose names weren't overridden by
         * the host. We pick names sequentially from the new pool,
         * each call's used-list including everyone already on the
         * lobby plus the names we've assigned in this loop, so the
         * picker doesn't collide with itself. */
        const char *usedNames[MAX_TANKS];
        int usedCount = 0;
        for (int i = 0; i < MAX_TANKS; i++) {
            if (serverSimIsPlayerConnected(sim, i)) {
                usedNames[usedCount++] = clientSimGetLobbySlot(cs, (BYTE)(i))->playerName;
            }
        }
        for (int slot = 0; slot < MAX_TANKS; slot++) {
            if (!serverSimIsPlayerConnected(sim, slot)) continue;
            if (!serverSimGetLobbyPlayer(sim, slot)->isBot) continue;
            if (serverSimGetLobbyPlayer(sim, slot)->teamNumber != teamId) continue;
            if (s_commands.botNameOverridden[slot]) continue;

            char pickBuf[32];
            lobbyBotPoolPick(namingPool, usedNames, usedCount,
                             pickBuf, sizeof(pickBuf));
            lobbySendBotConfig(cs, (uint8_t)slot,
                               clientSimGetLobbyBotMode(cs, (BYTE)(slot)),
                               clientSimGetLobbyBotDifficulty(cs, (BYTE)(slot)),
                               clientSimGetLobbyBotPersonality(cs, (BYTE)(slot)),
                               pickBuf);
            /* Replace this slot's entry in usedNames so subsequent
             * picks see the updated name (avoids picking the same
             * name twice within the loop). */
            for (int u = 0; u < usedCount; u++) {
                if (usedNames[u] == clientSimGetLobbySlot(cs, (BYTE)(slot))->playerName) {
                    usedNames[u] = clientSimGetLobbySlot(cs, (BYTE)(slot))->playerName;
                    break;
                }
            }
        }
        return;
    }
    /* MP path. */
    {
        /* If the client hasn't received the team's actual metadata yet
         * (e.g. fresh join), fall back to a sensible per-teamId default
         * color rather than the zero-initialised value, which would
         * paint every team RED until the server echoed the real one
         * back. Server is authoritative — TEAM_META_CHG will correct
         * this on the next round-trip. */
        uint8_t color = clientSimGetLobbyTeamColor(cs, (BYTE)(teamId));
        if (!clientSimGetLobbyTeamInUse(cs, (BYTE)(teamId))) {
            color = (uint8_t)((teamId - 1) & 7);
        }
        /* Same read-back for the start side: the packet carries every
         * field, so send the current side rather than resetting it. */
        uint8_t startSide = clientSimGetLobbyTeamStartSide(cs, (BYTE)(teamId));
        clientSimNetSendLobbyTeamMeta(cs, teamId, color, namingPool, startSide, teamName);

        /* The server can't rename existing bots when the pool changes
         * because the per-pool name table lives only on the client
         * (lobby_bot_pools.h). Fan out one BOT_CONFIG per bot on this
         * team with a name drawn from the newly-selected pool — same
         * behaviour as the SP branch above, just over the wire. */
        const char *usedNames[MAX_TANKS];
        int usedCount = 0;
        for (int i = 0; i < MAX_TANKS; i++) {
            if (clientSimGetLobbySlot(cs, (BYTE)(i))->connected &&
                clientSimGetLobbySlot(cs, (BYTE)(i))->playerName[0]) {
                usedNames[usedCount++] = clientSimGetLobbySlot(cs, (BYTE)(i))->playerName;
            }
        }
        /* Stash the assigned names locally so subsequent loop
         * iterations don't pick a name we just handed out. */
        char assigned[MAX_TANKS][32];
        for (int slot = 0; slot < MAX_TANKS; slot++) {
            if (!clientSimGetLobbySlot(cs, (BYTE)(slot))->connected) continue;
            if (!clientSimGetLobbySlot(cs, (BYTE)(slot))->isBot)       continue;
            if (clientSimGetLobbySlot(cs, (BYTE)(slot))->teamNumber != teamId) continue;
            if (slot < MAX_TANKS && s_commands.botNameOverridden[slot]) continue;

            lobbyBotPoolPick(namingPool, usedNames, usedCount,
                             assigned[slot], sizeof(assigned[slot]));
            clientSimNetSendLobbyBotConfig(cs, (uint8_t)slot,
                clientSimGetLobbyBotMode(cs, (BYTE)(slot)),
                clientSimGetLobbyBotDifficulty(cs, (BYTE)(slot)),
                clientSimGetLobbyBotPersonality(cs, (BYTE)(slot)),
                assigned[slot]);
            usedNames[usedCount++] = assigned[slot];
        }
    }
}

/* Update a team's start side. The team-meta packet carries every field,
 * so the current colour, naming pool and name are read back from the
 * client mirror and sent unchanged beside the new side — the read-back
 * lobbySendTeamPool does for colour and side, widened to the pool and
 * the name. A team the mirror has not marked in use gets the same
 * per-teamId default colour and "Team N" name the header row shows for
 * it. SP-host and MP both land on the wire wrapper (its local-transport
 * branch applies the fields under the threads mutex); SP only adds the
 * guard lobbySendTeamPool applies before its send. */
void lobbySendTeamSide(ClientSim *cs, uint8_t teamId, uint8_t startSide) {
    if (cs && clientSimIsSinglePlayer(cs)) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim || teamId == 0 || teamId >= MAX_TANKS) return;
    }
    uint8_t     color      = clientSimGetLobbyTeamColor(cs, (BYTE)(teamId));
    uint8_t     namingPool = clientSimGetLobbyTeamPool(cs, (BYTE)(teamId));
    const char *teamName   = clientSimGetLobbyTeamName(cs, (BYTE)(teamId));
    char        defaultName[16];
    if (!clientSimGetLobbyTeamInUse(cs, (BYTE)(teamId))) {
        MessageArgs args = {};
        args.number = teamId;
        SDL_snprintf(defaultName, sizeof(defaultName), "%s",
                     langGetTextFmt(STR_DLGLOBBY_TEAM_HEADER, &args));
        color    = (uint8_t)((teamId - 1) & 7);
        teamName = defaultName;
    }
    clientSimNetSendLobbyTeamMeta(cs, teamId, color, namingPool, startSide, teamName);
}

/* Game-settings dispatcher. settingType is one of LST_* (wire_limits.h);
 * payload is 1 to 3 bytes per server-side parser. Forwards to the
 * client_net wrapper, whose local-transport branch shares
 * serverSimApplyLobbySetting with the UDP-side packet handler so SP
 * and wire follow one code path. */
void lobbySendSetting(ClientSim *cs,
                             uint8_t settingType,
                             const uint8_t *value, uint8_t valueLen) {
    /* Pre-send validation for setting types that have a wire-side range
     * cap on the server. Drop out-of-range values rather than letting
     * the server reject them — the dispatcher has the authoritative
     * check (server_command_dispatch.c CMD_LOBBY_SET arm) and returns
     * CMD_REJECT_INVALID. */
    if (settingType == LST_TIME_MINUTES && valueLen == 2) {
        uint16_t mins = (uint16_t)((value[0] << 8) | value[1]);
        if (mins < LOBBY_TIME_MINUTES_MIN ||
            mins > LOBBY_TIME_MINUTES_MAX) {
            mpDiagLog("[ui] lobbySendSetting REJECTED type=%d (time-minutes out of range)",
                      (int)settingType);
            return;
        }
    }
    /* Surface the click at the UI boundary so we can distinguish
     * "click never reached the wire" from "click reached the wire but
     * the server rejected/no-op'd it" from "click reached the server,
     * was applied, but the radio's checked-state isn't updating". */
    {
        uint32_t valDump = 0;
        for (uint8_t k = 0; k < valueLen && k < 4; k++) {
            valDump = (valDump << 8) | value[k];
        }
        mpDiagLog("[ui] lobbySendSetting type=%d valueLen=%d value=0x%08x",
                  (int)settingType, (int)valueLen, valDump);
    }
    clientSimNetSendLobbySetting(cs, settingType, value, valueLen);
}
