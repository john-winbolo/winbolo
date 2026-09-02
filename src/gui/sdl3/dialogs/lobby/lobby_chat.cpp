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
 * Name:          lobby_chat.cpp
 * Purpose:       The lobby's chat. The screen rect of the
 *                chat block, which the map-chooser scrim
 *                punches its hole over, plus the
 *                refocus-after-send and nav-highlight
 *                flags. The history renderer, which turns
 *                an "@mm:ss" token in a line into a link
 *                that seeks the reel. The input + Send row.
 *                And the helper that appends the reel's
 *                position into the chat box.
 *********************************************************/

#include <cstdio>  /* snprintf — formats the "@mm:ss " token */

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_internal.h"  /* window DC.NavHideHighlightOneFrame, NavCursorVisible */
#include "lobby_internal.h"
extern "C" {
#include "client_sim.h"
#include "client_command.h"  /* CHAT_DEST_IS_TEAM */
#include "../../../lang.h"
#include "../../../sound.h"
}

/* Chat state shared between the chat panel and the map chooser.
 *
 * blockMin/blockMax are the screen-space rect of the lobby's chat
 * block, captured each frame so the map-chooser scrim can punch a hole
 * over it. We deliberately leave the chat reachable while the chooser
 * is open so players can keep talking while picking / generating a map,
 * but everything else behind the chooser was getting accidental clicks;
 * the scrim windows below (renderChooserScrim) darken + absorb input
 * over the non-chat area. ImVec2(0,0) on both means "no chat rect yet".
 *
 * refocusFrames/hideNav drive the refocus-after-send and nav-highlight
 * suppression in lobbyRenderChatInputAndSend. */
typedef struct LobbyChatState {
    ImVec2 blockMin;
    ImVec2 blockMax;
    int    refocusFrames;
    bool   hideNav;
} LobbyChatState;

static LobbyChatState   s_chat                   = {};

/* Core captures the chat block's screen rect each frame; the chooser scrim
 * reads it back to punch its hole. */
ImVec2 *lobbyChatBlockMin(void) {
    return &s_chat.blockMin;
}

ImVec2 *lobbyChatBlockMax(void) {
    return &s_chat.blockMax;
}

/* Drop the chat rect and the pending refocus / nav-suppression flags on
 * lobby teardown, so a session left mid-send does not carry a focus
 * grab or a stale hole position into the next one. */
void lobbyChatReset(void) {
    s_chat = LobbyChatState{};
}

/* ----------------------------------------------------------------------
 * Chat history with clickable timestamps
 *
 * A chat line may name a moment in the round as "@m:ss" / "@mm:ss". The token
 * is ordinary text everywhere else — it goes over the wire, into the history
 * and into the .wbv verbatim — so a client with no reel just reads it. Only
 * the render below turns it into something to click.
 * ------------------------------------------------------------------- */

#if !BOLO_MOBILE
/* Match a timestamp token at p. Returns one past its last digit and fills the
 * out-params on a match, NULL otherwise. Minutes are one or two digits,
 * seconds exactly two and under 60. A third seconds digit rejects the whole
 * candidate, so "@1:234" stays text instead of matching "@1:23" out of the
 * front of it. */
static const char *lobbyMatchChatTime(const char *p, const char *end,
                                      unsigned *outMins, unsigned *outSecs) {
    const char *q;
    unsigned    mins   = 0, secs = 0;
    int         digits = 0;

    if (p >= end || *p != '@') {
        return NULL;
    }
    q = p + 1;   /* past the '@' */
    while (q < end && digits < 2 && *q >= '0' && *q <= '9') {
        mins = mins * 10u + (unsigned)(*q - '0');
        q++;
        digits++;
    }
    if (digits == 0 || q >= end || *q != ':') {
        return NULL;
    }
    q++;
    if ((end - q) < 2 || q[0] < '0' || q[0] > '9' || q[1] < '0' || q[1] > '9') {
        return NULL;
    }
    secs = (unsigned)(q[0] - '0') * 10u + (unsigned)(q[1] - '0');
    q += 2;
    if (secs >= 60u || (q < end && *q >= '0' && *q <= '9')) {
        return NULL;
    }
    *outMins = mins;
    *outSecs = secs;
    return q;
}

