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
 * Name:          lobby_rating.cpp
 * Purpose:       The finished round's WinBolo.net page, on
 *                the recap: the aggregate star rating the
 *                round has been given and the count behind
 *                it, the comments left on it, and the form
 *                that adds one — a 0-10 vote and a line of
 *                text posted as the signed-in account, with
 *                the in-flight post held apart from the rest
 *                of the state. Keyed off the summary's
 *                WinBolo.net log key, with the fetch that
 *                loads it, its bounded retries while the
 *                host's upload catches up, and the re-read
 *                another player's post nudges.
 *********************************************************/

#include <cstdio>   /* snprintf — the comment section's count header */
#include <cstring>  /* strncmp — key comparison in the sync */
#include <vector>   /* std::vector<WbnComment> — the round's comment list */

#include <SDL3/SDL.h>

#include "imgui.h"
#include "lobby_internal.h"       /* BOLO_MOBILE / BOLO_RECAP_WBN_RATING; imguiHandOnHover */
#include "imgui_star_rating.h"    /* imguiStarRatingLoadIcons / imguiStarRating */
extern "C" {
#include "client_sim.h"   /* ClientSim, clientSimGetRatingPostedSeq; RoundStatsSummary + ROUND_STATS_LOGKEY_LEN */
#include "client_net.h"   /* clientSimNetSendRatingPosted */
#include "imgui_winbolonet.h"     /* imguiWinbolonetDrawSection — account state on the form */
#include "../../sdl3draw.h"       /* sdl3DrawGetRenderer — renderer for the star icons */
#include "../../input_gamepad.h"  /* inputGamepadGetScrollDirection — right stick pans the list */
#include "../../../lang.h"        /* langGetText / langGetTextFmt / MessageArgs / STR_DLGWBN_* */
#include "../../../gamefront.h"   /* gameFrontGetWinbolonetToken — the account a post goes out as */
#include "../../../../winbolonet/wbn_comments.h"  /* WbnComment / WbnCommentsFetch / WbnCommentPost, wbn_comments_* */
}

#if !BOLO_MOBILE

#if BOLO_RECAP_WBN_RATING
/* ── WinBolo.net rating & comments ────────────────────────────────
 * The finished round's own page on WinBolo.net: the aggregate stars it
 * has been given, the comments left on it, and a form to add one. All of
 * it hangs off the summary's log key, which only a round a
 * WinBolo.net-registered host uploaded ever carries — a LAN or
 * single-player round has nothing to fetch and draws nothing.
 *
 * The async lifecycle is the log viewer's comments window
 * (src/logviewer/imgui/imgui_comments.cpp): one fetch per key, polled
 * from the render, and a post that re-arms the fetch when it lands. */

/* The round's log reaches WinBolo.net a few seconds after the recap opens,
 * so the first GET can legitimately miss a round that is about to exist.
 * Bounded retries cover the upload lag without becoming a per-frame
 * request loop against a key the server will never have. */
static const int    RECAP_RATING_RETRY_MAX = 6;
static const Uint64 RECAP_RATING_RETRY_MS  = 5000;

/* Shortest gap between two nudge-driven re-reads of the round's page. */
static const Uint64 RECAP_RATING_NUDGE_MIN_MS = 10000;

/* The rating and comments the recap is holding for one round: the key they
 * belong to, the fetch that loaded them, and the add-comment form. */
typedef struct LobbyRatingState {
    /* The key everything below belongs to; a different one means the state is
     * for the previous round and is thrown away. */
    char ratingKey[ROUND_STATS_LOGKEY_LEN] = "";

    WbnCommentsFetch       *fetch         = nullptr;
    bool                    fetchComplete = false;
    int                     fetchStatus   = 0;
    char                    fetchErr[256] = "";
    std::vector<WbnComment> comments;
    float                   rating10      = 0.0f;
    int                     numRatings    = 0;

    char postMsg[256]     = "";
    int  postStatus       = 0;
    char commentText[512] = "";
    int  commentRating    = 0;

    /* Fetches spent on this key, and the earliest tick the next one may go
     * out. */
    int    fetchAttempts  = 0;
    Uint64 fetchRetryAtMs = 0;

    /* Another player posting against this round re-reads the page, so their
     * stars and comment show without waiting for the next round. The counter is
     * only watched for movement; it is consumed on every move but acted on at
     * most once per interval, so a burst of nudges cannot queue a re-read up
     * for later. The bound sits here rather than on the server because what it
     * protects is this client's traffic to WinBolo.net, and it holds whatever
     * the server or a modified client sends. */
    uint32_t ratingSeenSeq   = 0;
    Uint64   ratingNudgeAtMs = 0;

    /* Both expands, driven from our own flags the way the highlight and award
     * expands above are, so a new round's recap starts on the closed form. */
    bool showComments   = false;
    bool showAddComment = false;
} LobbyRatingState;

