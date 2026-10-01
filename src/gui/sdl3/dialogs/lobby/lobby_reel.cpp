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
 * Name:          lobby_reel.cpp
 * Purpose:       The embedded replay reel on the round
 *                recap: the playback and scrub state the
 *                log viewer is driven through, the round
 *                log that feeds it — the file this process
 *                recorded, else the copy the server sends
 *                over the game socket, else the copy
 *                WinBolo.net holds under the round's key —
 *                the optional crop frame drawn over the
 *                picture, and the render path that blits
 *                the viewer's visible slice and hands
 *                wheel, pinch and drag back to it.
 *********************************************************/

#include <cstdio>   /* snprintf — the seek slider's elapsed/total label */
#include <cstdlib>  /* malloc / free — the round-log blob the viewer takes over */
#include <cstring>  /* strncmp — the fetch's round-key comparison */
#include <cmath>    /* floorf — whole source pixels in the blitted slice */
#include <atomic>   /* std::atomic — the fetch's byte counters and done flag */
#include <thread>   /* std::thread — the curl transport's download worker */

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_internal.h"  /* IM_PI, ImGui::SetKeyOwner — the buffering ring and the wheel capture */
/* BOLO_MOBILE / BOLO_REEL_WBN_FETCH[_CURL] / BOLO_RECAP_CLIP_GIF; LobbyReelState
 * and lobbyReel; the lvEmbed* entry points and the non-curl wbRoundLogFetch*
 * transport; lobbyIcons / lobbyLoadStatusIconsOnce; the clip export's entry
 * points; and through imgui_dialog_utils.h imguiHelpTooltip and
 * imguiPush/PopNearestSampling. */
#include "lobby_internal.h"
extern "C" {
#include "client_sim.h"   /* ClientSim / ClientLobbySlot, the round-log getters and CLIENT_ROUND_LOG_*; RoundStatsSummary + ROUND_STATS_LOGKEY_LEN */
#include "client_net.h"   /* clientSimNetSendRoundLogRequest — the ask that starts a transfer */
#include "../../sdl3draw.h"      /* sdl3DrawGetWindow / sdl3DrawGetRenderer — what the viewer draws into */
#include "../../macos_pinch.h"   /* macOSPinchZoomConsume — trackpad pinch over the reel */
#include "../../../lang.h"       /* langGetText / langGetTextFmt / MessageArgs / STR_DLGLOBBY_REEL_* / STR_LV_* */
#include "../../../gamefront.h"  /* gameFrontHasLocalServer — only a round this process recorded plays from file */
#include "../../../../common/wb_log.h"  /* WB_LOG_INFO / WB_LOG_CAT_GUI — the transfer trace */
#include "../../../../server/server_dedicated_log.h"  /* serverDedicatedLogLastRoundFile — the last completed round's .wbv */
#include "../../../../bolo/public/wire_limits.h"      /* ROUND_LOG_MAX_BYTES — ceiling on the WinBolo.net download */
#include "../../../../winbolonet/http.h"              /* httpCreate / WbnProgressFn / wbn_api_download_to_memory_progress */
#include "../../../../winbolonet/winbolonet_core.h"   /* winbolonetKeyIsValid — the key is pasted into a request path */
}

#if !BOLO_MOBILE
/* ── Embedded replay reel ─────────────────────────────────────────
 * The log viewer decodes on its own timer threads and paints the round
 * into an SDL render target; the recap blits the visible slice of that
 * target as an image and feeds wheel/drag input back. The viewer owns a
 * process-wide decoder singleton, so the reel is torn down whenever the
 * recap stops drawing it or the summary clears. */

static LobbyReelState s_reel = {};

/* Core reads what is playing, recap the slack the reel hands back, clipgif
 * the crop frame and the last blitted slice. */
LobbyReelState *lobbyReel(void) {
    return &s_reel;
}

#if BOLO_RECAP_CLIP_GIF
/* Smallest crop the frame will shrink to, in displayed pixels. */
static const float REEL_CROP_MIN_PX = 16.0f;
/* Grab margin either side of an edge, in displayed pixels. */
static const float REEL_CROP_GRAB_PX = 6.0f;
#endif

/* A backward scrub cannot continue the decode — it has to restore the round's
 * snapshot and replay from there — so only the newest one in a burst is paid
 * for. Forward scrubs are incremental and run every frame. */
#define RECAP_SCRUB_BACK_MS 90

#if BOLO_REEL_WBN_FETCH
/* Where a round shared through WinBolo.net comes from. A host registered with
 * WinBolo.net uploads its log there rather than serving it over the game
 * connection, so a client that joined fetches the same bytes over HTTP using
 * the key the round summary carries. The upload lands a few seconds after the
 * recap opens, so the first GET is expected to miss a round that is about to
 * exist; a bounded ladder covers that lag without becoming a request a frame.
 *
 * Under the curl transport the worker writes buf/len/status and then releases
 * s_reelWbn.done; the render thread reads those three only after acquiring it,
 * which is the whole hand-off — one producer, one consumer, no mutex. Every
 * other field here is the render thread's own, s_reelWbn.running included, so
 * the guards that decide whether to start another attempt never read a field a
 * worker is writing. The fetch transport has no second thread at all and fills
 * the same three fields from its poll. */
static const int    REEL_WBN_RETRY_MAX = 12;
static const Uint64 REEL_WBN_RETRY_MS  = 5000;

typedef struct LobbyReelWbnFetch {
    char                   key[ROUND_STATS_LOGKEY_LEN] = "";
#if BOLO_REEL_WBN_FETCH_CURL
    /* The worker and the two fields that coordinate with it. Nothing outside
     * the curl transport has a second thread to coordinate with. */
    std::thread            thread;
    std::atomic<bool>      done{false};
    volatile int           cancel = 0;
#endif
    /* What the transfer has moved, for the reading below. Curl fills both from
     * its worker as bytes arrive; fetch resolves the whole body at once and
     * leaves them at zero, which that reading already treats as "nothing to
     * report yet". */
    std::atomic<long long> bytesNow{0};
    std::atomic<long long> bytesTotal{0};
    bool                   running   = false;
    uint8_t               *buf       = nullptr;
    size_t                 len       = 0;
    int                    status    = 0;
    int                    attempts  = 0;
    Uint64                 retryAtMs = 0;
} LobbyReelWbnFetch;

/* Cleared a field at a time by lobbyReelWbnAbort and never as a whole: the
 * worker handle and the two counters are not assignable, and the thread has to
 * be cancelled and joined before any of the rest may be touched. */
static LobbyReelWbnFetch s_reelWbn;

/* Stop whatever is in flight and forget the round it belonged to. The curl
 * worker is joined and never detached: it writes into the fields above, and a
 * lobby that has gone away leaves nothing for it to write into. Cancelling
 * first is what keeps the join short, since curl polls the flag as bytes
 * arrive. The fetch transport aborts its request and drops the slot its reply
 * would have landed in, which is the same thing without the join. */
static void lobbyReelWbnAbort(void) {
#if BOLO_REEL_WBN_FETCH_CURL
    s_reelWbn.cancel = 1;
    if (s_reelWbn.thread.joinable()) s_reelWbn.thread.join();
#else
    wbRoundLogFetchCancel();
#endif
    free(s_reelWbn.buf);
    s_reelWbn.buf       = nullptr;
    s_reelWbn.len       = 0;
    s_reelWbn.status    = 0;
    s_reelWbn.running   = false;
    s_reelWbn.attempts  = 0;
    s_reelWbn.retryAtMs = 0;
    s_reelWbn.key[0]    = '\0';
    s_reelWbn.bytesNow.store(0, std::memory_order_relaxed);
    s_reelWbn.bytesTotal.store(0, std::memory_order_relaxed);
#if BOLO_REEL_WBN_FETCH_CURL
    s_reelWbn.done.store(false, std::memory_order_relaxed);
    s_reelWbn.cancel = 0;
#endif
}

