/*
 * server_dedicated_log_settings.c - the log_GameSettings blob writer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Split out of server_dedicated_log.c so a unit test can drive the real
 * writer without linking the log file's lifecycle — the round stash, the
 * WinBolo.net upload and the rename all ride on that TU, and none of them
 * has anything to do with how these bytes are laid out.
 *
 * Layout: docs/replay-format.md, "log_GameSettings payload".
 */

#include <stdint.h>

#include "global.h"
#include "server_sim_internal.h"
#include "server_dedicated_log.h"

/* Build the settings blob in the pascal-string form logAddEvent takes: out[0]
 * is the byte count, out[1..] the fields. Every multi-byte value is
 * big-endian, matching the framing the serializer writes around it. The
 * values are read straight off the sim, so this records what the server is
 * running with rather than what the header captured when the file opened. */
void serverDedicatedLogBuildSettings(ServerSim *sim, char *out) {
    uint16_t pillDecay = sim->viewDecaySecs[viewCategoryPill];
    uint16_t baseDecay = sim->viewDecaySecs[viewCategoryBase];
    uint16_t allyDecay = sim->viewDecaySecs[viewCategoryAlly];
    uint16_t timeMinutes = serverSimGetTimeMinutes(sim);
    BYTE flags = 0;
    BYTE settingFlags = 0;

    if (sim->smartPingsOff) settingFlags |= LOG_SETTINGS_FLAG_SMART_PINGS_OFF;
    if (sim->positionalSound) {
        settingFlags |= LOG_SETTINGS_FLAG_POSITIONAL_SOUND;
    }

    if (sim->sim.hiddenMines)       flags |= 0x01u;
    if (serverSimGetTimeLimit(sim)) flags |= 0x02u;
    if (sim->autoLockOnGameStart)   flags |= 0x04u;
    if (sim->ranked)                flags |= 0x08u;
    /* Whether a password is set, never the password text. */
    if (sim->hasPassword)           flags |= 0x10u;
    if (sim->allowNewPlayers)       flags |= 0x20u;
    /* One bit each. The overview window has three values now and this bit
     * says only that it is not the expanded one; the viewer tells Classic
     * from None by the classic-mode bit in out[1], which is right for every
     * case but a host who picked None without classic mode. Widening the
     * blob for that would version a format on disk, so it stays as it is. */
    if (sim->overviewWindow != (BYTE)overviewWindowExpanded) flags |= 0x40u;
    if (sim->lineOfSight != (BYTE)lineOfSightOff)           flags |= 0x80u;

    out[0]  = (char)LOG_SETTINGS_PAYLOAD_LEN;
    /* Same packing as INFO_PACKET.view_policies, so this byte reads the
     * same wherever it is carried. The overview window and line of sight
     * ride the flags byte here rather than INFO's second view byte. */
    out[1]  = (char)infoPacketPackViewPolicies(sim->viewPolicy[viewCategoryPill],
                                               sim->viewPolicy[viewCategoryBase],
                                               sim->viewPolicy[viewCategoryAlly],
                                               sim->classicMode,
                                               sim->alliesInTrees);
    out[2]  = (char)((pillDecay >> 8) & 0xFF);
    out[3]  = (char)(pillDecay & 0xFF);
    out[4]  = (char)((baseDecay >> 8) & 0xFF);
    out[5]  = (char)(baseDecay & 0xFF);
    out[6]  = (char)((allyDecay >> 8) & 0xFF);
    out[7]  = (char)(allyDecay & 0xFF);
    out[8]  = (char)gameTypeGet(&sim->sim.game);
    out[9]  = (char)sim->botAiType;
    out[10] = (char)flags;
    out[11] = (char)((timeMinutes >> 8) & 0xFF);
    out[12] = (char)(timeMinutes & 0xFF);
    /* The lock mask, split across the blob by when each half was added.
     * Bytes 13-14 are its low 16 bits and have been there since the event
     * existed; a reader older than this change reads only those, and for
     * every lock below bit 16 it still reads them correctly. The high half
     * is appended below rather than folded in beside them, because moving
     * the low bits would make every old reader misread a new recording. */
    out[13] = (char)((sim->serverLocks >> 8) & 0xFF);
    out[14] = (char)(sim->serverLocks & 0xFF);
    /* Bits 16-31 of the same mask. LOBBY_LOCK_SMART_PINGS is 1u << 16 and
     * is the first lock to land up here. */
    out[15] = (char)((sim->serverLocks >> 24) & 0xFF);
    out[16] = (char)((sim->serverLocks >> 16) & 0xFF);
    /* Settings that arrived after the flags byte at out[10] filled up. */
    out[17] = (char)settingFlags;
}
