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
 * Name:          imgui_winbolonet.cpp
 * Purpose:       ImGui WinBolo.net login popup rendered
 *                inline within the settings dialog.
 *********************************************************/

#include <cstring>
#include <cmath>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_markdown.h"
#include "imgui_dialog_utils.h"
#include "dialog_footer.h"

extern "C" {
#include "../../gamefront.h"
#include "global.h"
#include "../../../winbolonet/winbolonet_client.h"
#include "../../lang.h"
#include "imgui_winbolonet.h"
#include "../../../steam/steam_wrapper.h"
#include "playername_validate.h"
#include "../sdl3draw.h"
#include "../../tiles.h"
}

/* White-masked Steam logo, tinted at draw time. Used on the
 * "Sign in with Steam" button. */
extern "C" SDL_Texture *sdl3ImguiGetSteamIcon(void);

/* Shared background-game handle; non-NULL when the welcome screen is
 * playing a demo behind the menus. The welcome menu buttons fade more
 * (lower alpha) over a live background, so the account chip/sign-in
 * button mirror that to match the "Single Player" button. */
extern "C" struct BgGame *bgGameGetShared(void);

/* ---- markdown (inline-link hint) ---- */

/* imgui_markdown link callback: open only http/https links in the browser. */
static void wbnMarkdownLinkCb(ImGui::MarkdownLinkCallbackData data) {
    if (data.isImage) return;
    char url[512];
    int len = data.linkLength;
    if (len > (int)sizeof(url) - 1) len = (int)sizeof(url) - 1;
    if (len < 0) len = 0;
    memcpy(url, data.link, (size_t)len);
    url[len] = '\0';
    if (SDL_strncmp(url, "http://", 7) == 0 || SDL_strncmp(url, "https://", 8) == 0) {
        imguiOpenUrl(url);
    }
}

