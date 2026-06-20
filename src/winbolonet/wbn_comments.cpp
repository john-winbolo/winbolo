/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          wbn_comments
 * Filename:      wbn_comments.cpp
 * Purpose:
 *   Async fetch and post of WinBolo.net log comments.
 *   Each operation is backed by a detached std::thread
 *   and polled via wbn_comments_*_done from the UI thread.
 *********************************************************/

#include "wbn_comments.h"

extern "C" {
#include <SDL3/SDL.h>
#include "http.h"
#include "cJSON.h"
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
    std::atomic<bool>        done{false};
    std::mutex               mtx;
    std::vector<WbnComment>  comments;
    int                      httpStatus = -1;
    char                     errMsg[256] = {0};
};

extern "C" WbnCommentsFetch *wbn_comments_fetch_start(const char *key32) {
    if (!key32 || !key32[0]) return nullptr;

    auto *f = new WbnCommentsFetch();
    std::string keyCopy(key32);

    std::thread([f, keyCopy]() {
        char path[128];
        SDL_snprintf(path, sizeof(path), "logs/%s", keyCopy.c_str());

        char *response = nullptr;
        int   status   = wbn_api_get(path, &response);

        std::vector<WbnComment> parsed;
        char err[256] = {0};

        if (status == 200 && response) {
            cJSON *json = cJSON_Parse(response);
            if (json) {
                parseCommentsArray(cJSON_GetObjectItem(json, "comments"), parsed);
                cJSON_Delete(json);
            }
        } else {
            extractServerError(response, err, sizeof(err));
        }

        free(response);

        {
            std::lock_guard<std::mutex> lock(f->mtx);
            f->comments = std::move(parsed);
            f->httpStatus = status;
            SDL_strlcpy(f->errMsg, err, sizeof(f->errMsg));
        }
        f->done.store(true);
    }).detach();

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

extern "C" void wbn_comments_fetch_free(WbnCommentsFetch *f) {
    delete f;
}

/* ===================================================== */
/* Post                                                   */
/* ===================================================== */

struct WbnCommentPost {
    std::atomic<bool> done{false};
    std::mutex        mtx;
    int               httpStatus = -1;
    char              message[256] = {0};
};

extern "C" WbnCommentPost *wbn_comments_post_start(const char *key32, const char *token,
                                                    const char *text, int rating) {
    if (!key32 || !key32[0] || !token || !token[0] || !text || !text[0]) return nullptr;

    auto *p = new WbnCommentPost();
    std::string keyCopy(key32);
    std::string tokenCopy(token);
    std::string textCopy(text);
    int ratingCopy = rating;

    std::thread([p, keyCopy, tokenCopy, textCopy, ratingCopy]() {
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
    }).detach();

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

extern "C" void wbn_comments_post_free(WbnCommentPost *p) {
    delete p;
}
