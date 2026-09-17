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
#include "server_voice_mode.h" /* ServerVoiceMode in lobbySettings */
#include "scenario_panel.h" /* SCN_PANEL_MAX for the panel event's byte list */
#include "sim_rules_names.h" /* SIM_RULE_COUNT — the scenario rules event's
                             * row cap, taken from the rule list itself */

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
    /* CTRL_STATS_SEED — the server's running per-slot round stats, unicast
     * to one joiner inside its sync replay while a round is running. The
     * client counts its live scoreboard from the game-event stream, so it
     * knows only what happened since it joined; this seeds that accumulator
     * from the server's own PlayerRoundStats so a mid-round joiner sees the
     * real numbers. Carries the same RoundPlayerSummary rows CTRL_ROUND_STATS
     * ships, without the awards, highlights and log key. */
    CTRL_STATS_SEED,
    /* CTRL_VOICE_TALKING — who is producing voice right now, as one
     * bitmap over the player slots. Broadcast in the lobby and the
     * countdown only, where voice is all-talk and the set says nothing a
     * listener could not already hear. Deliberately not sent in a running
     * game: voice there is alliance-only, and broadcasting the set would
     * tell a player that an enemy is speaking. The client intersects it
     * with its own mute list — a muted player sends nothing that reaches
     * you, so this is the only way to show that they are talking. */
    CTRL_VOICE_TALKING,
    /* CTRL_ENTITY_CHANGE — one pillbox, base or start has joined the map
     * or left it. The map's item lists are fixed at map load for a normal
     * round; a scenario can change them mid-round, and this is how a client
     * hears about it on the tick it happens. Removal keeps the item's slot
     * and its index, so every index above a removal goes on naming the same
     * item, and the record the event carries is the item's map data only.
     * Broadcast: which items are on the map is public, the way the map is. */
    CTRL_ENTITY_CHANGE,
    /* CTRL_ENTITY_SYNC — which pillboxes, bases and starts are on the map
     * right now, as one bit per index. A client builds its three item lists
     * from the compressed map blob, and the blob carries the items but not
     * which of them are still on the map, so installing one marks every item
     * up to the count as on the map. That is right for a map as it loads and
     * wrong for a client that arrives after a removal, which is what this
     * corrects: the server sends it once the client has the blob, and the
     * client clears the live flag of every index the mask does not name. The
     * counts stay the blob's — a mask says which of those slots hold an item,
     * never how many slots there are.
     *
     * Sent to one recipient rather than published: it answers a map the
     * recipient has just taken a copy of, so the send sites are the ones that
     * hand a copy over — the subscriber sync replay and the completion of a
     * client's bulk map transfer. The masks read the same for everyone, so
     * the body carries no per-recipient state; a change after this event
     * travels as a CTRL_ENTITY_CHANGE to everyone at once. */
    CTRL_ENTITY_SYNC,
    /* CTRL_SIM_RULES — the gameplay numbers the simulation is running on,
     * for every rule a client reads. A server and its clients clamp the
     * same pill and base records, predict the same tank movement and fire
     * the same shells, so they have to be reading the same table; a client
     * left on the classic one quietly disagrees with the server about how
     * far a tank slides, how much a shell takes off a base and what a
     * pillbox's armour caps at.
     *
     * Carries only the rules a client reads. The rest of the table — the
     * builder's costs, the base refuel and give amounts, the terrain
     * lifetimes and the tree weights — is read by server code alone and
     * stays on the server. Broadcast: the numbers are the same for
     * everybody and none of them is private.
     *
     * Published at every round start — from the two authoritative start
     * functions themselves, so no start path can forget to state its
     * table — again whenever a scenario changes a rule the event carries,
     * and replayed into a joining client's sync so it arrives with the
     * table the round is already using. */
    CTRL_SIM_RULES,
    /* CTRL_LOBBY_BRAIN_DOCS_CHUNK — one fragment of ONE brain's lobby
     * texts: its announce.txt line and its commands.txt docs. Sent per
     * BRAIN, not per bot, alongside the brain list, and only for the
     * brains that ship the files. The client reassembles fragments
     * seq 0..count-1 for brainIdx, then installs both strings.
     *
     * Appended at the END of this enum on purpose: the tables in
     * transport_control_codec.c are indexed by it, so a new type goes
     * last rather than shifting the ones already there. */
    CTRL_LOBBY_BRAIN_DOCS_CHUNK,
    /* The four a scenario presents with. Each is body-only on
     * CHANNEL_CONTROL, as CTRL_ENTITY_SYNC and CTRL_SIM_RULES are:
     * there is no full-packet wrapper and no PACKET_* type.
     *
     * The codecs are recipient-agnostic. Three of the four carry a
     * destTeam/destPlayer pair that never travels; udpClientDeliverControl
     * filters on it the way it filters CTRL_SERVER_TEXT, and
     * client_sim_control.c applies the same test for in-process
     * subscribers. CTRL_SCN_SCORE has no pair: it is broadcast, and its
     * target says whose score it is, not who receives it.
     *
     * Appended at the END, like CTRL_LOBBY_BRAIN_DOCS_CHUNK above: the
     * tables in transport_control_codec.c are indexed by this enum, so
     * a new type goes last rather than shifting the ones already there. */
    CTRL_SCN_PANEL,
    CTRL_SCN_SCORE,
    CTRL_SCN_ANNOUNCE,
    CTRL_SCN_MARKER,
    /* CTRL_SCENARIO_RULES — the rules a scenario's own manifest sets, so a
     * host can answer "what do these mods change?" without opening the file.
     * The author's table rather than the table the round is running on: a
     * scenario changing a rule mid-round moves the second and not this one,
     * and CTRL_SIM_RULES is what says what the simulation is reading now.
     *
     * Published beside the identity, so a scenario that attaches states its
     * set and one that detaches states an empty one. A map that has never
     * had a scenario publishes neither: an empty set says a scenario has
     * just gone, and a map with none never had one to go.
     *
     * Broadcast and body-only on CHANNEL_CONTROL, as CTRL_SIM_RULES is:
     * the manifest is as public as the name and the description the lobby
     * settings event already carries. Replayed into a joining client's sync
     * while a scenario is attached, so a mid-lobby joiner reads the same
     * table the host does.
     *
     * Appended at the END, like the four above: the tables in
     * transport_control_codec.c are indexed by this enum. */
    CTRL_SCENARIO_RULES,
    CTRL_EVENT_TYPE_COUNT   /* sentinel — must stay last */
} ControlEventType;