/* Spend one rung of the ladder, if one is due. Everything that would make a
 * fetch pointless is a guard rather than a condition at the call site, so the
 * caller can ask every frame. */
static void lobbyReelWbnKick(void) {
    if (s_reelWbn.running || s_reelWbn.buf) return;
    if (s_reelWbn.key[0] == '\0') return;
    /* The key is pasted into the request path below, so it never goes out
     * unless it is the 32-hex shape WBN issues. The codec already drops a
     * malformed one off the wire; this is the backstop on the path itself,
     * the same one wbn_comments_fetch_start applies to its own. */
    if (!winbolonetKeyIsValid(s_reelWbn.key)) return;
    /* The load below gets one attempt per summary; once it has spent it there
     * is nothing left to play another copy of the same round. */
    if (s_reel.tried) return;
    if (s_reelWbn.attempts >= REEL_WBN_RETRY_MAX) return;
    if (SDL_GetTicks() < s_reelWbn.retryAtMs) return;
#if BOLO_REEL_WBN_FETCH_CURL
    /* A worker that finished without the poll below seeing it still owns a
     * thread handle; std::thread destructs hard on a joinable one. */
    if (s_reelWbn.thread.joinable()) s_reelWbn.thread.join();

    /* The one WinBolo.net entry point that does not bring HTTP up on its own:
     * it fails outright when nothing has called httpCreate, where the GET and
     * POST paths create lazily. Reentrant, so the browser's own create/destroy
     * pair is unaffected. Once a round is enough. */
    if (s_reelWbn.attempts == 0) httpCreate();
#endif

    char keyCopy[ROUND_STATS_LOGKEY_LEN];
    SDL_strlcpy(keyCopy, s_reelWbn.key, sizeof(keyCopy));

#if BOLO_REEL_WBN_FETCH_CURL
    /* Curl's byte sink, running on the worker. Non-capturing so it converts to
     * the C function pointer, and it keeps the last total it was given, since
     * curl reports zero until it has read a Content-Length. */
    WbnProgressFn progressFn = [](void *user, int64_t now, int64_t total) {
        (void)user;
        s_reelWbn.bytesNow.store((long long)now, std::memory_order_relaxed);
        if (total > 0) {
            s_reelWbn.bytesTotal.store((long long)total, std::memory_order_relaxed);
        }
    };
#endif

    s_reelWbn.attempts++;
    s_reelWbn.running = true;
    s_reelWbn.status  = 0;
#if BOLO_REEL_WBN_FETCH_CURL
    s_reelWbn.done.store(false, std::memory_order_relaxed);
#endif
    s_reelWbn.bytesNow.store(0, std::memory_order_relaxed);
    s_reelWbn.bytesTotal.store(0, std::memory_order_relaxed);
    WB_LOG_INFO(WB_LOG_CAT_GUI,
                "[REEL] winbolo.net round log attempt %d/%d, key prefix '%.6s'",
                s_reelWbn.attempts, REEL_WBN_RETRY_MAX, keyCopy);

#if !BOLO_REEL_WBN_FETCH_CURL
    /* Returns at once; the reply lands in the page and the poll below takes it
     * on a later frame. The ceiling is applied there, where the bytes are. */
    wbRoundLogFetchStart(keyCopy);
#else
    s_reelWbn.thread = std::thread([keyCopy, progressFn]() {
        char path[128];
        SDL_snprintf(path, sizeof(path), "logs/%s/download", keyCopy);
        uint8_t *data = nullptr;
        size_t   size = 0;
        /* Held to the same ceiling the server-served path enforces on its own
         * transfer. Without it this is the one way into the reel that a round
         * log of any size at all can come through, and the recap opens and
         * fetches on its own between rounds. */
        int status = wbn_api_download_to_memory_progress(path, &data, &size,
                                                         ROUND_LOG_MAX_BYTES,
                                                         progressFn, nullptr,
                                                         &s_reelWbn.cancel);
        if (status != 200 || size == 0) {
            free(data);
            data = nullptr;
            size = 0;
        }
        s_reelWbn.buf    = data;
        s_reelWbn.len    = size;
        s_reelWbn.status = status;
        /* Last, and releasing: the three writes above are published by it. */
        s_reelWbn.done.store(true, std::memory_order_release);
    });
#endif
}

/* Collect a finished attempt. A 200 leaves its bytes standing for the load
 * below to take; anything else arms the next rung. */
static void lobbyReelWbnPoll(void) {
#if BOLO_REEL_WBN_FETCH_CURL
    if (!s_reelWbn.done.load(std::memory_order_acquire)) return;
    if (s_reelWbn.thread.joinable()) s_reelWbn.thread.join();
    s_reelWbn.done.store(false, std::memory_order_relaxed);
#else
    /* Still in flight reads as 0; any other answer settles the attempt, and
     * brings the bytes with it when there are any to bring. */
    uint8_t *fetched = nullptr;
    int      fetchedLen = 0;
    int      fetchedStatus = wbRoundLogFetchPoll(&fetched, &fetchedLen);
    if (fetchedStatus == 0) return;
    s_reelWbn.buf    = fetched;
    s_reelWbn.len    = (fetched != nullptr) ? (size_t)fetchedLen : 0;
    s_reelWbn.status = fetchedStatus;
#endif
    s_reelWbn.running = false;

    WB_LOG_INFO(WB_LOG_CAT_GUI,
                "[REEL] winbolo.net round log attempt %d done: status %d, "
                "%zu bytes",
                s_reelWbn.attempts, s_reelWbn.status, s_reelWbn.len);
    if (s_reelWbn.buf) return;

    s_reelWbn.len       = 0;
    s_reelWbn.retryAtMs = SDL_GetTicks() + REEL_WBN_RETRY_MS;
    if (s_reelWbn.attempts >= REEL_WBN_RETRY_MAX) {
        WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[REEL] winbolo.net round log gave up after %d attempts",
                    s_reelWbn.attempts);
    }
}
#endif /* BOLO_REEL_WBN_FETCH */

#if BOLO_RECAP_CLIP_GIF
/* Defined with the clip export below, which needs the reel's own state. */
void lobbyClipGifReset(void);
#endif

void lobbyReelEnd(void) {
#if BOLO_RECAP_CLIP_GIF
    /* An export in flight is holding encoder allocations and a playback
     * position to put back, and the reel it was reading is about to go. What
     * the capture held goes back to its declared values with it. */
    lobbyClipGifReset();
#endif
#if BOLO_REEL_WBN_FETCH
    /* And a WinBolo.net fetch is a thread writing into state this is about to
     * zero, so it is cancelled and joined here too. */
    lobbyReelWbnAbort();
#endif
    /* The viewer owns a process-wide decoder singleton, so it is torn down
     * before the state that says a reel is up is overwritten. */
    if (s_reel.active) {
        lvEmbedEnd();
    }
    /* Everything else goes back to its declared value in one write. The next
     * summary asks for its own round's log and reports nothing about a
     * transfer until it has one, and a crop frame belongs to the clip it was
     * drawn for rather than to the session. */
    s_reel = LobbyReelState{};
}

/* Trace the transfer as it moves, so winbolo.log tells a slow download apart
 * from a stalled one and both apart from a refusal — the recap draws nothing
 * about it yet. Ten-percent steps while bytes arrive keep a 4 MB transfer to a
 * handful of lines. */