static bool lobbyChatLineHasTime(const char *begin, const char *end) {
    unsigned mins = 0, secs = 0;
    for (const char *q = begin; q < end; q++) {
        if (*q == '@' && lobbyMatchChatTime(q, end, &mins, &secs) != NULL) {
            return true;
        }
    }
    return false;
}

/* Lay out one line that holds at least one token as alternating text and link
 * runs, butted together with zero-spacing SameLine so no gap is invented. Such
 * a line is not wrapped: a wrap position does not carry across SameLine, so a
 * long one clips at the panel edge rather than flowing onto a second row. That
 * is the deliberate trade for laying the line out by hand — chat is capped at
 * 128 characters, so the case is rare. TextLink takes its id from its label,
 * so the ids pushed here are what stop two identical tokens in one history
 * from sharing one. */
static void lobbyRenderChatTimeLine(const char *begin, const char *end,
                                    int lineIndex) {
    const char *p       = begin;
    int         segment = 0;

    ImGui::PushID(lineIndex);
    while (p < end) {
        const char *tok = NULL, *tokEnd = NULL, *q;
        unsigned    mins = 0, secs = 0;
        char        label[8];
        size_t      labelLen;

        for (q = p; q < end; q++) {
            if (*q != '@') {
                continue;
            }
            tokEnd = lobbyMatchChatTime(q, end, &mins, &secs);
            if (tokEnd != NULL) {
                tok = q;
                break;
            }
        }

        if (tok == NULL) {
            if (segment > 0) ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted(p, end);
            break;
        }
        if (tok > p) {
            if (segment > 0) ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted(p, tok);
            segment++;
        }

        labelLen = (size_t)(tokEnd - tok);
        if (labelLen >= sizeof(label)) {
            labelLen = sizeof(label) - 1;
        }
        SDL_memcpy(label, tok, labelLen);
        label[labelLen] = '\0';

        if (segment > 0) ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushID(segment);
        if (ImGui::TextLink(label)) {
            lvEmbedSeekToTime((mins * 60u + secs) * 1000u);
        }
        ImGui::PopID();
        segment++;

        p = tokEnd;
    }
    ImGui::PopID();
}
#endif

/* One chat history, the \n-separated blob the sim owns (read only — never
 * written to here). Runs of lines with no token are batched into a single
 * wrapped TextUnformatted, so a history without one renders as the same lone
 * blob draw it always did; only a line carrying a token is split up. Zero
 * vertical item spacing is what makes the two kinds of line stack at the pitch
 * a single text block would. With no reel to seek there is nothing to link, so
 * every line takes the plain path. */
void lobbyRenderChatHistory(const char *blob) {
    if (blob == NULL) {
        return;
    }

#if !BOLO_MOBILE
    if (lvEmbedIsActive()) {
        const ImGuiStyle &style     = ImGui::GetStyle();
        const char       *runBegin  = NULL;
        const char       *runEnd    = NULL;
        const char       *p         = blob;
        int               lineIndex = 0;

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(style.ItemSpacing.x, 0.0f));
        for (;;) {
            const char *lineEnd = p;
            while (*lineEnd != '\0' && *lineEnd != '\n') {
                lineEnd++;
            }
            if (lobbyChatLineHasTime(p, lineEnd)) {
                if (runBegin != NULL) {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted(runBegin, runEnd);
                    ImGui::PopTextWrapPos();
                    runBegin = NULL;
                }
                lobbyRenderChatTimeLine(p, lineEnd, lineIndex);
            } else {
                if (runBegin == NULL) {
                    runBegin = p;
                }
                runEnd = lineEnd;
            }
            if (*lineEnd == '\0') {
                break;
            }
            p = lineEnd + 1;
            lineIndex++;
        }
        if (runBegin != NULL) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(runBegin, runEnd);
            ImGui::PopTextWrapPos();
        }
        ImGui::PopStyleVar();
        return;
    }
#endif

    /* Wrap long lines at the child's right edge so a full-length (128-char)
     * message flows onto extra lines instead of running off the panel. */
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(blob);
    ImGui::PopTextWrapPos();
}

