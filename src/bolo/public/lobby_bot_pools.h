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
 *Name:          Lobby Bot Naming Pools
 *Filename:      lobby_bot_pools.h
 *Purpose:
 *  Hardcoded thematic name pools for AI players, used by the
 *  Layout A lobby. Each team picks one pool; bots added to
 *  that team draw names from it (auto-pick on add, dice-reroll
 *  via the AiConfig sub-panel).
 *
 *  Server-side: doesn't know pool contents. The server tracks
 *  the per-team pool index and the per-bot name (assigned by
 *  whichever client added the bot). This module exists so the
 *  client can render the dropdown labels and pick names.
 *
 *  Pool index 0 is "classic" (AI references); other indices
 *  match the order in s_pools[]. Adding a pool: append to
 *  the array. Old clients seeing an unknown index pick a
 *  random known pool for *display* — bot names themselves
 *  travel as strings, so functionality isn't affected.
 *********************************************************/

#ifndef LOBBY_BOT_POOLS_H
#define LOBBY_BOT_POOLS_H

#include <stdbool.h>
#include <stdint.h>

#include "global.h"

/* ── Hard limits ─────────────────────────────────────────────────
 * The per-team pool selection travels on the wire as a single
 * uint8_t (TeamMetadata.namingPool), so a pool index must fit in
 * 0..255.  The generated "Numbered Bots" pool is always appended as
 * the last index, so the themed-pool cap leaves one slot for it.
 *
 * Names themselves travel as strings (not indices), so there is no
 * wire limit on names-per-pool — but lobbyBotPoolPick's scratch
 * buffer caps how many a single pool can pick from, and each name
 * must fit the player-name wire cap once validated. */
#define LOBBY_BOT_POOL_MAX_POOLS       254  /* + 1 numbered = 255 */
#define LOBBY_BOT_POOL_MAX_NAMES       256  /* per pool (picker cap) */
#define LOBBY_BOT_POOL_MAX_NAME_BYTES   63  /* PACKET_MAX_PLAYER_NAME-1 */
#define LOBBY_BOT_POOL_MAX_LABEL_BYTES  48

/* Number of available pools. This is the count of themed pools plus
 * one for the always-present generated "Numbered Bots" pool. */
int  lobbyBotPoolCount(void);

/* Display label for the pool dropdown (e.g. "Famous Painters").
 * Returns "Unknown" for out-of-range indices. */
const char *lobbyBotPoolLabel(int poolIdx);

/* Name count in a pool (for fallback overflow naming). */
int  lobbyBotPoolNameCount(int poolIdx);

/* Read a single name from a pool. Returns NULL for out-of-range. */
const char *lobbyBotPoolName(int poolIdx, int nameIdx);

/* Pick the next free name from the pool, skipping any name in
 * the `used` array (caller-supplied list of currently-used names
 * across the lobby; case-sensitive compare). Pool exhaustion
 * falls back to "<label> N" overflow into outBuf.
 *
 *   poolIdx   — pool to draw from (0..lobbyBotPoolCount()-1)
 *   used[]    — null-terminated array of strings to avoid
 *   usedCount — entries in used[]
 *   outBuf    — caller buffer for the chosen name
 *   outBufLen — outBuf capacity (MUST be ≥ 32 for safety)
 *
 * Always writes a null-terminated name into outBuf. Returns
 * pointer to outBuf for convenience. */
char *lobbyBotPoolPick(int poolIdx,
                       const char **used, int usedCount,
                       char *outBuf, int outBufLen);

/* ── Runtime-loadable pools ──────────────────────────────────────
 *
 * By default the pools come from the built-in tables compiled into
 * lobby_bot_pools.c.  They can be replaced at runtime (e.g. from a
 * data/bot_names.json file) so hosts can ship their own themed name
 * sets.  Pool *indices* travel on the wire, so every peer must load
 * the same set for the dropdown labels to line up — the shipped
 * default file does this; a custom -botnames file only matches peers
 * that load the same file.  Bot names themselves always travel as
 * strings, so a mismatch only mislabels the dropdown, never breaks a
 * game. */

/* A single themed pool's source data, handed to lobbyBotPoolsInstall.
 * The strings are copied by the installer; the caller keeps ownership
 * of its own buffers. */
typedef struct {
    const char        *label;     /* dropdown label */
    const char *const *names;     /* nameCount entries */
    int                nameCount;
} LobbyBotPoolDef;

/* Outcome of an install/load, for logging and tests. Any field
 * pointer may be NULL if the caller doesn't care. */
typedef struct {
    int poolsIn;       /* themed pools offered */
    int poolsKept;     /* themed pools installed */
    int poolsDropped;  /* offered minus kept (empty / over cap) */
    int namesIn;       /* names offered across all kept pools */
    int namesKept;     /* names installed */
    int namesDropped;  /* empty / overlong / duplicate */
} LobbyBotPoolLoadStats;