static void lobbyReelLogTransfer(int state, uint8_t percent) {
    const int step = (state == CLIENT_ROUND_LOG_DOWNLOADING) ? percent / 10 : -1;
    if (state == s_reel.logStateSeen && step == s_reel.logStepSeen) return;
    s_reel.logStateSeen = state;
    s_reel.logStepSeen  = step;
    switch (state) {
        case CLIENT_ROUND_LOG_WAITING:
            WB_LOG_INFO(WB_LOG_CAT_GUI,
                        "[REEL] round log requested, no bytes yet");
            break;
        case CLIENT_ROUND_LOG_DOWNLOADING:
            WB_LOG_INFO(WB_LOG_CAT_GUI,
                        "[REEL] round log downloading %u%%", (unsigned)percent);
            break;
        case CLIENT_ROUND_LOG_READY:
            WB_LOG_INFO(WB_LOG_CAT_GUI, "[REEL] round log ready to play");
            break;
        case CLIENT_ROUND_LOG_UNAVAILABLE_DISABLED:
            WB_LOG_INFO(WB_LOG_CAT_GUI,
                        "[REEL] round log unavailable: server does not serve logs");
            break;
        case CLIENT_ROUND_LOG_UNAVAILABLE_NONE:
            WB_LOG_INFO(WB_LOG_CAT_GUI,
                        "[REEL] round log unavailable: server has no completed round");
            break;
        case CLIENT_ROUND_LOG_UNAVAILABLE_TOO_LARGE:
            WB_LOG_INFO(WB_LOG_CAT_GUI,
                        "[REEL] round log unavailable: over the transfer cap");
            break;
        default:
            /* Idle: nothing asked for, or the viewer has taken the blob. */
            break;
    }
}

/* Give back a downloaded round that nothing is going to play. A blob belongs
 * to the round its recap described, and the transport holds a completed one
 * until it is taken — so once the summary is gone (the countdown clears it)
 * the bytes are stale and the next round must fetch its own. Kept apart from
 * lobbyReelEnd, which also runs at the end of a lobby session with no
 * ClientSim in reach. */
void lobbyReelDropRoundLog(ClientSim *cs) {
    if (clientSimGetRoundLogState(cs) != CLIENT_ROUND_LOG_READY) return;
    size_t len = 0;
    uint8_t *buf = clientSimTakeRoundLog(cs, &len);
    free(buf);
    WB_LOG_INFO(WB_LOG_CAT_GUI,
                "[REEL] dropped round log (%zu bytes), its recap is gone", len);
}

/* Say where the round log has got to, centred in the space the reel would have
 * filled, so a recap with nothing to play yet reads as waiting or refused
 * rather than as a blank rectangle.
 *
 * Waiting and each of the three refusals get an icon and a line of their own —
 * the refusals are worded apart because they tell a player different things:
 * one is the host's choice, one is a round the server never finished, one is a
 * round too long to send. A transfer in flight gets a ring struck from the
 * percent the transport reports, which is a real fraction of the bytes and not
 * an animation, so a stalled download looks stalled.
 *
 * Reports only: the state is whatever the caller read off the transport this
 * frame, and nothing here times anything out, retries, or moves the transfer
 * along. */
static void lobbyRenderReelStatus(int state, uint8_t percent, ImVec2 rect,
                                  float s) {
    /* The recap can be the first thing on screen in the controller layout's
     * Last round tab, where the player list that usually brings the status
     * icons up is not drawn. The load is idempotent, so asking again is free. */
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (renderer) lobbyLoadStatusIconsOnce(renderer, s);

    const char  *msg = nullptr;
    SDL_Texture *ico = nullptr;
    char msgBuf[128];

    switch (state) {
        case CLIENT_ROUND_LOG_WAITING:
            ico = lobbyIcons()->info;
            msg = langGetText(STR_DLGLOBBY_REEL_WAITING);
            break;
        case CLIENT_ROUND_LOG_DOWNLOADING: {
            /* Copied out rather than held: langGetTextFmt returns a pointer
             * into a short ring of buffers that later calls reuse. */
            MessageArgs args = {};
            args.number = (int)percent;
            SDL_snprintf(msgBuf, sizeof(msgBuf), "%s",
                         langGetTextFmt(STR_DLGLOBBY_REEL_DOWNLOADING, &args));
            msg = msgBuf;
            break;
        }
        case CLIENT_ROUND_LOG_UNAVAILABLE_DISABLED:
            ico = lobbyIcons()->error;
            msg = langGetText(STR_DLGLOBBY_REEL_DISABLED);
            break;
        case CLIENT_ROUND_LOG_UNAVAILABLE_NONE:
            ico = lobbyIcons()->error;
            msg = langGetText(STR_DLGLOBBY_REEL_NONE);
            break;
        case CLIENT_ROUND_LOG_UNAVAILABLE_TOO_LARGE:
            ico = lobbyIcons()->error;
            msg = langGetText(STR_DLGLOBBY_REEL_TOO_LARGE);
            break;
        default:
            /* Idle: nothing was asked for on this connection, or the viewer has
             * already taken the bytes and a load is on its way. Neither is
             * something to tell the player about, and drawing nothing at all
             * leaves the recap the layout it had before there was a transfer to
             * describe. */
            return;
    }

    const ImGuiStyle &style = ImGui::GetStyle();
    const bool   ring     = (state == CLIENT_ROUND_LOG_DOWNLOADING);
    const float  ringR    = 22.0f * s;
    const float  iconSize = 18.0f * s;
    const float  gapX     = style.ItemSpacing.x;
    const float  gapY     = style.ItemSpacing.y;
    const ImVec2 textSz   = ImGui::CalcTextSize(msg);
    /* Icon and message share one row; the ring, when there is one, sits above
     * it and the pair is centred in the area as a block. */
    const float rowH   = (ico && iconSize > textSz.y) ? iconSize : textSz.y;
    const float rowW   = textSz.x + (ico ? iconSize + gapX : 0.0f);
    const float blockH = rowH + (ring ? ringR * 2.0f + gapY : 0.0f);

    /* A round still on its way holds the reel's whole rect, so the scoreboard
     * does not shuffle down the moment the reel appears in that same space. A
     * refusal never becomes a reel, so it keeps only the band its own line
     * needs and leaves the rest of the panel to the rows below. */
    const bool holdRect = (state == CLIENT_ROUND_LOG_WAITING || ring);
    ImVec2     area(rect.x, holdRect ? rect.y : blockH + gapY * 2.0f);
    if (area.y > rect.y) area.y = rect.y;

    ImDrawList  *dl = ImGui::GetWindowDrawList();
    const ImVec2 pmin = ImGui::GetCursorScreenPos();
    const ImVec2 pmax(pmin.x + area.x, pmin.y + area.y);

    /* Nothing may leave that area — a long translation is cut off rather than
     * written across the rows underneath. */
    dl->PushClipRect(pmin, pmax, true);
    dl->AddRectFilled(pmin, pmax, ImGui::GetColorU32(ImGuiCol_FrameBg),
                      style.FrameRounding);

    const float cx  = pmin.x + area.x * 0.5f;
    const float top = pmin.y + (area.y - blockH) * 0.5f;

    if (ring) {
        /* Struck clockwise from 12 o'clock on a full-circle track, the shape
         * the vote ring already uses. */
        const ImVec2 centre(cx, top + ringR);
        dl->AddCircle(centre, ringR, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                      36, 3.0f * s);
        const float frac = ((percent > 100) ? 100.0f : (float)percent) / 100.0f;
        if (frac > 0.0f) {
            const float a0 = -IM_PI * 0.5f;
            dl->PathArcTo(centre, ringR, a0, a0 + frac * IM_PI * 2.0f, 36);
            dl->PathStroke(ImGui::GetColorU32(ImGuiCol_PlotHistogram),
                           3.5f * s);
        }
    }

    const float rowY = top + blockH - rowH;
    float       x    = cx - rowW * 0.5f;
    if (ico) {
        const ImVec2 iconMin(x, rowY + (rowH - iconSize) * 0.5f);
        dl->AddImage((ImTextureID)ico, iconMin,
                     ImVec2(iconMin.x + iconSize, iconMin.y + iconSize));
        x += iconSize + gapX;
    }
    dl->AddText(ImVec2(x, rowY + (rowH - textSz.y) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_Text), msg);
    dl->PopClipRect();

    /* Spend what was drawn into, so the rows below start under it and the
     * height the recap feeds back has somewhere to land instead of climbing. */
    ImGui::Dummy(area);
}