static LobbyRatingState s_rating = {};

/* Held outside LobbyRatingState: a whole-struct reset would either drop the
 * pointer, leaking the handle and the worker behind it, or free it — and a
 * POST cannot be cancelled, so the free would block until the request answers
 * or times out. It is left to finish instead, and the poll clears it. */
static WbnCommentPost *s_recapPost = nullptr;

/* The fetch handle is owned here, so it has to be released before the struct
 * is overwritten. Freeing it cancels the transfer, so the wait is brief. */
void lobbyRatingReset(void) {
    if (s_rating.fetch) {
        wbn_comments_fetch_free(s_rating.fetch);
    }
    s_rating = LobbyRatingState{};
    /* An in-flight post is deliberately left running — it may still complete
     * against the old key. Until it does it blocks a new post, because the post
     * button only arms while s_recapPost is null, and when it lands its result
     * overwrites the postMsg and postStatus cleared just above; the round it
     * was posted against also does not auto-refresh. Freeing it here is not the
     * answer: a POST cannot be cancelled, so the free would block the
     * round-start path until the request answers or times out. */
}

/* Point the state at the round the lobby is holding, dropping whatever the
 * previous one loaded. Keyed off the summary rather than off the block being
 * drawn: between rounds the summary is gone, the desktop panel may be flipped
 * to the map and the controller layout may be on another tab, and none of
 * those paths reach the renderer — so a render-driven reset would leave the
 * finished round's stars and comments loaded and show them for the frames
 * before the next round's summary lands. Idempotent, so both the per-frame
 * hook and the renderer can call it. */
void lobbyRatingSyncKey(ClientSim *cs, const RoundStatsSummary *st) {
    const char *key = (st && st->wbnLogKey[0] != '\0') ? st->wbnLogKey : "";
    if (strncmp(s_rating.ratingKey, key, sizeof(s_rating.ratingKey)) == 0) return;

    lobbyRatingReset();
    /* Latched, not zeroed: the counter belongs to the sim and keeps climbing
     * across rounds, so a new round starting from zero would read the running
     * total as movement and read the page back a second time. */
    s_rating.ratingSeenSeq   = clientSimGetRatingPostedSeq(cs);
    s_rating.ratingNudgeAtMs = 0;
    if (key[0] != '\0') {
        SDL_strlcpy(s_rating.ratingKey, key, sizeof(s_rating.ratingKey));
    }
}

static void lobbyRatingKick(const char *key) {
    if (s_rating.fetch || s_rating.fetchComplete) return;
    if (s_rating.fetchAttempts >= RECAP_RATING_RETRY_MAX) return;
    if (SDL_GetTicks() < s_rating.fetchRetryAtMs) return;

    s_rating.fetch = wbn_comments_fetch_start(key);
    s_rating.fetchAttempts++;
    if (!s_rating.fetch) {
        /* HTTP isn't up yet. Space the next try like a failed one rather than
         * spending the whole budget over six consecutive frames. */
        s_rating.fetchRetryAtMs = SDL_GetTicks() + RECAP_RATING_RETRY_MS;
    }
}