/* Default link colour/underline plus a hand cursor on hover. */
static void wbnMarkdownFormatCb(const ImGui::MarkdownFormatInfo &info, bool start) {
    ImGui::defaultMarkdownFormatCallback(info, start);
    if (!start && info.type == ImGui::MarkdownFormatType::LINK && info.itemHovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
}

static const ImGui::MarkdownConfig &wbnMarkdownConfig(void) {
    static ImGui::MarkdownConfig cfg;
    static bool init = false;
    if (!init) {
        cfg.linkCallback   = wbnMarkdownLinkCb;
        cfg.formatCallback = wbnMarkdownFormatCb;
        init = true;
    }
    return cfg;
}

/* ---- async login state ---- */

enum WbnLoginState {
    WBN_IDLE,
    WBN_LOGGING_IN,
    WBN_VALIDATING,
    WBN_SUCCESS,
    WBN_ERROR
};

struct WbnLoginWork {
    /* inputs */
    char username[256];
    char password[256];
    char token[256];
    char steamTicketHex[2049];
    char regUsername[256];
    char regEmail[256];
    /* outputs */
    char tokenOut[256];
    char expiryOut[256];
    char playerNameOut[PLAYER_NAME_LEN];
    int rankOut;
    int rankTotalOut;
    WbnStats statsOut;
    char errorMsg[512];
    char errorCode[128];   /* register: server machine error code, if any */
    bool isValidate;
    bool isSteam;
    bool steamSilent;   /* isSteam: launch re-auth (true) vs visible button (false) */
    bool isRegister;
    bool success;
    SDL_AtomicInt done;
};

static WbnLoginState wbnState = WBN_IDLE;
static WbnLoginWork wbnWork;
static SDL_Thread *wbnThread = nullptr;
static char wbnErrorBuf[512];
static bool wbnPopupOpen = false;
static char wbnUsername[256];
static char wbnPassword[256];
static bool wbnFocusUser = false;
static bool wbnShowStats = false;
/* Steam inline-signup form state (login popup). wbnRegSeeded gates the
 * one-time persona seed of the username so re-edits aren't clobbered;
 * both buffers and the gate reset when the popup (re)opens. */
static char wbnRegUsername[256];
static char wbnRegEmail[256];
static bool wbnRegSeeded = false;

static int wbnLoginThreadFunc(void *data) {
    WbnLoginWork *w = (WbnLoginWork *)data;
    if (w->isRegister) {
        w->success = winbolonetAuthSteamRegister(w->steamTicketHex, w->regUsername, w->regEmail,
                                                 w->tokenOut, w->expiryOut, w->playerNameOut,
                                                 &w->rankOut, &w->rankTotalOut, &w->statsOut,
                                                 w->errorMsg, w->errorCode);
    } else if (w->isSteam) {
        w->success = winbolonetAuthSteam(w->steamTicketHex, w->tokenOut, w->expiryOut,
                                         w->playerNameOut, &w->rankOut, &w->rankTotalOut,
                                         &w->statsOut, w->errorMsg);
    } else if (w->isValidate) {
        w->success = winbolonetAuthValidate(w->token, w->playerNameOut, &w->rankOut, &w->rankTotalOut, &w->statsOut, w->errorMsg);
    } else {
        w->success = winbolonetAuthLogin(w->username, w->password, w->tokenOut, w->expiryOut, w->playerNameOut, &w->rankOut, &w->rankTotalOut, &w->statsOut, w->errorMsg);
    }
    SDL_SetAtomicInt(&w->done, 1);
    return 0;
}

static void wbnStartLogin(void) {
    memset(&wbnWork, 0, sizeof(wbnWork));
    SDL_SetAtomicInt(&wbnWork.done, 0);
    SDL_strlcpy(wbnWork.username, wbnUsername, sizeof(wbnWork.username));
    SDL_strlcpy(wbnWork.password, wbnPassword, sizeof(wbnWork.password));
    wbnWork.isValidate = false;
    wbnState = WBN_LOGGING_IN;
    wbnThread = SDL_CreateThread(wbnLoginThreadFunc, "WBNLogin", &wbnWork);
}

static void wbnStartValidate(const char *token) {
    memset(&wbnWork, 0, sizeof(wbnWork));
    SDL_SetAtomicInt(&wbnWork.done, 0);
    SDL_strlcpy(wbnWork.token, token, sizeof(wbnWork.token));
    wbnWork.isValidate = true;
    wbnState = WBN_VALIDATING;
    wbnThread = SDL_CreateThread(wbnLoginThreadFunc, "WBNValidate", &wbnWork);
}

static void wbnStartSteamReauth(const char *ticketHex) {
    memset(&wbnWork, 0, sizeof(wbnWork));
    SDL_SetAtomicInt(&wbnWork.done, 0);
    SDL_strlcpy(wbnWork.steamTicketHex, ticketHex, sizeof(wbnWork.steamTicketHex));
    wbnWork.isSteam = true;
    wbnWork.steamSilent = true;
    wbnState = WBN_VALIDATING;
    wbnThread = SDL_CreateThread(wbnLoginThreadFunc, "WBNSteam", &wbnWork);
}

/* Visible counterpart of wbnStartSteamReauth: signs in an existing
 * Steam-linked WinBolo.net account from the login popup, showing the
 * spinner and surfacing an error on failure (rather than the silent
 * launch re-auth). */
static void wbnStartSteamLogin(const char *ticketHex) {
    memset(&wbnWork, 0, sizeof(wbnWork));
    SDL_SetAtomicInt(&wbnWork.done, 0);
    SDL_strlcpy(wbnWork.steamTicketHex, ticketHex, sizeof(wbnWork.steamTicketHex));
    wbnWork.isSteam = true;       /* steamSilent stays false: visible login */
    wbnState = WBN_LOGGING_IN;
    wbnThread = SDL_CreateThread(wbnLoginThreadFunc, "WBNSteamLogin", &wbnWork);
}

/* Signup sibling of wbnStartSteamReauth: registers a new WinBolo.net
 * account from the Steam ticket plus the chosen username/email, then
 * signs the player in on success. Drives the visible login spinner. */
static void wbnStartSteamRegister(const char *ticketHex, const char *username, const char *email) {
    memset(&wbnWork, 0, sizeof(wbnWork));
    SDL_SetAtomicInt(&wbnWork.done, 0);
    SDL_strlcpy(wbnWork.steamTicketHex, ticketHex, sizeof(wbnWork.steamTicketHex));
    SDL_strlcpy(wbnWork.regUsername, username, sizeof(wbnWork.regUsername));
    SDL_strlcpy(wbnWork.regEmail, email, sizeof(wbnWork.regEmail));
    wbnWork.isRegister = true;
    wbnState = WBN_LOGGING_IN;
    wbnThread = SDL_CreateThread(wbnLoginThreadFunc, "WBNRegister", &wbnWork);
}

/* Basic sanity check on an email address: a single '@' with a non-empty
 * local part, a dotted domain with a non-empty TLD, and no whitespace. Not a
 * full RFC validator — just enough to catch obvious typos before the round
 * trip; the server is the authority on whether the address is acceptable. */
static bool wbnEmailLooksValid(const char *email) {
    const char *at = NULL;
    for (const char *p = email; *p; p++) {
        if (*p == ' ' || *p == '\t') return false;
        if (*p == '@') {
            if (at) return false; /* more than one '@' */
            at = p;
        }
    }
    if (!at || at == email) return false;       /* missing '@' or empty local part */
    const char *domain = at + 1;
    const char *dot = SDL_strrchr(domain, '.');
    if (!dot || dot == domain) return false;     /* no dot, or dot starts domain */
    if (dot[1] == '\0') return false;            /* nothing after the last dot */
    return true;
}

/* Maps a server-returned signup error code to a localized message, or NULL
 * when the code is empty/unrecognised so the caller can fall back to the
 * server's own message. Codes with the same user-facing text share one
 * string. */
static const char *wbnRegisterErrorText(const char *code) {
    if (!code || code[0] == '\0') return NULL;
    if (SDL_strcmp(code, "username_taken") == 0)
        return langGetText(STR_DLGWBN_ERR_USERNAME_TAKEN);
    if (SDL_strcmp(code, "email_taken") == 0)
        return langGetText(STR_DLGWBN_ERR_EMAIL_TAKEN);
    if (SDL_strcmp(code, "steam_already_linked") == 0)
        return langGetText(STR_DLGWBN_ERR_STEAM_LINKED);
    if (SDL_strcmp(code, "signup_rate_limited") == 0)
        return langGetText(STR_DLGWBN_ERR_RATE_LIMITED);
    if (SDL_strcmp(code, "username_unavailable") == 0 ||
        SDL_strcmp(code, "invalid_username") == 0)
        return langGetText(STR_DLGWBN_ERR_USERNAME_UNAVAILABLE);
    if (SDL_strcmp(code, "username_too_long") == 0)
        return langGetText(STR_DLGWBN_ERR_USERNAME_TOO_LONG);
    if (SDL_strcmp(code, "missing_username") == 0 ||
        SDL_strcmp(code, "username_required") == 0)
        return langGetText(STR_DLGWBN_ERR_USERNAME_REQUIRED);
    if (SDL_strcmp(code, "invalid_email") == 0)
        return langGetText(STR_DLGWBN_ERR_EMAIL_INVALID);
    /* Steam ticket verification problems (missing/invalid/expired ticket, no
     * Steam ID, or Steam unreachable) — all read to the player as "Steam
     * couldn't vouch for you, try again". */
    if (SDL_strcmp(code, "missing_ticket") == 0 ||
        SDL_strcmp(code, "invalid_ticket") == 0 ||
        SDL_strcmp(code, "ticket_validation_failed") == 0 ||
        SDL_strcmp(code, "steam_id_missing") == 0 ||
        SDL_strcmp(code, "steam_verification_failed") == 0)
        return langGetText(STR_DLGWBN_ERR_STEAM_TICKET);
    /* Server-side faults (misconfig / DB race / service not wired) — retry. */
    if (SDL_strcmp(code, "steam_not_configured") == 0 ||
        SDL_strcmp(code, "account_creation_failed") == 0 ||
        SDL_strcmp(code, "steam_signup_unavailable") == 0)
        return langGetText(STR_DLGWBN_ERR_GENERIC);
    return NULL;
}

/* Load-time re-auth decision: refresh a Steam-authenticated (or empty,
 * ticket-available) session via Steam; otherwise validate an existing
 * token; otherwise do nothing. A password session never re-auths via
 * Steam — it keeps validating. */
static void wbnStartSessionReauth(void) {
    /* Honour an explicit sign-out: never silently sign the player back in.
     * Only a manual sign-in (which clears the sticky flag) re-auths. */
    if (gameFrontGetWbnSignedOut()) {
        return;
    }
    char token[256], expiry[256], method[32];
    gameFrontGetWinbolonetToken(token, expiry);
    gameFrontGetWbnAuthMethod(method, sizeof(method));
    char ticketHex[2049];
    if ((token[0] == '\0' || SDL_strcmp(method, "steam") == 0) &&
        gameFrontGetSteamTicketHex(ticketHex, sizeof(ticketHex))) {
        wbnStartSteamReauth(ticketHex);
    } else if (token[0] != '\0') {
        wbnStartValidate(token);
    }
}

static void wbnCheckThread(void) {
    if (!wbnThread || !SDL_GetAtomicInt(&wbnWork.done)) return;

    SDL_WaitThread(wbnThread, nullptr);
    wbnThread = nullptr;

    if (wbnWork.success) {
        if (wbnWork.isRegister) {
            /* New account created and signed in: an explicit user action,
             * so clear any sticky sign-out before applying. */
            gameFrontSetWbnSignedOut(false);
            gameFrontApplySteamAuthResult(wbnWork.tokenOut, wbnWork.expiryOut,
                                          wbnWork.playerNameOut, wbnWork.rankOut,
                                          wbnWork.rankTotalOut, &wbnWork.statsOut);
            steam_cancel_auth_ticket();
            wbnState = WBN_SUCCESS;
        } else if (wbnWork.isSteam) {
            if (wbnWork.steamSilent && gameFrontGetWbnSignedOut()) {
                /* A silent launch re-auth started before the player signed
                 * out has landed. Honour the sign-out: discard it rather
                 * than re-applying the token. */
                steam_cancel_auth_ticket();
                wbnState = WBN_IDLE;
            } else {
                /* The visible "Sign in with Steam" button is an explicit
                 * sign-in: clear any sticky sign-out. A silent re-auth that
                 * reaches here (not signed out) just refreshes the session. */
                if (!wbnWork.steamSilent) {
                    gameFrontSetWbnSignedOut(false);
                }
                gameFrontApplySteamAuthResult(wbnWork.tokenOut, wbnWork.expiryOut,
                                              wbnWork.playerNameOut, wbnWork.rankOut,
                                              wbnWork.rankTotalOut, &wbnWork.statsOut);
                steam_cancel_auth_ticket();
                /* Silent launch re-auth stays quiet; the visible popup button
                 * closes via the success handler. */
                wbnState = wbnWork.steamSilent ? WBN_IDLE : WBN_SUCCESS;
            }
        } else {
            if (!wbnWork.isValidate) {
                /* Password sign-in: an explicit user action. */
                gameFrontSetWbnSignedOut(false);
                gameFrontSetWinbolonetToken(wbnWork.tokenOut, wbnWork.expiryOut);
                gameFrontSetWbnAuthMethod("password");
            }
            gameFrontSetWinbolonetRank(wbnWork.rankOut, wbnWork.rankTotalOut);
            gameFrontSetWinbolonetStats(&wbnWork.statsOut);
            if (wbnWork.playerNameOut[0] != '\0') {
                gameFrontSetPlayerName(wbnWork.playerNameOut);
            }
            /* Token is set (login) or confirmed (validate): pull the cloud prefs
             * once for this session. Runs after the player name is set so the
             * sync captures the account display_name to reassert over the synced
             * Player Name. Gated, so it fires at most once per sign-in. */
            gameFrontStartPrefsSync();
            wbnState = WBN_SUCCESS;
        }
    } else {
        if (wbnWork.isRegister) {
            steam_cancel_auth_ticket();
            /* Prefer a localized message for the known machine code; for any
             * unmapped code show the server's own (English) message; if even
             * that is absent, a generic retry. */
            const char *mapped = wbnRegisterErrorText(wbnWork.errorCode);
            if (!mapped) {
                mapped = (wbnWork.errorMsg[0] != '\0') ? wbnWork.errorMsg
                                                       : langGetText(STR_DLGWBN_ERR_GENERIC);
            }
            SDL_strlcpy(wbnErrorBuf, mapped, sizeof(wbnErrorBuf));
            wbnState = WBN_ERROR;
        } else if (wbnWork.isSteam) {
            steam_cancel_auth_ticket();
            if (wbnWork.steamSilent) {
                wbnState = WBN_IDLE;      /* keep any existing token; silent */
            } else {
                /* Visible button: a fresh, valid ticket failing here almost
                 * always means no WinBolo.net account is linked to this Steam
                 * (the server's per-code strings for auth/steam aren't part of
                 * the client contract, so we don't branch on them). Point the
                 * player at the create form below. */
                SDL_strlcpy(wbnErrorBuf, langGetText(STR_DLGWBN_ERR_STEAM_NOT_LINKED),
                            sizeof(wbnErrorBuf));
                wbnState = WBN_ERROR;
            }
        } else {
            SDL_strlcpy(wbnErrorBuf, wbnWork.errorMsg, sizeof(wbnErrorBuf));
            if (!wbnWork.isValidate) {
                wbnState = WBN_ERROR;
            } else {
                gameFrontClearWinbolonetToken();
                wbnState = WBN_IDLE;
            }
        }
    }
}

/* Spinner helper */
static void wbnDrawSpinner(const char *label) {
    float radius = ImGui::GetFontSize() * 0.6f;
    float phase = (float)SDL_GetTicks() * 0.002f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 centre(pos.x + radius, pos.y + radius);
    ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);

    /* A spinning WinBolo.net shield (the verified badge) stands in for a
     * generic spinner — narrower than tall, turning about its vertical axis. */
    imguiDrawSpinningShield(ImGui::GetWindowDrawList(), centre,
                            radius * 0.80f, radius, phase, col, true);

    ImGui::Dummy(ImVec2(radius * 2.0f, radius * 2.0f));
    ImGui::SameLine();
    ImGui::Text("%s", label);
}