/* Reel height: a share of whatever vertical room the container has left, plus
 * the room the rest of the recap turned out not to need, floored so it stays
 * watchable in a short controller tab and capped as a share of the container
 * so the transport row underneath is never pushed off. The share is the term
 * that decides the size in practice: the slack only ever hands over room the
 * rest of the recap genuinely left, which in a panel the scoreboard already
 * overflows is none. The floor is in unscaled pixels; the cap is a fraction,
 * because a tall panel is exactly the case the slack exists to fill. Taken
 * from the content region rather than the window so the same numbers serve
 * both containers. */
static const float REEL_HEIGHT_FRAC     = 0.60f;
static const float REEL_HEIGHT_MIN      = 180.0f;
static const float REEL_HEIGHT_MAX_FRAC = 0.85f;

/* Defined down with the chat input's state, which is declared after this. */
void lobbyChatInputAppendTime(uint32_t curMs);

#if BOLO_RECAP_CLIP_GIF
/* Defined with the clip export below, which needs the reel's own state. The
 * transport bar carries the same control the clip rows do, so both are reached
 * from here. */
bool lobbyClipGifButton(const char *id, bool compact);
void lobbyClipGifStartFromPlayhead(uint32_t curMs, const char *mapName);
/* Whether an export is running. The crop frame reads it to hold still: the
 * rect is fixed once at msf_gif_begin and every frame of the GIF is that size,
 * so letting it be dragged mid-capture would show a box the recording is not
 * following. */
bool lobbyClipGifActive(void);

/* The crop control, sitting with the export it crops. Names the frame rather
 * than describing it — the same rule the GIF control's caption follows, and
 * the reason both ship a word rather than a sentence. */
static const char *const REEL_CROP_TITLE = "Crop";

/* Toggle button carrying a drawn square-frame glyph. Drawn with the draw list
 * rather than loaded, so the control needs no new art asset and follows the
 * text colour and UI scale the way the icons beside it do. Stays pressed while
 * the frame is up, which is how the file's other state buttons read. */
static bool lobbyReelCropButton(const char *id) {
    const ImGuiStyle &sty = ImGui::GetStyle();
    const float lineH = ImGui::GetTextLineHeight();
    const bool  on    = s_reel.cropOn;

    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::Button(id, ImVec2(lineH + sty.FramePadding.x * 2.0f,
                                            lineH + sty.FramePadding.y * 2.0f));
    if (on) {
        ImGui::PopStyleColor();
    }

    /* The glyph: a square outline inset in the button, with the corners drawn
     * heavier so it reads as a crop frame rather than an empty box. */
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    ImVec2 a(p.x + sty.FramePadding.x + 1.0f, p.y + sty.FramePadding.y + 1.0f);
    ImVec2 b(a.x + lineH - 2.0f, a.y + lineH - 2.0f);
    dl->AddRect(a, b, col, 0.0f, 0, 1.0f);
    float tick = (b.x - a.x) * 0.32f;
    dl->AddLine(ImVec2(a.x, a.y), ImVec2(a.x + tick, a.y), col, 2.0f);
    dl->AddLine(ImVec2(a.x, a.y), ImVec2(a.x, a.y + tick), col, 2.0f);
    dl->AddLine(ImVec2(b.x, b.y), ImVec2(b.x - tick, b.y), col, 2.0f);
    dl->AddLine(ImVec2(b.x, b.y), ImVec2(b.x, b.y - tick), col, 2.0f);

    imguiHelpTooltip(REEL_CROP_TITLE);
    return clicked;
}

/* Draw the crop frame over the reel and let it be moved and resized. Returns
 * true when the frame owns this frame's mouse, so the caller sits its pan out.
 *
 * Precedence: while the frame is up, its interior and its handles win over the
 * reel's drag-pan. The handles are submitted after the reel's own overlay
 * button, which is marked allow-overlap, so ImGui's hit test hands them the
 * hover; the caller additionally gates the pan on the return value so the press
 * frame cannot slip through. The wheel is left alone entirely — zooming still
 * works with the cursor anywhere over the reel, frame included.
 *
 * Everything is computed in displayed pixels and stored back normalized, and
 * every edge is clamped inside the image, so the frame can neither leave the
 * view nor invert. */
