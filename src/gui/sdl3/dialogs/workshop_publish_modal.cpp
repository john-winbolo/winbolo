/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          workshop_publish_modal
 * Filename:      workshop_publish_modal.cpp
 * Purpose:
 *   The Steam Workshop publish window and the upload
 *   scratch behind it. See workshop_publish_modal.h.
 *
 *   Compiled into the desktop client only, beside
 *   skin_preview.c and workshop_sync.c; the mobile and wasm
 *   builds leave the calls into it out rather than link it.
 *********************************************************/

#include <cstring>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_dialog_utils.h"
#include "workshop_publish_modal.h"

extern "C" {
#include "../../../common/wb_log.h"
#include "../../lang.h"
#include "../../../steam/steam_wrapper.h"
}

/* Long enough for any path a caller hands over as its key or that the pref
   path puts the scratch at. */
#define WORKSHOP_PUBLISH_PATH_MAX 1024

/* Publish state.  A publish runs across frames — the Steam calls behind it
   finish asynchronously — so the window keeps what it needs between them.
   File scope because every caller shares the one window: the Steam wrapper
   runs one publish at a time. */
static char     s_pubKey[WORKSHOP_PUBLISH_PATH_MAX]; /* the item being sent */
static char     s_pubTitle[129];           /* the SDK's title limit */
static char     s_pubDesc[8000];           /* the SDK's description limit */
static uint64_t s_pubExistingId = 0;       /* the id the source carries */
static uint64_t s_pubAuthor     = 0;       /* who the source says published it */
static bool     s_pubAsNew      = true;    /* make an item vs update that id */
static bool     s_pubStarted    = false;   /* a begin said yes: poll it */
static uint64_t s_pubDoneId     = 0;       /* the item Steam published */
static bool     s_pubNeedsLegal = false;   /* the author still has to accept
                                              the Workshop agreement */
static bool     s_pubFailed     = false;   /* the begin or the poll said no */
static bool     s_pubInFlight   = false;   /* the last poll said in progress:
                                              Steam is still reading the
                                              upload folder */
/* The popup id of the window that drew last frame, "" for none.  A frame
   without it is an exit, but only for the caller whose popup it is: every
   caller draws every frame, and the ones whose popup is closed must not take
   another caller's open window for their own exit. */
static char     s_pubOpenId[64];

/* <prefpath>workshop_upload is the folder handed to Steam as the item's
   content; the preview PNG goes beside it, not in it, or it would be uploaded
   as part of the item.  SDL_GetPrefPath already ends in a separator. */
static bool publishPaths(char *folder, size_t folderLen,
                         char *preview, size_t previewLen) {
    const char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
    if (prefDir == nullptr) return false;
    SDL_snprintf(folder, folderLen, "%sworkshop_upload", prefDir);
    SDL_snprintf(preview, previewLen, "%sworkshop_preview.png", prefDir);
    SDL_free((void *)prefDir);
    return true;
}

/* Empties the upload folder and removes it.  Steam uploads everything the
   folder holds, so anything an earlier publish left there would go up with
   this one. */
static void publishClearFolder(const char *folder) {
    int    count = 0;
    char **list = SDL_GlobDirectory(folder, "*", 0, &count);
    if (list != nullptr) {
        for (int i = 0; i < count; i++) {
            char full[WORKSHOP_PUBLISH_PATH_MAX * 2];
            if (list[i] == nullptr || list[i][0] == '\0') continue;
            SDL_snprintf(full, sizeof(full), "%s/%s", folder, list[i]);
            SDL_RemovePath(full);
        }
        SDL_free(list);
    }
    SDL_RemovePath(folder);
}

bool workshopPublishCopyFile(const char *from, const char *to) {
    size_t        len = 0;
    void         *data = SDL_LoadFile(from, &len);
    SDL_IOStream *io;
    bool          ok;

    if (data == nullptr) return false;
    io = SDL_IOFromFile(to, "wb");
    if (io == nullptr) {
        SDL_free(data);
        return false;
    }
    ok = (len == 0 || SDL_WriteIO(io, data, len) == len);
    if (!SDL_CloseIO(io)) ok = false;
    SDL_free(data);
    if (!ok) SDL_RemovePath(to);
    return ok;
}

