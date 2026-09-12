/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          UDP Server Voice
 *Filename:      udp_server_voice.c
 *Author:        John Morrison
 *Purpose:
 *  The server's voice forwarding, split out of
 *  transport_udp_server.c.
 *    - The per-tick pump that takes voice segments off each
 *      client's channel, picks which talkers each recipient
 *      hears, and fans the frames back out.
 *    - The mute and enable controls the console and the
 *      command dispatch call.
 *********************************************************/

#if defined(WB_VOICEDEBUG)
#include <stdio.h>                         /* fopen, fprintf, fflush */
#endif
#include <string.h>                        /* memcpy, memset, NULL */

#include "transport_udp_server_internal.h" /* udpServer, serverPumpVoice,
                                            * MAX_SPECTATORS, PlayerBitMap */
#include "channel_mux.h"                   /* channelSendBestEffort,
                                            * channelReceiveBestEffort,
                                            * CHANNEL_VOICE, CHANNEL_MAX_SEG,
                                            * CHANNEL_VOICE_SEG */
#include "voice_segment.h"                 /* voiceSegmentUnpackUp, voiceSegmentPackDown */
#include "voice_talker_select.h"           /* VoiceTalkerCandidate, voiceSelectTalkers */
#include "players.h"                       /* playersIsAllie */
#include "game_sim.h"                      /* GameSim — serverSimGetGameSim(sim)->plyrs */
#include "server_sim.h"                    /* ServerSim, ServerState, serverSimGetState,
                                            * serverSimGetGameSim, serverSimPublishControl */
#include "control_event.h"                 /* ControlEvent, CTRL_VOICE_TALKING */

void transportUdpServerSetVoiceMute(BYTE clientSlot, BYTE targetPlayer,
                                    bool muted) {
    if (clientSlot >= MAX_TANKS || targetPlayer >= MAX_TANKS) {
        return;
    }
    if (!udpServer.clients[clientSlot].connected) {
        return;
    }
    if (muted) {
        udpServer.clients[clientSlot].voiceMuteMask |=
            (PlayerBitMap)1u << targetPlayer;
    } else {
        udpServer.clients[clientSlot].voiceMuteMask &=
            ~((PlayerBitMap)1u << targetPlayer);
    }
}

PlayerBitMap transportUdpServerGetVoiceMuteMask(BYTE clientSlot) {
    if (clientSlot >= MAX_TANKS || !udpServer.clients[clientSlot].connected) {
        return 0;
    }
    return udpServer.clients[clientSlot].voiceMuteMask;
}

void transportUdpServerSetVoiceEnabled(bool enabled) {
    udpServer.voiceDisabled = !enabled;
}

void transportUdpServerGetVoiceStats(uint32_t *outAccepted,
                                     uint32_t *outDropped,
                                     uint32_t *outTalkerCapped) {
    if (outAccepted != NULL) *outAccepted = udpServer.voiceSegsAccepted;
    if (outDropped != NULL) *outDropped = udpServer.voiceSegsDropped;
    if (outTalkerCapped != NULL) {
        *outTalkerCapped = udpServer.voiceSegsTalkerCapped;
    }
}

#if defined(WB_VOICEDEBUG)
/* One row per connected slot per second, written to voicestats-server.csv in
 * the working directory for the length of the run. Built only under
 * WB_VOICEDEBUG: the counters below it are always compiled in and cost an
 * increment, but a shipping server does not write a file nobody asked for.
 *
 * A voice fault is reported after the game, by someone who could not see it
 * happening and had nothing to capture. The counters above already separate
 * every way a frame can be refused; without a file they are only readable
 * from a debugger. Written unconditionally and flushed per row, so a server
 * stopped with a signal still leaves everything it had. */