static bool lobbyReelCropOverlay(ImVec2 imgMin, ImVec2 imgSize,
                                 bool panHoldsMouse) {
    if (!s_reel.cropOn || imgSize.x < 1.0f || imgSize.y < 1.0f) {
        return false;
    }
    const bool locked = lobbyClipGifActive();

    float x0 = imgMin.x + s_reel.cropX0 * imgSize.x;
    float y0 = imgMin.y + s_reel.cropY0 * imgSize.y;
    float x1 = imgMin.x + s_reel.cropX1 * imgSize.x;
    float y1 = imgMin.y + s_reel.cropY1 * imgSize.y;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    /* Dim the frame while a capture is running: it is showing what the GIF is
     * taking, and it is not going to answer a drag. */
    ImU32 line = locked ? IM_COL32(255, 255, 255, 110) : IM_COL32(255, 255, 255, 230);
    ImU32 shade = IM_COL32(0, 0, 0, 90);
    /* Shade everything the export will drop, so the kept area reads at a
     * glance rather than having to be traced along the outline. */
    dl->AddRectFilled(imgMin, ImVec2(imgMin.x + imgSize.x, y0), shade);
    dl->AddRectFilled(ImVec2(imgMin.x, y1),
                      ImVec2(imgMin.x + imgSize.x, imgMin.y + imgSize.y), shade);
    dl->AddRectFilled(ImVec2(imgMin.x, y0), ImVec2(x0, y1), shade);
    dl->AddRectFilled(ImVec2(x1, y0),
                      ImVec2(imgMin.x + imgSize.x, y1), shade);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 160), 0.0f, 0, 3.0f);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), line, 0.0f, 0, 1.0f);

    if (locked) {
        return false;
    }

    /* Which part of the frame a press took hold of. Latched, so a drag that
     * wanders off the handle keeps resizing the edge it started on. */
    static int  s_grab = 0;   /* bit 1 left, 2 right, 4 top, 8 bottom, 16 move */
    static bool s_dragging = false;

    /* A pan already holding the mouse keeps it. Ownership is decided at the
     * press and not re-decided per frame, so dragging the reel across the
     * frame does not hand the drag over halfway and stall the pan — the same
     * mistake the map preview's start markers used to make. */
    if (panHoldsMouse && !s_dragging) {
        return false;
    }

    const float g = REEL_CROP_GRAB_PX;
    ImVec2 mp = ImGui::GetMousePos();
    int hot = 0;
    bool overFrame = (mp.x >= x0 - g && mp.x <= x1 + g &&
                      mp.y >= y0 - g && mp.y <= y1 + g);
    if (overFrame) {
        if (mp.x >= x0 - g && mp.x <= x0 + g) hot |= 1;
        if (mp.x >= x1 - g && mp.x <= x1 + g) hot |= 2;
        if (mp.y >= y0 - g && mp.y <= y0 + g) hot |= 4;
        if (mp.y >= y1 - g && mp.y <= y1 + g) hot |= 8;
        if (hot == 0 && mp.x > x0 && mp.x < x1 && mp.y > y0 && mp.y < y1) {
            hot = 16;
        }
    }

    /* One invisible item over the whole frame plus its grab margin, so ImGui
     * knows the press belongs here and the reel's pan button does not take it.
     * Submitted after the reel's overlay (which allows overlap), which is what
     * puts it in front for hit-testing. */
    bool takesMouse = false;
    if (hot != 0 || s_dragging) {
        ImGui::SetCursorScreenPos(ImVec2(x0 - g, y0 - g));
        ImGui::InvisibleButton("##ReelCropGrab",
                               ImVec2((x1 - x0) + g * 2.0f, (y1 - y0) + g * 2.0f));
        takesMouse = ImGui::IsItemHovered() || ImGui::IsItemActive() || s_dragging;
        if (ImGui::IsItemActivated()) {
            s_grab = hot;
            s_dragging = true;
        }
    }

    int shape = s_dragging ? s_grab : hot;
    if (shape != 0) {
        ImGuiMouseCursor cur = ImGuiMouseCursor_ResizeAll;
        switch (shape & 15) {
            case 1: case 2:            cur = ImGuiMouseCursor_ResizeEW; break;
            case 4: case 8:            cur = ImGuiMouseCursor_ResizeNS; break;
            case 1 | 4: case 2 | 8:    cur = ImGuiMouseCursor_ResizeNWSE; break;
            case 2 | 4: case 1 | 8:    cur = ImGuiMouseCursor_ResizeNESW; break;
            default:                   break;   /* move */
        }
        ImGui::SetMouseCursor(cur);
    }

    if (s_dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        const float minW = REEL_CROP_MIN_PX;
        if (s_grab & 16) {
            /* Move: both edges together, stopped by the image rather than
             * squashed against it. */
            float w = x1 - x0, h = y1 - y0;
            x0 += d.x; y0 += d.y;
            if (x0 < imgMin.x) x0 = imgMin.x;
            if (y0 < imgMin.y) y0 = imgMin.y;
            if (x0 + w > imgMin.x + imgSize.x) x0 = imgMin.x + imgSize.x - w;
            if (y0 + h > imgMin.y + imgSize.y) y0 = imgMin.y + imgSize.y - h;
            x1 = x0 + w; y1 = y0 + h;
        } else {
            if (s_grab & 1) x0 += d.x;
            if (s_grab & 2) x1 += d.x;
            if (s_grab & 4) y0 += d.y;
            if (s_grab & 8) y1 += d.y;
            if (x0 < imgMin.x) x0 = imgMin.x;
            if (y0 < imgMin.y) y0 = imgMin.y;
            if (x1 > imgMin.x + imgSize.x) x1 = imgMin.x + imgSize.x;
            if (y1 > imgMin.y + imgSize.y) y1 = imgMin.y + imgSize.y;
            /* No inverting: the dragged edge stops a minimum short of its
             * opposite instead of crossing it. */
            if (x1 - x0 < minW) {
                if (s_grab & 1) x0 = x1 - minW; else x1 = x0 + minW;
            }
            if (y1 - y0 < minW) {
                if (s_grab & 4) y0 = y1 - minW; else y1 = y0 + minW;
            }
            /* The min-size correction can push an edge back out of the image
             * on a very small view; clamp once more so it never does. */
            if (x0 < imgMin.x) { x0 = imgMin.x; if (x1 < x0 + minW) x1 = x0 + minW; }
            if (y0 < imgMin.y) { y0 = imgMin.y; if (y1 < y0 + minW) y1 = y0 + minW; }
        }
        s_reel.cropX0 = (x0 - imgMin.x) / imgSize.x;
        s_reel.cropY0 = (y0 - imgMin.y) / imgSize.y;
        s_reel.cropX1 = (x1 - imgMin.x) / imgSize.x;
        s_reel.cropY1 = (y1 - imgMin.y) / imgSize.y;
    } else if (s_dragging) {
        s_dragging = false;
        s_grab = 0;
    }
    return takesMouse;
}
#endif

/* One zoom step per notch of travel, from either gesture a trackpad offers.
 *
 * The wheel arrives as whatever SDL had from the device: a mouse notch is a
 * clean 1.0, but a macOS trackpad scrolls with precise deltas, which SDL passes
 * on as fractions and the ImGui backend forwards unscaled. Stepping on every
 * frame that carried anything at all therefore ran the whole 0.5x..4x ladder
 * in the first few frames of a two-finger flick and sat at whichever end it
 * reached, which reads as a reel that will not zoom rather than one that zooms
 * too eagerly. Banking the deltas and spending them a whole notch at a time
 * leaves a mouse feeling exactly as it did and gives the trackpad the same
 * distance-per-step.
 *
 * Pinch is the gesture that never arrived at all: macOS sends magnification as
 * its own NSEvent and SDL does not turn it into a wheel, so the reel — alone
 * among the map views — could not answer the one gesture a laptop user reaches
 * for first. macos_pinch.m has been collecting it all along for the map
 * preview, the map editor and the standalone log viewer; the reel now spends it
 * on the same 0.15 threshold, so a pinch travels the same in all four.
 *
 * Both banks are dropped whenever the reel goes a frame without the cursor:
 * the pinch monitor is process-wide and keeps filling wherever the cursor is
 * (and the reel is not even drawn while the panel's Map tab is up), so a
 * gesture aimed at something else must not arrive here as a jump the moment
 * the reel is hovered again. Continuity is judged on the ImGui frame counter
 * rather than a hovered/not flag, which covers the frames the reel is not
 * drawn at all as well as the ones where the cursor is simply elsewhere. */
#define REEL_WHEEL_STEP 1.0f  /* a mouse notch, the unit the wheel reports in */
#define REEL_PINCH_STEP 0.15f /* magnification per step; matches the map views */

static void lobbyReelZoomInput(bool hovered, ImVec2 imgMin) {
    if (!hovered) {
        return;
    }

    /* Anything banked before a gap belongs to a gesture that has ended. */
    const int frame = ImGui::GetFrameCount();
    const bool continuing = (s_reel.zoomFrame == frame - 1);
    s_reel.zoomFrame = frame;
    if (!continuing) {
        s_reel.wheelAccum = 0.0f;
        s_reel.pinchAccum = 0.0f;
    }

    const ImVec2 mp = ImGui::GetMousePos();
    const int localX = (int)(mp.x - imgMin.x);
    const int localY = (int)(mp.y - imgMin.y);

    float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f) {
        /* A reversal is a new gesture, not a continuation of the old one: drop
         * what the other direction had banked so turning around answers on the
         * next notch instead of paying the leftover back first. */
        if ((wheel > 0.0f) != (s_reel.wheelAccum > 0.0f)) {
            s_reel.wheelAccum = 0.0f;
        }
        s_reel.wheelAccum += wheel;
        while (s_reel.wheelAccum >= REEL_WHEEL_STEP) {
            lvEmbedWheel(localX, localY, 1.0f);
            s_reel.wheelAccum -= REEL_WHEEL_STEP;
        }
        while (s_reel.wheelAccum <= -REEL_WHEEL_STEP) {
            lvEmbedWheel(localX, localY, -1.0f);
            s_reel.wheelAccum += REEL_WHEEL_STEP;
        }
    }

    /* Consumed on every hovered frame, gap or not: the accumulator is shared
     * with the other views, so the backlog has to be taken off it either way —
     * what a gap changes is that it is thrown away rather than spent. */
    const float pinch = macOSPinchZoomConsume();
    if (pinch != 0.0f && continuing) {
        s_reel.pinchAccum += pinch;
        while (s_reel.pinchAccum >= REEL_PINCH_STEP) {
            lvEmbedWheel(localX, localY, 1.0f);
            s_reel.pinchAccum -= REEL_PINCH_STEP;
        }
        while (s_reel.pinchAccum <= -REEL_PINCH_STEP) {
            lvEmbedWheel(localX, localY, -1.0f);
            s_reel.pinchAccum += REEL_PINCH_STEP;
        }
    }
}

