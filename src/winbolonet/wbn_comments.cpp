/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          wbn_comments
 * Filename:      wbn_comments.cpp
 * Purpose:
 *   Async fetch and post of WinBolo.net log comments.
 *   Each operation is backed by a joinable std::thread
 *   held in its handle and polled via wbn_comments_*_done
 *   from the UI thread. Freeing a handle joins its worker,
 *   so the worker cannot outlive the state it writes into.
 *********************************************************/

#include "wbn_comments.h"

extern "C" {
#include <SDL3/SDL.h>
#include "http.h"
#include "cJSON.h"
#include "winbolonet_core.h"
}

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

void parseCommentsArray(cJSON *arr, std::vector<WbnComment> &out) {
    if (!arr || !cJSON_IsArray(arr)) return;
    int n = cJSON_GetArraySize(arr);
    out.reserve((size_t)n);
    for (int i = 0; i < n; i++) {
        cJSON *co = cJSON_GetArrayItem(arr, i);
        if (!co) continue;
        WbnComment c{};
        cJSON *cv;

        cv = cJSON_GetObjectItem(co, "username");
        if (cv && cv->valuestring) SDL_strlcpy(c.username, cv->valuestring, sizeof(c.username));

        cv = cJSON_GetObjectItem(co, "user_id");
        c.user_id = cv ? cv->valueint : 0;

        cv = cJSON_GetObjectItem(co, "comment");
        if (cv && cv->valuestring) SDL_strlcpy(c.comment, cv->valuestring, sizeof(c.comment));

        cv = cJSON_GetObjectItem(co, "rating");
        c.rating = cv ? cv->valueint : 0;

        cv = cJSON_GetObjectItem(co, "timestamp");
        c.timestamp = cv ? cv->valueint : 0;

        cv = cJSON_GetObjectItem(co, "time_formatted");
        if (cv && cv->valuestring) SDL_strlcpy(c.time_formatted, cv->valuestring, sizeof(c.time_formatted));

        out.push_back(c);
    }
}

/* If response is JSON {"error":"..."}, copy that string into err. */
void extractServerError(const char *response, char *err, size_t err_size) {
    if (!response || !err || err_size == 0) return;
    cJSON *json = cJSON_Parse(response);
    if (!json) return;
    cJSON *e = cJSON_GetObjectItem(json, "error");
    if (e && e->valuestring) SDL_strlcpy(err, e->valuestring, err_size);
    cJSON_Delete(json);
}

} /* anonymous namespace */

/* ===================================================== */
/* Fetch                                                  */
/* ===================================================== */

struct WbnCommentsFetch {
    /* The worker writes every field below, so the handle holds it and joins
     * it on free rather than detaching. cancel is polled by curl as bytes
     * arrive, which is what keeps that join short. */
    std::thread              thr;
    volatile int             cancel = 0;
    std::atomic<bool>        done{false};
    std::mutex               mtx;
    std::vector<WbnComment>  comments;
    float                    rating10 = 0.0f;
    int                      numRatings = 0;
    int                      httpStatus = -1;
    char                     errMsg[256] = {0};
};

extern "C" WbnCommentsFetch *wbn_comments_fetch_start(const char *key32) {
    /* key32 lands in an API path below, so it is held to the 32-hex shape WBN
     * issues. Callers are expected to have checked it where it entered the
     * process; this is the backstop that makes the path safe on its own. */
    if (!winbolonetKeyIsValid(key32)) return nullptr;

    auto *f = new WbnCommentsFetch();
    std::string keyCopy(key32);

    f->thr = std::thread([f, keyCopy]() {
        char path[128];
        SDL_snprintf(path, sizeof(path), "logs/%s", keyCopy.c_str());

        char *response = nullptr;
        int   status   = wbn_api_get_cancellable(path, &response, &f->cancel);

        std::vector<WbnComment> parsed;
        char err[256] = {0};
        float rating10   = 0.0f;
        int   numRatings = 0;

        if (status == 200 && response) {
            cJSON *json = cJSON_Parse(response);
            if (json) {
                parseCommentsArray(cJSON_GetObjectItem(json, "comments"), parsed);
                cJSON *rv = cJSON_GetObjectItem(json, "rating");
                if (rv) rating10 = (float)rv->valuedouble;
                cJSON *nv = cJSON_GetObjectItem(json, "num_ratings");
                if (nv) numRatings = nv->valueint;
                cJSON_Delete(json);
            }
        } else {
            extractServerError(response, err, sizeof(err));
        }

        free(response);

        {
            std::lock_guard<std::mutex> lock(f->mtx);
            f->comments = std::move(parsed);
            f->rating10 = rating10;
            f->numRatings = numRatings;
            f->httpStatus = status;
            SDL_strlcpy(f->errMsg, err, sizeof(f->errMsg));
        }
        f->done.store(true);
    });

    return f;
}

extern "C" bool wbn_comments_fetch_done(const WbnCommentsFetch *f) {
    return f && f->done.load();
}

