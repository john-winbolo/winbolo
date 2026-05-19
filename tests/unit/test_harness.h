/*
 * Tiny in-repo test harness. Each test source declares an entry
 * point of the form `int run_<name>(void)` returning 0 on success
 * and non-zero on failure (printing diagnostics to stderr).
 *
 * test_main.c dispatches between them by name so each CTest entry
 * can run a single test, and so a developer can run one test at a
 * time from a single binary.
 */
#ifndef WINBOLO_UNITTEST_HARNESS_H
#define WINBOLO_UNITTEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>

#define UT_FAIL(fmt, ...)                                                   \
    do {                                                                    \
        fprintf(stderr, "FAIL %s:%d: " fmt "\n",                            \
                __FILE__, __LINE__, ##__VA_ARGS__);                         \
        return 1;                                                           \
    } while (0)

#define UT_ASSERT(cond)                                                     \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: assertion failed: %s\n",           \
                    __FILE__, __LINE__, #cond);                             \
            return 1;                                                       \
        }                                                                   \
    } while (0)

#define UT_ASSERT_MSG(cond, fmt, ...)                                       \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s — " fmt "\n",                   \
                    __FILE__, __LINE__, #cond, ##__VA_ARGS__);              \
            return 1;                                                       \
        }                                                                   \
    } while (0)

int run_transport_local_passive_threads(void);
int run_sp_subscriber_delivery(void);
int run_active_local_input_to_shot(void);
int run_sp_shoot_through_timer(void);
int run_ini_writer_persistence(void);
int run_ini_writer_insertion_point(void);
int run_ini_writer_security(void);
int run_lobby_settings_codec_and_apply(void);
int run_lobby_team_meta_codec_and_apply(void);
int run_lobby_bot_config_codec_and_apply(void);
int run_lobby_bot_brain_codec_and_apply(void);
int run_lobby_brain_list_codec_and_apply(void);
int run_upload_filename_safe(void);
int run_lobby_map_list_chunked(void);
int run_lobby_map_search_chunked(void);

/* Build a ready-to-tick ServerSim from the embedded Everard Island map
 * with one player added at slot 0. Caller is responsible for
 * serverSimDestroy. Returns NULL on failure. */
struct ServerSim *ut_make_running_sim(const char *player_name);

#endif /* WINBOLO_UNITTEST_HARNESS_H */