void lobbyRenderReel(ClientSim *cs, const RoundStatsSummary *st,
                            float s) {
    /* Source, in order: the file this process wrote, else the copy the server
     * sent us, else the copy WinBolo.net holds under the round's key.
     *
     * A triple gate on the file: only a round this process recorded, published
     * to a file that is actually there. gameFrontHasLocalServer() is the
     * load-bearing one — the accessor describes whatever round this process
     * last recorded, so a player who hosted, left and then joined someone
     * else's server would otherwise see a completely different game replayed
     * here. It does not cover a host that is local but is not the server that
     * recorded the round; what covers that is gameFrontShutdownServer clearing
     * the accessor as it tears each server down. */
    const char *replayPath = serverDedicatedLogLastRoundFile();
    SDL_PathInfo replayInfo;
    const bool haveLocalFile = gameFrontHasLocalServer() &&
                               replayPath[0] != '\0' &&
                               SDL_GetPathInfo(replayPath, &replayInfo);

    /* Sized before the source is settled, because a round still on its way from
     * the server draws its delivery state into this same rect. Nothing has been
     * drawn yet either way, so the room measured here is the room the reel gets
     * once there is one. */
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 rect(avail.x, avail.y * REEL_HEIGHT_FRAC + s_reel.recapSlack);
    if (rect.y < REEL_HEIGHT_MIN * s) rect.y = REEL_HEIGHT_MIN * s;
    /* Cap last, so a container too short for the floor is still not overrun. */
    if (rect.y > avail.y * REEL_HEIGHT_MAX_FRAC) {
        rect.y = avail.y * REEL_HEIGHT_MAX_FRAC;
    }
    if (rect.y < 1.0f) rect.y = 1.0f;
    if (rect.x < 1.0f) rect.x = 1.0f;

    /* A load that came to nothing is the end of it for this round: the bytes
     * were had and refused, so there is no source left to try. Said with the
     * arm that already means "no replay for this round", and said here because
     * the block below would otherwise re-read the transfer state and re-kick
     * the WinBolo.net fetch every frame, leaving the recap on "asking the
     * server" for the rest of the lobby. */
    if (s_reel.loadFailed) {
        lobbyRenderReelStatus(CLIENT_ROUND_LOG_UNAVAILABLE_NONE, 0, rect, s);
        return;
    }

    /* A client that joined recorded nothing, so it asks the server that ran
     * the round for the bytes and waits for them. The request goes out from
     * here and nowhere else, so a player who never opens the recap never costs
     * the server a transfer, and the latch closes on a send the transport
     * accepted rather than on the attempt — the recap can be drawing before
     * the return-to-lobby handshake has finished, and a request made then goes
     * nowhere. Until it is accepted the ask is retried each frame, which costs
     * one comparison against the join state. Asking comes before reading the
     * state, which is what stops a blob left over from an earlier round being
     * played as this one: the request supersedes whatever the transport is
     * still holding. Only until a reel is up: taking the blob returns the
     * state to idle, and past that point idle means the viewer has it, not
     * that there is nothing to play.
     *
     * Until the bytes are here the rect above carries the transfer's state
     * instead of a reel. A host or single-player session that recorded the
     * round never reaches this: it resolves to its own file above and goes
     * straight to playing it, asking for nothing and drawing no overlay. */
    if (!s_reel.active && !haveLocalFile) {
        if (!s_reel.logAsked && clientSimNetSendRoundLogRequest(cs)) {
            s_reel.logAsked = true;
        }
        s_reel.logState = clientSimGetRoundLogState(cs);
        s_reel.logPercent = (s_reel.logState == CLIENT_ROUND_LOG_DOWNLOADING)
                               ? clientSimGetRoundLogPercent(cs)
                               : 0;
        lobbyReelLogTransfer(s_reel.logState, s_reel.logPercent);

        /* A host registered with WinBolo.net hands the round there instead of
         * serving it itself, so its refusal is the cue to go and get the same
         * bytes over HTTP. Only the three terminal answers qualify: a transfer
         * still moving is left alone to finish. Without a key there is nowhere
         * to go, and the refusal stands as the thing the recap says. */
        const bool serverRefused =
            s_reel.logState == CLIENT_ROUND_LOG_UNAVAILABLE_DISABLED ||
            s_reel.logState == CLIENT_ROUND_LOG_UNAVAILABLE_NONE ||
            s_reel.logState == CLIENT_ROUND_LOG_UNAVAILABLE_TOO_LARGE;
        bool haveWbnBytes = false;
        if (serverRefused && st && st->wbnLogKey[0] != '\0') {
            /* A key that is not the one being fetched belongs to a later
             * round, and whatever the previous one gathered is stale. */
            if (strncmp(s_reelWbn.key, st->wbnLogKey, sizeof(s_reelWbn.key)) != 0) {
                lobbyReelWbnAbort();
                SDL_strlcpy(s_reelWbn.key, st->wbnLogKey, sizeof(s_reelWbn.key));
            }
            lobbyReelWbnKick();
            lobbyReelWbnPoll();
            haveWbnBytes = (s_reelWbn.buf != nullptr);
            if (!haveWbnBytes) {
                /* Said in the states the overlay already draws, so the fetch
                 * costs no state of its own and no string of its own: a
                 * transfer with a length behind it is a download with a real
                 * fraction of the bytes, a spent ladder is a round with no
                 * replay to be had, and anything else is still waiting. */
                const long long got =
                    s_reelWbn.bytesNow.load(std::memory_order_relaxed);
                const long long total =
                    s_reelWbn.bytesTotal.load(std::memory_order_relaxed);
                int     wbnState = CLIENT_ROUND_LOG_WAITING;
                uint8_t wbnPct   = 0;
                if (s_reelWbn.running && total > 0) {
                    long long pct = got * 100 / total;
                    if (pct < 0) pct = 0;
                    if (pct > 100) pct = 100;
                    wbnState = CLIENT_ROUND_LOG_DOWNLOADING;
                    wbnPct   = (uint8_t)pct;
                } else if (!s_reelWbn.running &&
                           s_reelWbn.attempts >= REEL_WBN_RETRY_MAX) {
                    wbnState = CLIENT_ROUND_LOG_UNAVAILABLE_NONE;
                }
                lobbyRenderReelStatus(wbnState, wbnPct, rect, s);
                return;
            }
        }

        if (!haveWbnBytes && s_reel.logState != CLIENT_ROUND_LOG_READY) {
            lobbyRenderReelStatus(s_reel.logState, s_reel.logPercent, rect, s);
            return;
        }
    }

    if (!s_reel.active && !s_reel.tried) {
        /* One attempt per summary either way — a failed load must not be
         * retried every frame. */
        s_reel.tried = true;
        s_reel.viewW = rect.x;
        s_reel.viewH = rect.y;
        if (haveLocalFile) {
            SDL_IOStream *io = SDL_IOFromFile(replayPath, "rb");
            if (io) {
                Sint64 len = SDL_GetIOSize(io);
                /* malloc, not SDL_malloc: the viewer releases the buffer with
                 * plain free(), and it owns it from the call on — including
                 * when the load fails. */
                uint8_t *buf = (len > 0) ? (uint8_t *)malloc((size_t)len) : NULL;
                if (buf) {
                    if (SDL_ReadIO(io, buf, (size_t)len) == (size_t)len) {
                        s_reel.active = lvEmbedBegin(sdl3DrawGetWindow(),
                                                    sdl3DrawGetRenderer(),
                                                    buf, (size_t)len,
                                                    (int)rect.x, (int)rect.y);
                    } else {
                        free(buf);
                    }
                }
                SDL_CloseIO(io);
            }
        } else {
            /* The downloaded blob is already the bytes the viewer wants, on
             * the same malloc terms as the read above — hand it straight over
             * rather than looking at it first, since the viewer owns it from
             * the call on and frees it even when it refuses the data.
             * WinBolo.net's copy goes first when there is one, since it is
             * only ever fetched after the server has already declined to send
             * one. Nulling the static as the pointer goes is what keeps the
             * ownership single: from here the viewer frees it, and nothing
             * else may. */
            size_t   len = 0;
            uint8_t *buf = nullptr;
            if (s_reelWbn.buf) {
                buf          = s_reelWbn.buf;
                len          = s_reelWbn.len;
                s_reelWbn.buf = nullptr;
                s_reelWbn.len = 0;
            } else {
                buf = clientSimTakeRoundLog(cs, &len);
            }
            if (buf) {
                s_reel.active = lvEmbedBegin(sdl3DrawGetWindow(),
                                            sdl3DrawGetRenderer(),
                                            buf, len,
                                            (int)rect.x, (int)rect.y);
            }
        }
        /* Tell a reel that came up who is watching it, so the round is drawn
         * from their side: their team's tanks green, the other side's red.
         * The lobby slot is where that lives — its name is the key the log
         * shares, and the team alliances the server applied at kickoff are in
         * the log itself, so the reel needs nothing else. A spectator has no
         * slot of their own (and myPlayerNum is a stale index for one), so
         * they are named as nobody and the reel draws as it always did. */
        const ClientLobbySlot *watcher =
            clientSimIsSpectator(cs)
                ? nullptr
                : clientSimGetLobbySlot(cs, clientSimGetMyPlayerNum(cs));
        lvEmbedSetSelfName(watcher ? watcher->playerName : "");

        /* No reel out of the attempt means there is no replay to be had for
         * this round, whichever way it fell short — the file would not open,
         * came up short, would not fit in memory, no bytes were handed over,
         * or the viewer refused the ones that were. They all read the same to
         * the player, and none of them get better by being tried again. */
        s_reel.loadFailed = !s_reel.active;
    }
    if (!s_reel.active) return;

    /* The frame-end hook pauses a reel the recap stopped drawing, which is
     * every frame the panel's Map tab is up. Drawing again undoes that pause
     * — but only when the pause was ours, so a deliberate one survives a
     * round trip through the other tab. */
    if (s_reel.autoPaused) {
        s_reel.autoPaused = false;
        lvEmbedPlay();
    }

    if (rect.x != s_reel.viewW || rect.y != s_reel.viewH) {
        lvEmbedSetViewportSize((int)rect.x, (int)rect.y);
        s_reel.viewW = rect.x;
        s_reel.viewH = rect.y;
    }

    void *tex = NULL;
    int texW = 0, texH = 0, srcX = 0, srcY = 0, srcW = 0, srcH = 0;
    ImVec2 imgMin = ImGui::GetCursorScreenPos();
    float  blockTopY = ImGui::GetCursorPosY();
    ImVec2 imgSize = rect;
    if (lvEmbedFrameTexture(&tex, &texW, &texH, &srcX, &srcY, &srcW, &srcH) &&
        tex && texW > 0 && texH > 0) {
        /* The tile grid is fitted to the rect by rounding to whole tiles, so
         * the slice it reports rarely lands on the rect exactly. Trim the
         * visible slice down to whole source pixels and take the drawn size
         * from that, so one source pixel is always exactly `zoom` host pixels
         * — the ratio the standalone viewer gets by blitting at that multiple
         * and letting the window edge clip. What the trim leaves over is under
         * one zoom step wide and stays as padding. */
        float zoom = lvEmbedGetZoomLevel();
        if (zoom <= 0.0f) zoom = 1.0f;
        int visW = (int)floorf(rect.x / zoom);
        int visH = (int)floorf(rect.y / zoom);
        if (visW < 1) visW = 1;
        if (visH < 1) visH = 1;
        if (visW > srcW) visW = srcW;
        if (visH > srcH) visH = srcH;
        imgSize.x = (float)visW * zoom;
        imgSize.y = (float)visH * zoom;
        /* The render target is a tile larger than the visible slice, so the
         * UVs pick whole source pixels out of it, starting at the sub-tile pan
         * offset. Whole pixels on both edges are what keeps the blit an exact
         * multiple instead of a resample. */
        ImVec2 uv0((float)srcX / (float)texW, (float)srcY / (float)texH);
        ImVec2 uv1((float)(srcX + visW) / (float)texW,
                   (float)(srcY + visH) / (float)texH);
        /* The reel is game pixel art magnified `zoom` times. Drawn through
         * ImGui the SDL_Renderer backend forces LINEAR on every texture it
         * binds, which is what made the replay look soft — the same defect the
         * lobby's inline map preview had. Point-sample it for the blit and put
         * LINEAR back for the surrounding UI. */
        imguiPushNearestSampling();
        ImGui::Image((ImTextureID)tex, imgSize, uv0, uv1);
        imguiPopNearestSampling();
#if BOLO_RECAP_CLIP_GIF
        /* Publish the exact slice this blit used. The crop frame is normalized
         * against it, so an export maps the frame back through the same
         * numbers the picture was drawn with. */
        s_reel.lastSlice.x = srcX;
        s_reel.lastSlice.y = srcY;
        s_reel.lastSlice.w = visW;
        s_reel.lastSlice.h = visH;
#endif
    } else {
        ImGui::Dummy(rect);
    }

    /* Input overlay on the image rect: the button takes the drag as an
     * active item, so a drag pans the reel instead of moving the window
     * under it. Mirrors the map preview popup. Sized to the image, not the
     * rect, so the coordinates handed back to the viewer are image-local. */
    ImGui::SetCursorScreenPos(imgMin);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##ReelView", imgSize);
    const bool reelHovered = ImGui::IsItemHovered();
    if (reelHovered) {
        /* Claim the wheel on every hovered frame, not just the ones that
         * carry a notch: the ownership set here is what ImGui reads at the
         * start of the next frame, and it is what stops the enclosing recap
         * window from scrolling under the reel as it zooms. */
        ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, ImGui::GetItemID());
    }
    lobbyReelZoomInput(reelHovered, imgMin);
    const bool reelPanActivated = ImGui::IsItemActivated();
    const bool reelPanActive =
        ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    ImVec2 reelPanDrag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);

    /* The crop frame goes on after the reel's own overlay so its handles win
     * the hit test, and it reports back whether it took the mouse — the pan
     * below sits out the frames it did. */
    bool cropTookMouse = false;
