/*
 * WinBoloUnitTests entry point. Dispatches one named test per
 * invocation so each CTest entry maps to a single test case.
 *
 *   WinBoloUnitTests --test <name>      run a single test
 *   WinBoloUnitTests --list             print test names
 *   WinBoloUnitTests                    run every test in sequence
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "test_harness.h"

#include "everard_map.h"
#include "server_sim.h"

typedef struct {
    const char *name;
    int (*fn)(void);
} UnitTestEntry;

static const UnitTestEntry s_tests[] = {
    { "transport_local_passive_threads", run_transport_local_passive_threads },
    { "sp_subscriber_delivery",          run_sp_subscriber_delivery          },
    { "active_local_input_to_shot",      run_active_local_input_to_shot      },
    { "sp_shoot_through_timer",          run_sp_shoot_through_timer          },
    { "ini_writer_persistence",          run_ini_writer_persistence          },
    { "ini_writer_insertion_point",      run_ini_writer_insertion_point      },
    { "ini_writer_security",             run_ini_writer_security             },
    { "lobby_settings_codec_and_apply",  run_lobby_settings_codec_and_apply  },
    { "lobby_team_meta_codec_and_apply", run_lobby_team_meta_codec_and_apply },
    { "lobby_bot_config_codec_and_apply",run_lobby_bot_config_codec_and_apply},
    { "lobby_bot_brain_codec_and_apply", run_lobby_bot_brain_codec_and_apply },
    { "lobby_brain_list_codec_and_apply",run_lobby_brain_list_codec_and_apply},
    { "game_vote_state_codec_back_to_lobby",     run_game_vote_state_codec_back_to_lobby     },
    { "game_vote_state_codec_surrender_passed",  run_game_vote_state_codec_surrender_passed  },
    { "game_vote_state_decoder_rejects_short",   run_game_vote_state_decoder_rejects_short   },
    { "vote_surrender_rejects_unassigned_caller",   run_vote_surrender_rejects_unassigned_caller   },
    { "vote_surrender_starts_for_real_team_caller", run_vote_surrender_starts_for_real_team_caller },
    { "vote_surrender_rejects_three_active_teams",  run_vote_surrender_rejects_three_active_teams  },
    { "vote_toggle_standalone_no_does_nothing",            run_vote_toggle_standalone_no_does_nothing            },
    { "vote_toggle_invalid_mode_dropped",                  run_vote_toggle_invalid_mode_dropped                  },
    { "vote_toggle_yes_opens_vote",                        run_vote_toggle_yes_opens_vote                        },
    { "vote_toggle_no_during_running_records_answered",    run_vote_toggle_no_during_running_records_answered    },
    { "lobby_set_team_clamps_max_tanks",            run_lobby_set_team_clamps_max_tanks            },
    { "lobby_set_team_accepts_max_legal",           run_lobby_set_team_accepts_max_legal           },
    { "lobby_set_team_accepts_unassigned",          run_lobby_set_team_accepts_unassigned          },
    { "lobby_set_team_clamps_above_max",            run_lobby_set_team_clamps_above_max            },
    { "server_text_codec_via_chat_wire",         run_server_text_codec_via_chat_wire         },
    { "md5_rfc1321_vectors",                     run_md5_rfc1321_vectors                     },
    { "md5_streaming_matches_oneshot",           run_md5_streaming_matches_oneshot           },
    { "md5_block_boundaries",                    run_md5_block_boundaries                    },
    { "ranked_flag_persists_with_one_player",    run_ranked_flag_persists_with_one_player    },
    { "ranked_shape_gate",                       run_ranked_shape_gate                       },
    { "lobby_lock_bit_lookup",                   run_lobby_lock_bit_lookup                   },
    { "lobby_lock_rejects_settings",             run_lobby_lock_rejects_settings             },
    { "lobby_lock_mask_roundtrip",               run_lobby_lock_mask_roundtrip               },
    { "upload_cap_enforced",                     run_upload_cap_enforced                     },
    { "map_field_clamps_evil",                   run_map_field_clamps_evil                   },
    { "map_field_clamps_passthrough",            run_map_field_clamps_passthrough            },
    { "map_field_clamps_angry_start",            run_map_field_clamps_angry_start            },
    { "map_reload_rollback",                     run_map_reload_rollback                     },
    { "upload_busy_predicate",                   run_upload_busy_predicate                   },
    { "upload_filename_safe",                    run_upload_filename_safe                    },
    { "lobby_time_minutes_valid",                run_lobby_time_minutes_valid                },
    { "lobby_map_list_chunked",                  run_lobby_map_list_chunked                  },
    { "lobby_map_search_chunked",                run_lobby_map_search_chunked                },
    { "wbn_bearer_state",                        run_wbn_bearer_state                        },
    { "wbn_rekey_codec",                         run_wbn_rekey_codec                         },
    { "wbn_news_parse",                          run_wbn_news_parse                          },
    { "wbn_country_cache",                       run_wbn_country_cache                       },
};
#define NUM_TESTS ((int)(sizeof(s_tests) / sizeof(s_tests[0])))

/* Defined in test_stubs.c — every binary that links ClientSim needs to
 * own this symbol because the engine reads it from menu-aware paths. */
extern bool isInMenu;

ServerSim *ut_make_running_sim(const char *player_name) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, 0, player_name, false);
    return sim;
}

static int run_one(const char *name) {
    int i;
    for (i = 0; i < NUM_TESTS; i++) {
        if (strcmp(s_tests[i].name, name) == 0) {
            fprintf(stderr, "RUN  %s\n", s_tests[i].name);
            int rc = s_tests[i].fn();
            fprintf(stderr, "%s %s\n", rc == 0 ? "PASS" : "FAIL",
                    s_tests[i].name);
            return rc;
        }
    }
    fprintf(stderr, "unknown test: %s\n", name);
    return 2;
}

int main(int argc, char **argv) {
    /* SDL_Init(0) brings up the SDL3 runtime so SDL_CreateMutex /
     * SDL_CreateThread are usable from the unit tests without
     * dragging in any subsystem-specific init. */
    if (!SDL_Init(0)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 3;
    }
    (void)isInMenu;

    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        int i;
        for (i = 0; i < NUM_TESTS; i++) {
            printf("%s\n", s_tests[i].name);
        }
        SDL_Quit();
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "--test") == 0) {
        int rc = run_one(argv[2]);
        SDL_Quit();
        return rc;
    }

    /* Default: run them all in sequence. */
    int fail = 0;
    int i;
    for (i = 0; i < NUM_TESTS; i++) {
        if (run_one(s_tests[i].name) != 0) {
            fail = 1;
        }
    }
    SDL_Quit();
    return fail;
}
