/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          workshop_publish_modal
 * Filename:      workshop_publish_modal.h
 * Purpose:
 *   The Steam Workshop publish window, shared by everything
 *   that publishes: title, description, update-or-new,
 *   progress, the legal note and "Open item page". A caller
 *   names what it publishes through a spec — its heading,
 *   its update label, its Workshop tag, a callback that
 *   fills the upload folder and one that records the id the
 *   item was given.
 *
 *   One publish at a time: the Steam wrapper runs one, so
 *   the state behind this window is one set, whichever
 *   caller opened it.
 *
 *   Desktop only. The .cpp is compiled into the desktop
 *   client alone; this header only declares, so it can be
 *   included by a file every build compiles as long as the
 *   calls sit behind !BOLO_MOBILE && !__EMSCRIPTEN__.
 *********************************************************/

#ifndef WORKSHOP_PUBLISH_MODAL_H
#define WORKSHOP_PUBLISH_MODAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *popupId;     /* ImGui popup id, e.g. "##SkinPublish" */
    int         headingStr;  /* lang id drawn at the top */
    int         updateStr;   /* lang id of the "update this item" radio */
    const char *tag;         /* Workshop tag: "Skin", "Scenario" or "Mod" */
    /* Fill the empty content folder. previewOut arrives holding the path a
       preview PNG goes to — beside the folder, not in it, or Steam would
       upload it as content — and the window removes that file when it is
       done. Write the PNG there and leave the path, or set previewOut to ""
       to send none. False stops the publish. */
    bool (*buildContent)(void *ctx, const char *folder,
                         char *previewOut, size_t previewLen);
    /* Record the published id and author in the source. False keeps the
       window offering publish-as-new, which is what the source on disk will
       do next time too. */
    bool (*recordId)(void *ctx, uint64_t id, uint64_t author);
    void       *ctx;
} WorkshopPublishSpec;

/* Prepare the window for the item identified by key (its path). Reopening
   the key whose publish is still running resumes it; any other key starts
   fresh from title, desc, existingId and author. False, with nothing
   changed, when a publish for a different key is still uploading. The
   caller calls ImGui::OpenPopup(popupId) on true. */
bool workshopPublishPrepare(const char *key, const char *title,
                            const char *desc, uint64_t existingId,
                            uint64_t author);

/* True when a publish for a key other than key is still uploading, which is
   when workshopPublishPrepare(key, ...) would answer false. A caller draws
   its Publish button disabled on it, so the button does not look as if it
   works and then does nothing. An empty key is no item's, so "" asks
   whether any publish is uploading. Reads state only: cheap enough to ask
   every frame. */
bool workshopPublishBusyFor(const char *key);

/* Draw the window if it is open: progress, the id write-back, close and the
   cleanup of the upload scratch. Call every frame the popup can be open on,
   so the frame it goes away is seen. */
void workshopPublishDraw(const WorkshopPublishSpec *spec);

/* Copy one file through memory, for content builders: the pinned SDL has no
   file-copy call. The destination is removed when the copy fails. */
bool workshopPublishCopyFile(const char *from, const char *to);

#endif /* WORKSHOP_PUBLISH_MODAL_H */