static void lobbyRatingPoll(ClientSim *cs) {
    if (s_rating.fetch && wbn_comments_fetch_done(s_rating.fetch)) {
        const WbnComment *raw = nullptr;
        size_t count = 0;
        int status = wbn_comments_fetch_result(s_rating.fetch, &raw, &count,
                                               s_rating.fetchErr,
                                               sizeof(s_rating.fetchErr));
        s_rating.comments.clear();
        if (raw && count > 0) {
            s_rating.comments.assign(raw, raw + count);
        }
        wbn_comments_fetch_rating(s_rating.fetch, &s_rating.rating10,
                                  &s_rating.numRatings);
        s_rating.fetchStatus = status;

        wbn_comments_fetch_free(s_rating.fetch);
        s_rating.fetch = nullptr;

        if (status == 200) {
            s_rating.fetchComplete = true;
        } else {
            /* Left incomplete so the kick above comes back for it once the
             * gap has passed, until the budget runs out. */
            s_rating.fetchRetryAtMs = SDL_GetTicks() + RECAP_RATING_RETRY_MS;
        }
    }

    if (s_recapPost && wbn_comments_post_done(s_recapPost)) {
        s_rating.postStatus =
            wbn_comments_post_result(s_recapPost, s_rating.postMsg,
                                     sizeof(s_rating.postMsg));
        wbn_comments_post_free(s_recapPost);
        s_recapPost = nullptr;

        if (s_rating.postStatus == 200 || s_rating.postStatus == 201) {
            s_rating.commentText[0] = '\0';
            s_rating.commentRating  = 0;
            /* Read the round back so the new comment and the rating it moved
             * both show. */
            s_rating.fetchComplete  = false;
            s_rating.fetchAttempts  = 0;
            s_rating.fetchRetryAtMs = 0;
            /* And tell the rest of the lobby, so their blocks read it back
             * too instead of listing this round without the new comment. */
            clientSimNetSendRatingPosted(cs, s_rating.ratingKey);
        }
    }
}

