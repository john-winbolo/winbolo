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
#include "scenario_settings.h" /* SCN_SETTING_ID_LEN for
                                  CTRL_LOBBY_SCRIPT_SETTING */
#include "upload_policy.h" /* UploadPolicy, ScriptUploadPolicy in lobbySettings */
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
    /* CTRL_LOBBY_BOT_POOL_CHUNK — RETIRED. One fragment of the server's
     * bot naming-pool catalog. Nothing sends it any more and a client
     * ignores it: CTRL_LOBBY_BOT_POOL_INFO names the catalogue, and a
     * client that does not hold it fetches it on CHANNEL_BULK. The type
     * and its codec stay so recordings made before still decode. */
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
    /* CTRL_LOBBY_BRAIN_DOCS_CHUNK — RETIRED. One fragment of ONE brain's
     * announce.txt and commands.txt together. Nothing sends it any more and
     * a client ignores it: CTRL_LOBBY_BRAIN_ANNOUNCE carries the announce
     * line, and the docs go on CHANNEL_BULK when a client asks for them.
     * The type and its codec stay, because recordings made before hold it
     * by this number and a reader must still be able to step over it.
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
     * Carried in fragments, like CTRL_LOBBY_BOT_POOL_CHUNK and
     * CTRL_LOBBY_BRAIN_DOCS_CHUNK: a whole set outgrew one control segment
     * once the rule list passed 113 rows, so each event is `seq` of
     * `fragCount` and a reader installs the set on the last of them. An
     * empty set is still one fragment, because an empty set is an answer.
     *
     * Appended at the END, like the four above: the tables in
     * transport_control_codec.c are indexed by this enum. */
    CTRL_SCENARIO_RULES,
    /* CTRL_LOBBY_SCRIPT_LIST — the ordered list of scripts this lobby will
     * run: number one first and highest priority, at most one of them a
     * scenario and the rest mods.
     *
     * Its own event rather than more of the CTRL_LOBBY_SETTINGS tail, and
     * that is a measurement rather than a preference. A whole list at the
     * cap is 2 + 10 * 202 = 2022 body bytes, which is past the 1021 a
     * control segment carries, so it has to be chunked whichever event it
     * rides. Chunking the settings tail would put a fragment number on every
     * lobby change, and the settings variant would have to hold the widest
     * fragment by value: at LOBBY_CHAT_BUFFER_MAX ControlEvents per sim,
     * growing that union member is paid two hundred times over on every sim.
     * This variant is 1048 bytes, under the 1108 the round-stats member
     * already spends, so the union does not grow at all and the settings
     * event is left the size it was.
     *
     * Chunked as { final, count, entries }, the shape
     * PACKET_LOBBY_SCENARIO_LIST_RSP uses for the same kind of list. The
     * reader appends each chunk and installs the list when one arrives with
     * final set; the control channel is reliable and ordered, so the chunks
     * of one list cannot interleave with another's, and an empty list is one
     * chunk with count 0 and final set.
     *
     * Broadcast and body-only on CHANNEL_CONTROL, as CTRL_SCENARIO_RULES is:
     * what a lobby is about to run is as public as the scenario name the
     * settings event already carries. Replayed into a joining client's sync
     * beside the settings, so a mid-lobby joiner reads the same list.
     *
     * Appended at the END, like every type above it: the tables in
     * transport_control_codec.c are indexed by this enum. */
    CTRL_LOBBY_SCRIPT_LIST,
    /* CTRL_LOBBY_SCRIPT_SETTING — one value the host chose for one of a
     * script's own settings (scenario_settings.h), or the order to forget
     * every value held.
     *
     * op LOBBY_SCRIPT_SETTING_CLEAR comes first in every join sync, even
     * when no value is held, and is followed by one
     * LOBBY_SCRIPT_SETTING_SET per value. A live change is one SET. A
     * client reads any event of this type as proof that the server takes
     * CMD_SET_SCRIPT_SETTING: an older server never sends one, and a client
     * that sent it that command anyway would stall its command stream on a
     * command the server cannot decode.
     *
     * The value is the one the server resolved against the declaration, so
     * what a client shows is what game.setting will answer. A value equal to
     * the default is still sent, so the dialog of every client moves when
     * the host picks the default back.
     *
     * Broadcast and body-only on CHANNEL_CONTROL, as CTRL_LOBBY_SCRIPT_LIST
     * is. An older client does not know the type and skips it, so it plays
     * with the values and cannot see them.
     *
     * Appended at the END, like every type above it: the tables in
     * transport_control_codec.c are indexed by this enum. */
    CTRL_LOBBY_SCRIPT_SETTING,
    /* CTRL_LOBBY_BRAIN_ANNOUNCE — ONE brain's announce.txt, and what the
     * server holds of its commands.txt: the length and a generation number,
     * but not the text. One event per brain that ships either file, sent
     * beside the brain list in a joiner's sync and again when a round hands
     * the lobby back.
     *
     * The announce line has to be here because the lobby drops it into team
     * chat by itself, the moment a bot running the brain joins the reader's
     * team. The docs do not: they are read only when a player clicks that
     * line, so the client asks for them then (PACKET_LOBBY_BRAIN_DOCS_REQ)
     * and they come back compressed on CHANNEL_BULK (BULK_KIND_BRAIN_DOCS).
     * docsLen 0 says the brain ships no commands.txt and there is nothing to
     * ask for. docsGen names the text the server holds now; the bulk answer
     * carries it, so a client can tell an answer for older docs from one
     * for the docs it was told about, and a new generation here makes a
     * client drop the docs it already has for the brain.
     *
     * One control segment at most: 9 + BRAIN_ANNOUNCE_MAX = 521 bytes.
     * Body-only on CHANNEL_CONTROL, as CTRL_SIM_RULES is. Like the chunk it
     * replaces, it is left out of the delayed spectator ring's control
     * snapshot.
     *
     * Appended at the END, like every type above it: the tables in
     * transport_control_codec.c are indexed by this enum. */
    CTRL_LOBBY_BRAIN_ANNOUNCE,
    /* CTRL_LOBBY_BOT_POOL_INFO — which bot naming-pool catalogue the server
     * holds: its id (lobbyBotPoolsCatalogId, a CRC-32 of the uncompressed
     * pools) and the length of its compressed blob. id 0 and len 0 say the
     * server has no themed pools, and the client keeps its own.
     *
     * Sent in a joiner's lobby sync where the catalogue itself used to be,
     * up to 78 chunks of it. Most servers run the shipped bot_names.json,
     * which the client holds already, so a client whose own pools have the
     * same id does nothing. One with another id asks for the blob
     * (PACKET_LOBBY_BOT_POOL_REQ) once it is connected, and it comes back
     * on CHANNEL_BULK (BULK_KIND_BOT_POOL).
     *
     * Body-only on CHANNEL_CONTROL, 8 bytes. Players only: spectators add
     * no bots and the allowlist leaves it out, and it is left out of the
     * spectator ring's control snapshot.
     *
     * Appended at the END, like every type above it: the tables in
     * transport_control_codec.c are indexed by this enum. */
    CTRL_LOBBY_BOT_POOL_INFO,
    /* CTRL_SCN_STATUS — a scenario's status line: one line at the very top
     * of the game view that stays until the scenario changes it or clears
     * it, with an optional countdown the client runs down itself.
     *
     * Body-only on CHANNEL_CONTROL like the four presentation events above,
     * and addressed the same way: destTeam/destPlayer never travel, and the
     * delivery path filters on them. The server keeps the last line per
     * destination and replays it into a joining client's sync, as it does a
     * panel's list.
     *
     * An older client has no decoder for the type and skips it, so it shows
     * nothing for the line and nothing else changes.
     *
     * Appended at the END, like every type above it: the tables in
     * transport_control_codec.c are indexed by this enum. */
    CTRL_SCN_STATUS,
    /* CTRL_VOICE_EVERYONE — whether a scenario has sent voice in the round to
     * every player (game.set_voice_everyone) rather than to the talker's
     * allies alone. Published when the server's flag changes value: from the
     * op, and from the three places that clear it (a round start, the reset
     * back to the lobby and the scenario detaching) when it was on. Replayed
     * into a joining client's sync while it is on.
     *
     * The client keeps the value and prints a line of its own when the rule
     * a running round plays by changes, so the line is in the reader's
     * language; a server chat line could only be English.
     *
     * Broadcast and body-only on CHANNEL_CONTROL. Not on the spectator
     * allowlist: a spectator hears nobody, whatever the flag says.
     *
     * Appended at the END, like every type above it: the tables in
     * transport_control_codec.c are indexed by this enum. */
    CTRL_VOICE_EVERYONE,
    CTRL_EVENT_TYPE_COUNT   /* sentinel — must stay last */
} ControlEventType;