/* Renders the 1v1 ladder line for a signed-in player: "#N of M" or
 * "Unranked" when the stored rank is < 0. Reads the rank gamefront
 * captured on the last auth/validate. */
static void wbnDrawRankLine(void) {
    int rank = -1, rankTotal = 0;
    gameFrontGetWinbolonetRank(&rank, &rankTotal);
    if (rank < 0) {
        ImGui::TextDisabled("%s", langGetText(STR_DLGWBN_UNRANKED));
    } else {
        MessageArgs args = {};
        args.number = rank;
        args.number2 = rankTotal;
        ImGui::TextDisabled("%s", langGetTextFmt(STR_DLGWBN_RANK, &args));
    }
}

/* Opens the shared sign-in popup, resetting its transient state. Both the
 * settings section and the welcome-screen status block call this. */
static void wbnOpenLoginPopup(void) {
    wbnPopupOpen = true;
    wbnFocusUser = true;
    wbnState = WBN_IDLE;
    wbnErrorBuf[0] = '\0';
    wbnUsername[0] = '\0';
    wbnPassword[0] = '\0';
    wbnRegUsername[0] = '\0';
    wbnRegEmail[0] = '\0';
    wbnRegSeeded = false;
    ImGui::OpenPopup(langGetText(STR_DLGWBN_SIGNIN_TITLE));
}

