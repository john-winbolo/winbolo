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
 *Name:          Control Event
 *Filename:      control_event.h
 *Author:        John Morrison
 *Purpose:
 *  In-memory tagged-union event delivered from a server
 *  simulation to its in-process subscribers (bot ClientSims
 *  and, in single-player, the human ClientSim). Carries the
 *  out-of-band roster / lobby / phase information that does
 *  not ride on snapshots. Has no wire encoding.
 *********************************************************/

#ifndef CONTROL_EVENT_H
#define CONTROL_EVENT_H

#include "global.h"
#include "wire_limits.h"  /* PACKET_MAX_PLAYER_NAME */
#include "client_enums.h" /* netStatus, gameType */
#include "client_sim.h"   /* ClientLobbySlot */
#include "brain_list.h"   /* BrainList for CTRL_LOBBY_BRAIN_LIST */
#include "round_stats.h"  /* RoundStatsSummary for CTRL_ROUND_STATS */
#include "upload_policy.h" /* UploadPolicy in lobbySettings */
#include "view_policy.h"   /* ViewPolicy / VIEW_CATEGORY_COUNT in lobbySettings */

#ifndef LOBBY_TEAM_NAME_LEN
#define LOBBY_TEAM_NAME_LEN 32
#endif

typedef enum {
    CTRL_ALLIANCE_REQUEST,
    CTRL_ALLIANCE_ACCEPT,
    CTRL_ALLIANCE_LEAVE,
    CTRL_PLAYER_JOIN,
    CTRL_PLAYER_NAME,
    CTRL_LOBBY_SLOT,
    CTRL_LOBBY_SETTINGS,
    CTRL_LOBBY_MAP_CHANGE,
    CTRL_MAP_DOWNLOAD_COMPLETE,
    CTRL_BALANCE_PROPOSAL,
    CTRL_MAP_SKIP_STATE,
    CTRL_GAME_PHASE_LOBBY,
    CTRL_GAME_PHASE_COUNTDOWN,
    CTRL_GAME_PHASE_RUNNING,
    CTRL_GAME_PHASE_GAME_OVER,
    CTRL_GAME_OVER,
    CTRL_SERVER_SHUTDOWN,
    CTRL_CHAT,
    CTRL_PLAYER_LEAVE,
    /* Lobby state-change variants — per-team metadata, per-bot config,
     * per-bot brain path, brain-list catalogue. */
    CTRL_LOBBY_TEAM_META,
    CTRL_LOBBY_BOT_CONFIG,
    CTRL_LOBBY_BOT_BRAIN,
    CTRL_LOBBY_BRAIN_LIST,
    /* CTRL_LOBBY_BOT_POOL_CHUNK — one fragment of the server's bot
     * naming-pool catalog (a zlib-compressed blob), streamed during
     * join sync so clients render/pick from the SERVER's pools. The
     * client reassembles fragments seq 0..count-1, then installs. */
    CTRL_LOBBY_BOT_POOL_CHUNK,
    CTRL_GAME_VOTE_STATE,
    CTRL_SERVER_TEXT,
    CTRL_COMMAND_REJECTED,
    /* Single-event batch of the lobby alliance matrix. Replaces the
     * O(N²) per-pair CTRL_ALLIANCE_ACCEPT burst that serverSimReapply-
     * TeamAlliances used to fan out at every game start (worst case 120
     * events for 16 players on one team). The burst overflowed the
     * 128-deep per-client reliable control queue under loopback
     * latency, kicking the host from their own server. One event
     * carrying the full bitmap → one queue slot, regardless of N. */
    CTRL_ALLIANCE_RESET,
    /* CTRL_BALANCE_FAILED — server's balance worker finished without a
     * usable proposal (WBN returned non-200, null body, or an error
     * field). Unicast to the host slot via udpClientDeliverControl so
     * the lobby's "Asking WBN…" pill can flip to a failure label
     * immediately instead of waiting out the 8 s NOREPLY timeout. */
    CTRL_BALANCE_FAILED,
    /* CTRL_SHELL_DEATH — server tells a shell's owner their shell ended.
     * Unicast to `owner` via udpClientDeliverControl. The client matches
     * fireTick against its predicted shells to cull the ghost and draw the
     * impact at the authoritative position. */
    CTRL_SHELL_DEATH,
    /* CTRL_CHANNEL_RESET — at game start the server drops its previous-game
     * unacked send tail on the game (channel 0) and map (channel 1) reliable
     * channels and tells this client the new per-channel baselines. The client
     * lifts its game/map receive baselines to match, so any previous-game
     * straggler (seq below the baseline) dedup-drops instead of applying in the
     * new game. Carries no sim semantics — it must never reach the sim
     * dispatcher; the client consumes it at the channel-drain site. Per-client:
     * the baselines are this recipient's own channel state, set at enqueue. */
    CTRL_CHANNEL_RESET,
    /* CTRL_SPECTATOR_SLOT — one message per spectator roster slot,
     * transport-published to player clients so they can show who is
     * watching. specIdx is in [0, MAX_SPECTATORS). Mirrors
     * CTRL_LOBBY_SLOT but carries the trimmed spectator fields only. */
    CTRL_SPECTATOR_SLOT,
    /* CTRL_SPECTATOR_CHAT — a lobby chat line typed by a spectator. The
     * server stamps the sender's specIdx; clients resolve the name via
     * clientSimGetSpectatorSlot and render it [Spectator]-tagged in the
     * shared lobby chat log. Body carries the raw message text. */
    CTRL_SPECTATOR_CHAT,
    /* CTRL_LOBBY_SYNC_COMPLETE — terminal marker the server delivers as the
     * final event of a subscriber's join sync replay. The roster replay sets
     * inLobby before re-announcing every existing player/slot, so the client
     * cannot otherwise tell a replayed event from a live one. The client arms
     * lobbySyncSettled on this marker and plays lobby event sounds only once
     * it is set. No payload — header only. */
    CTRL_LOBBY_SYNC_COMPLETE,
    /* CTRL_ROUND_STATS — end-of-round scoreboard + awards, broadcast to all
     * connected clients at game over. Carries a RoundStatsSummary by value. */
    CTRL_ROUND_STATS,
    /* CTRL_ROUND_RATING_POSTED — fromPlayer has just had a rating or comment
     * accepted on the WinBolo.net page for round `key`. Broadcast to all
     * connected clients; each one re-reads that page if its own recap is on
     * the same round. Carries no rating or comment text — the round's page
     * on WinBolo.net stays the only source. */
    CTRL_ROUND_RATING_POSTED,
    /* CTRL_VIEW_TARGET — the server's answer to a CMD_VIEW_CYCLE request:
     * the item the sender should watch, picked from live state. Unicast to
     * origSlot via udpClientDeliverControl. kind is the ViewStateKind of the
     * chosen item and target its player number; mapX/mapY are where it is.
     * found == 0 means there was nothing to watch, and the client returns to
     * the tank view. fromEcho carries the request's `from` back so a late
     * answer to an earlier press can be told apart from the answer to the
     * current one. */
    CTRL_VIEW_TARGET,
    /* CTRL_NEWSWIRE_MUTE — server-owned "stop showing event newswire
     * lines" switch, broadcast to every client and replayed to a late
     * joiner. While it is set, clients drop the ENGINE-GENERATED
     * newswire (player quit, base and pill captures, builder lost,
     * name handover, ...) and the server drops its own "X has joined."
     * / "X has left." broadcasts. Deliberately-sent server text
     * (game.message -> CTRL_SERVER_TEXT) and player chat are NOT
     * affected: the scenario's own wave banners must still arrive.
     * Set by the scripted scenario around a staggered wave spawn or
     * despawn (see game.newswire_mute in src/server/scenario.c).
     * Cleared at every round reset so it can never stick. */
    CTRL_NEWSWIRE_MUTE,
    CTRL_EVENT_TYPE_COUNT   /* sentinel — must stay last */
} ControlEventType;