/* How many rows a whole scenario rules set can hold: one per rule there is,
 * taken from SIM_RULE_LIST rather than written out, so a rule added to that
 * list cannot overflow the set. A manifest names each rule at most once, so
 * a set can hold every rule and no more.
 *
 * Each row's rule index travels as one byte, which is what the assertion
 * below holds the list to. */
#define CTRL_SCENARIO_RULES_MAX SIM_RULE_COUNT
BOLO_STATIC_ASSERT(CTRL_SCENARIO_RULES_MAX <= 255,
                   ctrl_scenario_rules_index_fits_a_byte);

/* Rows in ONE fragment, and the number of fragments a whole set needs.
 *
 * A row is 9 bytes on the wire ([rule 1][value 8]) and a fragment spends 3
 * more on seq, fragCount and its own row count, so 64 rows is a 579-byte
 * body. One control event is one channel segment, and a segment carries
 * CHANNEL_CONTROL_SEG (1024) bytes less the channel frame's type(1) and
 * bodyLen(2) — 1021. 579 sits well inside that, the way the 900-byte
 * fragments of the other two chunked events do; transport_control_codec.c
 * pins it against the segment, which it can see and this public header
 * cannot.
 *
 * The row cap is a number of its own rather than the rule count because a
 * fragment has to stay a fixed, modest size while the rule list grows: at
 * 64 rows the variant is 584 bytes and stays well under the round-stats
 * member that sets the union's size, and a ServerSim holds 200
 * ControlEvents in its lobby chat buffer, so every byte the union grows by
 * is paid two hundred times over on every sim.
 *
 * seq and fragCount travel as one byte each, which the assertion holds. */