static void voiceStatsWrite(void) {
    static FILE *fp = NULL;
    static bool tried = false;
    static uint32_t nextTick = 0;
    int i;

    if (udpServer.tickCount < nextTick) {
        return;
    }
    /* SERVER_TICK_LENGTH is 20 ms, so fifty ticks is a second. */
    nextTick = udpServer.tickCount + 50;

    if (!tried) {
        tried = true;
        fp = fopen("voicestats-server.csv", "w");
        if (fp != NULL) {
            fprintf(fp, "tick,slot,name,pingMs,accepted,dropVoiceOff,"
                        "dropPerTickCap,dropNotInGame,dropUnpack,dropRepack,"
                        "forwarded,capped,sent,ringDropped,budgetSkipped\n");
        }
    }
    if (fp == NULL) {
        return;
    }

    for (i = 0; i < MAX_TANKS; i++) {
        uint32_t sent, ringDropped, budgetSkipped;

        if (!udpServer.clients[i].connected) continue;
        channelGetBestEffortStats(&udpServer.channelMux[i], CHANNEL_VOICE,
                                  &sent, &ringDropped, &budgetSkipped);
        fprintf(fp, "%u,%d,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                udpServer.tickCount, i, udpServer.clients[i].playerName,
                (unsigned)udpServer.clients[i].pingMs,
                udpServer.voiceSlot[i].accepted,
                udpServer.voiceSlot[i].dropVoiceOff,
                udpServer.voiceSlot[i].dropPerTickCap,
                udpServer.voiceSlot[i].dropNotInGame,
                udpServer.voiceSlot[i].dropUnpack,
                udpServer.voiceSlot[i].dropRepack,
                udpServer.voiceSlot[i].forwarded,
                udpServer.voiceSlot[i].capped,
                sent, ringDropped, budgetSkipped);
    }
    fflush(fp);
}
#else
#define voiceStatsWrite() ((void)0)
#endif /* WB_VOICEDEBUG */

/* Most voice frames a client may have accepted from it in one tick: one is
 * the steady state at the 20 ms wire cadence, and the second absorbs drift
 * between the capture clock and the server tick. Anything past that is
 * drained and dropped so a flooding client cannot buy itself extra
 * bandwidth or leave a backlog behind. */
#define VOICE_SEGMENTS_PER_TICK 2

/* Most talkers forwarded to any one recipient at once. Not an MTU limit —
 * four ~64 B frames on top of a snapshot sit far inside UDP_MAX_PAYLOAD — but
 * an intelligibility one: nobody can follow more than three or four voices at
 * the same time, so a crowded channel is kept to the four worth hearing. */
#define VOICE_MAX_FORWARDED_TALKERS 4

/* Silence longer than this ends an utterance: the next frame from that slot
 * starts a new one and ranks as a fresh arrival for the cap above. Measured in
 * udpServer.tickCount, one per 20 ms server frame, so ten ticks is 200 ms in
 * every server state — long enough to ride out the gaps inside ordinary
 * speech, short enough that taking a turn to speak counts as one. */
#define VOICE_ONSET_GAP_TICKS 10

/* One accepted, packed server->client voice segment held between the two
 * passes of serverPumpVoice: the sender, the sequence number the cap's
 * tie-break ranks on, and the bytes as they will go out. */
typedef struct {
    uint8_t  from;
    uint8_t  seq;
    uint16_t len;
    uint8_t  bytes[CHANNEL_VOICE_SEG];
} VoiceStagedFrame;

/* Carry voice from each client to the clients allowed to hear it. The
 * payload is opaque: it arrives encoded, is re-framed with the sender's
 * player number, and goes back out untouched — the server never decodes.
 *
 * Runs once per tick from the send path (while running) and from the
 * timeout sweep (every other state), ahead of the carriers that put the
 * channel data on the wire, so a frame is forwarded in the tick it landed. */