/* The scratch the upload was built from, once the window is done with it. */
static void publishCleanup(void) {
    char folder[WORKSHOP_PUBLISH_PATH_MAX];
    char preview[WORKSHOP_PUBLISH_PATH_MAX];

    if (!publishPaths(folder, sizeof(folder), preview, sizeof(preview))) {
        return;
    }
    publishClearFolder(folder);
    SDL_RemovePath(preview);
}

/* Empties the upload folder, has the caller fill it and starts the upload.
   Returns what steam_workshop_publish_begin said, so the window knows whether
   polling means anything. */
static bool publishStart(const WorkshopPublishSpec *spec) {
    char folder[WORKSHOP_PUBLISH_PATH_MAX];
    char preview[WORKSHOP_PUBLISH_PATH_MAX];

    if (!publishPaths(folder, sizeof(folder), preview, sizeof(preview))) {
        return false;
    }
    publishClearFolder(folder);
    if (!SDL_CreateDirectory(folder)) return false;

    if (!spec->buildContent(spec->ctx, folder, preview, sizeof(preview))) {
        return false;
    }

    return steam_workshop_publish_begin(folder, s_pubTitle, s_pubDesc,
                                        preview[0] != '\0' ? preview : nullptr,
                                        s_pubAsNew ? 0 : s_pubExistingId,
                                        spec->tag);
}

bool workshopPublishBusyFor(const char *key) {
    return s_pubStarted && strcmp(s_pubKey, key) != 0 &&
           steam_workshop_publish_poll(nullptr, nullptr) == 0;
}

bool workshopPublishPrepare(const char *key, const char *title,
                            const char *desc, uint64_t existingId,
                            uint64_t author) {
    const bool sameKey = strcmp(s_pubKey, key) == 0;

    /* The wrapper runs one publish at a time and the state here is that
       publish's, so another item waits until the upload is over rather than
       taking the state from under it. */
    if (workshopPublishBusyFor(key)) {
        return false;
    }

    /* Reopening on the item whose publish is still running picks the live
       state back up rather than restarting it; any other item starts from
       what its source says. */
    const bool resume = s_pubStarted && sameKey;
    SDL_strlcpy(s_pubKey, key, sizeof(s_pubKey));
    if (!resume) {
        SDL_strlcpy(s_pubTitle, title, sizeof(s_pubTitle));
        SDL_strlcpy(s_pubDesc, desc, sizeof(s_pubDesc));
        s_pubExistingId = existingId;
        s_pubAuthor     = author;
        /* An update is offered only when the recorded publisher is this
           account.  Steam refuses an update to somebody else's item, and an
           unknown publisher is no proof the item is ours — publishing a
           second item by mistake is the recoverable error. */
        s_pubAsNew = !(s_pubExistingId != 0 &&
                       s_pubAuthor != 0 &&
                       s_pubAuthor == steam_get_steam_id());
        s_pubStarted    = false;
        s_pubDoneId     = 0;
        s_pubNeedsLegal = false;
        s_pubFailed     = false;
    }
    return true;
}