#if BOLO_RECAP_CLIP_GIF
    cropTookMouse = lobbyReelCropOverlay(imgMin, imgSize, reelPanActive);
#endif

    if (!cropTookMouse) {
        if (reelPanActivated) {
            lvEmbedPanBegin();
        }
        if (reelPanActive) {
            lvEmbedPanDelta(reelPanDrag.x, reelPanDrag.y);
        }
    }

    /* Claim the whole rect whatever the image came out at, so zooming does
     * not shuffle everything below the reel up and down. */
    ImGui::SetCursorPosY(blockTopY + rect.y);

    /* Transport toggle as a glyph: a written caption is the widest thing on
     * this row and its width moves as the label swaps and as the language
     * changes, which shoves everything after it. The icon is square and the
     * two states are the same size, so the row holds still. The written label
     * stays as the tooltip — it is already translated, and a bare glyph does
     * not say what it does for someone meeting it the first time. */
    const bool reelPlaying = lvEmbedIsPlaying();
    SDL_Texture *transportIcon = reelPlaying ? lobbyIcons()->pause : lobbyIcons()->play;
    const char  *transportText = langGetText(reelPlaying ? STR_LV_PAUSE
                                                         : STR_LV_PLAY_BTN);
    bool transportClicked;
    if (transportIcon) {
        transportClicked = ImGui::ImageButton(
            "##reelplay", (ImTextureID)transportIcon,
            ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()),
            ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0),
            ImGui::GetStyleColorVec4(ImGuiCol_Text));
        imguiHelpTooltip(transportText);
    } else {
        transportClicked = ImGui::Button(transportText);
    }
    if (transportClicked) {
        /* Whichever way it goes, the player has now said what they want —
         * drop any claim we had on the transport. */
        s_reel.autoPaused = false;
        if (reelPlaying) {
            lvEmbedPause();
        } else {
            lvEmbedPlay();
        }
    }

    uint32_t curMs = 0, totalMs = 0;
    lvEmbedGetProgress(&curMs, &totalMs);

    /* Drop where the reel is sitting into the chat box, so the moment can be
     * talked about. Punctuation rather than a lang string, like the zoom
     * buttons: the token it writes is the caption. */
    ImGui::SameLine();
    if (ImGui::Button("@")) {
        lobbyChatInputAppendTime(curMs);
    }