/* How many rows CTRL_SCENARIO_RULES can carry: one per rule there is, taken
 * from SIM_RULE_LIST rather than written out, so a rule added to that list
 * cannot overflow the event. A manifest names each rule at most once, so a
 * set can hold every rule and no more.
 *
 * The count and each row's rule index travel as one byte, which is what the
 * assertion below holds the list to. */
#define CTRL_SCENARIO_RULES_MAX SIM_RULE_COUNT
BOLO_STATIC_ASSERT(CTRL_SCENARIO_RULES_MAX <= 255,
                   ctrl_scenario_rules_count_and_index_fit_a_byte);

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

/* Per-fragment payload cap for CTRL_LOBBY_BRAIN_DOCS_CHUNK, and the size
 * of one brain's whole text blob on the wire:
 *   [announceLen 2 BE][announce][docsLen 2 BE][docs]
 * The blob is at most 2 + 512 + 2 + 16384 = 16900 bytes, so at 900 bytes a
 * fragment the worst case is ceil(16900/900) = 19 fragments per brain and
 * the seq/count byte is never near its limit. The fragment cap itself does
 * NOT move with BRAIN_DOCS_MAX: it is what makes one fragment plus its
 * header fit a single control datagram. */
#define LOBBY_BRAIN_DOCS_FRAG_MAX 900
#define LOBBY_BRAIN_DOCS_WIRE_MAX (2 + BRAIN_ANNOUNCE_MAX + 2 + BRAIN_DOCS_MAX)