/* Render the lobby chat InputText + Send button pair, with the
 * 2-frame refocus-after-send and nav-highlight suppression logic
 * shared between the chat tab and the in-game lobby chat panel.
 * SetKeyboardFocusHere(0) is issued before the InputText (targeting
 * the next widget) rather than SetKeyboardFocusHere(-1) after the
 * Send button (which would target Send, not the input). The 2-frame
 * counter survives ImGui's internal InputText deactivation on Enter,
 * which stomps a single-frame focus request. */
void lobbyRenderChatInputAndSend(ClientSim *cs, char *chatInput,
                                        BYTE myPlayerNum, bool hasTransport,
                                        float s, BYTE destPlayer) {
    /* A spectator may chat in the live lobby: the input is enabled and Enter/
     * Send route through the same CMD_CHAT path, which the server re-tags as
     * spectator chat. The local echo below stays player-only — a spectator owns
     * no slot to name and relies on the server's round-trip line instead. */
    const bool spectator = clientSimIsSpectator(cs);
    float btnW = 60.0f * s;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btnW - 8.0f);
    if (s_chat.refocusFrames > 0) {
        ImGui::SetKeyboardFocusHere(0);
        s_chat.refocusFrames--;
    }
    if (s_chat.hideNav)
        ImGui::GetCurrentWindow()->DC.NavHideHighlightOneFrame = true;
    bool enterPressed = ImGui::InputText("##ChatInput", chatInput, LOBBY_CHAT_INPUT_SIZE,
                                         ImGuiInputTextFlags_EnterReturnsTrue);
    if (s_chat.hideNav) {
        ImGui::GetCurrentContext()->NavCursorVisible = false;
        if (ImGui::IsItemActive()) s_chat.hideNav = false;
    }
    ImGui::SameLine();
    bool chatEmpty = (chatInput[0] == '\0');
    if (chatEmpty) ImGui::BeginDisabled();
    bool sendClicked = ImGui::Button(langGetText(STR_DLGMSG_BUTTON), ImVec2(btnW, 0));
    if (chatEmpty) ImGui::EndDisabled();
    if ((sendClicked || enterPressed) && !chatEmpty && hasTransport) {
        clientSimNetSendChat(cs, destPlayer, chatInput);
        if (!spectator) {
            const ClientLobbySlot *mySlot = clientSimGetLobbySlot(cs, myPlayerNum);
            const char *myName = (mySlot && mySlot->connected)
                ? mySlot->playerName : langGetText(STR_DLGLOBBY_ME);
            if (CHAT_DEST_IS_TEAM(destPlayer))
                clientSimAppendLobbyTeamChat(cs, myName, chatInput);
            else
                clientSimAppendLobbyChat(cs, myName, chatInput);
            /* Emit the cue locally for our own send. The incoming-chat cue
             * (CTRL_CHAT / CTRL_SERVER_TEXT) only fires for fromPlayer != myPN,
             * so self-sends are silent without this. A spectator instead gets
             * its line (and cue) from the server's round-trip CTRL_SPECTATOR_CHAT. */
            soundPlayEffect(lobbyChatReceived);
        }
        chatInput[0] = '\0';
        s_chat.refocusFrames = 2;
        s_chat.hideNav = true;
    }
}

#if !BOLO_MOBILE
/* Append the reel's position to the chat box as "@mm:ss ", then take the
 * keyboard focus the way a send does so the player types straight after it.
 * Silently does nothing when the whole token will not fit: half a token in
 * the box reads as a link nobody can follow. A round past 99 minutes writes
 * three minute digits, which lobbyMatchChatTime leaves as plain text. */
void lobbyChatInputAppendTime(uint32_t curMs) {
    unsigned secs = (unsigned)(curMs / 1000u);
    char     token[16];
    size_t   used, room;

    snprintf(token, sizeof(token), "@%02u:%02u ", secs / 60u, secs % 60u);
    used = SDL_strlen(lobbyFrameChatInput());
    room = (size_t)(LOBBY_CHAT_INPUT_SIZE - 1) - used;
    if (SDL_strlen(token) > room) {
        return;
    }
    SDL_strlcat(lobbyFrameChatInput(), token, LOBBY_CHAT_INPUT_SIZE);
    s_chat.refocusFrames = 2;
}
#endif