#define SCN_RULES_FRAG_ROWS 64
#define CTRL_SCENARIO_RULES_FRAGS_MAX \
    ((CTRL_SCENARIO_RULES_MAX + SCN_RULES_FRAG_ROWS - 1) / SCN_RULES_FRAG_ROWS)
BOLO_STATIC_ASSERT(CTRL_SCENARIO_RULES_FRAGS_MAX <= 255,
                   ctrl_scenario_rules_seq_and_frag_count_fit_a_byte);

/* Body capacity for CTRL_CHAT.  Worst case is the localized server
 * message: 2 langid + 1 argCount + 4 * (1 lenByte + (PLAYER_NAME_LEN-1)
 * name bytes) = 263 bytes; rounded up for headroom. fromPlayer and
 * destPlayer are separate struct fields, not part of body[]. */
#define CHAT_BODY_MAX 272

/* Per-fragment payload cap for the retired CTRL_LOBBY_BOT_POOL_CHUNK. Sized so one
 * fragment plus its header fits a single control datagram (well under
 * MAX_CONTROL_PACKET). A 64 KiB catalog therefore needs at most
 * ceil(65536/900) ≈ 73 fragments (< 255, the seq/count cap). */
#define LOBBY_BOT_POOL_CHUNK_FRAG_MAX 900

/* Per-fragment payload cap for the retired CTRL_LOBBY_BRAIN_DOCS_CHUNK,
 * kept so a recording that holds one still decodes. */