/* Which of the three item lists a CTRL_ENTITY_CHANGE names. The values
 * ride the wire, so they are written out rather than left to the order
 * of the members. */
typedef enum {
    ENTITY_KIND_PILL  = 0,
    ENTITY_KIND_BASE  = 1,
    ENTITY_KIND_START = 2
} EntityKind;

/* Where the lobby's scenario came from. The values ride the wire, so they
 * are written out. None is 0, so a lobby with no scenario is the zeroed
 * event and writes no scenario bytes at all. A scenario read from beside
 * the map is lobbyScenarioMap; lobbyScenarioMod is one the host picked from
 * the server's scenarios directory, which plays over whichever map is
 * committed and stands in place of that map's own scenario. */
typedef enum {
    lobbyScenarioNone = 0,
    lobbyScenarioMap  = 1,
    lobbyScenarioMod  = 2
} LobbyScenarioSource;

/* What CTRL_LOBBY_SETTINGS carries of a scenario's identity. Independent of
 * the scenario host's own caps (which a public header must not reach for)
 * and chosen to match them: the name and the description are the manifest's,
 * and the file name is a name rather than a path, so a server's disk layout
 * does not travel. Longer text is truncated where the sim is told, not on
 * the wire.
 *
 * The file length is the scenarios directory's own (SCN_DIR_FILE_LEN, 128),
 * so a file name that a listing shows in full is the same name this event
 * carries in full rather than one cut to fit. It costs no wire bytes to hold
 * the wider figure: the tail is written only when a scenario is attached, and
 * each of the three strings is a length byte and that many bytes, so a
 * 20-character file name travels as 21 bytes either way.
 *
 * transport_udp_client.c is where the three file lengths — this one, the
 * client list accumulator's LOBBY_SCENARIO_LIST_FILE_LEN and the public
 * SERVER_SCENARIO_FILE_LEN the directory entry is copied through — are held
 * against each other, because it is the translation unit that sees all
 * three. */
#define LOBBY_SCENARIO_NAME_LEN 64
#define LOBBY_SCENARIO_FILE_LEN 128
#define LOBBY_SCENARIO_DESC_LEN 256

/* Which rules CTRL_SIM_RULES carries, and how wide each one goes.
 *
 * A rule is here because code a ClientSim reaches reads it: the movement
 * and gunsight code the client predicts with, the shell numbers it fires
 * predicted shells on, the base thresholds it draws and tests captures
 * against, the caps its own clamp pass applies to the pill and base
 * records a map hands it, and the rules view a bot's brain is given every
 * tick. A rule only the server reads is not here — sending it would be
 * bytes no client could use.
 *
 * The widths are each rule's own range, from the checks in sim_rules.c:
 * a row bounded 0..255 travels as one byte and a row bounded by another
 * row travels as wide as that bound allows. Two of them lean on a pair
 * rather than on their own row: pill_attack_min_ticks cannot exceed
 * pill_attack_ticks and base_capture_armour cannot exceed
 * base_full_armour, both of which are bounded at 255, so both fit a byte.
 *
 * The float rules travel as their four raw bytes. A fixed-point scale
 * would round them: tank_accel_rate is allowed down to 0.01, and at ×256
 * that is 2.56, which rounds to 3 and arrives as 0.0117 — a client
 * predicting with a number the server is not simulating with, which is
 * the disagreement this event exists to end.
 *
 * One list per width, each in the order SimRules declares its fields. The
 * variant's members, the encoder, the decoder, the server's fill and the
 * client's apply are all written from these lists, so a rule cannot be
 * encoded and not decoded, or sent and not applied. */