extern "C" int wbn_comments_fetch_result(WbnCommentsFetch *f,
                                         const WbnComment **out_comments,
                                         size_t *out_count,
                                         char *err_msg, size_t err_size) {
    if (!f) {
        if (out_comments) *out_comments = nullptr;
        if (out_count)    *out_count = 0;
        if (err_msg && err_size > 0) err_msg[0] = '\0';
        return -1;
    }
    std::lock_guard<std::mutex> lock(f->mtx);
    if (out_comments) *out_comments = f->comments.empty() ? nullptr : f->comments.data();
    if (out_count)    *out_count    = f->comments.size();
    if (err_msg && err_size > 0)    SDL_strlcpy(err_msg, f->errMsg, err_size);
    return f->httpStatus;
}

extern "C" void wbn_comments_fetch_rating(WbnCommentsFetch *f,
                                          float *out_rating10,
                                          int *out_num_ratings) {
    if (!f) {
        if (out_rating10)    *out_rating10 = 0.0f;
        if (out_num_ratings) *out_num_ratings = 0;
        return;
    }
    std::lock_guard<std::mutex> lock(f->mtx);
    if (out_rating10)    *out_rating10    = f->rating10;
    if (out_num_ratings) *out_num_ratings = f->numRatings;
}

/* Cancels the transfer and blocks until the worker has stopped, whether or
 * not it finished — it writes into the handle, so nothing may free the handle
 * while it runs. The wait is a poll interval, not a request timeout. Must not
 * be called from the worker itself: a thread cannot join itself. */
extern "C" void wbn_comments_fetch_free(WbnCommentsFetch *f) {
    if (!f) return;
    f->cancel = 1;
    if (f->thr.joinable()) f->thr.join();
    delete f;
}

/* ===================================================== */
/* Post                                                   */
/* ===================================================== */

struct WbnCommentPost {
    /* Held and joined like the fetch's worker, for the same reason: it writes
     * the fields below. cancel is set on free but nothing polls it — http.h
     * has no cancellable POST — so the join waits for the request itself. The
     * flag is here so both handles have one shape and a cancellable POST can
     * be dropped in without touching the lifetime code. */
    std::thread       thr;
    volatile int      cancel = 0;
    std::atomic<bool> done{false};
    std::mutex        mtx;
    int               httpStatus = -1;
    char              message[256] = {0};
};

extern "C" WbnCommentPost *wbn_comments_post_start(const char *key32, const char *token,
                                                    const char *text, int rating) {
    /* Same gate as the fetch, and it matters more here: this request carries
     * the signed-in user's token in its body. */
    if (!winbolonetKeyIsValid(key32)) return nullptr;
    if (!token || !token[0] || !text || !text[0]) return nullptr;

    auto *p = new WbnCommentPost();
    std::string keyCopy(key32);
    std::string tokenCopy(token);
    std::string textCopy(text);
    int ratingCopy = rating;

    p->thr = std::thread([p, keyCopy, tokenCopy, textCopy, ratingCopy]() {
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "token",   tokenCopy.c_str());
        cJSON_AddStringToObject(body, "comment", textCopy.c_str());
        if (ratingCopy > 0)
            cJSON_AddNumberToObject(body, "rating", ratingCopy);

        char endpoint[128];
        SDL_snprintf(endpoint, sizeof(endpoint), "logs/%s/comment", keyCopy.c_str());

        char *json_str = cJSON_PrintUnformatted(body);
        char *response = nullptr;
        int   status   = wbn_api_post(endpoint, json_str ? json_str : "", &response);

        char msg[256] = {0};
        if (status != 200 && status != 201) {
            extractServerError(response, msg, sizeof(msg));
        }

        free(json_str);
        free(response);
        cJSON_Delete(body);

        {
            std::lock_guard<std::mutex> lock(p->mtx);
            p->httpStatus = status;
            SDL_strlcpy(p->message, msg, sizeof(p->message));
        }
        p->done.store(true);
    });

    return p;
}

extern "C" bool wbn_comments_post_done(const WbnCommentPost *p) {
    return p && p->done.load();
}

extern "C" int wbn_comments_post_result(WbnCommentPost *p, char *msg, size_t msg_size) {
    if (!p) {
        if (msg && msg_size > 0) msg[0] = '\0';
        return -1;
    }
    std::lock_guard<std::mutex> lock(p->mtx);
    if (msg && msg_size > 0) SDL_strlcpy(msg, p->message, msg_size);
    return p->httpStatus;
}

/* Blocks until the worker has stopped. Nothing polls the POST for
 * cancellation, so a call made while one is in flight waits for the request to
 * answer or time out. Must not be called from the worker itself: a thread
 * cannot join itself. */
extern "C" void wbn_comments_post_free(WbnCommentPost *p) {
    if (!p) return;
    p->cancel = 1;
    if (p->thr.joinable()) p->thr.join();
    delete p;
}