#define LOBBY_BRAIN_DOCS_FRAG_MAX 900

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

/* How many scripts one lobby may run at once.
 *
 * Independent of the scenario host's own SCN_SCRIPTS_MAX for the reason the
 * three lengths above are independent of the scenarios directory's: a public
 * header cannot reach into src/scenario/. The two are held against each
 * other in scenario_host.c, which is the translation unit that sees both, so
 * one moving without the other is a build failure rather than a list the
 * host truncates in silence.
 *
 * Ten, which is Andrew's number: one scenario deciding the round and nine
 * mods changing how it plays. Nothing on the wire binds it — the event that
 * carries the list is chunked, and the command that sets it was measured
 * against COMMAND_MAX_WIRE_BYTES and fits at ten with room over. What it
 * does bind is memory: the command's in-union buffer is
 * LOBBY_SCRIPT_LIST_MAX * LOBBY_SCENARIO_FILE_LEN bytes, which is 1280 at
 * ten. See CmdSetScriptList in client_command.h for what that costs. */
#define LOBBY_SCRIPT_LIST_MAX 10

/* How many entries one CTRL_LOBBY_SCRIPT_LIST chunk carries, and this is
 * measured rather than chosen.
 *
 * One entry is at worst [flags 1][fileLen 1][file 127][nameLen 1][name 63]
 * [source 1][workshopId 8] = 193 + 9 = 202 bytes, and a body spends 2 more
 * on final and count. The flags byte is a byte and not a bool, so the two
 * flags an entry carries — keeps the win condition, bound to a map — are
 * bits in it and the entry does not widen when a third one arrives. A
 * control event is one channel segment, which is CHANNEL_CONTROL_SEG (1024)
 * less the channel frame's type(1) and bodyLen(2) — 1021 bytes. So:
 *
 *   5 entries: 2 + 5 * 202 = 1012   fits, 9 bytes spare
 *   6 entries: 2 + 6 * 202 = 1214   193 over, and a body past the segment is
 *                                   logged and dropped with nothing visible
 *                                   to the client
 *
 * Five it is. transport_control_codec.c pins it against the segment, which
 * it can see and this public header cannot. A whole list at
 * LOBBY_SCRIPT_LIST_MAX is therefore two chunks. */
#define LOBBY_SCRIPT_LIST_CHUNK 5

/* What a CTRL_LOBBY_SCRIPT_SETTING says. */
#define LOBBY_SCRIPT_SETTING_CLEAR 0 /* forget every value held */
#define LOBBY_SCRIPT_SETTING_SET   1 /* file's setting id is now value */

/* One script on the list, as CTRL_LOBBY_SCRIPT_LIST carries it and as a
 * client holds it afterwards.
 *
 * No description. One would be 257 bytes on top of the 202 here and would
 * cut a chunk to two entries, and a client already has every description it
 * needs: PACKET_LOBBY_SCENARIO_LIST_RSP carries them for the whole
 * directory and the chooser already fetches it, keyed by this same file
 * name. A second request/response pair for text a client is holding would
 * be bytes nobody reads. */
typedef struct LobbyScriptEntry {
    char file[LOBBY_SCENARIO_FILE_LEN];  /* the name in the directory */
    char name[LOBBY_SCENARIO_NAME_LEN];  /* the manifest's */
    /* Whether this script leaves the win condition alone, which is the kind
     * split read as a flag: true is a mod, false is a scenario. Held this
     * way round rather than as "isScenario" so it matches
     * scenarioManifestKeepsWinCondition, which is what the server reads it
     * from, and so a zeroed entry does not claim to be a mod. */
    bool keepsWinCondition;
    /* Whether the script is tied to one particular map, which is the
     * manifest's own bound flag. A bound script is not something a chooser
     * removes on its own: the map decides it, so changing it means changing
     * the map. An unbound one is added and dropped freely. Carried per entry
     * rather than looked up from the directory listing because a list may
     * name a file the listing no longer holds, and a row that cannot say
     * whether it is removable is worse than one row of wire. */
    bool bound;
    /* Where the server got the file: SCN_DIR_SOURCE_SERVER, _UPLOAD or
     * _WORKSHOP (scenario_defs.h; SERVER_SCENARIO_SOURCE_* in server_sim.h
     * for a gui reader). The map's own script reads SERVER. */
    uint8_t  source;
    /* The Workshop item the file came from, 0 for none. */
    uint64_t workshopId;
} LobbyScriptEntry;

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