#define CTRL_SIM_RULES_U8_FIELDS(F)                                      \
    F(tank_reload_ticks) F(tank_full_shells) F(tank_full_mines)          \
    F(tank_full_trees) F(tank_full_armour) F(tank_water_ticks)           \
    F(shell_damage) F(mine_damage) F(mine_fatal_divisor)                 \
    F(water_loss_shells) F(water_loss_mines) F(just_fired_ticks)         \
    F(gunsight_min) F(gunsight_max) F(tank_min_move) F(tank_hit_radius)  \
    F(tank_nudge_amount) F(tank_nudge_iterations)                        \
    F(tank_bump_decay_shift) F(tank_pill_pickup_inset)                   \
    F(tank_boat_exit_inset) F(tank_slide_step) F(speed_road)             \
    F(speed_grass) F(speed_forest) F(speed_river) F(speed_swamp)         \
    F(speed_crater) F(speed_rubble) F(speed_boat) F(speed_deep_sea)      \
    F(speed_refuel_base) F(shell_life) F(shell_speed) F(pill_max_armour) \
    F(pill_attack_ticks) F(pill_attack_min_ticks) F(pill_cooldown_ticks) \
    F(base_full_armour) F(base_full_shells) F(base_full_mines)           \
    F(base_capture_armour) F(base_hit_armour) F(sound_soft_range)        \
    F(sound_none_range)

#define CTRL_SIM_RULES_U16_FIELDS(F)                                     \
    F(tank_death_ticks) F(mine_damage_range) F(tree_hide_distance)       \
    F(tank_collision_distance) F(tank_nudge_threshold)                   \
    F(base_status_range) F(base_reveal_range)

#define CTRL_SIM_RULES_U32_FIELDS(F)                                         \
    F(shell_start_add) F(base_regen_ticks) F(tree_grow_initial_ticks)

#define CTRL_SIM_RULES_F32_FIELDS(F)                                         \
    F(tank_accel_rate) F(tank_decel_rate) F(tank_brake_rate)                 \
    F(tank_autoslow_rate) F(tank_wall_glide)                                 \
    F(turn_road) F(turn_grass) F(turn_forest) F(turn_river) F(turn_swamp)    \
    F(turn_crater) F(turn_rubble) F(turn_boat) F(turn_deep_sea)              \
    F(turn_refuel_base)

/* Every carried rule, whatever its width, for the callers that do not care
 * how wide one goes — the check for whether a changed rule is one this
 * event carries, and the round-trip case that walks them all. */
#define CTRL_SIM_RULES_ALL_FIELDS(F)                                         \
    CTRL_SIM_RULES_U8_FIELDS(F)                                              \
    CTRL_SIM_RULES_U16_FIELDS(F)                                             \
    CTRL_SIM_RULES_U32_FIELDS(F)                                             \
    CTRL_SIM_RULES_F32_FIELDS(F)

/* The member each list entry becomes. Integer rules keep the int32_t
 * SimRules declares them as whatever width they travel in, so reading one
 * back out needs no cast. */
#define CTRL_SIM_RULES_INT_MEMBER(name) int32_t name;
#define CTRL_SIM_RULES_FLT_MEMBER(name) float   name;

/* Body size, so the encoder and the decoder agree on it by construction
 * rather than by counting: one byte, two, four and four per entry. */
#define CTRL_SIM_RULES_COUNT_ONE(name) + 1
#define CTRL_SIM_RULES_BODY_LEN                                              \
    ((size_t)((0 CTRL_SIM_RULES_U8_FIELDS(CTRL_SIM_RULES_COUNT_ONE)) * 1 +   \
              (0 CTRL_SIM_RULES_U16_FIELDS(CTRL_SIM_RULES_COUNT_ONE)) * 2 +  \
              (0 CTRL_SIM_RULES_U32_FIELDS(CTRL_SIM_RULES_COUNT_ONE)) * 4 +  \
              (0 CTRL_SIM_RULES_F32_FIELDS(CTRL_SIM_RULES_COUNT_ONE)) * 4))