/* Clears the stored token and re-arms the once-per-session prefs-sync
 * gate so signing back in resyncs. Records a sticky sign-out so the
 * silent re-auth paths (and any auth worker still in flight) won't sign
 * the player back in until they explicitly sign in again. Shared
 * sign-out path. */
static void wbnSignOut(void) {
    gameFrontClearWinbolonetToken();
    gameFrontSetWbnSignedOut(true);
    gameFrontResetPrefsSyncSession();
}

/* Full-width button showing the Steam logo to the left of `label`. The
 * white-masked Steam icon is tinted to the button text colour. `id` must be
 * unique among Steam buttons on screen (the visible label is drawn separately,
 * so the button's own ID carries no text). Returns true when clicked. */
static bool wbnSteamButton(const char *id, const char *label) {
    SDL_Texture *icon = sdl3ImguiGetSteamIcon();
    ImVec2 cur = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float h = ImGui::GetFrameHeight();
    bool clicked = ImGui::Button(id, ImVec2(w, h));

    float lineH = ImGui::GetTextLineHeight();
    float iconSz = lineH;
    float gap = 6.0f;
    float textW = ImGui::CalcTextSize(label).x;
    float contentW = (icon ? iconSz + gap : 0.0f) + textW;
    float tx = cur.x + (w - contentW) * 0.5f;
    float ty = cur.y + (h - lineH) * 0.5f;
    ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (icon) {
        dl->AddImage((ImTextureID)icon, ImVec2(tx, ty),
                     ImVec2(tx + iconSz, ty + iconSz), ImVec2(0, 0), ImVec2(1, 1), col);
        tx += iconSz + gap;
    }
    dl->AddText(ImVec2(tx, ty), col, label);
    return clicked;
}

/* Submits the username/password form: validates non-empty, then starts the
 * login worker. Shared by the Sign in button and the Enter key. */
static void wbnSubmitPasswordLogin(void) {
    if (strlen(wbnUsername) == 0 || strlen(wbnPassword) == 0) {
        SDL_strlcpy(wbnErrorBuf, langGetText(STR_DLGWBN_NEEDCREDS), sizeof(wbnErrorBuf));
        wbnState = WBN_ERROR;
    } else {
        wbnStartLogin();
    }
}

/* Left column: sign in with Steam (one click) or username/password. */
static void wbnRenderSignInColumn(bool onSteam) {
    ImGui::SeparatorText(langGetText(STR_DLGWBN_SIGNIN_HEADER));
    ImGui::Spacing();

    if (onSteam) {
        if (wbnSteamButton("##wbnsigninsteam", langGetText(STR_DLGWBN_SIGNIN_STEAM_BTN))) {
            char ticketHex[2049];
            if (gameFrontGetSteamTicketHex(ticketHex, sizeof(ticketHex))) {
                wbnStartSteamLogin(ticketHex);
            } else {
                SDL_strlcpy(wbnErrorBuf, langGetText(STR_DLGWBN_ERR_GENERIC),
                            sizeof(wbnErrorBuf));
                wbnState = WBN_ERROR;
            }
        }
        imguiHandOnHover();
        ImGui::Spacing();
        ImGui::SeparatorText(langGetText(STR_DLGWBN_OR_PASSWORD));
        ImGui::Spacing();
    }

    ImGui::TextUnformatted(langGetText(STR_DLGWBN_USERNAME));
    ImGui::SetNextItemWidth(-1);
    if (wbnFocusUser) {
        ImGui::SetKeyboardFocusHere();
        wbnFocusUser = false;
    }
    ImGui::InputText("##wbnuser", wbnUsername, sizeof(wbnUsername));

    ImGui::TextUnformatted(langGetText(STR_DLGWBN_PASSWORD));
    ImGui::SetNextItemWidth(-1);
    bool enterPressed = ImGui::InputText("##wbnpass", wbnPassword, sizeof(wbnPassword),
                                         ImGuiInputTextFlags_Password |
                                         ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::Spacing();
    bool doLogin = ImGui::Button(langGetText(STR_DLGWBN_SIGNIN_OK), ImVec2(-1, 0));
    imguiHandOnHover();
    if (doLogin || enterPressed) {
        wbnSubmitPasswordLogin();
    }

    /* A password account can be linked to this Steam on the website. Rendered
     * as markdown so the embedded "winbolo.net" link wraps inline with the
     * dimmed sentence (ImGui's plain text can't host an inline link). */
    if (onSteam) {
        const char *hint = langGetText(STR_DLGWBN_LINK_HINT);
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::Markdown(hint, SDL_strlen(hint), wbnMarkdownConfig());
        ImGui::PopStyleColor();
    }
}

/* Right column: create an account via Steam (inline form) or in the browser. */
static void wbnRenderCreateColumn(bool onSteam, const char *persona) {
    ImGui::SeparatorText(langGetText(STR_DLGWBN_CREATE_STEAM));
    ImGui::Spacing();

    if (onSteam) {
        /* Seed the username from the Steam persona once per open so the
         * player can freely edit (or clear) it afterwards. */
        if (!wbnRegSeeded) {
            SDL_strlcpy(wbnRegUsername, persona, sizeof(wbnRegUsername));
            wbnRegSeeded = true;
        }

        ImGui::TextUnformatted(langGetText(STR_DLGWBN_USERNAME));
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##wbnreguser", wbnRegUsername, sizeof(wbnRegUsername));

        ImGui::TextUnformatted(langGetText(STR_DLGWBN_EMAIL));
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##wbnregemail",
                                 langGetText(STR_DLGWBN_EMAIL_OPTIONAL),
                                 wbnRegEmail, sizeof(wbnRegEmail));

        ImGui::Spacing();
        if (wbnSteamButton("##wbncreatesteam", langGetText(STR_DLGWBN_CREATE_BTN))) {
            char validated[PLAYER_NAME_LEN];
            char ticketHex[2049];
            if (!playerNameValidate(wbnRegUsername, validated, sizeof(validated), nullptr)) {
                SDL_strlcpy(wbnErrorBuf, langGetText(STR_DLGWBN_ERR_USERNAME_UNAVAILABLE),
                            sizeof(wbnErrorBuf));
                wbnState = WBN_ERROR;
            } else if (wbnRegEmail[0] != '\0' && !wbnEmailLooksValid(wbnRegEmail)) {
                SDL_strlcpy(wbnErrorBuf, langGetText(STR_DLGWBN_ERR_EMAIL_INVALID),
                            sizeof(wbnErrorBuf));
                wbnState = WBN_ERROR;
            } else if (gameFrontGetSteamTicketHex(ticketHex, sizeof(ticketHex))) {
                wbnStartSteamRegister(ticketHex, wbnRegUsername, wbnRegEmail);
            } else {
                SDL_strlcpy(wbnErrorBuf, langGetText(STR_DLGWBN_ERR_GENERIC),
                            sizeof(wbnErrorBuf));
                wbnState = WBN_ERROR;
            }
        }
        imguiHandOnHover();

        ImGui::Spacing();
        ImGui::SeparatorText(langGetText(STR_DLGWBN_OR));
        ImGui::Spacing();
    }

    /* Standalone (browser) signup — captcha + email verification live there.
     * Rendered as markdown so the link wraps within the column instead of
     * overflowing it (TextLinkOpenURL is single-line). */
    char browserMd[256];
    SDL_snprintf(browserMd, sizeof(browserMd), "[%s](https://www.winbolo.net/signup)",
                 langGetText(STR_DLGWBN_CREATE_BROWSER));
    ImGui::Markdown(browserMd, SDL_strlen(browserMd), wbnMarkdownConfig());
}