/* Body capacity for CTRL_CHAT.  Worst case is the localized server
 * message: 2 langid + 1 argCount + 4 * (1 lenByte + (PLAYER_NAME_LEN-1)
 * name bytes) = 263 bytes; rounded up for headroom. fromPlayer and
 * destPlayer are separate struct fields, not part of body[]. */
#define CHAT_BODY_MAX 272

/* Per-fragment payload cap for CTRL_LOBBY_BOT_POOL_CHUNK. Sized so one
 * fragment plus its header fits a single control datagram (well under
 * MAX_CONTROL_PACKET). A 64 KiB catalog therefore needs at most
 * ceil(65536/900) ≈ 73 fragments (< 255, the seq/count cap). */
#define LOBBY_BOT_POOL_CHUNK_FRAG_MAX 900

typedef struct ControlEvent {
    ControlEventType type;
    union {
        /* CTRL_ALLIANCE_REQUEST */
        struct {
            BYTE fromPlayer;
            BYTE toPlayer;
        } allianceRequest;

        /* CTRL_ALLIANCE_ACCEPT — acceptedBy invites newMember (a/b in plan) */
        struct {
            BYTE acceptedBy;
            BYTE newMember;
        } allianceAccept;

        /* CTRL_ALLIANCE_LEAVE — alliance leave, not player leave */
        struct {
            BYTE playerNum;
        } allianceLeave;

        /* CTRL_ALLIANCE_RESET — full alliance matrix snapshot.
         * Per-player ally bitmap: bit j set in allies[i] ⇔ slot i and
         * slot j are allied. Apply order on the client: clear every
         * slot's alliance, then re-accept per the matrix (mirrors the
         * server's reapplyTeamAlliances rebuild). Self-bit is set for
         * every connected slot; unconnected slots are 0. */
        struct {
            uint16_t allies[MAX_TANKS];
        } allianceReset;

        /* CTRL_PLAYER_JOIN */
        struct {
            BYTE  playerNum;
            char  name[PACKET_MAX_PLAYER_NAME];
            char  country[3];           /* 2 chars + NUL */
            uint8_t clientType;
            uint8_t clientFlags;
            BYTE  numAllies;
            BYTE  allies[MAX_TANKS];
        } playerJoin;

        /* CTRL_PLAYER_LEAVE — server announces a player has disconnected.
         * Wire counterpart is PACKET_PLAYER_LEFT. */
        struct {
            BYTE playerNum;
            char name[PACKET_MAX_PLAYER_NAME];
            char country[3];            /* 2 chars + NUL */
            BYTE silent;                /* 1 = scenario-scripted removal:
                                         * clients skip the "has quit"
                                         * newswire line (wave churn).
                                         * Optional on the wire — old
                                         * senders decode as 0. */
        } playerLeave;

        /* CTRL_PLAYER_NAME */
        struct {
            BYTE playerNum;
            char name[PACKET_MAX_PLAYER_NAME];
        } playerName;

        /* CTRL_LOBBY_SLOT */
        struct {
            BYTE             playerNum;
            ClientLobbySlot  slot;
        } lobbySlot;

        /* CTRL_SPECTATOR_SLOT — one message per spectator slot. */
        struct {
            uint8_t             specIdx;
            ClientSpectatorSlot slot;
        } spectatorSlot;

        /* CTRL_SPECTATOR_CHAT — a spectator's lobby chat line. specIdx is
         * the sender's spectator slot; body/bodyLen is the raw message
         * text (no length prefix, bodyLen <= PACKET_MAX_CHAT_MESSAGE). */
        struct {
            uint8_t  specIdx;
            uint16_t bodyLen;
            uint8_t  body[PACKET_MAX_CHAT_MESSAGE];
        } spectatorChat;

        /* CTRL_LOBBY_SETTINGS */
        struct {
            char     mapName[MAP_STR_SIZE];
            gameType lobbyGameType;
            bool     lobbyHiddenMines;
            uint8_t  lobbyAiType;
            int32_t  lobbyTimeLimit;
            int32_t  lobbyStartDelay;
            uint8_t  lobbyPillCount;
            uint8_t  lobbyBaseCount;
            uint8_t  lobbyStartCount;
            bool     mapSkipAvailable;
            netStatus netStat;          /* current phase (lobby/running/...) */
            bool     hasLobby;          /* server capability: runs a lobby at all
                                         * (= sim->lobbyEnabled). True even mid-game;
                                         * distinct from netStat/the client's inLobby
                                         * phase flag. */
            /* Layout A flags */
            bool     lobbyOpenHost;
            bool     lobbyAutoLockOnGameStart;
            bool     lobbyRanked;
            bool     lobbyAllowNewPlayers;
            bool     lobbyWbnAvailable;  /* host's winbolonetIsRunning() —
                                          * gates WBN-only UI (Balance
                                          * from WBN) on remote clients */
            uint16_t lobbyServerLocks;
            UploadPolicy uploadPolicy;
            uint8_t  hostSlot;   /* current lobby host's player slot */
            /* Visibility rules, indexed by ViewCategory. */
            ViewPolicy viewPolicy[VIEW_CATEGORY_COUNT];
            uint16_t   viewDecaySecs[VIEW_CATEGORY_COUNT];
            bool     lobbyClassicMode;  /* server is running classic mode */
            bool     lobbyAlliesInTrees; /* server sends allies standing in
                                          * trees to their allies */
            bool     lobbyScenarioMap; /* current map has a scenario
                                        * sidecar loaded — gates the
                                        * "Scenario" game-type option */
            char     lobbyScenarioDesc[256]; /* scenario.description blurb
                                        * for the lobby map info; "" =
                                        * none */
            bool     lobbyScenarioExtraTeams; /* scenario allows teams
                                        * beyond its two sides (Add Team
                                        * button); true on plain maps */
        } lobbySettings;

        /* CTRL_LOBBY_MAP_CHANGE — no payload fields needed */
        struct {
            uint8_t _unused;
        } lobbyMapChange;

        /* CTRL_MAP_DOWNLOAD_COMPLETE — no payload fields needed */
        struct {
            uint8_t _unused;
        } mapDownloadComplete;

        /* CTRL_BALANCE_PROPOSAL — proposed team per slot (0 = none) */
        struct {
            BYTE teamForSlot[MAX_TANKS];
        } balanceProposal;

        /* CTRL_MAP_SKIP_STATE — one byte per slot, 0 or 1, mirrors wire */
        struct {
            BYTE votes[MAX_TANKS];
        } mapSkipState;

        /* CTRL_GAME_PHASE_COUNTDOWN carries countdownSeconds; the other
         * CTRL_GAME_PHASE_* siblings have no body. */
        struct {
            int countdownSeconds;
        } gamePhase;

        /* CTRL_GAME_OVER */
        struct {
            uint8_t _unused;
        } gameOver;

        /* CTRL_SERVER_SHUTDOWN */
        struct {
            uint8_t _unused;
        } serverShutdown;

        /* CTRL_CHAT — server-fanned PACKET_CHAT_BROADCAST.  fromPlayer
         * discriminates the body interpretation: 0..MAX_TANKS-1 = real
         * player chat (raw text), 0xFE = server raw English, 0xFF =
         * server localized (packed langid+args).  body[] is opaque to
         * the codec; consumers interpret it according to fromPlayer.
         * destPlayer is 0xFF for broadcast or a slot index for unicast. */
        struct {
            BYTE     fromPlayer;
            BYTE     destPlayer;
            uint16_t bodyLen;
            uint8_t  body[CHAT_BODY_MAX];
        } chat;

        /* CTRL_LOBBY_TEAM_META — per-team metadata (name, color,
         * naming pool, start side, in_use). teamId 0 is the unassigned
         * sentinel and is never carried by this event. */
        struct {
            uint8_t teamId;        /* 1..MAX_TANKS-1 */
            uint8_t in_use;
            uint8_t color;
            uint8_t namingPool;
            uint8_t startSide;     /* START_SIDE_* (start_sides.h) */
            char    name[LOBBY_TEAM_NAME_LEN];
        } lobbyTeamMeta;

        /* CTRL_LOBBY_BOT_CONFIG — per-bot difficulty/personality +
         * the display name pulled from the players table at fill
         * time. (Name is informational here — players.c remains the
         * source of truth via CTRL_PLAYER_NAME / lobbySlot.) */
        struct {
            uint8_t slot;
            uint8_t difficulty;
            uint8_t personality;
            char    name[PACKET_MAX_PLAYER_NAME];
        } lobbyBotConfig;

        /* CTRL_LOBBY_BOT_BRAIN — per-bot brain selection as an index
         * into the server's brain catalogue. brainIdx == 0xFF means
         * "fall back to the server's global bot brain". */
        struct {
            uint8_t slot;
            uint8_t brainIdx;
        } lobbyBotBrain;

        /* CTRL_LOBBY_BRAIN_LIST — server's discovered brain catalogue,
         * used to populate the AiConfig combobox. */
        struct {
            BrainList list;
        } lobbyBrainList;

        /* CTRL_LOBBY_BOT_POOL_CHUNK — fragment `seq` of `count` of the
         * server's compressed bot-pool catalog blob. fragLen bytes live
         * in frag[]. Reassembled and installed client-side. */
        struct {
            uint8_t  seq;
            uint8_t  count;
            uint16_t fragLen;
            uint8_t  frag[LOBBY_BOT_POOL_CHUNK_FRAG_MAX];
        } lobbyBotPoolChunk;

        /* CTRL_SERVER_TEXT — server-originated chat broadcast.
         * Mirrors what UDP clients receive as
         * PACKET_CHAT_BROADCAST(fromPlayer=0xFE). Lets in-process
         * subscribers (SP / host) see the same lines. */
        struct {
            char    text[PACKET_MAX_CHAT_MESSAGE + 1];
            uint8_t destTeam;  /* 0 = everyone; 1-16 = deliver only to that team.
                                  Server-side recipient filter (udpClientDeliver +
                                  the in-process handler); not sent on the wire. */
        } serverText;

        /* CTRL_GAME_VOTE_STATE — mirrors PACKET_GAME_VOTE_STATE. */
        struct {
            uint8_t  kind;             /* GAME_VOTE_KIND_* */
            uint8_t  active;           /* GAME_VOTE_ACTIVE_* */
            uint8_t  triggerSrc;       /* GAME_VOTE_TRIGGER_* */
            uint8_t  teamId;           /* surrender only; 0 = all-teams */
            uint8_t  threshold;        /* yes-count needed to pass */
            uint8_t  yesCount;
            uint8_t  noCount;
            uint8_t  eligibleCount;
            uint8_t  secondsRemaining; /* 0..60 */
            uint16_t votes;            /* bitmask of slots that voted yes */
        } gameVoteState;

        /* CTRL_COMMAND_REJECTED — serverSimApplyCommand rejected a
         * ClientCommand. origSlot is the senderSlot the dispatcher
         * attributed the command to; udpClientDeliverControl drops the
         * event for any recipient whose playerNum != origSlot, so it
         * reaches only the originator. origCmdSeq is the client-
         * assigned cmdSeq from the offending ClientCommand (zero from
         * callers that don't yet maintain a counter). origCmdType is
         * (uint8_t)cmd->type; reasonCode is (uint8_t)CmdResult.
         * Subscribers correlate by origCmdSeq and dismiss when stale —
         * the event is informational, not authoritative. */
        struct {
            uint32_t origCmdSeq;
            uint8_t  origCmdType;
            uint8_t  reasonCode;
            uint8_t  origSlot;
        } commandRejected;

        /* CTRL_BALANCE_FAILED — single-byte reason code so the host
         * UI can distinguish "WBN said no" from "no eligible players"
         * later. reasons today: 1 = http (transport/status), 2 = error
         * field in WBN body, 3 = internal (thread/state). */
        struct {
            uint8_t reasonCode;
        } balanceFailed;

        /* CTRL_SHELL_DEATH — server tells a shell's owner their shell ended.
         * Unicast to `owner` via udpClientDeliverControl. The client matches
         * fireTick against its predicted shells and culls the ghost so it
         * stops flying on past the server's impact at high ping. outcome is a
         * SHELL_OUTCOME_* (shells.h); the server emits EXPIRED / IMPACT /
         * TANK_HIT / TANK_KILL, with SHELL_OUTCOME_REJECTED reserved on the
         * wire (never emitted today). impactWX/impactWY/outcome are carried
         * for forward use (e.g. a future kill cue at the death position) and
         * are NOT consumed by the client today — the impact visual already
         * comes from the authoritative EVENT_EXPLOSION the owner receives. */
        struct {
            uint32_t fireTick;   /* originating client input tick (matches predictedShells[].fireTick) */
            uint16_t impactWX;   /* world X of the death/impact (tank position for REJECTED) */
            uint16_t impactWY;
            uint8_t  owner;      /* shell owner's player slot — the sole recipient */
            uint8_t  outcome;    /* SHELL_OUTCOME_* */
        } shellDeath;

        /* CTRL_CHANNEL_RESET — per-channel receive baselines the client must
         * adopt when the server re-bases a reliable channel. channelMask names
         * which channels this event re-bases (bit c set => channel index c);
         * each named channel's post-reset sequence floor is carried in the
         * matching baseline field below. The game-start reset re-bases the game
         * (ch0) and map (ch1) channels together; a lobby map change re-bases the
         * bulk (ch3) channel alone so an in-flight map download drops cleanly.
         * The control channel (ch2) is the carrier and is never reset. */
        struct {
            uint8_t  channelMask;   /* bit c set => ch<c>Baseline is valid     */
            uint32_t ch0Baseline;   /* CHANNEL_GAME  floor (bit 0)             */
            uint32_t ch1Baseline;   /* CHANNEL_MAP   floor (bit 1)             */
            uint32_t ch3Baseline;   /* CHANNEL_BULK  floor (bit 3)             */
        } channelReset;

        /* CTRL_ROUND_STATS — end-of-round scoreboard + awards, broadcast to all. */
        RoundStatsSummary roundStats;

        /* CTRL_ROUND_RATING_POSTED — who posted, and the round they posted
         * against. The server copies the key through without inspecting it. */
        struct {
            BYTE fromPlayer;
            char key[ROUND_STATS_LOGKEY_LEN];
        } ratingPosted;

        /* CTRL_VIEW_TARGET — the item the server picked for the requesting
         * client to watch, and the request's `from` copied back. */
        struct {
            BYTE origSlot;   /* slot the answer is for */
            BYTE kind;       /* ViewStateKind of the chosen item */
            BYTE target;     /* player number of the chosen ally */
            BYTE mapX;
            BYTE mapY;
            BYTE found;      /* 0 when there was nothing to watch */
            BYTE fromEcho;   /* the request's `from`, copied back */
        } viewTarget;

        /* CTRL_NEWSWIRE_MUTE — 1 = engine newswire suppressed, 0 = normal.
         * One byte on the wire; the server only publishes it on a change. */
        struct {
            BYTE muted;
        } newswireMute;
    } u;
} ControlEvent;

#endif /* CONTROL_EVENT_H */