void workshopPublishDraw(const WorkshopPublishSpec *spec) {
    if (ImGui::BeginPopupModal(spec->popupId, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const float fieldW = ImGui::GetFontSize() * 20.0f;
        SDL_strlcpy(s_pubOpenId, spec->popupId, sizeof(s_pubOpenId));

        ImGui::TextUnformatted(langGetText((langid)spec->headingStr));
        ImGui::Separator();

        ImGui::TextUnformatted(langGetText(STR_DLGSKIN_PUBLISH_NAME));
        ImGui::SetNextItemWidth(fieldW);
        ImGui::InputText("##pubtitle", s_pubTitle, sizeof(s_pubTitle));
        ImGui::TextUnformatted(langGetText(STR_DLGSKIN_PUBLISH_DESC));
        ImGui::InputTextMultiline("##pubdesc", s_pubDesc, sizeof(s_pubDesc),
                                  ImVec2(fieldW, ImGui::GetFontSize() * 6.0f));

        /* Only an item that already carries an id has one to update, so the
           choice is not offered otherwise. */
        if (s_pubExistingId != 0) {
            if (ImGui::RadioButton(langGetText((langid)spec->updateStr),
                                   !s_pubAsNew)) {
                s_pubAsNew = false;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("#%llu", (unsigned long long)s_pubExistingId);
            if (ImGui::RadioButton(langGetText(STR_DLGSKIN_PUBLISH_NEW),
                                   s_pubAsNew)) {
                s_pubAsNew = true;
            }
        }

        /* The poll answers -1 with nothing in flight, so it is only asked
           after a begin that said yes. */
        int state = 0;
        if (s_pubStarted) {
            uint64_t doneId = 0;
            bool     needsLegal = false;
            state = steam_workshop_publish_poll(&doneId, &needsLegal);
            if (state == 1 && doneId != 0 && s_pubDoneId == 0) {
                s_pubDoneId     = doneId;
                s_pubNeedsLegal = needsLegal;
                /* Written back once, so the next publish of this item updates
                   it instead of making another, and knows the item is this
                   account's. The window takes the new pair from here rather
                   than from anything the caller read before the write.
                   When the write fails the source still carries no id, so
                   the window keeps offering publish-as-new, which is what the
                   file on disk will do next time too. */
                if (spec->recordId(spec->ctx, doneId, steam_get_steam_id())) {
                    s_pubExistingId = doneId;
                    s_pubAuthor     = steam_get_steam_id();
                    s_pubAsNew      = false;
                } else {
                    WB_LOG_WARN(WB_LOG_CAT_ASSET,
                                "workshop_publish: published %s as Workshop "
                                "item %llu but could not record the id in it; "
                                "the next publish will make a new item",
                                s_pubKey, (unsigned long long)doneId);
                }
            } else if (state == -1) {
                s_pubFailed = true;
            }
        }
        s_pubInFlight = (s_pubStarted && state == 0);

        ImGui::Spacing();
        if (s_pubStarted && state == 0) {
            ImGui::TextUnformatted(langGetText(STR_DLGSKIN_PUBLISH_WORKING));
            uint64_t bytesDone = 0, bytesTotal = 0;
            if (steam_workshop_publish_progress(&bytesDone, &bytesTotal) != 0 &&
                bytesTotal > 0) {
                ImGui::ProgressBar((float)((double)bytesDone /
                                           (double)bytesTotal),
                                   ImVec2(fieldW, 0.0f));
            }
        } else if (s_pubStarted && state == 1) {
            ImGui::TextUnformatted(langGetText(STR_DLGSKIN_PUBLISH_DONE));
            if (s_pubDoneId != 0) {
                if (ImGui::Button(langGetText(STR_DLGSKIN_PUBLISH_OPENITEM))) {
                    steam_workshop_open_item_page(s_pubDoneId);
                }
                imguiHandOnHover();
            }
            if (s_pubNeedsLegal) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                                   langGetText(STR_DLGSKIN_PUBLISH_LEGAL));
            }
        } else if (s_pubFailed) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                               langGetText(STR_DLGSKIN_PUBLISH_FAILED));
        }

        ImGui::Separator();
        ImGui::BeginDisabled(s_pubStarted && state == 0);
        if (ImGui::Button(langGetText(STR_DLGSKIN_PUBLISH_GO))) {
            s_pubDoneId     = 0;
            s_pubNeedsLegal = false;
            s_pubFailed     = false;
            s_pubStarted    = publishStart(spec);
            if (!s_pubStarted) s_pubFailed = true;
        }
        ImGui::EndDisabled();
        imguiHandOnHover();
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_CLOSE))) {
            /* Closing does not stop an upload Steam has already been given —
               the UGC API has no cancel — so reopening shows it still
               running.  Steam reads the content folder after the update is
               submitted, so the scratch only goes once the upload is over;
               one abandoned mid-upload is cleared by the next publish, which
               empties the folder before it builds. */
            if (!s_pubInFlight) publishCleanup();
            ImGui::CloseCurrentPopup();
        }
        imguiHandOnHover();
        ImGui::EndPopup();
    } else if (s_pubOpenId[0] != '\0' &&
               strcmp(s_pubOpenId, spec->popupId) == 0) {
        /* This caller's window is gone.  Usually the frame after the Close
           button ran, where the scratch is already dealt with, but the dialog
           it sits in can also be closed out from under it — same rule, so no
           exit deletes files Steam is still reading. */
        s_pubOpenId[0] = '\0';
        if (!s_pubInFlight) publishCleanup();
    }
}