/* Replace the active themed pools with a copied, safety-clamped copy
 * of `defs`.  Clamping: at most LOBBY_BOT_POOL_MAX_POOLS pools and
 * LOBBY_BOT_POOL_MAX_NAMES names/pool; names longer than
 * LOBBY_BOT_POOL_MAX_NAME_BYTES, empty names, and case-insensitive
 * duplicates within a pool are skipped; pools left with no names are
 * dropped; a missing label is replaced with "Pool N".
 *
 * The generated "Numbered Bots" pool is unaffected and stays the last
 * index.  If nothing usable survives, the built-in defaults are left
 * active and the function returns 0.  Returns the number of themed
 * pools installed.  `stats` (nullable) is filled in either way. */
int  lobbyBotPoolsInstall(const LobbyBotPoolDef *defs, int defCount,
                          LobbyBotPoolLoadStats *stats);

/* Revert to the built-in compiled-in pools, freeing any installed
 * custom set. */
void lobbyBotPoolsReset(void);

/* Parse a bot-names JSON file and install it (replacing the active
 * pools).  Format:
 *   { "pools": [ { "label": "Classic AI", "names": ["HAL-9000", ...] }, ... ] }
 * Returns true if at least one themed pool was installed; on any
 * error (missing file, bad JSON, no usable pools) returns false and
 * leaves the previously-active pools untouched.  `stats` is nullable.
 *
 * Lives in lobby_bot_pools_json.c (cJSON); only linked into targets
 * that need runtime loading. */
bool lobbyBotPoolsLoadFromFile(const char *path,
                               LobbyBotPoolLoadStats *stats);

/* Load the shipped default file (data/bot_names.json) relative to the
 * executable's base path, then the CWD as a fallback.  Returns true
 * on success; on failure the built-in defaults remain active. */
bool lobbyBotPoolsLoadDefault(LobbyBotPoolLoadStats *stats);

/* ── Wire catalog (server → client) ──────────────────────────────
 *
 * So clients render and pick from the *server's* pools (not their own
 * shipped file), the server serialises its active themed pools into a
 * compact, zlib-compressed blob. A joiner is told the blob's id and
 * length in the lobby join sync (CTRL_LOBBY_BOT_POOL_INFO), and asks for
 * the blob on CHANNEL_BULK only when its own pools have another id.
 * The generated "Numbered Bots" pool is NOT serialised — both ends
 * append it locally, so indices line up.
 *
 * Blob layout: [u32 rawLen BE][zlib-deflated payload], where the
 * payload is:
 *   u8  poolCount
 *   per pool: u8 labelLen, label bytes,
 *             u16 nameCount BE,
 *               per name: u8 nameLen, name bytes
 */

/* Cap on the *uncompressed* serialized payload. */
#define LOBBY_BOT_CATALOG_MAX_BYTES (64u * 1024u)

/* Upper bound on the compressed wire blob (zlib compressBound(64KiB) +
 * the 4-byte rawLen prefix, rounded up). Sizes the server serialize
 * buffer and the client reassembly buffer. */
#define LOBBY_BOT_CATALOG_WIRE_MAX (68u * 1024u)

/* Serialize the active themed pools into `out` (compressed). Returns
 * the number of bytes written, or -1 if `outCap` is too small or the
 * uncompressed payload would exceed LOBBY_BOT_CATALOG_MAX_BYTES (in
 * which case trailing pools are dropped to fit rather than failing —
 * see the .c). Writes nothing and returns 0 if there are no themed
 * pools. */
int  lobbyBotPoolsSerialize(unsigned char *out, int outCap);

/* The same, and *outId set to the catalogue's id (see below), or 0 when
 * nothing was written. */
int  lobbyBotPoolsSerializeWithId(unsigned char *out, int outCap,
                                  uint32_t *outId);

/* The id of the active themed pools: a CRC-32 of the uncompressed payload,
 * never 0, and 0 when there are no themed pools. Two peers holding the same
 * pools get the same id, which is how a client tells whether the server's
 * catalogue is one it already has (CTRL_LOBBY_BOT_POOL_INFO). */
uint32_t lobbyBotPoolsCatalogId(void);

/* Decompress + parse a catalog blob produced by lobbyBotPoolsSerialize
 * and install it (replacing the active pools, same clamping as
 * lobbyBotPoolsInstall). Returns themed pools installed, or -1 on a
 * malformed/oversize blob (active pools left untouched on -1).
 * `stats` is nullable. */
int  lobbyBotPoolsDeserializeInstall(const unsigned char *blob, int len,
                                     LobbyBotPoolLoadStats *stats);

#endif /* LOBBY_BOT_POOLS_H */