/* Renders the shared sign-in popup modal. Must be called every frame by
 * whichever surface (settings section or welcome status block) owns the
 * popup this frame; OpenPopup scopes it to the current ImGui window.
 *
 * Layout: a full-width intro, then two columns split by a vertical rule —
 * "Sign in" on the left, "Create an account" on the right — with the error
 * line and a Cancel button spanning the full width beneath. */
static void wbnRenderLoginPopup(void) {
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_Appearing);

    static float s_fadeWbnSignIn = 0.0f;
    if (ImGui::BeginPopupModal(langGetText(STR_DLGWBN_SIGNIN_TITLE), &wbnPopupOpen,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                            imguiPopupFadeAlpha(&s_fadeWbnSignIn));
        bool busy = (wbnState == WBN_LOGGING_IN);
        char persona[256];
        bool onSteam = steam_get_persona_name(persona, sizeof(persona));
        const float colW = 290.0f;

        /* Intro spanning both columns. */
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + colW * 2.0f);
        ImGui::TextWrapped("%s", langGetText(STR_DLGWBN_SIGNIN_BLURB));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (busy) {
            wbnDrawSpinner(langGetText(STR_DLGWBN_SIGNINGIN));
        } else {
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(12.0f, 4.0f));
            if (ImGui::BeginTable("##wbnsignincols", 2, ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("##signin", ImGuiTableColumnFlags_WidthFixed, colW);
                ImGui::TableSetupColumn("##create", ImGuiTableColumnFlags_WidthFixed, colW);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                wbnRenderSignInColumn(onSteam);
                ImGui::TableSetColumnIndex(1);
                wbnRenderCreateColumn(onSteam, persona);
                ImGui::EndTable();
            }
            ImGui::PopStyleVar();
        }

        /* Error line — spans the full width beneath the columns. */
        if (wbnState == WBN_ERROR && wbnErrorBuf[0] != '\0') {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextWrapped("%s", wbnErrorBuf);
            ImGui::PopStyleColor();
        }

        /* Success closes the popup. */
        if (wbnState == WBN_SUCCESS) {
            ImGui::CloseCurrentPopup();
            wbnPopupOpen = false;
            wbnState = WBN_IDLE;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* Cancel — centred beneath everything. Esc also closes. */
        const char *cancel = langGetText(STR_CANCEL);
        float bw = 120.0f;
        float avail = ImGui::GetContentRegionAvail().x;
        if (avail > bw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - bw) * 0.5f);
        if (ImGui::Button(cancel, ImVec2(bw, 0)) || WBUI::CancelKeyPressed()) {
            ImGui::CloseCurrentPopup();
            wbnPopupOpen = false;
        }
        imguiHandOnHover();

        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
}

/* Draws a 16x16 tile from the game atlas inline at text height, used to
 * mark the Bases/Pills/Tanks column headers with their map sprites. Tile
 * coords are in 1x units; the atlas is assembled at gSheetScale, but UVs
 * normalised against the 1x reference size (TILE_FILE_X/Y) stay correct
 * at any scale. */
static void wbnDrawTileIcon(int tileX, int tileY) {
    SDL_Texture *tex = sdl3DrawGetTilesTexture();
    if (!tex) return;
    float sz = ImGui::GetTextLineHeight();
    ImVec2 uv0((float)tileX / TILE_FILE_X, (float)tileY / TILE_FILE_Y);
    ImVec2 uv1((float)(tileX + TILE_SIZE_X) / TILE_FILE_X,
               (float)(tileY + TILE_SIZE_Y) / TILE_FILE_Y);
    ImGui::Image((ImTextureID)tex, ImVec2(sz, sz), uv0, uv1);
}

/* Builds a table header cell whose label is preceded by an inline map
 * sprite (icon to the left of the text). */
static void wbnDrawIconHeader(int tileX, int tileY, const char *label) {
    wbnDrawTileIcon(tileX, tileY);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TableHeader(label);
}

/* Formats a non-negative integer with thousands separators, e.g.
 * 12345 -> "12,345". The separator is a fixed comma rather than locale
 * aware: the cross-platform builds don't set a C locale, and MSVC has no
 * printf grouping flag, so a portable manual grouping is used. */
static void wbnFormatGrouped(int value, char *buf, size_t bufLen) {
    char raw[16];
    SDL_snprintf(raw, sizeof(raw), "%d", value);
    int len = (int)SDL_strlen(raw);
    size_t out = 0;
    for (int i = 0; i < len && out + 1 < bufLen; i++) {
        if (i > 0 && (len - i) % 3 == 0 && out + 2 < bufLen) {
            buf[out++] = ',';
        }
        buf[out++] = raw[i];
    }
    buf[out] = '\0';
}

/* Renders one numeric cell of the stats table, dashing out values that
 * were absent from the response (negative). */
static void wbnDrawStatCell(int value) {
    ImGui::TableNextColumn();
    if (value < 0) {
        ImGui::TextDisabled("-");
    } else {
        char buf[24];
        wbnFormatGrouped(value, buf, sizeof(buf));
        ImGui::TextUnformatted(buf);
    }
}

/* Renders one mode's row in the stats table. Open carries only the
 * counter fields, so its rank/rating/W-L cells dash out; ranked modes
 * print a ladder position (or a dash when unranked), their ELO rating
 * and a win/loss tally. */