#if BOLO_RECAP_CLIP_GIF
    /* Export the next few seconds from wherever the reel is sitting. The view
     * is left as the player framed it — they have already chosen what they are
     * looking at, which is the whole point of exporting from here rather than
     * off a row. Full height, so it sits level with the two buttons before
     * it rather than shrinking the transport row. */
    ImGui::SameLine();
    if (lobbyClipGifButton("##reelgif", false)) {
        lobbyClipGifStartFromPlayhead(curMs, clientSimGetMapName(cs));
    }

    /* Crop frame, next to the export it crops: with it up, that export takes
     * the framed rectangle instead of the whole visible view. */
    ImGui::SameLine();
    if (lobbyReelCropButton("##reelcrop")) {
        s_reel.cropOn = !s_reel.cropOn;
    }
#endif

    /* Seek slider shares the transport row with Play/Pause and takes the rest
     * of the width. Times are the presented window's, which with the lobby
     * hidden is the round itself. */
    ImGui::SameLine();
    if (!s_reel.seeking) {
        s_reel.seekRatio = (totalMs > 0) ? ((float)curMs / (float)totalMs) : 0.0f;
        if (s_reel.seekRatio > 1.0f) s_reel.seekRatio = 1.0f;
        /* Idle: the applied value is wherever the reel actually is, so the
         * next drag measures its first step from the truth. */
        s_reel.seekApplied = s_reel.seekRatio;
    }
    unsigned curSecs = (unsigned)(curMs / 1000u);
    char seekLabel[48];
    if (totalMs > 0) {
        unsigned totalSecs = (unsigned)(totalMs / 1000u);
        snprintf(seekLabel, sizeof(seekLabel), "%02u:%02u / %02u:%02u",
                 curSecs / 60u, curSecs % 60u, totalSecs / 60u, totalSecs % 60u);
    } else {
        snprintf(seekLabel, sizeof(seekLabel), "%02u:%02u / --:--",
                 curSecs / 60u, curSecs % 60u);
    }
    ImGui::PushItemWidth(-1);
    /* NoRoundToFormat is essential: seekLabel is a pre-rendered string
     * ("01:12 / 04:30"), not a numeric printf format. Without the flag ImGui
     * rounds the dragged value by round-tripping it through that label, which
     * parses back to 0 and pins every seek to the start of the log. */
    if (ImGui::SliderFloat("##ReelSeek", &s_reel.seekRatio, 0.0f, 1.0f, seekLabel,
                           ImGuiSliderFlags_NoRoundToFormat)) {
        if (!s_reel.seeking) {
            /* Freeze playback for the scrub instead of letting every applied
             * seek pause and resume it — lvEmbedSeekRatio does that by
             * removing and re-adding the two SDL playback timers, which is
             * not something to do sixty times a second. */
            s_reel.seeking        = true;
            s_reel.seekWasPlaying = lvEmbedIsPlaying();
            if (s_reel.seekWasPlaying) lvEmbedPause();
            s_reel.seekAppliedMs  = 0;
        }
    }
    /* Live scrub: the tanks follow the handle as it is dragged, not only when
     * it is dropped.
     *
     * The two directions cost very different amounts. A seek restores the
     * newest snapshot at or before the target and re-decodes forward to it at
     * 20 ms of log per tick, and a round carries essentially one snapshot — at
     * its start — so a naive per-frame seek re-decoded the whole round every
     * frame, which is what made this release-only. Forward seeks no longer pay
     * that: lv_screenSeekToAbsoluteMs now continues the decode from where it
     * already stands, so dragging right costs only the ticks the handle
     * crossed since the last frame and can run every frame. Dragging left
     * still has to rewind through the snapshot, so it is throttled and only
     * the newest seek in a burst is paid for.
     *
     * The release below always applies the exact dropped value, so where it
     * lands is never a throttled approximation. */
    if (s_reel.seeking && ImGui::IsItemActive()) {
        Uint64 nowMs = SDL_GetTicks();
        bool   apply = false;
        if (s_reel.seekRatio > s_reel.seekApplied) {
            apply = true;                       /* forward: incremental */
        } else if (s_reel.seekRatio < s_reel.seekApplied) {
            apply = (nowMs - s_reel.seekAppliedMs >= RECAP_SCRUB_BACK_MS);
        }
        if (apply) {
            lvEmbedSeekRatio(s_reel.seekRatio);
            s_reel.seekApplied   = s_reel.seekRatio;
            s_reel.seekAppliedMs = nowMs;
        }
    }
    /* IsItemDeactivated, not ...AfterEdit: this also has to un-pause, and a
     * release that ImGui does not count as an edit would otherwise leave the
     * reel frozen for good. */
    if (s_reel.seeking && ImGui::IsItemDeactivated()) {
        s_reel.seeking = false;
        /* Land exactly on the dropped value. A no-op when the last live apply
         * already got there — a forward seek to the current time decodes
         * nothing. */
        lvEmbedSeekRatio(s_reel.seekRatio);
        s_reel.seekApplied = s_reel.seekRatio;
        if (s_reel.seekWasPlaying) lvEmbedPlay();
        s_reel.seekWasPlaying = false;
    }
    ImGui::PopItemWidth();

    s_reel.drawn = true;
}
#endif /* !BOLO_MOBILE */
