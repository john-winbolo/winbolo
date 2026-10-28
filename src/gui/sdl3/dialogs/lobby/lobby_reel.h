/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
*Name:          Lobby Reel
*Filename:      lobby_reel.h
*Purpose:
*  The replay reel's public face: the calls a screen outside
*  the lobby uses to show a reel of its own, and the part of
*  the embedded log viewer (src/logviewer/lv_embed.c) such a
*  screen drives directly. The mobile log player and spectator
*  (src/gui/mobile) are those screens. Everything else about
*  the reel belongs to the lobby and stays in
*  lobby_internal.h, which includes this header.
*
*  C++ only: the rects are ImVec2.
*********************************************************/

#ifndef LOBBY_REEL_H
#define LOBBY_REEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <SDL3/SDL.h>
#include "imgui.h"

extern "C" {
#include "spectator_drain.h"  /* SpecDrainRecord — the embedded spectator's records */
}

/* Closes whatever reel is up: the lobby's, a standalone one or a spectator
 * one. Safe when none is. */
void lobbyReelEnd(void);

/* A reel outside the lobby, for a client with no modal log viewer to open a
 * downloaded game in. The reel owns zipData from the call on. lobbyReelEnd
 * closes it. */
bool lobbyReelOpenStandalone(uint8_t *zipData, size_t zipLen, ImVec2 rect);
void lobbyReelRenderStandalone(ImVec2 rect);

/* The live spectator feed in a reel, for a client with no modal viewer to
 * host it. The host feeds it through lvEmbedSpecSeed / lvEmbedSpecRecord.
 * lobbyReelEnd closes it. */
bool lobbyReelOpenSpectator(ImVec2 rect);
void lobbyReelRenderSpectator(ImVec2 rect);

#if defined(__IPHONEOS__) || defined(__ANDROID__)
/* Hand every polled event over; the reel keeps the pinch ones for its zoom. */
void lobbyReelPinchEvent(const SDL_Event *ev);
#endif

/* The embedded log viewer's half (src/logviewer/lv_embed.c). Hand-declared
 * rather than included: logviewer.h pulls in backend.h / viewport_types.h,
 * whose screen / screenMines types collide with the client's. Scalars, void *
 * and spectator_drain.h's dependency-free SpecDrainRecord only, so no viewer
 * type crosses the seam. */
extern "C" {
bool lvEmbedIsActive(void);
bool lvEmbedSpecBegin(struct SDL_Window *window, struct SDL_Renderer *renderer,
                      int viewW, int viewH);
bool lvEmbedSpecSeed(const uint8_t *seed, uint32_t seedLen);
bool lvEmbedSpecRecord(const SpecDrainRecord *rec);
void lvEmbedSpecFrame(void);
int  lvEmbedSpecPhase(void);
}

#endif /* LOBBY_REEL_H */
