/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          wbn_comments
 * Filename:      wbn_comments.h
 * Purpose:
 *   Async fetch and post of WinBolo.net log comments.
 *   Used by both the WBN browser dialog (in the SDL3
 *   client) and the LogViewer's game-info panel.
 *********************************************************/

#ifndef WBN_COMMENTS_H
#define WBN_COMMENTS_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WbnComment {
    char username[64];
    char comment[512];
    char time_formatted[32];
    int  user_id;
    int  rating;     /* 1..10, 0 = no rating attached */
    int  timestamp;
} WbnComment;

/* ===== Async comment fetch =====
 * GETs logs/<key> and extracts the comments[] array plus the log's
 * aggregate rating. The rest of the response is discarded. */

typedef struct WbnCommentsFetch WbnCommentsFetch;

/* Returns NULL if key is empty or HTTP isn't initialised. The handle is
 * owned by the caller — must be passed to wbn_comments_fetch_free. */
WbnCommentsFetch *wbn_comments_fetch_start(const char *key32);

/* True once the worker thread has finished. Safe to call every frame. */
bool wbn_comments_fetch_done(const WbnCommentsFetch *f);

/* Reads the result. *out_comments points to internal storage owned by f
 * and remains valid until wbn_comments_fetch_free.
 * Returns the HTTP status code, or -1 on transport error.
 * On non-200 status, *out_count is 0 and err_msg may carry a server-provided
 * error message (caller may fall back to a localised string if empty). */
int wbn_comments_fetch_result(WbnCommentsFetch *f,
                              const WbnComment **out_comments, size_t *out_count,
                              char *err_msg, size_t err_size);

/* Aggregate rating carried by the same logs/<key> response the comment
 * fetch reads. rating10 is 0-10; num_ratings is how many ratings are
 * behind it. Both are 0 until the fetch completes with status 200. */
void wbn_comments_fetch_rating(WbnCommentsFetch *f, float *out_rating10,
                               int *out_num_ratings);

void wbn_comments_fetch_free(WbnCommentsFetch *f);

/* ===== Async comment post =====
 * POSTs logs/<key>/comment with {token, comment, [rating]} body. */

typedef struct WbnCommentPost WbnCommentPost;

/* Returns NULL if any required arg is empty/null or HTTP isn't initialised.
 * rating: 0 = no rating, 1..10 = star rating attached. */
WbnCommentPost *wbn_comments_post_start(const char *key32, const char *token,
                                        const char *text, int rating);

bool wbn_comments_post_done(const WbnCommentPost *p);

/* HTTP status (200/201 = posted, 401 = bad/expired token, etc.) or -1.
 * msg receives a human-readable result string (server "error" field or
 * empty); caller maps to a localised label if empty. */
int wbn_comments_post_result(WbnCommentPost *p, char *msg, size_t msg_size);

void wbn_comments_post_free(WbnCommentPost *p);

#ifdef __cplusplus
}
#endif

#endif /* WBN_COMMENTS_H */
