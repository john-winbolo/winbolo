/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Ping Sounds
 *Filename:      ping_sounds.h
 *Purpose:
 *  Which sound a received smart ping plays, and the one
 *  rule for a kind that has no sound of its own.
 *
 *  Every kind has its own effect id, and the file behind it
 *  is named after the same key the kind's icon uses: the
 *  attack ping draws data/ui/ping/attack.svg and plays
 *  data/sounds/ping_attack.wav. The game only ships two of
 *  the seven files, so most kinds resolve to ping_default
 *  at load; dropping ping_attack.wav into data/sounds/ or
 *  into a skin is all it takes for the attack ping to get
 *  its own sound, with nothing here to change.
 *
 *  pingSoundResolve is that rule and the only copy of it.
 *  The sound backend owns the "found" bitmap it is given —
 *  a bit per kind, set when a file for that kind resolved,
 *  from the game's data or from the active skin — because
 *  only the backend knows what loaded. Passing the bitmap in
 *  rather than asking for it keeps this header free of SDL
 *  and of any backend, so the rule is the same one on the
 *  desktop and in the browser and can be tested on its own.
 *
 *  Add a kind in src/bolo/public/input_packet.h, a row in
 *  src/gui/ping_kinds.h, an effect id in
 *  src/bolo/public/client_enums.h and its two switch arms
 *  here, in the same change.
 *********************************************************/

#ifndef WINBOLO_PING_SOUNDS_H
#define WINBOLO_PING_SOUNDS_H

#include "input_packet.h"  /* PING_KIND_* / PING_KIND_COUNT */
#include "client_enums.h"  /* sndEffects */

#ifdef __cplusplus
extern "C" {
#endif

/* How loud a ping plays, relative to the effects volume; 1.0 = as loud as
 * every other effect. About half, because a ping is a teammate asking for
 * attention rather than something that happened in the world, and at full
 * strength it talked over the shells and the explosions. Tuned by ear in two
 * steps: a third off first, then a fifth off what was left (0.67 * 0.8).
 *
 * It multiplies the gain the player's master and effects settings make rather
 * than replacing it: turning the effects down still turns the pings down, and
 * a skin shipping its own louder ping_*.wav is cut by the same fraction as the
 * ones that ship. It applies to the seven ping effects only — ping_default and
 * the six kinds, the ones pingSoundKindOf names — and to no other effect. */
#define PING_SOUND_GAIN 0.53f

/* pingSoundKindOf's answer for an effect that is not a per-kind ping sound.
 * Past every kind, so a caller can test the answer with a plain
 * `< PING_KIND_COUNT` instead of comparing against this. */
#define PING_SOUND_KIND_NONE 0xFF

/*********************************************************
*NAME:          pingSoundEffect
*PURPOSE:
*  The effect id for a kind's own ping sound, before it is
*  known whether a file for it was found. A kind this build
*  does not know clamps to the standard ping, the same
*  fallback pingKindStyle and pingKindMessageId make.
*
*ARGUMENTS:
*  kind - PING_KIND_*
*********************************************************/
static inline sndEffects pingSoundEffect(unsigned char kind) {
    switch (kind) {
    case PING_KIND_CAUTION:     return pingCaution;
    case PING_KIND_ASSIST:      return pingAssist;
    case PING_KIND_ATTACK:      return pingAttack;
    case PING_KIND_ON_MY_WAY:   return pingOnMyWay;
    case PING_KIND_BOT_COMMAND: return pingBotCommand;
    default:                    return pingStandard;
    }
}

/*********************************************************
*NAME:          pingSoundKindOf
*PURPOSE:
*  Which kind's sound an effect id is, so a sound backend
*  can spot the ones the fallback applies to. Everything
*  else, ping_default included, answers
*  PING_SOUND_KIND_NONE: the default is what the fallback
*  resolves to and is never resolved itself.
*
*ARGUMENTS:
*  value - the effect id to classify
*********************************************************/
static inline unsigned char pingSoundKindOf(sndEffects value) {
    switch (value) {
    case pingStandard:   return PING_KIND_STANDARD;
    case pingCaution:    return PING_KIND_CAUTION;
    case pingAssist:     return PING_KIND_ASSIST;
    case pingAttack:     return PING_KIND_ATTACK;
    case pingOnMyWay:    return PING_KIND_ON_MY_WAY;
    case pingBotCommand: return PING_KIND_BOT_COMMAND;
    default:             return PING_SOUND_KIND_NONE;
    }
}

/*********************************************************
*NAME:          pingSoundResolve
*PURPOSE:
*  The effect a ping of this kind actually plays: the kind's
*  own sound where a file for it resolved, and ping_default
*  where none did. The one copy of that rule.
*
*  "Resolved" is the backend's word for a file that loaded
*  and holds audio, from the game's data or from the active
*  skin — not a file that merely exists. A kind whose only
*  file is empty or will not decode therefore falls back
*  rather than pinging silently.
*
*ARGUMENTS:
*  kind      - PING_KIND_*
*  foundMask - bit PING_KIND_x set when that kind's own
*              sound resolved to a file
*********************************************************/
static inline sndEffects pingSoundResolve(unsigned char kind,
                                          unsigned int foundMask) {
    if (kind >= PING_KIND_COUNT) kind = PING_KIND_STANDARD;
    if ((foundMask & (1u << kind)) != 0) return pingSoundEffect(kind);
    return pingDefault;
}

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_PING_SOUNDS_H */