void lobbyRenderRatingBlock(ClientSim *cs, const RoundStatsSummary *st,
                                   float s) {
    lobbyRatingSyncKey(cs, st);

    if (st->wbnLogKey[0] == '\0') {
        /* Nothing on WinBolo.net to rate, so not a separator and not a
         * disabled line — the block costs the body no height at all. */
        return;
    }

    /* Someone else in the lobby has posted against this round. Take the new
     * value whether or not the fetch is allowed yet, so a burst leaves nothing
     * armed behind it, and re-arm only once the interval has passed. */
    {
        uint32_t postedSeq = clientSimGetRatingPostedSeq(cs);
        if (postedSeq != s_rating.ratingSeenSeq) {
            s_rating.ratingSeenSeq = postedSeq;
            if (SDL_GetTicks() >= s_rating.ratingNudgeAtMs) {
                s_rating.fetchComplete   = false;
                s_rating.fetchAttempts   = 0;
                s_rating.fetchRetryAtMs  = 0;
                s_rating.ratingNudgeAtMs =
                    SDL_GetTicks() + RECAP_RATING_NUDGE_MIN_MS;
            }
        }
    }

    /* Every frame: the textures are shared with the log browser, whose exit
     * destroys them, so re-entering the lobby afterwards has to be able to
     * rebuild them. Once they are up the call is an early-out. */
    imguiStarRatingLoadIcons(sdl3DrawGetRenderer());
    lobbyRatingKick(s_rating.ratingKey);
    lobbyRatingPoll(cs);

    ImGui::Separator();

    if (s_rating.fetchStatus == 200) {
        if (s_rating.numRatings > 0) {
            imguiStarRating(s_rating.rating10);
            ImGui::SameLine();
            char ratingBuf[16];
            SDL_snprintf(ratingBuf, sizeof(ratingBuf), "%.1f", s_rating.rating10);
            MessageArgs args = {};
            SDL_strlcpy(args.string1, ratingBuf, sizeof(args.string1));
            args.number = s_rating.numRatings;
            ImGui::TextUnformatted(langGetTextFmt(STR_DLGWBN_RATING, &args));
        } else {
            /* Nobody has rated the round, so an average of 0.0 out of 0 is a
             * score nobody gave it. Said the way the browser's rating column
             * says it, and with no stars, since five empty ones read as a
             * verdict rather than as an absence of one. */
            ImGui::TextDisabled("%s: --", langGetText(STR_DLGWBN_COL_RATING));
        }
    } else if (!s_rating.fetch &&
               s_rating.fetchAttempts >= RECAP_RATING_RETRY_MAX) {
        const char *err = s_rating.fetchErr[0] ? s_rating.fetchErr
                                               : langGetText(STR_DLGWBN_NETERR);
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", err);
    } else {
        ImGui::TextDisabled("%s", langGetText(STR_DLGWBN_LOADINGDETAIL));
    }

    {
        MessageArgs args = {};
        args.number = (int)s_rating.comments.size();
        char cmtHeader[128];
        snprintf(cmtHeader, sizeof(cmtHeader), "%s###recapWbnComments",
                 langGetTextFmt(STR_DLGWBN_COMMENTS_FMT, &args));
        ImGui::SetNextItemOpen(s_rating.showComments, ImGuiCond_Always);
        s_rating.showComments = ImGui::CollapsingHeader(cmtHeader);
    }
    if (s_rating.showComments) {
        /* Height-bounded: the reel is fed whatever the body leaves unused, so
         * a list free to grow with the round's comment count would starve it. */
        ImGui::BeginChild("##recapCommentList",
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 6.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);

        /* The list is text only, so nothing in it can take focus and a round
         * with more comments than the six lines fit is out of reach on a pad.
         * The right stick pans it instead — nothing else in the lobby reads
         * that stick. Covers Steam Input and a native pad alike, +Y = down. */
        float sdx, sdy;
        if (inputGamepadGetScrollDirection(&sdx, &sdy)) {
            ImGui::SetScrollY(ImGui::GetScrollY() + sdy * ImGui::GetTextLineHeight());
        }

        if (s_rating.comments.empty()) {
            ImGui::TextDisabled("%s", langGetText(STR_DLGWBN_NOCOMMENTS));
        } else {
            for (const WbnComment &c : s_rating.comments) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.85f, 1.0f, 1.0f));
                ImGui::TextUnformatted(c.username);
                ImGui::PopStyleColor();
                if (c.rating > 0) {
                    ImGui::SameLine();
                    imguiStarRating((float)c.rating);
                }
                if (c.time_formatted[0]) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("- %s", c.time_formatted);
                }
                ImGui::TextWrapped("  %s", c.comment);
                ImGui::Spacing();
            }
        }
        ImGui::EndChild();
    }

    /* Closed unless the player opens it, so the text field stays out of the
     * nav graph and the Deck's on-screen keyboard never comes up while the
     * recap is only being read. */
    ImGui::SetNextItemOpen(s_rating.showAddComment, ImGuiCond_Always);
    s_rating.showAddComment =
        ImGui::CollapsingHeader(langGetText(STR_DLGWBN_ADDCOMMENT));
    if (s_rating.showAddComment) {
        char wbnToken[256], wbnExpiry[256];
        gameFrontGetWinbolonetToken(wbnToken, wbnExpiry);

        if (wbnToken[0] == '\0') {
            /* Read-only, the way the section renders in game: it reports the
             * account state and, in place of a sign-in button, says where
             * accounts are changed. Signing in is a welcome-screen action —
             * the lobby only reports which account it already has. */
            imguiWinbolonetDrawSection(true);
        } else {
            float cw = ImGui::GetContentRegionAvail().x;
            /* "-" is a comment with no rating, so the combo needs to say what
             * it sets — on its own it reads as an unexplained number picker. */
            ImGui::TextDisabled("%s", langGetText(STR_DLGWBN_COL_RATING));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80 * s);
            ImGui::Combo("##recapRating", &s_rating.commentRating,
                         "-\0 1\0 2\0 3\0 4\0 5\0 6\0 7\0 8\0 9\0 10\0");
            ImGui::SetNextItemWidth(cw);
            ImGui::InputTextWithHint("##recapCmtText",
                                     langGetText(STR_DLGWBN_HINT_COMMENT),
                                     s_rating.commentText,
                                     sizeof(s_rating.commentText));

            /* Until the fetch has found the round, WinBolo.net does not have
             * it yet and a comment posted against the key would be refused. */
            bool canPost = s_rating.commentText[0] != '\0' &&
                           s_recapPost == nullptr &&
                           s_rating.fetchStatus == 200;
            if (!canPost) ImGui::BeginDisabled();
            if (ImGui::Button(langGetText(STR_DLGWBN_POST), ImVec2(cw, 0))) {
                s_rating.postStatus = 0;
                s_rating.postMsg[0] = '\0';
                s_recapPost = wbn_comments_post_start(s_rating.ratingKey, wbnToken,
                                                      s_rating.commentText,
                                                      s_rating.commentRating);
            }
            imguiHandOnHover();
            if (!canPost) ImGui::EndDisabled();

            if (s_recapPost) {
                ImGui::TextDisabled("%s", langGetText(STR_DLGWBN_LOADINGDETAIL));
            } else if (s_rating.postStatus == 200 || s_rating.postStatus == 201) {
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "%s",
                                   langGetText(STR_DLGWBN_POSTED));
            } else if (s_rating.postStatus != 0) {
                const char *err = s_rating.postMsg[0]
                                      ? s_rating.postMsg
                                      : langGetText(STR_DLGWBN_NETERR);
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", err);
            }
        }
    }
}
#endif /* BOLO_RECAP_WBN_RATING */
#endif /* !BOLO_MOBILE */
