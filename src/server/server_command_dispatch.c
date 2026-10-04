/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bot_manager.h"              /* botManagerSetBrainIdx, botManagerAddBot */
#include "brain_list.h"               /* BRAIN_MODES_MAX, BRAIN_LEVELS_MAX */
#include "client_command.h"
#include "client_sim_internal.h"  /* LOBBY_SCENARIO_LIST_MAX — the cap the
                                   * scenario list packet and the client's
                                   * chooser share */
#include "control_event.h"            /* ControlEvent, CTRL_CHAT, CTRL_ALLIANCE_REQUEST */
#include "server_sim_scenario.h"      /* serverSimScenarioReload,
                                        serverSimScenarioListDir, ScnDirEntry */
#include "log.h"
#include "mapgen.h"                   /* MapGenConfig, mapGenSeedToConfig */
#include "netpacks.h"                 /* lobbyBotNameAcceptable */
#include "player_flags.h"             /* PLAYER_FLAG_ADMIN */
#include "players.h"                  /* playersGetClientFlags */
#include "playername_validate.h"      /* playerNameValidate, playerNameCompare */
#include "server_sim.h"
#include "server_sim_internal.h"      /* serverSimGameVoteToggle */
#include "server_sim_join.h"          /* serverSimAssignLobbyStartOnJoin, serverSimLobbyStartSideMask, serverSimLobbyClosedMaskFor; serverSimFindFreeSlot */
#include "sim/server_sim_shared.h"    /* serverSimResolveMapPath */
#include "server_sim_lifecycle.h"     /* serverSimSetTeam, lobbyAutoUnreadyOnChange */
#include "lobby_shared_starts.h"      /* lobbySharedStartsEnabled — several players per start */
#include "start_sides.h"              /* startSideEligible — the claim command's side check */
#include "threads.h"
#include "../common/wb_log.h"
#include "transport_udp.h"            /* transportUdpServerGetPlayerName,
                                         transportUdpServerSetBotName,
                                         transportUdpServerKickPlayer,
                                         transportUdpServerSetVoiceMute */
#include "../winbolonet/winbolonet_server.h" /* winboloNetIsPlayerParticipant */
#include "../winbolonet/winbolonet_core.h"   /* winbolonetIsRunning */

/* Authority gate shared by the command dispatcher (this TU) and the
 * lobby command handlers in transport_udp_server.c, where the function
 * is defined. */
extern bool lobbyClientMayEdit(ServerSim *sim, int clientIdx);

/* How many connected slots other than exceptSlot are on this team. There is
 * no accessor for this — the nearest walk is the reservation pick in
 * server_sim_players.c, which counts teammates for a different purpose — so
 * the two team arms below share this one. Pass MAX_TANKS to exclude nobody.
 * Bots count: a seat with a bot on it holds the team as much as a person's
 * does. */
static int lobbyTeamMemberCount(const ServerSim *sim, BYTE team,
                                BYTE exceptSlot) {
    int count = 0;
    BYTE i;

    if (team == 0 || team >= MAX_TANKS) {
        return 0;
    }
    for (i = 0; i < MAX_TANKS; i++) {
        if (i == exceptSlot) continue;
        if (!sim->playerConnected[i]) continue;
        if (sim->lobbyPlayers[i].teamNumber == team) count++;
    }
    return count;
}

/* Whether this slot may be put on this team. The policy is asked about
 * extra teams — teams beyond the ones that exist — so it is asked only when
 * the write is what brings the team into existence. Three writes create
 * nothing and go through unasked:
 *
 *   - team 0, which is no team at all but where a slot goes to be
 *     unassigned, and a number past the end, which both arms turn down on
 *     their own;
 *   - a re-send of the team the slot already holds, which moves nothing;
 *   - a move onto a team another connected slot is already on.
 *
 * The moving slot is left out of the count because it is the one being
 * placed: the question is whether the team it is going to exists without
 * it. So the sole member of a team moving off it and onto an empty one is
 * asked, even though the number of teams in play does not rise — the team
 * it lands on is one the scenario did not lay out, which is what the
 * pointer is there to refuse.
 *
 * With no policy, or one with no opinion, the answer is yes and both arms
 * behave exactly as they did. The roster is not walked in that case. */
static bool scenarioAllowsExtraTeams(ServerSim *sim, BYTE slot, BYTE team) {
    const LobbyPlayer *lp;
    bool allow;

    if (sim->scenarioPolicy == NULL ||
        sim->scenarioPolicy->allowExtraTeams == NULL) {
        return TRUE;
    }
    if (team == 0 || team >= MAX_TANKS) {
        return TRUE;
    }
    lp = serverSimGetLobbyPlayer(sim, slot);
    if (lp != NULL && lp->teamNumber == team) {
        return TRUE;
    }
    if (lobbyTeamMemberCount(sim, team, slot) > 0) {
        return TRUE;
    }
    serverSimScenarioPolicyEnter(sim);
    allow = sim->scenarioPolicy->allowExtraTeams(sim->scenarioPolicy->ctx);
    serverSimScenarioPolicyLeave(sim);
    return allow;
}

/* Whether the scenario lets player ally with other: player is the one who
 * asked, other the seat asked. Put to the policy at the request and again at
 * the accept, the two places a player makes an alliance. Alliances a script
 * makes itself — set_team, spawning a bot onto a team, seating — go through
 * other paths and are not asked. With no policy, or one with no opinion, the
 * answer is yes. */
static bool scenarioAllowsAlliance(ServerSim *sim, BYTE player, BYTE other) {
    bool allow;

    if (sim->scenarioPolicy == NULL || sim->scenarioPolicy->canAlly == NULL) {
        return TRUE;
    }
    if (player >= MAX_TANKS || other >= MAX_TANKS) {
        return TRUE;
    }
    serverSimScenarioPolicyEnter(sim);
    allow = sim->scenarioPolicy->canAlly(sim->scenarioPolicy->ctx, player,
                                         other);
    serverSimScenarioPolicyLeave(sim);
    return allow;
}

/* The START_SIDE_* choice of a slot's team; a slot on team 0 has no side. */
static BYTE lobbySlotStartSide(const ServerSim *sim, BYTE slot) {
    const LobbyPlayer *lp = serverSimGetLobbyPlayer(sim, slot);
    BYTE t = lp ? lp->teamNumber : 0;
    if (t == 0 || t >= MAX_TANKS) return START_SIDE_ANY;
    return sim->teams[t].startSide;
}

/* Whether a slot may hold a 1-based start under the side rules: the same
 * question the lobby pick, the map-change release and the batch placement
 * ask, so a claim can never land a slot on a start the next lobby event
 * would take away again. A side team takes what its side accepts; a slot
 * with no side stays off the sides the other teams present chose. */
static bool lobbySlotMayHoldStart(ServerSim *sim, BYTE slot, BYTE idx1) {
    return startSideEligible(serverSimLobbyStartSideMask(sim, idx1),
                             lobbySlotStartSide(sim, slot),
                             serverSimLobbyClosedMaskFor(sim, slot));
}

/* The three path shapes the map command refuses, refused the same way: an
   absolute path, a Windows drive letter, and a ".." segment. A scenario is
   picked by a name in a flat directory, so any of the three means the sender
   is asking for a file somewhere else. */
static bool lobbyScenarioNameShapeOk(const char *relPath) {
    const char *s;

    if (relPath[0] == '/' || relPath[0] == '\\') return false;
    if (relPath[0] != '\0' && relPath[1] == ':') return false;
    for (s = relPath; *s;) {
        if (s[0] == '.' && s[1] == '.' &&
            (s[2] == '\0' || s[2] == '/' || s[2] == '\\')) {
            return false;
        }
        while (*s && *s != '/' && *s != '\\') s++;
        while (*s == '/' || *s == '\\') s++;
    }
    return true;
}

/* The widths this file holds against each other. client_command.h keeps its
   own copies of the list cap and the file length because a header on the gui
   include path cannot reach control_event.h, and control_event.h keeps its
   own because a public header cannot reach src/scenario/. This translation
   unit is the one that sees all three, so a number moving alone is a build
   failure here rather than a list that is silently cut somewhere on the way
   through. The same pattern control_event.h documents for the three scenario
   string lengths. */
BOLO_STATIC_ASSERT(CMD_SCRIPT_LIST_MAX == LOBBY_SCRIPT_LIST_MAX,
                   cmd_script_list_max_matches_lobby_script_list_max);
BOLO_STATIC_ASSERT(CMD_SCRIPT_LIST_FILE_LEN == LOBBY_SCENARIO_FILE_LEN,
                   cmd_script_list_file_len_matches_lobby_scenario_file_len);
BOLO_STATIC_ASSERT(SCN_DIR_FILE_LEN == LOBBY_SCENARIO_FILE_LEN,
                   scn_dir_file_len_matches_lobby_scenario_file_len);
BOLO_STATIC_ASSERT(CMD_SCRIPT_SETTING_ID_LEN == SCN_SETTING_ID_LEN,
                   cmd_script_setting_id_len_matches_scn_setting_id_len);
