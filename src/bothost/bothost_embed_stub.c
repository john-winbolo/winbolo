/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The Bot Host inside a WinBolo client, for the clients that run no bots:
 * iOS, Android and the web client. Their lobby and front end call the same
 * functions as the desktop game (bothost_embed.h); here the host never
 * starts, the lobby offers no opt-out, and the status is all zero. The
 * desktop game and WinBoloHeadless build bothost_embed.c and
 * bothost_embed_service.c in place of this file, never with it.
 */
#include <string.h>

#include "bothost_embed.h"

bool botHostEmbedStart(const BotHostEmbedConfig *cfg) {
    (void)cfg;
    return false;
}

void botHostEmbedStop(bool tellServer) {
    (void)tellServer;
}

bool botHostEmbedRunning(void) {
    return false;
}

void botHostEmbedSweepSessions(const char *cacheDir) {
    (void)cacheDir;
}

void botHostEmbedGetStatus(BotHostEmbedStatus *out) {
    if (out != NULL) memset(out, 0, sizeof(*out));
}

void botHostEmbedSetWithhold(bool on) {
    (void)on;
}

int botHostEmbedDefaultSlots(void) {
    return 0;
}

void botHostEmbedSetSettings(const BotHostEmbedSettings *settings) {
    (void)settings;
}

void botHostEmbedService(struct ClientSim *cs) {
    (void)cs;
}

void botHostEmbedLeave(const char *why) {
    (void)why;
}

bool botHostEmbedOffered(struct ClientSim *cs) {
    (void)cs;
    return false;
}

void botHostEmbedSetOptOut(struct ClientSim *cs, bool on) {
    (void)cs;
    (void)on;
}

bool botHostEmbedOptOut(struct ClientSim *cs) {
    (void)cs;
    return false;
}