static void wbnDrawStatsRow(const char *mode, const WbnModeStats *m) {
    ImGui::TableNextRow();

    ImGui::TableNextColumn();
    ImGui::TextUnformatted(mode);

    ImGui::TableNextColumn();
    if (m->rank < 0) {
        ImGui::TextDisabled("-");
    } else {
        MessageArgs args = {};
        args.number = m->rank;
        args.number2 = m->rankTotal;
        ImGui::TextUnformatted(langGetTextFmt(STR_DLGWBN_RANK, &args));
    }

    wbnDrawStatCell(m->score);
    wbnDrawStatCell(m->numGames);

    ImGui::TableNextColumn();
    if (m->wins < 0 && m->loses < 0) {
        ImGui::TextDisabled("-");
    } else {
        char w[24], l[24];
        wbnFormatGrouped(m->wins < 0 ? 0 : m->wins, w, sizeof(w));
        wbnFormatGrouped(m->loses < 0 ? 0 : m->loses, l, sizeof(l));
        ImGui::Text("%s/%s", w, l);
    }

    wbnDrawStatCell(m->numBases);
    wbnDrawStatCell(m->numPills);
    wbnDrawStatCell(m->numTanks);
}

/* Draws the signed-in account chip for the welcome screen: a translucent
 * framed button showing the WBN-verified shield + player name, with the
 * strict-ladder rank on a second line once the player has at least one
 * strict win or loss (a fresh account shows just the name on one line).
 * Returns true when clicked. The caller pushes the ghost (translucent)
 * button colours so the chip matches the welcome menu buttons; textAlpha
 * is the menu's dimmed text alpha, brightened to full white on hover the
 * same way. The chip is at least as wide as a menu button (180*s) so it
 * reads as the same family. */
static bool wbnDrawAccountChip(float s, float textAlpha) {
    char playerName[PLAYER_NAME_LEN];
    playerName[0] = '\0';
    gameFrontGetPlayerName(playerName);
    if (playerName[0] == '\0') {
        SDL_strlcpy(playerName, langGetText(STR_DLGWBN_SIGNED_IN), sizeof(playerName));
    }

    /* Strict-ladder rank ("#N of M"); only shown once the player has a
     * recorded strict result, so brand-new accounts don't display a
     * placeholder. */
    WbnStats st;
    gameFrontGetWinbolonetStats(&st);
    bool hasRank = st.valid && st.strict.rank >= 0 &&
                   (st.strict.wins > 0 || st.strict.loses > 0);
    char rankBuf[64];
    rankBuf[0] = '\0';
    if (hasRank) {
        MessageArgs args = {};
        args.number = st.strict.rank;
        args.number2 = st.strict.rankTotal;
        SDL_strlcpy(rankBuf, langGetTextFmt(STR_DLGWBN_RANK, &args), sizeof(rankBuf));
    }

    float lineH = ImGui::GetTextLineHeight();
    float iconSz = lineH;
    float padX = 10.0f * s, padY = 6.0f * s;
    float gapIcon = 6.0f * s;
    float lineGap = 2.0f * s;

    float nameW = ImGui::CalcTextSize(playerName).x;
    float line1W = iconSz + gapIcon + nameW;
    float rankW = hasRank ? ImGui::CalcTextSize(rankBuf).x : 0.0f;
    float contentW = (line1W > rankW ? line1W : rankW);

    float btnW = contentW + padX * 2.0f;
    if (btnW < 180.0f * s) btnW = 180.0f * s;   /* match the menu button width */
    float btnH = hasRank ? (lineH * 2.0f + lineGap + padY * 2.0f)
                         : (lineH + padY * 2.0f);
    ImVec2 btnSize(btnW, btnH);

    ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::Button("##wbnacct", btnSize);
    imguiHandOnHover();
    bool hov = ImGui::IsItemHovered();

    /* Dimmed text matching the menu buttons; full white on hover. */
    int nameA = hov ? 255 : (int)(textAlpha * 255.0f);
    int rankA = hov ? 220 : (int)(textAlpha * 0.75f * 255.0f);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    float tx = p0.x + padX;
    float ty = p0.y + padY;
    /* Filled shield, same geometry as the hollow sign-in CTA so signed-in
     * and signed-out read as the same badge (filled vs outline). Gold for
     * supporters, matching the in-game / players-panel badge tint. */
    {
        float halfH = iconSz * 0.5f;
        ImU32 shieldCol = gameFrontIsSupporter()
                              ? IM_COL32(255, 214, 51, nameA)   /* supporter gold */
                              : IM_COL32(255, 255, 255, nameA);
        imguiDrawSpinningShield(dl, ImVec2(tx + iconSz * 0.5f, ty + halfH),
                                halfH * 0.80f, halfH, 0.0f, shieldCol, true);
        tx += iconSz + gapIcon;
    }
    dl->AddText(ImVec2(tx, ty), IM_COL32(255, 255, 255, nameA), playerName);
    if (hasRank) {
        dl->AddText(ImVec2(p0.x + padX, ty + lineH + lineGap),
                    IM_COL32(210, 210, 210, rankA), rankBuf);
    }

    return clicked;
}

/* When the active session was authenticated via Steam, append a muted
 * "via Steam" suffix on the same line as the preceding status text. */
static void wbnDrawViaSteamSuffix(void) {
    char method[32];
    gameFrontGetWbnAuthMethod(method, sizeof(method));
    if (SDL_strcmp(method, "steam") == 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", langGetText(STR_DLGWBN_VIA_STEAM));
    }
}

/* ---- Public API ---- */

/* Renders the shared "My Stats" modal from gamefront's stored per-mode
 * stats. Driven by wbnShowStats, set by the My Stats buttons in both the
 * welcome status block and the settings section. Both surfaces call this
 * each frame; one OpenPopup scopes it to the active ImGui window. */
