/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Lifetime regression for the async WinBolo.net news fetch.
 *
 * The field crash (Sentry: mtx_do_lock, EXCEPTION_ACCESS_VIOLATION_READ):
 * the welcome-screen news popup kicked wbn_news_fetch_start(), the worker
 * thread parked in a slow network GET (the reporter was on a network where
 * winbolo.net was slow to answer), and the popup shutdown then called
 * wbn_news_fetch_free() — deleting the fetch object while the worker still
 * held it. When the GET finally returned, the worker took its terminal
 * lock_guard on the now-destroyed mutex: a use-after-free.
 *
 * This test forces exactly that interleaving. wbn_news.cpp exposes a test
 * seam (wbn_news_set_api_get_for_test) so the "network GET" can be parked on
 * a condition variable while the test frees the handle, then released.
 *
 * On the pre-fix code this is a heap-use-after-free that AddressSanitizer
 * (-DENABLE_ASAN=ON) reports as a hard failure. With the fix — the fetch
 * state is ref-counted between the handle and the worker — the worker keeps
 * the state alive past the free() and the interleaving is clean.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "test_harness.h"
#include "wbn_news.h"

/* Declared in wbn_news.cpp; test-only seam, kept out of the shipping header. */
extern "C" void wbn_news_set_api_get_for_test(
    int (*fn)(const char *path, char **response_out));

namespace {

std::mutex              g_m;
std::condition_variable g_cv;
bool                    g_workerEntered = false;  /* worker reached the GET   */
bool                    g_release       = false;  /* test says "return now"   */
std::atomic<bool>       g_apiReturned{false};     /* GET is about to return   */

/* Stand-in for wbn_api_get: blocks inside the "network GET" until the test
 * releases it, so the test can free the handle while the worker is parked
 * here. On release it returns a small valid news payload so the worker takes
 * the parse-and-populate path (allocating body_md) — the same path that
 * wrote through the freed object in the field. */
int blockingApiGet(const char *path, char **response_out) {
    (void)path;
    {
        std::unique_lock<std::mutex> lk(g_m);
        g_workerEntered = true;
        g_cv.notify_all();
        g_cv.wait(lk, [] { return g_release; });
    }
    if (response_out) {
        const char *json =
            "{\"news\":[{\"id\":7,\"title\":\"T\",\"body\":\"B\","
            "\"comments\":0,\"url\":\"https://winbolo.net/x\","
            "\"date\":\"2026-01-01 00:00:00\"}],\"countryCode\":\"US\"}";
        *response_out = strdup(json);
    }
    g_apiReturned.store(true);
    return 200;
}

}  // namespace

extern "C" int run_wbn_news_free_during_fetch(void) {
    /* The seam and signalling globals are process-scoped; reset them so the
     * test is independent of run order. */
    {
        std::lock_guard<std::mutex> lk(g_m);
        g_workerEntered = false;
        g_release       = false;
    }
    g_apiReturned.store(false);

    wbn_news_set_api_get_for_test(blockingApiGet);

    WbnNewsFetch *f = wbn_news_fetch_start();
    UT_ASSERT_MSG(f != NULL, "wbn_news_fetch_start returned NULL");

    /* Wait until the worker is parked inside the GET. */
    {
        std::unique_lock<std::mutex> lk(g_m);
        g_cv.wait(lk, [] { return g_workerEntered; });
    }

    /* Free the handle while the worker is still in the GET — the exact
     * ordering that crashed in the field. */
    wbn_news_fetch_free(f);

    /* Release the worker. It now runs free(response), parses the payload, and
     * takes its terminal lock on the state the handle used to own. */
    {
        std::lock_guard<std::mutex> lk(g_m);
        g_release = true;
    }
    g_cv.notify_all();

    /* Let the worker run to completion so any bad access happens (and, under
     * ASan, is reported) before we return, and so the detached thread stops
     * touching this file's globals. The GET flag flips just before the
     * worker's lock/store/teardown tail; the short settle covers that tail —
     * the worker is detached, so there is no handle left to join on. */
    while (!g_apiReturned.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    wbn_news_set_api_get_for_test(NULL);
    return 0;
}