/* Optional tail after the original 131-byte body. Omitted when zero so
 * classic games keep their wire format; old recordings decode as zero. */
#define CTRL_SIM_RULES_EXT_U8_FIELDS(F) F(tank_collision_mac)

/* Every carried rule, whatever its width, for the callers that do not care
 * how wide one goes — the check for whether a changed rule is one this
 * event carries, and the round-trip case that walks them all. */
#define CTRL_SIM_RULES_ALL_FIELDS(F)                                         \
    CTRL_SIM_RULES_U8_FIELDS(F)                                              \
    CTRL_SIM_RULES_U16_FIELDS(F)                                             \
    CTRL_SIM_RULES_U32_FIELDS(F)                                             \
    CTRL_SIM_RULES_F32_FIELDS(F)                                             \
    CTRL_SIM_RULES_EXT_U8_FIELDS(F)

/* The member each list entry becomes. Integer rules keep the int32_t
 * SimRules declares them as whatever width they travel in, so reading one
 * back out needs no cast. */
#define CTRL_SIM_RULES_INT_MEMBER(name) int32_t name;
#define CTRL_SIM_RULES_FLT_MEMBER(name) float   name;

/* Body size, so the encoder and the decoder agree on it by construction
 * rather than by counting: one byte, two, four and four per entry. */
#define CTRL_SIM_RULES_COUNT_ONE(name) + 1
#define CTRL_SIM_RULES_BASE_BODY_LEN                                         \
    ((size_t)((0 CTRL_SIM_RULES_U8_FIELDS(CTRL_SIM_RULES_COUNT_ONE)) * 1 +   \
              (0 CTRL_SIM_RULES_U16_FIELDS(CTRL_SIM_RULES_COUNT_ONE)) * 2 +  \
              (0 CTRL_SIM_RULES_U32_FIELDS(CTRL_SIM_RULES_COUNT_ONE)) * 4 +  \
              (0 CTRL_SIM_RULES_F32_FIELDS(CTRL_SIM_RULES_COUNT_ONE)) * 4))