BOLO_STATIC_ASSERT(SCN_DIR_NAME_LEN == LOBBY_SCENARIO_NAME_LEN,
                   scn_dir_name_len_matches_lobby_scenario_name_len);

/* The selection changed, so which scenario plays is decided again. The same
   three calls a map commit makes, in the same order: whoever owns the
   scenario is asked first and swaps what is attached, the seats it asks for
   are built, the lobby's own settings are brought into line with it, and the
   result goes out.

   The map path is handed over unchanged because the map has not changed —
   what the decision weighs is the pick against the committed map's own
   script, and it needs both. Nothing here reaches into the scenario library:
   serverSimScenarioOnMapChanged calls whatever registered itself, which is
   the only direction that links. */
static void lobbyScenarioReselect(ServerSim *sim) {
    serverSimScenarioOnMapChanged(sim, sim->mapFilePath);
    serverSimScenarioApplyLobbyRules(sim);
    serverSimPublishLobbySettings(sim);
    /* And the list itself, behind the settings that name its first entry.
       Both go out on every pick, because a chooser draws its rows from the
       list and its attached-scenario detail from the settings, and a lobby
       that had only one of them would show the two disagreeing. */
    serverSimPublishScriptList(sim);
    /* And everyone who was ready is ready no longer, as a map commit does it
       at the end of the same sequence. A pick changes the rules the round
       runs, the seats the lobby holds and the game it is played by, so a
       lobby that was all-ready would otherwise start on a scenario nobody
       agreed to. */
    lobbyAutoUnreadyOnChange(sim);
    /* And the tracker's scenario and mod names follow the list, inside the
       lobby update's usual rate limit. */
    serverSimWbnLobbyUpdate(sim, FALSE);
}