void serverPumpVoice(ServerSim *sim) {
    bool inGame = (serverSimGetState(sim) == serverStateRunning);
    bool voiceOn = !udpServer.voiceDisabled;
    /* The transport's own tick, not the sim's: this measures how long ago a
     * frame arrived, and the sim's tick does not advance at a constant rate —
     * it stands still through the countdown and the game-over hold, and runs
     * twice per frame inside a game. Both the onset bookkeeping and the
     * talking set below read it, so they stay on one clock. */
    uint32_t tick = udpServer.tickCount;
    /* Every frame accepted this tick, held until pass 2 knows who hears it.
     * Bounded by the per-sender flood cap, so it cannot overflow. */
    VoiceStagedFrame staged[MAX_TANKS * VOICE_SEGMENTS_PER_TICK];
    int stagedCount = 0;
    /* One entry per sender that staged at least one frame. */
    VoiceTalkerCandidate talkers[MAX_TANKS];
    int talkerCount = 0;
    int from;
    int to;
    int s;

    /* ── Pass 1: drain every sender's ring and stage what is accepted ──── */
    for (from = 0; from < MAX_TANKS; from++) {
        uint8_t segBuf[CHANNEL_MAX_SEG];
        uint16_t segLen;
        int accepted = 0;
        bool onsetDone = false;
        bool anyStaged = false;
        uint8_t newestSeq = 0;

        /* A slot with no live remote client behind it — an empty slot or a
         * bot — has no voice to forward. */
        if (!udpServer.clients[from].connected) continue;

        while (channelReceiveBestEffort(&udpServer.channelMux[from],
                                        CHANNEL_VOICE, segBuf, &segLen)) {
            uint8_t seq, flags;
            const uint8_t *opus;
            int opusLen;
            uint8_t downBuf[CHANNEL_VOICE_SEG];
            int downLen;

            /* Voice off for this server: the loop still drains the ring, so
             * a client that sends anyway cannot fill it and stall behind
             * frames nobody will read.  Nothing is forwarded and nothing
             * counts as accepted, but the segment is counted as dropped so
             * an operator can see traffic arriving at a server that will
             * not carry it. */
            if (!voiceOn) {
                udpServer.voiceSegsDropped++;
                udpServer.voiceSlot[from].dropVoiceOff++;
                continue;
            }

            if (accepted >= VOICE_SEGMENTS_PER_TICK) {
                udpServer.voiceSegsDropped++;
                udpServer.voiceSlot[from].dropPerTickCap++;
                continue;
            }
            /* Not in the game yet: a client still taking the map is not a
             * talker, and the same rule keeps it off the receiving end. */
            if (!udpServer.mapDownload[from].downloadComplete) {
                udpServer.voiceSegsDropped++;
                udpServer.voiceSlot[from].dropNotInGame++;
                continue;
            }
            if (!voiceSegmentUnpackUp(segBuf, (int)segLen, &seq, &flags,
                                      &opus, &opusLen)) {
                udpServer.voiceSegsDropped++;
                udpServer.voiceSlot[from].dropUnpack++;
                continue;
            }
            /* Counts towards the per-sender flood cap from here, whatever
             * the pack below does with it. */
            accepted++;

            downLen = voiceSegmentPackDown(downBuf, (int)sizeof(downBuf),
                                           (uint8_t)from, seq, flags,
                                           opus, opusLen);
            if (downLen <= 0) {
                udpServer.voiceSegsDropped++;
                udpServer.voiceSlot[from].dropRepack++;
                continue;
            }
            udpServer.voiceSegsAccepted++;
            udpServer.voiceSlot[from].accepted++;

            if (stagedCount < (int)(sizeof(staged) / sizeof(staged[0]))) {
                VoiceStagedFrame *st = &staged[stagedCount];
                st->from = (uint8_t)from;
                st->seq  = seq;
                st->len  = (uint16_t)downLen;
                memcpy(st->bytes, downBuf, (size_t)downLen);
                /* The tie-break ranks on the newest sequence number this
                 * sender staged, compared wrap-safely: seq 3 arriving after
                 * seq 250 is newer, not older. */
                if (!anyStaged || (int8_t)(seq - newestSeq) > 0) {
                    newestSeq = seq;
                }
                anyStaged = true;
                stagedCount++;
            }

            /* Onset bookkeeping, once per sender per tick: two frames landing
             * in one tick are one arrival, and the second must not read as a
             * fresh onset.  A slot that has never spoken has lastFrameTick 0
             * (zero-initialised, and cleared again on disconnect), which is
             * an onset rather than the continuation the subtraction below
             * would otherwise make of it.  That subtraction is unsigned on
             * purpose: if the tick restarts under this bookkeeping it
             * underflows to a large gap, which reads as a new onset — the
             * safe answer. */
            if (!onsetDone) {
                if (udpServer.voiceLastFrameTick[from] == 0 ||
                    tick - udpServer.voiceLastFrameTick[from] >
                        VOICE_ONSET_GAP_TICKS) {
                    udpServer.voiceOnsetTick[from] = tick;
                }
                udpServer.voiceLastFrameTick[from] = tick;
                onsetDone = true;
            }
        }

        if (anyStaged) {
            talkers[talkerCount].slot      = (uint8_t)from;
            talkers[talkerCount].onsetTick = udpServer.voiceOnsetTick[from];
            talkers[talkerCount].newestSeq = newestSeq;
            talkerCount++;
        }
    }

    /* ── Pass 2: per recipient, choose the talkers, then send their frames ─
     *
     * The cap is per recipient, so it cannot be applied while draining: at
     * the point sender 2's frame is packed there is no way to know how many
     * senders this recipient will end up hearing, and a running counter would
     * keep whichever four came first by slot index rather than the four that
     * started talking most recently. */
    for (to = 0; to < MAX_TANKS; to++) {
        VoiceTalkerCandidate audible[MAX_TANKS];
        uint8_t chosen[VOICE_MAX_FORWARDED_TALKERS];
        PlayerBitMap audibleMask = 0;
        PlayerBitMap chosenMask = 0;
        int audibleCount = 0;
        int chosenCount;
        int t, f;

        if (!udpServer.clients[to].connected) continue;
        if (!udpServer.mapDownload[to].downloadComplete) continue;

        for (t = 0; t < talkerCount; t++) {
            int talker = talkers[t].slot;

            if (talker == to) continue;
            /* This recipient has muted the talker. The same bit gates
             * their chat, so one toggle covers both. */
            if ((udpServer.clients[to].voiceMuteMask &
                 ((PlayerBitMap)1u << talker)) != 0) {
                continue;
            }
            /* In game, voice follows the live alliance — so a mid-game
             * alliance change takes effect on the next frame. Outside a
             * game everyone hears everyone: the lobby is where teams get
             * argued out, and scoping voice by team there works against
             * the room. */
            if (inGame &&
                !playersIsAllie(&serverSimGetGameSim(sim)->plyrs,
                                (BYTE)to, (BYTE)talker)) {
                continue;
            }
            audible[audibleCount++] = talkers[t];
            audibleMask |= (PlayerBitMap)1u << talker;
        }
        if (audibleCount == 0) continue;

        chosenCount = voiceSelectTalkers(audible, audibleCount,
                                         VOICE_MAX_FORWARDED_TALKERS, chosen);
        for (t = 0; t < chosenCount; t++) {
            chosenMask |= (PlayerBitMap)1u << chosen[t];
        }

        /* Every frame of a chosen talker goes, in the order it was staged: a
         * talker who survives the cap must not lose their second frame of the
         * tick. A frame the recipient could not hear anyway is not the cap's
         * doing and is not counted as suppressed. */
        for (f = 0; f < stagedCount; f++) {
            PlayerBitMap bit = (PlayerBitMap)1u << staged[f].from;

            if ((audibleMask & bit) == 0) continue;
            if ((chosenMask & bit) == 0) {
                udpServer.voiceSegsTalkerCapped++;
                udpServer.voiceSlot[to].capped++;
                continue;
            }
            channelSendBestEffort(&udpServer.channelMux[to], CHANNEL_VOICE,
                                  staged[f].bytes, staged[f].len);
            udpServer.voiceSlot[to].forwarded++;
        }
    }

    /* A viewer is sent no voice. It watches the game on the anti-ghosting
     * delay, but voice would arrive live, and a viewer relaying what it
     * heard would hand out exactly what the delay is there to withhold.
     *
     * A viewer sees the whole map, so anything it says would be coaching.
     * Its voice is forwarded nowhere; draining it keeps the channel's ring
     * from filling and stalling behind frames nobody will ever read. */
    for (s = 0; s < MAX_SPECTATORS; s++) {
        uint8_t segBuf[CHANNEL_MAX_SEG];
        uint16_t segLen;

        if (!udpServer.spectators[s].connected) continue;
        while (channelReceiveBestEffort(&udpServer.spectators[s].channelMux,
                                        CHANNEL_VOICE, segBuf, &segLen)) {
            /* discarded */
        }
    }

    /* ── Tell everyone who is talking ───────────────────────────────────
     *
     * A player you have muted is culled above, so nothing of theirs ever
     * reaches you and nothing local can show that they are speaking. This
     * set is what lets a client show it; each client intersects it with
     * its own mute list.
     *
     * Lobby and countdown only. There voice is all-talk, so the set says
     * nothing a listener could not already hear. In a running game voice
     * follows the alliance, and broadcasting the set would tell a player
     * that an enemy is speaking — so outside those two states the set is
     * empty rather than unsent, which sends one final empty set as the
     * game starts and leaves nobody stuck talking for the round.
     *
     * "Talking" is the same utterance the forwarding cap works in: a frame
     * within VOICE_ONSET_GAP_TICKS of now. Reads the bookkeeping pass 1
     * has already done this tick, and keeps no clock of its own. */
    {
        ServerState state = serverSimGetState(sim);
        PlayerBitMap talking = 0;

        if (state == serverStateLobby || state == serverStateCountdown) {
            int slot;
            for (slot = 0; slot < MAX_TANKS; slot++) {
                if (udpServer.voiceLastFrameTick[slot] != 0 &&
                    tick - udpServer.voiceLastFrameTick[slot] <=
                        VOICE_ONSET_GAP_TICKS) {
                    talking |= (PlayerBitMap)1u << slot;
                }
            }
        }

        /* Still only on a change, so a quiet lobby costs nothing — plus the
         * explicit ask a departure leaves behind, which is the one case where
         * the set has to go out again while comparing equal. */
        if (talking != udpServer.voiceTalkingPublished ||
            udpServer.voiceTalkingResend) {
            ControlEvent evt;
            memset(&evt, 0, sizeof(evt));
            evt.type = CTRL_VOICE_TALKING;
            evt.u.voiceTalking.talking = talking;
            serverSimPublishControl(sim, &evt);
            udpServer.voiceTalkingPublished = talking;
            udpServer.voiceTalkingResend = false;
        }
    }

    voiceStatsWrite();
}