#define CTRL_SIM_RULES_BODY_LEN                                              \
    (CTRL_SIM_RULES_BASE_BODY_LEN +                                         \
     (size_t)(0 CTRL_SIM_RULES_EXT_U8_FIELDS(CTRL_SIM_RULES_COUNT_ONE)))

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
            ScriptUploadPolicy scriptUploadPolicy;
            bool     scriptSharing;  /* 1 = players may save a copy of this
                                      * server's mods and scenarios */
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
            bool     lobbyModsOff;         /* the round composes none of the
                                            * picked mods or scenarios. Held
                                            * in the negative sense for the
                                            * same reason as
                                            * lobbySmartPingsOff above, and read
                                            * back through the positive accessor
                                            * clientSimGetLobbyModsEnabled.
                                            *
                                            * Its byte is in the fixed part of
                                            * the settings body, ahead of the
                                            * scenario tail: adding it grew the
                                            * fixed part and moved that tail
                                            * down, so a body written by a build
                                            * without the byte does not decode
                                            * against one that has it. Nothing
                                            * here tries to read one. */
            bool     lobbyPositionalSound; /* sound events to a human carry a
                                            * side and a banded distance. Its
                                            * byte follows modsOff in the fixed
                                            * part; a body without it reads as
                                            * off, every sound centred. */
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
            /* True when the script says it is a mod: it changes how the game
             * plays and leaves the win condition alone, so the ops that end
             * or decide a round raise when it calls them. False for a
             * scenario and false when there is no script at all, so a reader
             * telling those two apart tests scenarioSource first. Held this
             * way round rather than the other because the tail is
             * append-only and a decoder leaves zero for a byte the sender
             * never wrote: every server that predates this field sent
             * scenarios, and false is what a scenario reads as. */
            bool     scenarioKeepsWinCondition;
            /* True when the attached script is tied to one particular map.
             * The catalogue rows already carry this — PACKET_LOBBY_SCENARIO_
             * LIST_RSP packs a bound byte per entry — but the attached script
             * is not a catalogue row, and a lobby has to know whether the one
             * it is running may be taken off or only replaced by changing the
             * map. Appended behind the byte above for the same reason that
             * one was: the tail is append-only and a decoder leaves zero for
             * a byte the sender never wrote, and false is what every script
             * that predates this field should read as, since an unbound
             * script is the one a host can still remove. */
            bool     scenarioBound;
            /* True when this server runs every scenario script with the full
             * Lua library and no limits (-allow-unsafe-scripts), so a player
             * can see that before they play. Appended behind scenarioBound
             * for the same reason: the tail is append-only and a decoder
             * leaves zero for a byte the sender never wrote, and zero reads
             * as sandboxed, which every server that predates this field
             * was. */
            bool     scenarioUnsafe;
            /* True when the attached list said needs_bots, so the server
             * refuses the AI policy that takes every bot off the roster.
             * Appended behind scenarioUnsafe. Unlike the bytes above, a
             * decoder that finds this byte missing sets it TRUE: every
             * server that predates it refused that policy for any script,
             * and a lobby that offers a row the server turns down is worse
             * than one that greys a row the server would take. */
            bool     scenarioNeedsBots;
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

        /* CTRL_LOBBY_BRAIN_DOCS_CHUNK — retired; see the enum. Fragment
         * `seq` of `count` of the lobby texts belonging to brain
         * `brainIdx`. fragLen bytes live in frag[]. */
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

        /* CTRL_VOICE_EVERYONE — the flag's new value. */
        struct {
            bool on;
        } voiceEveryone;

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
            CTRL_SIM_RULES_EXT_U8_FIELDS(CTRL_SIM_RULES_INT_MEMBER)
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

        /* CTRL_SCN_ANNOUNCE — a big line across the game view. */
        struct {
            char     text[PACKET_MAX_CHAT_MESSAGE + 1];
            uint16_t ticks;      /* how long it stays up */
            uint8_t  hasPos;     /* 0: the usual place; else posX/posY */
            uint8_t  posX;       /* centre, 0..SCN_ANNOUNCE_POS_MAX across */
            uint8_t  posY;       /* centre, 0..SCN_ANNOUNCE_POS_MAX down */
            uint8_t  destTeam;
            uint8_t  destPlayer;
        } scnAnnounce;

        /* CTRL_SCN_STATUS — the status line at the top of the game view.
         * Empty text clears it. endsAt is the server tick a countdown runs
         * to, SCN_STATUS_NO_COUNTDOWN for none. */
        struct {
            char     text[PACKET_MAX_CHAT_MESSAGE + 1];
            uint32_t endsAt;
            uint8_t  destTeam;
            uint8_t  destPlayer;
        } scnStatus;

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

        /* CTRL_SCENARIO_RULES — fragment `seq` of `fragCount` of the rules a
         * scenario's own manifest sets, as its author wrote them. Empty when
         * no scenario is attached, and an empty set is one fragment with no
         * rows rather than no event.
         *
         * count is the rows in THIS fragment, never the whole set: a reader
         * appends each fragment's rows in order and has the set once it has
         * taken seq == fragCount - 1. The fragments of one set run 0..
         * fragCount-1 with no gaps, on a reliable ordered channel, so a
         * fragment that does not continue the one in hand means the stream
         * was interrupted and the partial set is thrown away rather than
         * spliced.
         *
         * rule[i] is a SimRuleIndex, which is the index every other side of
         * the fence names a rule by, and value[i] is what the manifest set
         * that rule to — a double because sixteen of the rules are
         * float-valued and the rest are whole numbers a double carries
         * exactly. Entries at or above count name no rule and read zero.
         *
         * Two arrays rather than one array of {rule, value} pairs, and it
         * has to stay two: a pair pads to sixteen bytes to carry one byte of
         * index and eight of value, which would make this variant 1032 bytes
         * instead of 584. A ServerSim holds 200 ControlEvents in its lobby
         * chat buffer, so every byte the union grows by is paid two hundred
         * times over on every sim. Split, the union's size stays the one the
         * round stats summary sets and the sim pays nothing for this event.
         *
         * The wire form is the same either way: the body is seq, fragCount,
         * a row count and then a [rule][value] row per entry, which the
         * encoder builds by walking the two arrays together. */
        struct {
            uint8_t seq;        /* which fragment, 0-based */
            uint8_t fragCount;  /* how many make the whole set; never 0 */
            uint8_t count;      /* rows in THIS fragment */
            uint8_t rule[SCN_RULES_FRAG_ROWS];   /* a SimRuleIndex per row */
            double  value[SCN_RULES_FRAG_ROWS];  /* the value at the same index */
        } scenarioRules;

        /* CTRL_LOBBY_SCRIPT_LIST — one chunk of the lobby's ordered script
         * list, entries[0..count) in list order.
         *
         * count is the entries in THIS chunk and never the whole list; a
         * reader appends them and installs the list when it takes a chunk
         * with final set. final is on the last chunk of a list and on that
         * one alone, so an empty list arrives as one chunk with count 0 and
         * final 1 rather than as no event at all — a host clearing the list
         * has something to say and a client has to hear it.
         *
         * No seq/fragCount pair, unlike CTRL_SCENARIO_RULES beside it. That
         * pair exists there so a reader can tell a continuation from a
         * restart on a stream it may join partway; this list is published as
         * a back-to-back run inside one call under the sim mutex, on a
         * reliable ordered channel, so the chunk after a final one is always
         * the start of the next list. The reader still throws away a partial
         * list that would overrun LOBBY_SCRIPT_LIST_MAX, which is the only
         * way the two could ever disagree. */
        struct {
            uint8_t          final;  /* 1 on the last chunk of a list */
            uint8_t          count;  /* entries in THIS chunk */
            LobbyScriptEntry entries[LOBBY_SCRIPT_LIST_CHUNK];
        } lobbyScriptList;

        /* CTRL_LOBBY_SCRIPT_SETTING — see the enum. file and id are empty
         * on a CLEAR. */
        struct {
            uint8_t op;                            /* LOBBY_SCRIPT_SETTING_* */
            char    file[LOBBY_SCENARIO_FILE_LEN]; /* the script's file name */
            char    id[SCN_SETTING_ID_LEN];        /* the setting's id */
            int32_t value;
        } lobbyScriptSetting;

        /* CTRL_LOBBY_BRAIN_ANNOUNCE — see the enum. announce holds
         * announceLen bytes and a NUL. docsLen is commands.txt's length in
         * bytes, 0 for none; docsGen is 0 exactly when docsLen is. */
        struct {
            uint8_t  brainIdx;       /* index into CTRL_LOBBY_BRAIN_LIST */
            uint32_t docsGen;
            uint16_t docsLen;
            uint16_t announceLen;
            char     announce[BRAIN_ANNOUNCE_MAX + 1];
        } lobbyBrainAnnounce;

        /* CTRL_LOBBY_BOT_POOL_INFO — see the enum. id is 0 exactly when len
         * is. */
        struct {
            uint32_t id;
            uint32_t len;    /* compressed blob bytes, <= LOBBY_BOT_CATALOG_WIRE_MAX */
        } lobbyBotPoolInfo;
    } u;
} ControlEvent;

#endif /* CONTROL_EVENT_H */