extern "C" void imguiWinbolonetDrawStatsDialog(void) {
    char title[128];
    SDL_snprintf(title, sizeof(title), "%s###wbnstats",
                 langGetText(STR_DLGWBN_STATS_TITLE));
    if (wbnShowStats) {
        ImGui::OpenPopup(title);
        wbnShowStats = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);
    /* Translucent panel matching the Local game finder. */
    ImGui::SetNextWindowBgAlpha(0.85f);

    static float s_fadeWbnStats = 0.0f;
    bool open = true;
    if (!ImGui::BeginPopupModal(title, &open,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoTitleBar)) {
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, imguiPopupFadeAlpha(&s_fadeWbnStats));

    /* Gold title drawn into the body, matching the Local game finder panel
     * (which uses NoTitleBar and renders its own title). */
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.75f, 0.3f, 1.0f));
    ImGui::SetWindowFontScale(1.3f);
    ImGui::TextUnformatted(langGetText(STR_DLGWBN_STATS_TITLE));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();

    /* Signed-in identity: shield badge + player name. */
    {
        char playerName[PLAYER_NAME_LEN];
        playerName[0] = '\0';
        gameFrontGetPlayerName(playerName);
        imguiShieldBadge(ImGui::GetTextLineHeight(),
                         IM_COL32(255, 255, 255, 255));
        ImGui::SameLine();
        if (playerName[0] != '\0') {
            ImGui::TextUnformatted(playerName);
            ImGui::SameLine();
        }
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s",
                           langGetText(STR_DLGWBN_SIGNED_IN));
        wbnDrawViaSteamSuffix();
    }
    ImGui::Spacing();

    WbnStats st;
    gameFrontGetWinbolonetStats(&st);
    if (!st.valid) {
        ImGui::TextWrapped("%s", langGetText(STR_DLGWBN_STATS_NONE));
    } else {
        ImGuiTableFlags flags = ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_BordersInnerV |
                                ImGuiTableFlags_BordersOuter |
                                ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTable("##wbnstats", 8, flags)) {
            ImGui::TableSetupColumn("");   /* game mode */
            ImGui::TableSetupColumn(langGetText(STR_DLGWBN_STATS_COL_RANK));
            ImGui::TableSetupColumn(langGetText(STR_DLGWBN_STATS_COL_RATING));
            ImGui::TableSetupColumn(langGetText(STR_DLGWBN_STATS_COL_GAMES));
            ImGui::TableSetupColumn(langGetText(STR_DLGWBN_STATS_COL_WL));
            ImGui::TableSetupColumn(langGetText(STR_DLGWBN_STATS_COL_BASES));
            ImGui::TableSetupColumn(langGetText(STR_DLGWBN_STATS_COL_PILLS));
            ImGui::TableSetupColumn(langGetText(STR_DLGWBN_STATS_COL_TANKS));

            /* Header row drawn by hand so the count columns can show the
             * matching map sprite beside their label. */
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            for (int c = 0; c <= 4; c++) {
                ImGui::TableSetColumnIndex(c);
                ImGui::TableHeader(ImGui::TableGetColumnName(c));
            }
            ImGui::TableSetColumnIndex(5);
            wbnDrawIconHeader(BASE_GOOD_X, BASE_GOOD_Y, ImGui::TableGetColumnName(5));
            ImGui::TableSetColumnIndex(6);
            wbnDrawIconHeader(PILL_EVIL15_X, PILL_EVIL15_Y, ImGui::TableGetColumnName(6));
            ImGui::TableSetColumnIndex(7);
            wbnDrawIconHeader(TANK_SELF_0_X, TANK_SELF_0_Y, ImGui::TableGetColumnName(7));

            wbnDrawStatsRow(langGetText(STR_DLGWBN_STATS_OPEN),   &st.open);
            wbnDrawStatsRow(langGetText(STR_DLGWBN_STATS_TOURN),  &st.tourn);
            wbnDrawStatsRow(langGetText(STR_DLGWBN_STATS_STRICT), &st.strict);

            ImGui::EndTable();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    /* Footer: Sign out (left, muted) and Close (right). Esc closes the
     * dialog rather than signing out. Both buttons share a width and are
     * centred as a group, mirroring the standard dialog footer. */
    const char *signOutLbl = langGetText(STR_DLGWBN_SIGN_OUT);
    const char *closeLbl    = langGetText(STR_OK);
    float pad = ImGui::GetStyle().FramePadding.x * 2.0f;
    float sw = ImGui::CalcTextSize(signOutLbl).x;
    float cw = ImGui::CalcTextSize(closeLbl).x;
    float btnW = (sw > cw ? sw : cw) + pad;
    if (btnW < 120.0f) btnW = 120.0f;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float availW = ImGui::GetContentRegionAvail().x;
    float totalW = btnW * 2.0f + spacing;
    float startX = ImGui::GetCursorPosX() + (availW - totalW) * 0.5f;
    if (startX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(startX);

    WBUI::PushCancelStyle();
    bool signOut = ImGui::Button(signOutLbl, ImVec2(btnW, 0));
    WBUI::PopCancelStyle();
    imguiHandOnHover();
    ImGui::SameLine(0.0f, spacing);
    bool close = ImGui::Button(closeLbl, ImVec2(btnW, 0));
    imguiHandOnHover();

    if (signOut) {
        wbnSignOut();
        ImGui::CloseCurrentPopup();
    } else if (close || WBUI::CancelKeyPressed()) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::PopStyleVar();
    ImGui::EndPopup();
}

extern "C" void imguiWinbolonetReset(void) {
    /* Reset only the transient popup/input state. A running auth worker is
     * left in flight so its result is applied through wbnCheckThread like
     * everywhere else; the old behaviour joined and discarded it here, which
     * silently dropped an in-flight sign-in and made the WBN state depend on
     * which screen happened to be open when the worker landed. */
    wbnPopupOpen = false;
    wbnUsername[0] = '\0';
    wbnPassword[0] = '\0';
    wbnRegUsername[0] = '\0';
    wbnRegEmail[0] = '\0';
    wbnRegSeeded = false;
    wbnErrorBuf[0] = '\0';
    wbnFocusUser = false;
    if (!wbnThread) {
        wbnState = WBN_IDLE;
    }
}

extern "C" void imguiWinbolonetStartValidation(void) {
    /* A worker already in flight (e.g. the launch re-auth) will be applied by
     * wbnCheckThread; don't start a second thread over the top of it. */
    if (wbnThread) {
        return;
    }
    char token[256];
    char expiry[256];
    gameFrontGetWinbolonetToken(token, expiry);
    if (token[0] != '\0') {
        wbnStartValidate(token);
    }
}

extern "C" void imguiWinbolonetDrawSection(bool inGame) {
    /* Check for async completion */
    wbnCheckThread();

    char token[256];
    char expiry[256];
    gameFrontGetWinbolonetToken(token, expiry);
    bool loggedIn = (token[0] != '\0');

    if (wbnState == WBN_VALIDATING) {
        wbnDrawSpinner(langGetText(STR_DLGWBN_CHECKING));
        return;
    }

    if (loggedIn) {
        ImGui::TextUnformatted(langGetText(STR_DLGWBN_LABEL));
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", langGetText(STR_DLGWBN_SIGNED_IN));
        wbnDrawViaSteamSuffix();
        if (expiry[0] != '\0') {
            ImGui::SameLine();
            MessageArgs args = {};
            SDL_strlcpy(args.string1, expiry, sizeof(args.string1));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::TextUnformatted(langGetTextFmt(STR_DLGWBN_EXPIRES, &args));
            ImGui::PopStyleColor();
        }
        wbnDrawRankLine();
        if (ImGui::Button(langGetText(STR_DLGWBN_STATS_BTN))) {
            wbnShowStats = true;
        }
        imguiHandOnHover();
        ImGui::SameLine();
        if (inGame) ImGui::BeginDisabled();
        if (ImGui::Button(langGetText(STR_DLGWBN_SIGN_OUT))) {
            wbnSignOut();
        }
        imguiHandOnHover();
        if (inGame) ImGui::EndDisabled();
    } else {
        ImGui::TextUnformatted(langGetText(STR_DLGWBN_LABEL));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", langGetText(STR_DLGWBN_NOT_SIGNED_IN));
        if (inGame) ImGui::BeginDisabled();
        if (ImGui::Button(langGetText(STR_DLGWBN_SIGN_IN_BTN))) {
            wbnOpenLoginPopup();
        }
        imguiHandOnHover();
        if (inGame) ImGui::EndDisabled();
    }

    wbnRenderLoginPopup();
    imguiWinbolonetDrawStatsDialog();
}

/* Compact account status block for the welcome screen. Signed out: a
 * single translucent "Sign in to WBN" button. Signed in: a translucent
 * account chip showing the WBN shield, player name, and 1v1 ladder rank;
 * clicking it opens the stats dialog (which hosts Sign out). Reuses the
 * same popup/worker as the settings section. Render inside an existing
 * ImGui window. */
extern "C" void imguiWinbolonetDrawStatusBlock(void) {
    /* Check for async completion */
    wbnCheckThread();

    /* Cold-start: re-auth the WinBolo.net session once per session the
     * first time the welcome screen draws, so rank/stats populate shortly
     * after launch instead of only on the first server join. A Steam
     * session (or an empty token with a ticket available) refreshes via
     * Steam; a password session validates its stored token. Gated by a
     * static flag so it does not re-fire every frame; a no-op when there
     * is nothing to re-auth. */
    static bool s_welcomeValidated = false;
    if (!s_welcomeValidated) {
        s_welcomeValidated = true;
        if (wbnState == WBN_IDLE && !wbnThread) {
            wbnStartSessionReauth();
        }
    }

    char token[256];
    char expiry[256];
    gameFrontGetWinbolonetToken(token, expiry);
    bool loggedIn = (token[0] != '\0');

    if (wbnState == WBN_VALIDATING) {
        wbnDrawSpinner(langGetText(STR_DLGWBN_CHECKING));
        wbnRenderLoginPopup();
        return;
    }

    ImGuiViewport *vp = ImGui::GetMainViewport();
    float s = dialogComputeScale((int)vp->Size.x, (int)vp->Size.y);

    /* Mirror the welcome menu buttons exactly so this reads as the same
     * family as "Single Player": same translucent ghost colours, the same
     * background-aware alphas (fainter over a live demo, more opaque over
     * the solid dark screen), and the same fixed 180*s x 30*s size. */
    bool hasBg = (bgGameGetShared() != nullptr);
#if BOLO_MOBILE
    const float ghostBtnAlpha  = 0.6f;
    const float ghostTextAlpha = 0.9f;
#else
    const float ghostBtnAlpha  = hasBg ? 0.15f : 0.6f;
    const float ghostTextAlpha = hasBg ? 0.75f : 0.9f;
#endif
    const float btnW = 180.0f * s;
    const float btnH = 30.0f * s;

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f * s);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f, 0.1f, 0.1f, ghostBtnAlpha));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.7f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.3f, 0.3f, 0.3f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.0f, 1.0f, 1.0f, ghostTextAlpha));

    if (loggedIn) {
        /* Account chip: name + rank on a button background. Clicking opens
         * the stats dialog, which also hosts Sign out. */
        if (wbnDrawAccountChip(s, ghostTextAlpha)) {
            wbnShowStats = true;
        }
    } else {
        /* Sign-in button: WBN shield + label, drawn manually over an empty
         * button so the signed-out CTA reads as a WBN affordance. The shield
         * is the login spinner's crisp vector shield — held face-on at rest,
         * spinning while hovered — rather than the badge texture, which turns
         * muddy when scaled down to this size. Hover also brightens the label
         * to full white, like the menu buttons. */
        const char *label = langGetText(STR_DLGWBN_SIGN_IN_BTN);

        ImVec2 cur = ImGui::GetCursorScreenPos();
        bool hov = ImGui::IsMouseHoveringRect(cur, ImVec2(cur.x + btnW, cur.y + btnH));
        if (ImGui::Button("##wbnsignin", ImVec2(btnW, btnH))) {
            wbnOpenLoginPopup();
        }
        imguiHandOnHover();

        float lineH = ImGui::GetTextLineHeight();
        float iconSz = lineH;
        float gap = 6.0f * s;
        float textW = ImGui::CalcTextSize(label).x;
        float contentW = iconSz + gap + textW;
        float tx = cur.x + (btnW - contentW) * 0.5f;
        float ty = cur.y + (btnH - lineH) * 0.5f;
        int textA = hov ? 255 : (int)(ghostTextAlpha * 255.0f);

        /* phase 0 == face-on (full width); advancing phase spins it. Drawn
         * hollow (outline only) so the signed-out CTA reads as an empty
         * shield, filling in once signed in (the account-chip badge). */
        float halfH = iconSz * 0.5f;
        float phase = hov ? (float)SDL_GetTicks() * 0.002f : 0.0f;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        imguiDrawSpinningShield(dl, ImVec2(tx + iconSz * 0.5f, ty + halfH),
                                halfH * 0.80f, halfH, phase,
                                IM_COL32(255, 255, 255, textA), false);
        tx += iconSz + gap;
        dl->AddText(ImVec2(tx, ty), IM_COL32(255, 255, 255, textA), label);
    }

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(1);

    wbnRenderLoginPopup();
    imguiWinbolonetDrawStatsDialog();
}

/* Opens the shared sign-in / create-account popup. Lets surfaces that
 * aren't the settings section or welcome status block (e.g. the WBN log
 * browser's "My Games" tab) trigger the same dialog. The caller MUST also
 * pump imguiWinbolonetRenderLoginPopup() every frame so the popup renders
 * in its own ImGui context/window. */
extern "C" void imguiWinbolonetOpenLoginPopup(void) {
    wbnOpenLoginPopup();
}

/* Renders the shared sign-in popup for the frame. No-op unless the popup
 * was opened (BeginPopupModal early-outs); also drives the async login
 * worker's state machine, so call it once per frame on whichever surface
 * opened the popup. */
extern "C" void imguiWinbolonetRenderLoginPopup(void) {
    wbnRenderLoginPopup();
}