static CmdResult applyCommandInner(ServerSim *sim, int senderSlot,
                                   const ClientCommand *cmd) {
    switch (cmd->type) {
    case CMD_TEAM_SET: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (cmd->u.teamSet.slot >= MAX_TANKS ||
            cmd->u.teamSet.team >= MAX_TANKS) {
            return CMD_REJECT_INVALID;
        }
        if ((int)cmd->u.teamSet.slot != senderSlot &&
            !lobbyClientMayEdit(sim, senderSlot)) {
            return CMD_REJECT_NOT_HOST;
        }
        /* A scenario that has laid out its teams can refuse the lobby a new
           one. Nothing has been written yet, so the move is turned down
           outright and the sender hears it, the way this arm answers every
           other refusal. */
        if (!scenarioAllowsExtraTeams(sim, cmd->u.teamSet.slot,
                                      cmd->u.teamSet.team)) {
            return CMD_REJECT_INVALID;
        }
        serverSimSetTeam(sim, cmd->u.teamSet.slot, cmd->u.teamSet.team);
        logAddEvent(log_TeamSet,
                    cmd->u.teamSet.slot, cmd->u.teamSet.team,
                    0, 0, 0, NULL);
        serverSimPublishLobbySlot(sim, cmd->u.teamSet.slot);
        lobbyAutoUnreadyOnChange(sim);
        return CMD_OK;
    }
    case CMD_LOBBY_CLAIM_START: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        const CmdLobbyClaimStart *p = &cmd->u.lobbyClaimStart;
        BYTE target = p->targetSlot;
        BYTE idx    = p->startIdx;
        BYTE numStarts = startsGetNumStarts(&sim->sim.ss);
        if (target >= MAX_TANKS || !serverSimIsPlayerConnected(sim, target)) {
            return CMD_REJECT_INVALID;
        }
        if (idx != 0xFF && idx != START_CLAIM_TEAM_SIDE &&
            (idx < 1 || idx > numStarts ||
             startsIsActive(&sim->sim.ss, idx) == FALSE)) {
            return CMD_REJECT_INVALID;
        }
        bool isHost = lobbyClientMayEdit(sim, senderSlot);
        if ((int)target != senderSlot && !isHost) {
            return CMD_REJECT_NOT_HOST;
        }
        /* A player picking for themselves may take only a start the side
         * rules let their slot hold; the host may hand anyone any start.
         * Whether anyone already holds the start makes no difference to
         * this test — with shared starts on, joining a held start is
         * judged exactly like taking a free one. */
        if (!isHost && idx != 0xFF && idx != START_CLAIM_TEAM_SIDE &&
            !lobbySlotMayHoldStart(sim, target, idx)) {
            return CMD_REJECT_INVALID;
        }
        if (idx == START_CLAIM_TEAM_SIDE) {
            /* Team side: drop the reservation and pick a fresh start on
             * the slot's side at once; 0xFF when the side is full. */
            serverSimSetLobbyStartIdx(sim, target, 0xFF);
            serverSimAssignLobbyStartOnJoin(sim, target);
            serverSimPublishLobbySlot(sim, target);
            lobbyAutoUnreadyOnChange(sim);
            return CMD_OK;
        }
        /* Connected slot currently holding idx (none when idx == 0xFF).
         * With shared starts on nobody is displaced, so the holder does
         * not need looking up at all: the claim always just lands. */
        BYTE holder = 0xFF;
        if (idx != 0xFF && !lobbySharedStartsEnabled()) {
            for (BYTE k = 0; k < MAX_TANKS; k++) {
                if (!serverSimIsPlayerConnected(sim, k)) continue;
                const LobbyPlayer *lp = serverSimGetLobbyPlayer(sim, k);
                if (lp && lp->startIdx == idx) { holder = k; break; }
            }
        }
        if (holder != 0xFF && holder != target) {
            /* One player per start (LOBBY_SHARED_STARTS off): a non-host
             * may not take a start someone else holds, and a host putting
             * anyone on one swaps the two. */
            if (!isHost) return CMD_REJECT_INVALID;
            const LobbyPlayer *tlp = serverSimGetLobbyPlayer(sim, target);
            BYTE oldTarget = tlp ? tlp->startIdx : 0xFF;
            serverSimSetLobbyStartIdx(sim, target, idx);
            serverSimSetLobbyStartIdx(sim, holder, oldTarget);
            /* The displaced holder inherits the assignee's old start. When
             * that is none, or one the holder's side rules reject, pick the
             * holder a fresh start now instead. */
            if (oldTarget == 0xFF || !lobbySlotMayHoldStart(sim, holder, oldTarget)) {
                serverSimAssignLobbyStartOnJoin(sim, holder);
            }
            serverSimPublishLobbySlot(sim, target);
            serverSimPublishLobbySlot(sim, holder);
        } else {
            /* Free start, release, already mine — or, with shared starts
             * on, joining a start others hold: they keep it too. */
            serverSimSetLobbyStartIdx(sim, target, idx);
            serverSimPublishLobbySlot(sim, target);
        }
        lobbyAutoUnreadyOnChange(sim);
        return CMD_OK;
    }
    case CMD_READY: {
        if (!serverSimIsLobbyEnabled(sim)) return CMD_REJECT_BAD_STATE;
        /* Ranked-shape gate: silently drop ready=true that doesn't
         * qualify. No reject code — the client tooltip explains why. */
        if (cmd->u.ready.ready && serverSimGetRanked(sim) &&
            serverSimGetState(sim) == serverStateLobby &&
            !serverSimRankedShapeReady(sim)) {
            return CMD_OK;
        }
        if (serverSimGetState(sim) == serverStateLobby) {
            serverSimSetReady(sim, (BYTE)senderSlot, cmd->u.ready.ready);
            logAddEvent(cmd->u.ready.ready ? log_PlayerReady : log_PlayerUnready,
                        (BYTE)senderSlot, 0, 0, 0, 0, NULL);
            serverSimPublishLobbySlot(sim, (BYTE)senderSlot);
            serverSimLobbyCheckAllReady(sim);
            if (serverSimGetState(sim) == serverStateCountdown) {
                logAddEvent(log_CountdownStart, 0, 0, 0, 0, 0, NULL);
            }
        } else if (serverSimGetState(sim) == serverStateCountdown &&
                   !cmd->u.ready.ready) {
            serverSimSetReady(sim, (BYTE)senderSlot, false);
            serverSimAbortCountdown(sim);
            logAddEvent(log_PlayerUnready, (BYTE)senderSlot, 0, 0, 0, 0, NULL);
            logAddEvent(log_CountdownCancel, 0, 0, 0, 0, 0, NULL);
            serverSimConsoleMessage("Countdown cancelled — player unreadied.");
            serverSimPublishLobbySlot(sim, (BYTE)senderSlot);
        }
        /* Other states (Running, GameOver): silent drop, no state
         * change. */
        return CMD_OK;
    }
    case CMD_LOBBY_BOT_CONFIG: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        const CmdLobbyBotConfig *p = &cmd->u.lobbyBotConfig;
        /* mode indexes the brain's own mode list and difficulty that mode's
         * level list, so the bounds are the manifest maxima rather than the
         * old fixed 0..2 — the server does not read the client's brain
         * files, and bot_manager clamps both against the real list when it
         * builds the init tokens. */
        if (p->slot >= MAX_TANKS || p->mode >= BRAIN_MODES_MAX ||
            p->difficulty >= BRAIN_LEVELS_MAX || p->personality > 3 ||
            p->nameLen > 31 || !serverSimIsBot(sim, p->slot)) {
            return CMD_REJECT_INVALID;
        }
        char validatedName[PACKET_MAX_PLAYER_NAME];
        if (p->nameLen > 0) {
            char rawName[PACKET_MAX_PLAYER_NAME];
            memset(rawName, 0, sizeof(rawName));
            memcpy(rawName, p->name, p->nameLen);
            if (!lobbyBotNameAcceptable(rawName, validatedName,
                                        sizeof(validatedName), (int)p->slot,
                                        transportUdpServerGetPlayerName,
                                        NULL, NULL)) {
                return CMD_REJECT_INVALID;
            }
            transportUdpServerSetBotName(p->slot, validatedName);
        }
        {
            /* The gear popup sends this command for a rename or a
             * personality edit too, carrying the bot's unchanged mode and
             * difficulty. Only a real change is the host choosing a
             * difficulty, so only that is remembered for the next Add Bot. */
            uint8_t prevMode  = sim->botConfigs[p->slot].mode;
            uint8_t prevLevel = sim->botConfigs[p->slot].difficulty;
            serverSimSetBotConfig(sim, p->slot, p->mode, p->difficulty,
                                  p->personality,
                                  p->nameLen > 0 ? validatedName : NULL);
            if (p->mode != prevMode || p->difficulty != prevLevel) {
                serverSimRememberManualBotPick(sim, p->slot);
            }
            /* A mode change is a person choosing the mode: the seat keeps
               it when the host changes the game type. */
            if (p->mode != prevMode) {
                serverSimMarkBotModeSetByHand(sim, p->slot);
            }
        }
        return CMD_OK;
    }
    case CMD_LOBBY_TEAM_META: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        const CmdLobbyTeamMeta *p = &cmd->u.lobbyTeamMeta;
        if (p->teamId == 0 || p->teamId >= MAX_TANKS ||
            p->nameLen > LOBBY_TEAM_NAME_LEN - 1) {
            return CMD_REJECT_INVALID;
        }
        serverSimSetTeamMeta(sim, p->teamId, p->color, p->namingPool,
                             p->startSide,
                             (const uint8_t *)p->name, p->nameLen);
        return CMD_OK;
    }
    case CMD_LOBBY_TEAM_CLEAR: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        if (cmd->u.lobbyTeamClear.teamId == 0 ||
            cmd->u.lobbyTeamClear.teamId >= MAX_TANKS) {
            return CMD_REJECT_INVALID;
        }
        serverSimClearTeamMeta(sim, cmd->u.lobbyTeamClear.teamId);
        return CMD_OK;
    }
    case CMD_LOBBY_SETTING: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        const CmdLobbySetting *p = &cmd->u.lobbySetting;
        if (p->valueLen > 32) return CMD_REJECT_INVALID;
        uint32_t lockBit = serverSimGetSettingLockBit(p->settingType);
        if (lockBit == 0xFFFFFFFFu) {
            /* Unknown setting — silent forward-compat drop. */
            return CMD_OK;
        }
        if (lockBit != 0u &&
            (serverSimGetServerLocks(sim) & lockBit) != 0u) {
            return CMD_REJECT_LOCKED;
        }
        /* The Result form, so a setting the map's scenario fixes comes back
           as that rather than as a bare invalid: those three were the host's
           to set a map ago and the toast has to say what took them away. */
        {
            CmdResult r = serverSimApplyLobbySettingResult(
                sim, p->settingType, p->value, p->valueLen);

            /* LST_MODS_OFF decides which scripts compose, so the decision is
               asked again — the same call a pick makes, because the question
               is the same one. Without it the mods the host just turned off
               would stay attached until the next map commit.

               Only on a setting that took: a refused edit changed nothing,
               and recomposing after one would unready the lobby over a click
               the server turned down. Only for this id, because every other
               setting on this arm is a value the round reads rather than a
               change to what the round is made of. */
            if (r == CMD_OK && p->settingType == LST_MODS_OFF) {
                lobbyScenarioReselect(sim);
            }
            return r;
        }
    }
    case CMD_CHAT: {
        const CmdChat *p = &cmd->u.chat;
        BYTE dest = p->destPlayer;
        /* Reject out-of-range high values: anything >= 0x80 that is
         * neither broadcast (0xFF), the server-message sentinel (0xFE),
         * nor a valid team address (0x81..0x90). */
        if (dest >= 0x80 && dest != 0xFF && dest != 0xFE
            && CHAT_DEST_IS_TEAM(dest) == 0) {
            return CMD_REJECT_INVALID;
        }
        if (CHAT_DEST_IS_TEAM(dest)) {
            const LobbyPlayer *sender =
                serverSimGetLobbyPlayer(sim, (BYTE)senderSlot);
            BYTE senderTeam = sender ? sender->teamNumber : 0;
            if (senderTeam == 0 || CHAT_DEST_TEAM_OF(dest) != senderTeam) {
                return CMD_REJECT_INVALID;  /* not your team / unassigned phantom */
            }
        }
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_CHAT;
        evt.u.chat.fromPlayer = (BYTE)senderSlot;
        evt.u.chat.destPlayer = p->destPlayer;
        evt.u.chat.bodyLen    = p->bodyLen;
        if (p->bodyLen > 0) {
            memcpy(evt.u.chat.body, p->body, p->bodyLen);
        }
        serverSimPublishControl(sim, &evt);
        {
            char pstr[256];
            int pLen = p->bodyLen;
            if (pLen > 255) pLen = 255;
            pstr[0] = (char)pLen;
            memcpy(pstr + 1, p->body, pLen);
            if (p->destPlayer == 0xFF) {
                logAddEvent(log_MessageAll, (BYTE)senderSlot, 0, 0, 0, 0, pstr);
            } else {
                logAddEvent(log_MessagePlayers, (BYTE)senderSlot,
                            p->destPlayer, 0, 0, 0, pstr);
            }
        }
        return CMD_OK;
    }
    case CMD_RATING_POSTED: {
        const CmdRatingPosted *p = &cmd->u.ratingPosted;
        if (p->key[0] == '\0') return CMD_REJECT_INVALID;
        /* The server has no view of WinBolo.net, so the key is passed
         * through unchecked; each receiving client compares it against the
         * round its own recap is showing. */
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_ROUND_RATING_POSTED;
        evt.u.ratingPosted.fromPlayer = (BYTE)senderSlot;
        memcpy(evt.u.ratingPosted.key, p->key, sizeof(evt.u.ratingPosted.key));
        evt.u.ratingPosted.key[sizeof(evt.u.ratingPosted.key) - 1] = '\0';
        serverSimPublishControl(sim, &evt);
        return CMD_OK;
    }
    case CMD_PLAYER_MUTE: {
        const CmdPlayerMute *p = &cmd->u.playerMute;
        if (p->targetPlayer >= MAX_TANKS) return CMD_REJECT_INVALID;
        /* Muting yourself is meaningless — you never receive your own
         * voice or chat. */
        if ((int)p->targetPlayer == senderSlot) return CMD_REJECT_INVALID;
        transportUdpServerSetVoiceMute((BYTE)senderSlot, p->targetPlayer,
                                       p->muted != 0);
        /* No control event: the mute is private to the muting client.
         * Broadcasting it would tell the muted player they were muted. */
        return CMD_OK;
    }
    case CMD_PLAYER_PING_MUTE: {
        const CmdPlayerPingMute *p = &cmd->u.playerPingMute;
        if (p->targetPlayer >= MAX_TANKS) return CMD_REJECT_INVALID;
        /* Muting yourself is meaningless — you always see your own ping. */
        if ((int)p->targetPlayer == senderSlot) return CMD_REJECT_INVALID;
        /* Sim-level state (unlike the voice mute, which lives in the UDP
         * transport): serverSimPingReachesClient is a sim predicate and reads
         * this mask, so the mask has to live where it can. No transport stub
         * needed as a result. Private to the muting client, so no event. */
        serverSimSetPingMute(sim, (BYTE)senderSlot, p->targetPlayer,
                             p->muted != 0);
        return CMD_OK;
    }
    case CMD_VOICE_STATE: {
        const CmdVoiceState *p = &cmd->u.voiceState;
        GameSim *gs = serverSimGetGameSim(sim);
        uint8_t oldFlags = playersGetClientFlags(&gs->plyrs, (BYTE)senderSlot);
        uint8_t flags = oldFlags;
        flags &= (uint8_t)~PLAYER_VOICE_FLAG_MASK;
        if (p->hasMic) {
            flags |= PLAYER_FLAG_HAS_MIC;
            /* Muted only means anything with a mic. Never setting the two
             * together leaves the receiving end a clean three states — no
             * mic, muted, live — rather than four with a nonsense one. */
            if (p->selfMuted) flags |= PLAYER_FLAG_VOICE_MUTED;
        }
        /* A client packs many commands into one PACKET_COMMAND_TICK, and
         * a re-send of the state the slot already holds is not a change.
         * Publishing it anyway would fan one reliable lobby-slot control
         * event per command to every client. */
        if (flags == oldFlags) return CMD_OK;
        playersSetClientFlags(&gs->plyrs, (BYTE)senderSlot, flags);
        /* During a running game the snapshot carries clientFlags every
         * tick, so the new bits reach every client on their own. The lobby
         * slot only goes out when it is published, so a change made while
         * clients are looking at the lobby has to publish it here. Same
         * gate as the lobby-slot heartbeat in server_lifecycle.c. */
        if (serverSimGetState(sim) == serverStateLobby ||
            serverSimGetState(sim) == serverStateCountdown) {
            serverSimPublishLobbySlot(sim, (BYTE)senderSlot);
        }
        return CMD_OK;
    }
    case CMD_VIEW_STATE: {
        /* Range check only, in any server state. The claim decides nothing by
         * itself, so one that names a target this server does not have is
         * degraded to the tank view rather than rejected — a client that was
         * viewing through a pill when it died would otherwise collect a
         * reject for a view it has already left. Whether the target still
         * qualifies (allied, alive, not carried, policy) is decided by the
         * viewport builder and serverSimValidateViewTargets. */
        uint8_t kind   = cmd->u.viewState.kind;
        uint8_t target = cmd->u.viewState.target;
        bool inRange = false;
        switch (kind) {
        case VIEW_KIND_PILL:
            inRange = (sim->sim.pb != NULL &&
                       target < pillsGetNumPills(&sim->sim.pb) &&
                       pillsIsActive(&sim->sim.pb, (BYTE)(target + 1)));
            break;
        case VIEW_KIND_BASE:
            inRange = (sim->sim.bs != NULL &&
                       target < basesGetNumBases(&sim->sim.bs) &&
                       basesIsActive(&sim->sim.bs, (BYTE)(target + 1)));
            break;
        case VIEW_KIND_ALLY:
            inRange = (target < MAX_TANKS && target != (uint8_t)senderSlot);
            break;
        default:
            break;  /* tank, or a kind this server does not know */
        }
        if (!inRange) {
            kind   = VIEW_KIND_TANK;
            target = 0;
        }
        sim->viewKind[senderSlot]   = kind;
        sim->viewTarget[senderSlot] = target;
        return CMD_OK;
    }
    case CMD_VIEW_CYCLE: {
        /* The server picks rather than the client because the client only
         * knows who is watchable from the snapshots it has been sent, and
         * under viewPolicyKey that is exactly the state the policy withholds:
         * left to itself the client would only ever reach allies it had
         * already seen. Pill and base selection stays client-side, so any
         * kind other than ally is accepted and answered with nothing — the
         * field is there so they could move here later. */
        BYTE target = 0, mapX = 0, mapY = 0;
        bool found;
        ControlEvent evt;

        if (cmd->u.viewCycle.kind != VIEW_KIND_ALLY) {
            return CMD_OK;
        }

        found = serverSimPickAlly(sim, (BYTE)senderSlot,
                                  cmd->u.viewCycle.direction,
                                  cmd->u.viewCycle.from,
                                  &target, &mapX, &mapY);
        if (found) {
            sim->viewKind[senderSlot]   = VIEW_KIND_ALLY;
            sim->viewTarget[senderSlot] = target;
        }
        /* Nothing found leaves the stored view alone: the client decides what
         * to do with the answer, and serverSimValidateViewTargets already
         * clears a target that has stopped qualifying. */

        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_VIEW_TARGET;
        evt.u.viewTarget.origSlot = (BYTE)senderSlot;
        evt.u.viewTarget.kind     = VIEW_KIND_ALLY;
        evt.u.viewTarget.fromEcho = cmd->u.viewCycle.from;
        evt.u.viewTarget.found    = found ? 1 : 0;
        if (found) {
            evt.u.viewTarget.target = target;
            evt.u.viewTarget.mapX   = mapX;
            evt.u.viewTarget.mapY   = mapY;
        }
        serverSimPublishControl(sim, &evt);
        /* Nothing to watch is an answer, not a rejection — a reject would
         * reach the player as an error. */
        return CMD_OK;
    }
    case CMD_ALLIANCE_REQUEST: {
        /* Alliances are made in a running round. In the lobby, teams are how
           sides change, and the client offers these commands in game only. */
        if (serverSimGetState(sim) != serverStateRunning) {
            return CMD_REJECT_BAD_STATE;
        }
        if (serverSimGetRanked(sim)) return CMD_REJECT_BAD_STATE;
        const CmdAllianceRequest *p = &cmd->u.allianceRequest;
        if (p->toPlayer >= MAX_TANKS) return CMD_REJECT_INVALID;
        if (!serverSimIsPlayerConnected(sim, p->toPlayer)) {
            return CMD_REJECT_INVALID;  /* silent on wire today; preserved */
        }
        /* Refused here rather than at the accept alone, so nobody is shown
           a request the scenario will not let them take. */
        if (!scenarioAllowsAlliance(sim, (BYTE)senderSlot, p->toPlayer)) {
            return CMD_REJECT_BAD_STATE;
        }
        /* Recorded so the accept can check it was asked for. */
        sim->allianceAskedBy[p->toPlayer] |= (uint16_t)(1u << senderSlot);
        logAddEvent(log_AllyRequest, (BYTE)senderSlot, p->toPlayer,
                    0, 0, 0, NULL);
        ControlEvent reqEvt;
        memset(&reqEvt, 0, sizeof(reqEvt));
        reqEvt.type = CTRL_ALLIANCE_REQUEST;
        reqEvt.u.allianceRequest.fromPlayer = (BYTE)senderSlot;
        reqEvt.u.allianceRequest.toPlayer   = p->toPlayer;
        serverSimPublishControl(sim, &reqEvt);
        return CMD_OK;
    }
    case CMD_ALLIANCE_ACCEPT: {
        BYTE newMember = cmd->u.allianceAccept.newMember;
        uint16_t asked;
        if (serverSimGetState(sim) != serverStateRunning) {
            return CMD_REJECT_BAD_STATE;
        }
        if (serverSimGetRanked(sim)) return CMD_REJECT_BAD_STATE;
        if (newMember >= MAX_TANKS) return CMD_REJECT_INVALID;
        /* Only a request the new member made can be accepted. Checked here
           and not in serverSimAcceptAlliance, so a scenario seating players
           and the tests that call the sim directly are not affected. */
        asked = (uint16_t)(1u << newMember);
        if ((sim->allianceAskedBy[senderSlot] & asked) == 0) {
            return CMD_REJECT_BAD_STATE;
        }
        /* Asked again: the scenario's answer may have changed since the
           request. The new member is the one who asked. A refusal leaves
           the request in place, as a decline does. */
        if (!scenarioAllowsAlliance(sim, newMember, (BYTE)senderSlot)) {
            return CMD_REJECT_BAD_STATE;
        }
        sim->allianceAskedBy[senderSlot] &= (uint16_t)~asked;
        serverSimAcceptAlliance(sim, (BYTE)senderSlot, newMember);
        return CMD_OK;
    }
    case CMD_ALLIANCE_LEAVE: {
        if (serverSimGetState(sim) != serverStateRunning) {
            return CMD_REJECT_BAD_STATE;
        }
        serverSimLeaveAlliance(sim, (BYTE)senderSlot);
        return CMD_OK;
    }
    case CMD_GAME_VOTE_TOGGLE: {
        serverSimGameVoteToggle(sim, (uint8_t)senderSlot,
                                cmd->u.gameVoteToggle.kind,
                                cmd->u.gameVoteToggle.toggleMode);
        return CMD_OK;
    }
    case CMD_LOBBY_REMOVE_BOT: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        if (cmd->u.lobbyRemoveBot.slot >= MAX_TANKS ||
            !serverSimIsBot(sim, cmd->u.lobbyRemoveBot.slot)) {
            return CMD_REJECT_INVALID;
        }
        serverSimRemoveBot(sim, cmd->u.lobbyRemoveBot.slot);
        return CMD_OK;
    }
    case CMD_LOBBY_SET_BOT_BRAIN: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        const CmdLobbySetBotBrain *p = &cmd->u.lobbySetBotBrain;
        if (p->slot >= MAX_TANKS || !serverSimIsBot(sim, p->slot) ||
            serverSimGetBrainPathForIdx(sim, p->brainIdx) == NULL) {
            return CMD_REJECT_INVALID;
        }
        serverSimSetBotBrainIdxFor(sim, p->slot, p->brainIdx);
        botManagerSetBrainIdx(sim, p->slot, p->brainIdx);
        return CMD_OK;
    }
    case CMD_LOBBY_OPEN_HOST: {
        /* Host-only: the toggle that enables openHost cannot be
         * gated through openHost itself. */
        if (senderSlot != serverSimGetHostSlot(sim)) return CMD_REJECT_NOT_HOST;
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (serverSimGetServerLocks(sim) & LOBBY_LOCK_OPEN_HOST) {
            return CMD_REJECT_LOCKED;
        }
        serverSimSetOpenHost(sim, cmd->u.lobbyOpenHost.openHost);
        return CMD_OK;
    }
    case CMD_MAP_SKIP_VOTE: {
        if (serverSimGetServerLocks(sim) & LOBBY_LOCK_MAP) {
            return CMD_REJECT_LOCKED;
        }
        serverSimMapSkipVoteToggle(sim, (uint8_t)senderSlot);
        return CMD_OK;
    }
    case CMD_NAME_CHANGE: {
        const CmdNameChange *p = &cmd->u.nameChange;
        if (winboloNetIsPlayerParticipant((BYTE)senderSlot)) {
            return CMD_REJECT_BAD_STATE;
        }
        char validated[PACKET_MAX_PLAYER_NAME];
        PlayerNameValidationError vErr = PLAYER_NAME_OK;
        if (!playerNameValidate(p->newName, validated, sizeof(validated), &vErr)) {
            switch (vErr) {
                case PLAYER_NAME_ERR_EMPTY:
                    return CMD_REJECT_NAME_EMPTY;
                case PLAYER_NAME_ERR_RESERVED_PREFIX:
                    return CMD_REJECT_NAME_RESERVED_PREFIX;
                case PLAYER_NAME_ERR_RESERVED_SUFFIX:
                    return CMD_REJECT_NAME_RESERVED_SUFFIX;
                case PLAYER_NAME_ERR_MIXED_SCRIPTS:
                    return CMD_REJECT_NAME_MIXED_SCRIPTS;
                case PLAYER_NAME_ERR_INVALID_UTF8:
                case PLAYER_NAME_ERR_DISALLOWED_CHAR:
                case PLAYER_NAME_ERR_TOO_LONG:
                default:
                    return CMD_REJECT_NAME_INVALID;
            }
        }
        /* Uniqueness check via the connected-player table. On UDP that's
         * udpServer.clients[]; the stub on SP returns NULL for every
         * slot (so SP-host effectively skips the check). */
        for (int j = 0; j < MAX_TANKS; j++) {
            if (j == senderSlot) continue;
            const char *other = transportUdpServerGetPlayerName((BYTE)j);
            if (other != NULL && playerNameCompare(other, validated) == 0) {
                return CMD_REJECT_NAME_TAKEN;
            }
        }
        if (validated[0] != '\0') {
            const char *oldName = transportUdpServerGetPlayerName((BYTE)senderSlot);
            char msg[30 + 2 * PACKET_MAX_PLAYER_NAME];
            snprintf(msg, sizeof(msg), "Player '%s' changed name to '%s'.",
                     oldName != NULL ? oldName : "?", validated);
            serverSimConsoleMessage(msg);
            transportUdpServerSetBotName((BYTE)senderSlot, validated);
            serverSimSetPlayerName(sim, (BYTE)senderSlot, validated);
        }
        return CMD_OK;
    }
    case CMD_LOCK_TOGGLE: {
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        bool allow = cmd->u.lockToggle.allow;
        if (serverSimIsAcceptingJoins(sim) == allow) return CMD_OK;  /* no-op */
        serverSimSetAllowNewPlayers(sim, allow);
        return CMD_OK;
    }
    case CMD_LOBBY_ADD_BOT: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby ||
            serverSimGetBotAiType(sim) == aiNone ||
            serverSimGetBotBrainPath(sim)[0] == '\0') {
            fprintf(stderr, "ADD_BOT reject BAD_STATE: lobbyEnabled=%d state=%d aiType=%d brain='%s'\n",
                    (int)serverSimIsLobbyEnabled(sim), (int)serverSimGetState(sim),
                    (int)serverSimGetBotAiType(sim), serverSimGetBotBrainPath(sim));
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) {
            fprintf(stderr, "ADD_BOT reject NOT_HOST: senderSlot=%d\n", (int)senderSlot);
            return CMD_REJECT_NOT_HOST;
        }
        /* Enforce the operator-configured -maxbots cap (0 = no cap). */
        BYTE maxBots = serverSimGetMaxBots(sim);
        if (maxBots > 0 && serverSimGetLobbyBotCount(sim) >= maxBots) {
            fprintf(stderr, "ADD_BOT reject BOT_LIMIT: count=%d max=%d\n",
                    (int)serverSimGetLobbyBotCount(sim), (int)maxBots);
            return CMD_REJECT_BOT_LIMIT;
        }
        const CmdLobbyAddBot *p = &cmd->u.lobbyAddBot;
        char validatedName[PACKET_MAX_PLAYER_NAME];
        bool haveName = (p->nameLen > 0);
        if (haveName) {
            char rawName[PACKET_MAX_PLAYER_NAME];
            memset(rawName, 0, sizeof(rawName));
            memcpy(rawName, p->name, p->nameLen);
            if (!lobbyBotNameAcceptable(rawName, validatedName,
                                        sizeof(validatedName), -1,
                                        transportUdpServerGetPlayerName,
                                        NULL, NULL)) {
                fprintf(stderr, "ADD_BOT reject INVALID: bot name not acceptable ('%s')\n", rawName);
                return CMD_REJECT_INVALID;
            }
        }
        BYTE slot;
        /* The one seat rule a bot has, shared with the scenario's spawn and
           lobby-add arms: the first free seat below MAX_TANKS, outside the
           human caps. */
        int freeSlot = serverSimFindFreeSlot(sim, true);
        if (freeSlot < 0) {
            fprintf(stderr, "ADD_BOT reject INVALID: no free slot (all %d slots in use)\n", MAX_TANKS);
            return CMD_REJECT_INVALID;
        }
        slot = (BYTE)freeSlot;
        char botName[64];
        if (haveName) {
            SDL_strlcpy(botName, validatedName, sizeof(botName));
        } else {
            snprintf(botName, sizeof(botName), "Bot %d", slot + 1);
        }
        /* A lobby Add Bot carries no configuration: NULL init, so the
         * brain sees an empty BRAIN_INIT. The team stays out of the add
         * and is written below, which is where this arm has always put
         * it. */
        if (!botManagerAddBot(sim, slot, serverSimGetBotBrainPath(sim), botName,
                              serverSimGetBotAiType(sim),
                              gameTypeGet(&serverSimGetGameSim(sim)->game),
                              serverSimGetGameSim(sim)->hiddenMines, 0, NULL)) {
            fprintf(stderr, "ADD_BOT reject INVALID: botManagerAddBot failed (slot=%d name='%s' brain='%s')\n",
                    (int)slot, botName, serverSimGetBotBrainPath(sim));
            return CMD_REJECT_INVALID;
        }
        transportUdpServerSetBotName(slot, botName);
        /* The same question the team-set arm asks. The bot is already seated
           by here, so unlike that arm the refusal cannot be a return code:
           it is the team write, and the bot lands unassigned where the
           roster shows it. */
        if (p->teamNumber > 0 && p->teamNumber < MAX_TANKS &&
            scenarioAllowsExtraTeams(sim, slot, p->teamNumber)) {
            serverSimSetTeam(sim, slot, p->teamNumber);
        }
        /* The new bot's brain mode and difficulty (serverSimResolveNewBotConfig):
         * the lobby default, then what the map requires for this side, then
         * what the host last picked by hand. Applied now the team is final —
         * a plain Add Bot only learns its team inside the add — and the lobby
         * brain reloads from this config at round start. Shown in the lobby
         * by a queued bot-config event, not one sent inside the command. */
        {
            const LobbyPlayer *lp = serverSimGetLobbyPlayer(sim, slot);
            serverSimApplyNewBotDefaults(sim, slot,
                                         lp ? (int)lp->teamNumber : 0,
                                         serverSimGetBotBrainPath(sim), TRUE);
        }
        serverSimPublishLobbySlot(sim, slot);
        serverSimPublishLobbyBotBrain(sim, slot);
        lobbyAutoUnreadyOnChange(sim);
        return CMD_OK;
    }
    case CMD_LOBBY_SET_MAP: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        const CmdLobbySetMap *p = &cmd->u.lobbySetMap;
        if (p->relPathLen == 0) return CMD_REJECT_INVALID;
        char relPath[256];
        memcpy(relPath, p->relPath, p->relPathLen);
        relPath[p->relPathLen] = '\0';
        bool safe = true;
        if (relPath[0] == '/' || relPath[0] == '\\') safe = false;
        else if (relPath[0] != '\0' && relPath[1] == ':') safe = false;
        else {
            for (const char *s = relPath; *s;) {
                if (s[0] == '.' && s[1] == '.' &&
                    (s[2] == '\0' || s[2] == '/' || s[2] == '\\')) {
                    safe = false; break;
                }
                while (*s && *s != '/' && *s != '\\') s++;
                while (*s == '/' || *s == '\\') s++;
            }
        }
        if (!safe) return CMD_REJECT_INVALID;
        /* Through the resolve rather than "<map root>/<relPath>": the host
           picked this name off a listing, and with a persist directory
           configured the virtual Uploads folder lives somewhere else, so
           building the path here would commit a different file from the one
           that was listed. */
        char fullPath[FILENAME_MAX];
        serverSimResolveMapPath(sim, relPath, fullPath, sizeof(fullPath));
        if (!serverSimReloadMap(sim, fullPath)) return CMD_REJECT_INVALID;
        return CMD_OK;
    }
    case CMD_SET_SCRIPT_LIST: {
        /* The whole script list at once: one scenario deciding the round and
           mods behind it changing how it plays. Lobby-only and host-only,
           like the map change above it: which scripts a round plays by is
           the operator's choice and not a joiner's. No LOBBY_LOCK_MAP test,
           for the same reason — the map command has none either. The lock
           hides the chooser on the client; the commands that test it
           server-side are the preview pair and the skip vote.

           Recording the list is not all this does: the list decides which
           scripts play, so the decision is made again the moment it changes
           (lobbyScenarioReselect), and the seating, the lobby rules and the
           settings event follow it exactly as they do on a map commit.

           A list and not an index-and-file pair: two hosts editing at the
           same moment would otherwise interleave into a list neither asked
           for, and the state here would be a splice rather than an
           assignment. */
        int                count;
        int                i;
        int                j;
        int                scenarios = 0;
        const ScnDirEntry *mapOwn;
        ScnDirEntry        rows[CMD_SCRIPT_LIST_MAX];
        ScnDirEntry       *dirRows;
        int                dirCount;
        CmdResult          result = CMD_OK;

        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        count = (int)cmd->u.setScriptList.count;
        /* The wire decoder refuses this already; a command submitted through
           the local transport never met it, and this arm is the one place
           both paths pass through. */
        if (count > CMD_SCRIPT_LIST_MAX) return CMD_REJECT_INVALID;
        /* The operator's -mod-required and -mod-locked rows, held here and
           nowhere else: the chooser draws them fixed, but a client is only
           ever asked, and this is the arm every list passes through.

           A locked list takes no list at all, an empty one included — not
           even the list it already holds, which would change nothing and
           still stamp the gap and recompose. Tested on the flag as well as on
           the lock bit, because the lock bit is the operator's -lock mask and
           a locked list is locked whatever that mask says.

           Otherwise a list must carry every row the operator fixed. The host
           may add around them, move them and take off anything else, a
           default-on -mod row included. Before the empty-list path, which
           would otherwise clear them, and before the gap is stamped or the
           directory read, so a refused list costs neither. The host, an
           admin and an open lobby's anyone are held to it alike: it is the
           operator's rule, not the host's. */
        if (serverSimGetOperatorModsLocked(sim) ||
            (serverSimGetServerLocks(sim) & LOBBY_LOCK_SCRIPT_LIST) != 0) {
            return CMD_REJECT_LOCKED;
        }
        for (i = 0; i < serverSimGetOperatorModCount(sim); i++) {
            const char *fixed = serverSimGetOperatorModFile(sim, i);
            bool        named = false;

            if (!serverSimOperatorModFixed(sim, fixed)) continue;
            for (j = 0; j < count && j < CMD_SCRIPT_LIST_MAX; j++) {
                if (strcmp(cmd->u.setScriptList.files[j], fixed) == 0) {
                    named = true;
                    break;
                }
            }
            if (!named) return CMD_REJECT_LOCKED;
        }
        /* An empty list clears, and is the one value that needs no name
           checks: there is no name to test the shape of or look up. It is
           exempt from the ranked refusal too, so a host who turned ranked on
           with scripts picked can still put the list back to empty.

           It is not exempt from the tick gap. No directory is read for it,
           but it recomposes like any other list, and that is the work the gap
           spaces out. */
        if (count <= 0) {
            if (sim->scenarioPickTick != 0 &&
                sim->tick + 1 - sim->scenarioPickTick <
                    SCENARIO_RELOAD_GAP_TICKS) {
                return CMD_REJECT_COOLDOWN;
            }
            sim->scenarioPickTick = sim->tick + 1;
            serverSimSetScriptList(sim, NULL, 0);
            lobbyScenarioReselect(sim);
            return CMD_OK;
        }
        /* A ranked game runs no script of any kind. Read the other way round
           from LST_RANKED, which is refused while a script is attached. */
        if (serverSimGetRanked(sim)) {
            return CMD_REJECT_BAD_STATE;
        }
        /* One list a second, per sim, before the directory is read, held to
           the same gap the reload is: looking a name
           up means listing the scenarios directory, which opens every file in
           it and runs the top level of every loose script on this thread. One
           reading serves the whole list, and this is what spaces those out. */
        if (sim->scenarioPickTick != 0 &&
            sim->tick + 1 - sim->scenarioPickTick < SCENARIO_RELOAD_GAP_TICKS) {
            return CMD_REJECT_COOLDOWN;
        }
        sim->scenarioPickTick = sim->tick + 1;
        /* The committed map's own script, or NULL for a map that brought
           none. It is the one bound file a list may name, and naming it is
           how a host says where on the list it goes: the round used to
           compose it at position 0 whatever the host did, because a region's
           identity was its place in the composed list, and a region now
           carries an identity of its own. See scnDecideScenario in
           src/scenario/scenario_host.c for what the row does once it is
           recorded. */
        mapOwn = serverSimGetMapScript(sim);
        /* One listing for the whole command, matched against every name on
           it. Reading the directory opens every file in it and runs the top
           level of every loose script, on this thread and under the sim lock,
           and it answers the same for the tenth name as for the first.

           On the heap rather than this thread's stack, the way
           serverSimEnumerateScenarioDir reads it: an entry carries a
           description, so a full listing runs to ~58 KB,
           which is more than a command handler a client's datagram reaches
           should put on the stack. No memory to read the directory into is no
           listing, which answers the same as a name the directory does not
           hold — so the allocation failing is not a separate refusal. */
        dirRows = (ScnDirEntry *)calloc((size_t)LOBBY_SCENARIO_LIST_MAX,
                                        sizeof(*dirRows));
        dirCount = dirRows != NULL
                       ? serverSimScenarioListDir(sim, dirRows,
                                                  LOBBY_SCENARIO_LIST_MAX)
                       : 0;
        for (i = 0; i < count; i++) {
            const char *file = cmd->u.setScriptList.files[i];
            if (file[0] == '\0') {
                result = CMD_REJECT_INVALID;
                goto scriptListDone;
            }
            if (!lobbyScenarioNameShapeOk(file)) {
                result = CMD_REJECT_INVALID;
                goto scriptListDone;
            }
            if (mapOwn != NULL && strcmp(mapOwn->file, file) == 0) {
                /* The map's own script, recorded from the row the server
                   published rather than looked up: the file sits beside the
                   .map or inside it, so the listing above does not hold it
                   and the lookup below would turn the whole list down. The
                   row is taken whole, which keeps its name, its
                   kind and its bound flag exactly as the attach read them —
                   and bound is what tells this row from a pick everywhere it
                   is read afterwards.

                   Named twice, it is refused by the duplicate test below
                   along with every other repeated name. */
                rows[i] = *mapOwn;
            } else {
                int  k;
                bool held = false;

                memset(&rows[i], 0, sizeof(rows[i]));
                for (k = 0; k < dirCount; k++) {
                    if (strcmp(dirRows[k].file, file) == 0) {
                        /* The directory's row rather than the wire's name, so
                           what is recorded is what the lister reported: the
                           file, the manifest's name and both flags, which is
                           what the list event publishes. */
                        rows[i] = dirRows[k];
                        held    = true;
                        break;
                    }
                }
                /* A name the directory does not carry is not one a host could
                   have picked. */
                if (!held) {
                    result = CMD_REJECT_INVALID;
                    goto scriptListDone;
                }
                /* Any other bound script belongs to a map that is not the
                   committed one: its tags, its regions and its entity indices
                   are that map's, so over this map they name items that are
                   not there. It arrives with its own map and plays when that
                   map is committed, which leaves nothing here for a host to
                   pick. */
                if (rows[i].bound) {
                    result = CMD_REJECT_INVALID;
                    goto scriptListDone;
                }
            }
            if (!rows[i].keepsWinCondition) scenarios++;
            /* The same file twice would load the same script twice, with two
               sets of its timers and its state running against each other.
               Refused rather than folded, so the host is told. */
            for (j = 0; j < i; j++) {
                if (strcmp(rows[j].file, rows[i].file) == 0) {
                    result = CMD_REJECT_INVALID;
                    goto scriptListDone;
                }
            }
        }
        /* At most one script that may end the round. Two of them would each
           believe the round is theirs to decide, and which one won would come
           down to the order they were called in.

           The map's own row counts here like any other. A list holding it and
           a picked scenario is two scenarios, which the compose would refuse
           anyway once it had read both files; refusing it here means the host
           is told at the moment of asking and the list that was playing is
           still playing. Dropping the map's row from the list is how a host
           says the picked scenario replaces it — which is what leaving it off
           the list has always meant. */
        if (scenarios > 1) {
            result = CMD_REJECT_INVALID;
            goto scriptListDone;
        }
        serverSimSetScriptList(sim, rows, count);
        lobbyScenarioReselect(sim);
scriptListDone:
        /* Every refusal above reaches here, so the one listing the arm read
           is freed whichever way it leaves. */
        free(dirRows);
        return result;
    }
    case CMD_SET_SCRIPT_SETTING: {
        /* The host choosing a value for one of a script's own settings.
           Lobby-only and host-only, like the list it is chosen beside: the
           value changes how the next round plays. The script need not be on
           the list yet, so a host can set a mod up before adding it.

           Nothing from the client is trusted past the names.
           serverSimSetScriptSetting reads the declaration for the file on
           this server, refuses an id it does not declare, a bool setting
           given anything but 0 or 1 and a choice given anything but the
           index of one of its words, and falls back to the default
           for an int value outside the range or off the step. */
        const CmdSetScriptSetting *s = &cmd->u.setScriptSetting;

        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        if (memchr(s->file, '\0', sizeof(s->file)) == NULL ||
            memchr(s->id, '\0', sizeof(s->id)) == NULL ||
            !lobbyScenarioNameShapeOk(s->file)) {
            return CMD_REJECT_INVALID;
        }
        /* A value the operator's -setting holds on a -mod-required or
           -mod-locked row is not the host's, whatever the client's dialog
           drew. Its other settings still are. */
        if (serverSimOperatorSettingLocked(sim, s->file, s->id)) {
            return CMD_REJECT_LOCKED;
        }
        if (!serverSimSetScriptSetting(sim, s->file, s->id, s->value, NULL)) {
            return CMD_REJECT_INVALID;
        }
        /* What the round plays by changed, so a ready player is asked to
           look again, as a map or list change asks. */
        lobbyAutoUnreadyOnChange(sim);
        return CMD_OK;
    }
    case CMD_LOBBY_RELOAD_SCENARIO: {
        /* The edit-reload-play loop the dedicated server's console already
           has, for a host with no console. Lobby-only and host-only, like
           the map change beside it: re-reading the script changes what the
           next round plays by, which is not a joiner's to decide. */
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        /* One reload a second, per sim, before the file is opened: the read
           and the check behind it are disk and Lua work on this thread, and
           a datagram may carry several commands. The sender is told by the
           toast, which is why this returns rather than sending a line. */
        if (sim->scenarioReloadTick != 0 &&
            sim->tick + 1 - sim->scenarioReloadTick <
                SCENARIO_RELOAD_GAP_TICKS) {
            return CMD_REJECT_COOLDOWN;
        }
        sim->scenarioReloadTick = sim->tick + 1;
        {
            char err[512];
            char line[672];
            ControlEvent evt;
            bool ok = serverSimScenarioReload(sim, err, sizeof(err));

            /* Addressed to whoever asked. A reload says what happened even
               when it worked, because what it changed is not visible until
               the next round starts.
               What a reload changes and what it does not: the script's bytes
               are swapped and nothing else is, so a round starting later
               boots the new rules and hooks, while the lobby the template
               seats, the scenario's name and description and the game type
               it declares are the map commit's and only change when the map
               is committed again. Kept inside the 128 bytes the text event
               carries, so the whole of it reaches the sender. */
            if (ok) {
                SDL_snprintf(line, sizeof(line),
                             "Scenario re-read: the next round boots its new "
                             "rules and hooks. Seats, name and game type "
                             "change on a map commit.");
            } else {
                SDL_snprintf(line, sizeof(line), "%s", err);
            }
            memset(&evt, 0, sizeof(evt));
            evt.type = CTRL_SERVER_TEXT;
            SDL_strlcpy(evt.u.serverText.text, line,
                        sizeof(evt.u.serverText.text));
            evt.u.serverText.destPlayer = (BYTE)senderSlot;
            serverSimPublishControl(sim, &evt);
        }
        /* Including a reload that failed: the line above went to whoever
           asked and carries the reason, and a reject code on top of it would
           add a second, contentless toast saying only that something was
           wrong. */
        return CMD_OK;
    }
    case CMD_LOBBY_PREVIEW_CANCEL: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        (void)serverSimRevertPreview(sim);
        return CMD_OK;
    }
    case CMD_LOBBY_PREVIEW_COMMIT: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        if (serverSimGetServerLocks(sim) & LOBBY_LOCK_MAP) {
            return CMD_REJECT_LOCKED;
        }
        serverSimCommitPreview(sim);
        return CMD_OK;
    }
    case CMD_LOBBY_PREVIEW_RANDOM: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        if (serverSimGetServerLocks(sim) & LOBBY_LOCK_MAP) {
            return CMD_REJECT_LOCKED;
        }
        const CmdLobbyPreviewRandom *p = &cmd->u.lobbyPreviewRandom;
        if (p->seedLen == 0 || p->seedLen > 63) return CMD_REJECT_INVALID;
        char seedStr[64];
        memset(seedStr, 0, sizeof(seedStr));
        memcpy(seedStr, p->seed, p->seedLen);
        MapGenConfig cfg;
        if (!mapGenSeedToConfig(seedStr, &cfg)) return CMD_REJECT_INVALID;
        cfg.x1 = MAP_MINE_EDGE_LEFT + 1;
        cfg.y1 = MAP_MINE_EDGE_TOP + 1;
        cfg.x2 = MAP_MINE_EDGE_RIGHT - 1;
        cfg.y2 = MAP_MINE_EDGE_BOTTOM - 1;
        if (!serverSimReloadRandomMap(sim, &cfg)) return CMD_REJECT_INVALID;
        return CMD_OK;
    }
    case CMD_LOBBY_KICK: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_NOT_HOST;
        }
        if (!lobbyClientMayEdit(sim, senderSlot)) return CMD_REJECT_NOT_HOST;
        uint8_t slot = cmd->u.lobbyKick.slot;
        if (slot >= MAX_TANKS || slot == serverSimGetHostSlot(sim) || (int)slot == senderSlot) {
            return CMD_REJECT_INVALID;
        }
        const char *name = transportUdpServerGetPlayerName(slot);
        if (name != NULL) transportUdpServerKickPlayer(sim, name);
        lobbyAutoUnreadyOnChange(sim);
        return CMD_OK;
    }
    case CMD_LOBBY_TRANSFER_HOST: {
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        /* Host-only — openHost must NOT grant transfer (a connected
         * player must not be able to hand off the host role). */
        if (senderSlot != serverSimGetHostSlot(sim)) return CMD_REJECT_NOT_HOST;
        uint8_t slot = cmd->u.lobbyTransferHost.slot;
        /* Target must be a connected human other than the current host
         * (self == host here, so the self/already-host cases coincide). */
        if (slot >= MAX_TANKS ||
            (int)slot == senderSlot ||
            slot == serverSimGetHostSlot(sim) ||
            !serverSimIsPlayerConnected(sim, slot) ||
            serverSimIsBot(sim, slot)) {
            return CMD_REJECT_INVALID;
        }
        serverSimSetHostSlot(sim, slot);
        return CMD_OK;
    }
    case CMD_LOBBY_SET_PASSWORD: {
        /* Host or admin only — openHost does NOT grant this. */
        bool isHost  = (senderSlot == serverSimGetHostSlot(sim));
        bool isAdmin = serverSimIsPlayerConnected(sim, (BYTE)senderSlot) &&
            (playersGetClientFlags(&serverSimGetGameSim(sim)->plyrs,
                                   (BYTE)senderSlot) & PLAYER_FLAG_ADMIN);
        if (!isHost && !isAdmin) return CMD_REJECT_NOT_HOST;
        if (serverSimGetServerLocks(sim) & LOBBY_LOCK_PASSWORD) {
            return CMD_REJECT_LOCKED;
        }
        const CmdLobbySetPassword *p = &cmd->u.lobbySetPassword;
        if (p->pwLen >= MAP_STR_SIZE) return CMD_REJECT_INVALID;
        serverSimSetPassword(sim, p->pwLen > 0 ? p->password : "", p->pwLen);
        serverSimSetHasPassword(sim, p->pwLen > 0);
        return CMD_OK;
    }
    case CMD_BALANCE_REQUEST: {
        if (senderSlot != serverSimGetHostSlot(sim)) return CMD_REJECT_NOT_HOST;
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        const BalanceProposal *bp = serverSimGetBalanceProposal(sim);
        if (bp->requestInFlight || bp->pending) return CMD_REJECT_BAD_STATE;
        if (!transportUdpServerStartBalanceRequest(
                sim, cmd->u.balanceRequest.teamSize,
                cmd->u.balanceRequest.includeBots)) {
            return CMD_REJECT_BAD_STATE;
        }
        return CMD_OK;
    }
    case CMD_BALANCE_APPLY: {
        if (senderSlot != serverSimGetHostSlot(sim)) return CMD_REJECT_NOT_HOST;
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        const BalanceProposal *bp = serverSimGetBalanceProposal(sim);
        if (!bp->pending) return CMD_REJECT_BAD_STATE;
        /* "Humans only" kicks every bot before applying the human-only
         * team assignments — the proposal contains no team for those
         * slots. The scenario's seats go too, and the next map change
         * seats them again. */
        if (!bp->includeBots) {
            serverSimRemoveAllBots(sim);
        }
        for (int i = 0; i < MAX_TANKS; i++) {
            if (bp->teamForSlot[i] != 0) {
                serverSimSetTeamBatch(sim, (BYTE)i, bp->teamForSlot[i]);
                serverSimPublishLobbySlot(sim, (BYTE)i);
            }
        }
        serverSimReapplyTeamAlliances(sim);
        /* The batch moves teamNumber and nothing else, so a swapped slot
         * still holds the start it picked for its old team, on its old
         * team's side, and the round honours a reservation as it stands.
         * Drop each reservation the new teams' sides no longer allow and
         * pick again for those slots, the way a team change does. A start
         * that is still allowed, one a player chose by hand included, is
         * kept; the slots that move are republished. */
        serverSimReleaseIneligibleStartsAndBackfill(sim, 0xFF);
        serverSimClearBalanceProposal(sim);
        /* Publish the cleared proposal so balanceProposalActive flips
         * back to false on every client — keeps canBalance gating from
         * staying disabled on the Balance-from-WBN button. */
        {
            ControlEvent clrEvt;
            memset(&clrEvt, 0, sizeof(clrEvt));
            clrEvt.type = CTRL_BALANCE_PROPOSAL;
            serverSimPublishControl(sim, &clrEvt);
        }
        logAddEvent(log_BalanceApplied, 0, 0, 0, 0, 0, NULL);
        serverSimConsoleMessage("Team balance applied");
        return CMD_OK;
    }
    case CMD_BALANCE_DISMISS: {
        if (senderSlot != serverSimGetHostSlot(sim)) return CMD_REJECT_NOT_HOST;
        if (!serverSimIsLobbyEnabled(sim) ||
            serverSimGetState(sim) != serverStateLobby) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!serverSimGetBalanceProposal(sim)->pending) return CMD_REJECT_BAD_STATE;
        serverSimClearBalanceProposal(sim);
        {
            ControlEvent evt;
            memset(&evt, 0, sizeof(evt));
            evt.type = CTRL_BALANCE_PROPOSAL;
            serverSimPublishControl(sim, &evt);
        }
        return CMD_OK;
    }
    case CMD_WBN_REAUTH: {
        /* Gate on WBN availability, not ranked mode: identity verification
         * (globe / supporter / steam-linked badges) applies on any
         * WBN-registered server, matching the join-time verify path. The
         * handler re-checks winbolonetIsRunning before touching the net. */
        if (!winbolonetIsRunning()) return CMD_REJECT_BAD_STATE;
        transportUdpServerHandleWbnReauth(sim, (BYTE)senderSlot,
                                          cmd->u.wbnReauth.token);
        return CMD_OK;
    }
    case CMD_PING: {
        /* A ping is a game-time signal drawn on the map, so it needs a
         * running game and a sender that still occupies a player slot in it:
         * a lobby sender has no map to point at, and an empty slot is
         * somebody who has left. The test is occupancy, not a live tank —
         * being dead is fine, because a player waiting to respawn has as much
         * to say about the map as anyone. */
        const CmdPing *p = &cmd->u.ping;
        BYTE slot = (BYTE)senderSlot;
        uint32_t now = sim->tick;
        if (serverSimGetState(sim) != serverStateRunning) {
            return CMD_REJECT_BAD_STATE;
        }
        /* The host can switch smart pings off for the whole server from the
         * lobby. Enforced here rather than only hidden in the sending
         * client's UI, so a modified client gains nothing by ignoring the
         * setting. Ahead of the rate limiting below on purpose: a ping the
         * server was never going to accept must not consume the sender's
         * budget, or turning pings back on would leave them throttled by
         * attempts that never drew anything. CMD_REJECT_BAD_STATE is the
         * same answer the two tests around it give — "this server is not
         * taking pings right now". */
        if (serverSimGetSmartPingsOff(sim)) {
            return CMD_REJECT_BAD_STATE;
        }
        if (!playersIsInUse(&sim->sim.plyrs, slot)) {
            return CMD_REJECT_BAD_STATE;
        }
        if (p->kind >= PING_KIND_COUNT) {
            return CMD_REJECT_INVALID;
        }
        /* World units are 256 per map square over a 256x256 map, so the
         * whole u16 range is on the map and only the sentinel-free bound
         * matters. Checked anyway so the arm still reads as a range check if
         * either constant ever changes. */
        if ((p->worldX >> M_W_SHIFT_SIZE) >= MAP_ARRAY_SIZE ||
            (p->worldY >> M_W_SHIFT_SIZE) >= MAP_ARRAY_SIZE) {
            return CMD_REJECT_INVALID;
        }
        /* Rate limit: a minimum gap, and two nested sliding windows on top of
         * it. All measured in sim ticks, which only advance while the game
         * runs — the only state this arm accepts. Every store holds tick+1 so
         * that 0 can mean "never", because tick 0 is itself a real tick. */
        if (sim->pingLastTick[slot] != 0 &&
            now + 1 - sim->pingLastTick[slot] < PING_RATE_MIN_GAP_TICKS) {
            return CMD_REJECT_COOLDOWN;
        }
        /* Count accepted pings still inside each window. The ring is small
         * (PING_SPAM_RING == the 30s cap), so a linear pass per ping is cheap.
         * A ping is refused when the ring already holds PING_SPAM_MAX_5S inside
         * the 5s window, or PING_SPAM_MAX_30S inside the 30s window. */
        {
            int count5 = 0, count30 = 0, j;
            for (j = 0; j < PING_SPAM_RING; j++) {
                uint32_t stamp = sim->pingBurstTicks[slot][j];
                uint32_t age;
                if (stamp == 0) continue;
                age = now + 1 - stamp;
                if (age < PING_SPAM_WINDOW_30S_TICKS) count30++;
                if (age < PING_SPAM_WINDOW_5S_TICKS)  count5++;
            }
            if (count5 >= PING_SPAM_MAX_5S)  return CMD_REJECT_COOLDOWN;
            if (count30 >= PING_SPAM_MAX_30S) return CMD_REJECT_COOLDOWN;
        }
        sim->pingLastTick[slot] = now + 1;
        sim->pingBurstTicks[slot][sim->pingBurstIdx[slot]] = now + 1;
        sim->pingBurstIdx[slot] =
            (uint8_t)((sim->pingBurstIdx[slot] + 1) % PING_SPAM_RING);

        {
            GameEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = EVENT_PING;
            ev.data[0] = slot;
            ev.data[1] = p->kind;
            ev.data[2] = (BYTE)(p->worldX >> 8);
            ev.data[3] = (BYTE)(p->worldX & 0xFF);
            ev.data[4] = (BYTE)(p->worldY >> 8);
            ev.data[5] = (BYTE)(p->worldY & 0xFF);
            /* The pending record IS this ping's queue — the arm deliberately
             * does not call serverSimAddEvent. It runs during packet receive,
             * before serverSimTick clears the per-frame event buffer, so an
             * event buffered here would be wiped before the post-tick UDP drain
             * could send it; and buffering it both here and at the flush would
             * deliver it twice to an in-process client, whose snapshot poll
             * dedups per serverTick and so would take the pre-clear copy on one
             * tick and the flushed copy on the next. serverSimFlushPendingPings,
             * at the top of the running tick, is the single point at which an
             * EVENT_PING enters sim->events. */
            sim->pendingPing[slot] = ev;
            sim->hasPendingPing[slot] = true;
        }
        /* Recorded whole so a replay can draw the marker where the sender
         * put it; the viewer culls nothing, since a replay watches every
         * team at once. */
        logAddEvent(log_Ping, slot, p->kind,
                    (BYTE)(p->worldX >> 8), (BYTE)(p->worldX & 0xFF),
                    p->worldY, NULL);
        return CMD_OK;
    }
    case CMD_NONE:
    default:
        return CMD_REJECT_BAD_STATE;
    }
}

CmdResult serverSimApplyCommand(ServerSim *sim, int senderSlot,
                                const ClientCommand *cmd) {
    assert(threadsCurrentlyHoldsMutex());
    /* Defense-in-depth: every inbound call site resolves the sender through
     * serverFindClient over the player table, which never yields a spectator
     * (spectators are tracked in a separate table and hold no tank slot). A
     * connected player slot is always in [0, MAX_TANKS); reject anything else
     * before dispatch so a non-player sender can never index sim state. Real
     * players always pass, so this cannot alter their behaviour. */
    if (senderSlot < 0 || senderSlot >= MAX_TANKS) {
        return CMD_REJECT_INVALID;
    }
    CmdResult r = applyCommandInner(sim, senderSlot, cmd);
    if (r != CMD_OK) {
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_COMMAND_REJECTED;
        evt.u.commandRejected.origCmdSeq  = cmd->cmdSeq;
        evt.u.commandRejected.origCmdType = (uint8_t)cmd->type;
        evt.u.commandRejected.reasonCode  = (uint8_t)r;
        evt.u.commandRejected.origSlot    = (uint8_t)senderSlot;
        serverSimPublishControl(sim, &evt);
    }
    return r;
}