/* `quiet` on the five variants that carry one is the announce policy's
 * answer, stamped by the server where it built the event: 0 to announce the
 * fact, 1 to hold the line back. It rides the wire like any other field, and
 * the client's line-emitting site reads it before writing a line. Nothing
 * else about the event changes — the roster update, the alliance bitmap and
 * the rename all still apply. With no scenario policy registered it is
 * always 0. */
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
            BYTE quiet;
        } allianceAccept;

        /* CTRL_ALLIANCE_LEAVE — alliance leave, not player leave */
        struct {
            BYTE playerNum;
            BYTE quiet;
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
            BYTE  quiet;
        } playerJoin;

        /* CTRL_PLAYER_LEAVE — server announces a player has disconnected.
         * Wire counterpart is PACKET_PLAYER_LEFT. */
        struct {
            BYTE playerNum;
            char name[PACKET_MAX_PLAYER_NAME];
            char country[3];            /* 2 chars + NUL */
            BYTE quiet;
        } playerLeave;

        /* CTRL_PLAYER_NAME */
        struct {
            BYTE playerNum;
            char name[PACKET_MAX_PLAYER_NAME];
            BYTE quiet;
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
            uint32_t lobbyServerLocks;
            UploadPolicy uploadPolicy;
            uint8_t  hostSlot;   /* current lobby host's player slot */
            /* Visibility rules, indexed by ViewCategory. */
            ViewPolicy viewPolicy[VIEW_CATEGORY_COUNT];
            uint16_t   viewDecaySecs[VIEW_CATEGORY_COUNT];
            bool     lobbyClassicMode;  /* server is running classic mode */
            bool     lobbyAlliesInTrees; /* server sends allies standing in
                                          * trees to their allies */
            ServerVoiceMode voiceMode;  /* what the server does with the voice
                                         * its clients send it. serverVoiceOff
                                         * means a client here has nowhere to
                                         * send voice, so it captures none. */
            uint8_t  lobbyOverviewWindow;  /* OverviewWindow */
            uint8_t  lobbyLineOfSight;     /* LineOfSightMode */
            bool     lobbySmartPingsOff;   /* server refuses CMD_PING. Held in
                                            * the negative sense so the zero a
                                            * decoder leaves for an absent byte
                                            * reads as "pings allowed" — what
                                            * every server did before the field
                                            * existed. */
            /* The scenario this lobby is running, if any. scenarioSource
             * none means there is none and the five fields below are empty:
             * a lobby with no scenario writes none of these bytes, so a
             * plain map's settings body is the length it always was. */
            LobbyScenarioSource scenarioSource;
            char     scenarioName[LOBBY_SCENARIO_NAME_LEN];
            char     scenarioFileName[LOBBY_SCENARIO_FILE_LEN];
            char     scenarioDescription[LOBBY_SCENARIO_DESC_LEN];
            bool     scenarioExtraTeams;  /* the manifest's extra_teams: may a
                                           * host add teams beyond the
                                           * scenario's own */
            /* The game type the scenario declared, as a gameType value, 0 for
             * none. lobbyGameType reads gameScripted for the whole of a
             * scripted round, and this is the game underneath it — what every
             * site that picks behaviour from the game type resolves to. A
             * client needs it to predict its first life's loadout and its
             * start before the first snapshot lands. */
            uint8_t  scenarioBaseGame;
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

        /* CTRL_LOBBY_BOT_CONFIG — per-bot mode/difficulty/personality +
         * the display name pulled from the players table at fill
         * time. (Name is informational here — players.c remains the
         * source of truth via CTRL_PLAYER_NAME / lobbySlot.)
         * mode indexes the brain's own mode list and difficulty indexes
         * that mode's level list — see brain_list.h. */
        struct {
            uint8_t slot;
            uint8_t difficulty;
            uint8_t personality;
            uint8_t mode;
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

        /* CTRL_LOBBY_BRAIN_DOCS_CHUNK — fragment `seq` of `count` of the
         * lobby texts belonging to brain `brainIdx` (an index into the
         * brain catalogue CTRL_LOBBY_BRAIN_LIST carries). fragLen bytes
         * live in frag[]. */
        struct {
            uint8_t  brainIdx;
            uint8_t  seq;
            uint8_t  count;
            uint16_t fragLen;
            uint8_t  frag[LOBBY_BRAIN_DOCS_FRAG_MAX];
        } lobbyBrainDocsChunk;

        /* CTRL_SERVER_TEXT — server-originated chat broadcast.
         * Mirrors what UDP clients receive as
         * PACKET_CHAT_BROADCAST(fromPlayer=0xFE). Lets in-process
         * subscribers (SP / host) see the same lines. */
        struct {
            char    text[PACKET_MAX_CHAT_MESSAGE + 1];
            uint8_t destTeam;  /* 0 = everyone; 1-15 = deliver only to that team
                                  (teams run 1..MAX_TANKS-1, see ServerSim.teams[]).
                                  Server-side recipient filter (udpClientDeliver +
                                  the in-process handler); not sent on the wire. */
            uint8_t destPlayer; /* 0xFF = everyone; otherwise deliver only to that
                                   slot. Server-side recipient filter like destTeam,
                                   and not sent on the wire either. 0 is a real
                                   slot, so a memset-zeroed event would unicast to
                                   slot 0: every producer of this variant — the
                                   sim-side publishers and the body decoder that
                                   rebuilds it — must set 0xFF explicitly. */
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
        /* CTRL_STATS_SEED — the in-progress round's scoreboard rows, one per
         * connected slot. Same row type as RoundStatsSummary.players, so the
         * seed and the end-of-round recap cannot disagree on what a column
         * means. dmgDealt and builds ride along in the row and the client
         * drops them: no client-side event keeps them current. */
        struct {
            uint8_t            playerCount;             /* present slots, <= MAX_TANKS */
            RoundPlayerSummary players[MAX_TANKS];
        } statsSeed;

        /* CTRL_VOICE_TALKING — bit N set means slot N is producing voice
         * right now. */
        struct {
            PlayerBitMap talking;
        } voiceTalking;

        /* CTRL_ENTITY_CHANGE — the item, where it sits in its list, and
         * whether it is now on the map or off it.
         *
         * index is 0-based, the way the snapshots, the game events and the
         * brain API number an item; the pillbox, bases and starts modules
         * take the number one higher, and the boundary converts.
         *
         * The record is the item's map data and nothing more. A pillbox's
         * reload, coolDown and justSeen and a base's refuelTime, baseTime
         * and justStopped are the server's per-tick working state: no
         * client rebuilds them from an event, and they would be stale by
         * the time the event arrived. On a removal the record is the item
         * as it stood, so a script that puts it back has it to hand. */
        struct {
            uint8_t kind;    /* EntityKind */
            uint8_t index;   /* 0-based */
            uint8_t added;   /* 1 on the map, 0 removed */
            union {
                struct {
                    uint8_t x, y;
                    uint8_t owner;
                    uint8_t armour;
                    uint8_t speed;
                    uint8_t inTank;
                } pill;
                struct {
                    uint8_t x, y;
                    uint8_t owner;
                    uint8_t armour;
                    uint8_t shells;
                    uint8_t mines;
                } base;
                struct {
                    uint8_t x, y;
                    uint8_t dir;
                } start;
            } rec;
        } entityChange;

        /* CTRL_ENTITY_SYNC — one bit per 0-based index in each list: bit i
         * set means index i holds an item that is on the map, clear means
         * the index is in range but its item has been taken off. Each list
         * holds at most 16 items (MAX_PILLS, MAX_BASES and MAX_STARTS are
         * all 16), so 16 bits covers every index a list can name. Bits at
         * or above a list's own count name no item and are ignored. */
        struct {
            uint16_t pills;
            uint16_t bases;
            uint16_t starts;
        } entitySync;

        /* CTRL_SIM_RULES — one member per rule the event carries, named
         * exactly as SimRules names it, so the server fill and the client
         * apply are plain assignments and a reader can find the field the
         * value came from. Declared in four width groups because that is
         * the order they sit in on the wire, and the widths follow each
         * rule's own range: nothing is sent wider than it can be. */
        struct {
            CTRL_SIM_RULES_U8_FIELDS(CTRL_SIM_RULES_INT_MEMBER)
            CTRL_SIM_RULES_U16_FIELDS(CTRL_SIM_RULES_INT_MEMBER)
            CTRL_SIM_RULES_U32_FIELDS(CTRL_SIM_RULES_INT_MEMBER)
            CTRL_SIM_RULES_F32_FIELDS(CTRL_SIM_RULES_FLT_MEMBER)
        } simRules;

        /* CTRL_SCN_PANEL — one panel's display list, replacing whatever
         * that panel held. An empty list clears it. The bytes are the
         * primitives scenario_panel.h describes; nothing on this path
         * parses them, because the arm validates a list on the way out
         * and the drawer does on the way in. */
        struct {
            uint8_t  panel;      /* 0..3 */
            uint16_t len;
            uint8_t  bytes[SCN_PANEL_MAX];
            uint8_t  destTeam;   /* 0 = everyone; 1..15 = only that team. Server-side
                                    recipient filter; not sent on the wire. */
            uint8_t  destPlayer; /* 0xFF = everyone; otherwise only that slot. Same,
                                    and 0 is a real slot, so every producer — the
                                    publishers and the body decoder — sets 0xFF. */
        } scnPanel;

        /* CTRL_SCN_SCORE — a scenario's own score for one slot or one
         * team. Broadcast: target is whose score it is, not who
         * receives it, so this variant carries no recipient pair. */
        struct {
            uint8_t kind;        /* a slot's score, or a team's */
            uint8_t target;      /* the slot, or the team */
            int32_t score;
            char    label[16];
        } scnScore;

        /* CTRL_SCN_ANNOUNCE — a line drawn across the centre of the screen. */
        struct {
            char     text[PACKET_MAX_CHAT_MESSAGE + 1];
            uint16_t ticks;      /* how long it stays up */
            uint8_t  destTeam;
            uint8_t  destPlayer;
        } scnAnnounce;

        /* CTRL_SCN_MARKER — a mark on the map, kept by id. */
        struct {
            uint8_t id;
            uint8_t kind;        /* square, follow a player, or clear */
            uint8_t x, y;        /* square kind */
            uint8_t slot;        /* follow-player kind */
            uint8_t colour;
            uint8_t destTeam;
            uint8_t destPlayer;
        } scnMarker;

        /* CTRL_SCENARIO_RULES — the rules a scenario's own manifest sets, as
         * its author wrote them. Empty when no scenario is attached.
         *
         * rule[i] is a SimRuleIndex, which is the index every other side of
         * the fence names a rule by, and value[i] is what the manifest set
         * that rule to — a double because sixteen of the rules are
         * float-valued and the rest are whole numbers a double carries
         * exactly. Entries at or above count name no rule and read zero.
         *
         * Two arrays rather than one array of {rule, value} pairs, and it
         * has to stay two: a pair pads to sixteen bytes to carry one byte of
         * index and eight of value, which would make this variant 1480 bytes
         * and the largest member of the union. A ServerSim holds 200
         * ControlEvents in its lobby chat buffer, so every byte the union
         * grows by is paid two hundred times over on every sim. Split, the
         * variant is 832 bytes, the union's size stays the one the round
         * stats summary sets, and the sim pays nothing for this event.
         *
         * The wire form is the same either way: the body is a count and then
         * a [rule][value] row per entry, which the encoder builds by walking
         * the two arrays together. */
        struct {
            uint8_t count;
            uint8_t rule[CTRL_SCENARIO_RULES_MAX];   /* a SimRuleIndex per row */
            double  value[CTRL_SCENARIO_RULES_MAX];  /* the value at the same index */
        } scenarioRules;
    } u;
} ControlEvent;

#endif /* CONTROL_EVENT_H */
