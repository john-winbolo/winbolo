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
#include "server_sim_lifecycle.h"

typedef struct {
    const char *name;
    int (*fn)(void);
} UnitTestEntry;

static const UnitTestEntry s_tests[] = {
    { "transport_local_passive_threads", run_transport_local_passive_threads },
    { "sp_subscriber_delivery",          run_sp_subscriber_delivery          },
    { "active_local_input_to_shot",      run_active_local_input_to_shot      },
    { "sp_shoot_through_timer",          run_sp_shoot_through_timer          },
    { "prefs_document_roundtrip",        run_prefs_document_roundtrip        },
    { "prefs_keys_roundtrip",            run_prefs_keys_roundtrip            },
    { "prefs_doc_roundtrip",             run_prefs_doc_roundtrip             },
    { "prefs_doc_defaults",              run_prefs_doc_defaults              },
    { "prefs_doc_special_chars",         run_prefs_doc_special_chars         },
    { "prefs_doc_unknown_preserved",     run_prefs_doc_unknown_preserved     },
    { "prefs_doc_save_atomic",           run_prefs_doc_save_atomic           },
    { "prefs_api_roundtrip",             run_prefs_api_roundtrip             },
    { "prefs_api_defaults",              run_prefs_api_defaults              },
    { "prefs_api_corrupt_backup",        run_prefs_api_corrupt_backup        },
    { "prefs_api_debounce",              run_prefs_api_debounce              },
    { "prefs_api_shutdown_flush",        run_prefs_api_shutdown_flush        },
    { "prefs_api_upload_excludes_local", run_prefs_api_upload_excludes_local },
    { "prefs_api_sync_dirty",            run_prefs_api_sync_dirty            },
    { "prefs_api_device_identity",       run_prefs_api_device_identity       },
    { "prefs_api_adopt_server",          run_prefs_api_adopt_server          },
    { "prefs_api_mark_synced",           run_prefs_api_mark_synced           },
    { "join_running_phase_not_lobby",    run_join_running_phase_not_lobby    },
    { "lobby_settings_codec_and_apply",  run_lobby_settings_codec_and_apply  },
    { "lobby_team_meta_codec_and_apply", run_lobby_team_meta_codec_and_apply },
    { "lobby_bot_config_codec_and_apply",run_lobby_bot_config_codec_and_apply},
    { "lobby_bot_brain_codec_and_apply", run_lobby_bot_brain_codec_and_apply },
    { "lobby_brain_list_codec_and_apply",run_lobby_brain_list_codec_and_apply},
    { "command_codec_roundtrip_variants",run_command_codec_roundtrip_variants},
    { "command_codec_lobby_claim_start", run_command_codec_lobby_claim_start },
    { "lobby_claim_start_host_swaps_occupied",
                                         run_lobby_claim_start_host_swaps_occupied },
    { "lobby_claim_start_host_swap_into_none",
                                         run_lobby_claim_start_host_swap_into_none },
    { "lobby_claim_start_non_host_occupied_rejected",
                                         run_lobby_claim_start_non_host_occupied_rejected },
    { "lobby_claim_start_non_host_other_slot_rejected",
                                         run_lobby_claim_start_non_host_other_slot_rejected },
    { "lobby_claim_start_self_free_and_release",
                                         run_lobby_claim_start_self_free_and_release },
    { "lobby_claim_start_validation",    run_lobby_claim_start_validation },
    { "command_codec_cmdseq_slot",       run_command_codec_cmdseq_slot       },
    { "command_codec_bounds_checks",     run_command_codec_bounds_checks     },
    { "command_rejected_parks_name_codes",        run_command_rejected_parks_name_codes        },
    { "command_rejected_ignores_other_slot",      run_command_rejected_ignores_other_slot      },
    { "command_rejected_clear_resets_both_fields",run_command_rejected_clear_resets_both_fields},
    { "shell_death_codec_roundtrip",              run_shell_death_codec_roundtrip              },
    { "shell_death_culls_matching_predicted_shell",run_shell_death_culls_matching_predicted_shell},
    { "shell_death_rejected_culls_without_impact",run_shell_death_rejected_culls_without_impact},
    { "lobby_add_bot_rejects_empty_brain_path",   run_lobby_add_bot_rejects_empty_brain_path   },
    { "lobby_add_bot_rejects_ai_none",            run_lobby_add_bot_rejects_ai_none            },
    { "lobby_add_bot_rejects_not_in_lobby",       run_lobby_add_bot_rejects_not_in_lobby       },
    { "lobby_add_bot_rejects_non_host_sender",    run_lobby_add_bot_rejects_non_host_sender    },
    { "lobby_bot_count_counts_only_connected_bots", run_lobby_bot_count_counts_only_connected_bots },
    { "lobby_maxbots_cap_boundary",               run_lobby_maxbots_cap_boundary               },
    { "lobby_add_bot_rejects_when_at_maxbots",    run_lobby_add_bot_rejects_when_at_maxbots    },
    { "transfer_host_promotes_target",             run_transfer_host_promotes_target             },
    { "transfer_host_rejects_non_host_sender",     run_transfer_host_rejects_non_host_sender     },
    { "transfer_host_openhost_does_not_grant",     run_transfer_host_openhost_does_not_grant     },
    { "transfer_host_rejects_self",                run_transfer_host_rejects_self                },
    { "transfer_host_rejects_unconnected",         run_transfer_host_rejects_unconnected         },
    { "transfer_host_rejects_bot_target",          run_transfer_host_rejects_bot_target          },
    { "command_queue_first_submit_drains",        run_command_queue_first_submit_drains        },
    { "command_queue_second_submit_does_not_drain", run_command_queue_second_submit_does_not_drain },
    { "command_queue_ack_drains_pending_tail",    run_command_queue_ack_drains_pending_tail    },
    { "command_queue_ack_clearing_queue_skips_drain", run_command_queue_ack_clearing_queue_skips_drain },
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
    { "md5_to_hex",                              run_md5_to_hex                              },
    { "pill_armour_intank_roundtrip",            run_pill_armour_intank_roundtrip            },
    { "base_event_classification",               run_base_event_classification               },
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
    { "map_read_memory_matches_file",            run_map_read_memory_matches_file            },
    { "map_read_memory_handbuilt",               run_map_read_memory_handbuilt               },
    { "map_convert_file_to_compressed",          run_map_convert_file_to_compressed          },
    { "map_read_memory_rejects_garbage",         run_map_read_memory_rejects_garbage         },
    { "upload_busy_predicate",                   run_upload_busy_predicate                   },
    { "upload_filename_safe",                    run_upload_filename_safe                    },
    { "lobby_time_minutes_valid",                run_lobby_time_minutes_valid                },
    { "lobby_bot_name_rejects_reserved_prefix",  run_lobby_bot_name_rejects_reserved_prefix  },
    { "lobby_bot_name_rejects_disallowed_char",  run_lobby_bot_name_rejects_disallowed_char  },
    { "lobby_bot_name_rejects_collision",        run_lobby_bot_name_rejects_collision        },
    { "lobby_bot_name_skip_does_not_bypass_validator_prefix",  run_lobby_bot_name_skip_does_not_bypass_validator_prefix  },
    { "lobby_bot_name_skip_does_not_bypass_validator_control", run_lobby_bot_name_skip_does_not_bypass_validator_control },
    { "lobby_bot_name_skip_excludes_self_from_uniqueness",     run_lobby_bot_name_skip_excludes_self_from_uniqueness     },
    { "lobby_bot_name_accepts_clean_unique",     run_lobby_bot_name_accepts_clean_unique     },
    { "lobby_map_list_chunked",                  run_lobby_map_list_chunked                  },
    { "lobby_map_search_chunked",                run_lobby_map_search_chunked                },
    { "wbn_bearer_state",                        run_wbn_bearer_state                        },
    { "wbn_rekey_codec",                         run_wbn_rekey_codec                         },
    { "wbn_news_parse",                          run_wbn_news_parse                          },
    { "wbn_country_cache",                       run_wbn_country_cache                       },
    { "wbn_prefs_parse_get",                     run_wbn_prefs_parse_get                     },
    { "wbn_prefs_parse_updatedat",               run_wbn_prefs_parse_updatedat               },
    { "wbn_prefs_decide",                        run_wbn_prefs_decide                        },
    { "wbn_prefs_build_put_body",                run_wbn_prefs_build_put_body                },
    { "wbn_serverlist_parse",                    run_wbn_serverlist_parse                    },
    { "wbn_serverlist_players",                  run_wbn_serverlist_players                  },
    { "wbn_serverlist_motd",                     run_wbn_serverlist_motd                     },
    { "wbn_serverlist_malformed",                run_wbn_serverlist_malformed                },
    { "wbn_map_parse",                           run_wbn_map_parse                           },
    { "brain_crash_log_writes_file",             run_brain_crash_log_writes_file             },
    { "brain_crash_log_falls_back_to_luaptr",    run_brain_crash_log_falls_back_to_luaptr    },
    { "brain_inbox_push_peek_fifo",              run_brain_inbox_push_peek_fifo              },
    { "brain_inbox_overflow_drops_oldest",       run_brain_inbox_overflow_drops_oldest       },
    { "brain_inbox_legacy_drain_fifo",           run_brain_inbox_legacy_drain_fifo           },
    { "brain_inbox_clear_resets",                run_brain_inbox_clear_resets                },
    { "bolo_rand_golden_sequence",               run_bolo_rand_golden_sequence               },
    { "net_impair",                              run_net_impair                              },
    { "channel_mux",                             run_channel_mux                             },
    { "bulk_transfer",                           run_bulk_transfer                           },
    { "overflow_guards",                         run_overflow_guards                         },
    { "channel_reset_codec_roundtrip",           run_channel_reset_codec_roundtrip           },
    { "channel_reset_two_slot_baselines",        run_channel_reset_two_slot_baselines        },
    { "countdown_abort_publishes_phase",         run_countdown_abort_publishes_phase         },
    { "lobby_auto_unready_clears_humans_keeps_bots",
                                                 run_lobby_auto_unready_clears_humans_keeps_bots },
    { "mapdir_root_fallback",                    run_mapdir_root_fallback                    },
    { "bot_chat_send_to_human_lands_in_human_inbox",
                                                 run_bot_chat_send_to_human_lands_in_human_inbox },
    { "bot_chat_send_to_other_bot_lands_in_recipient_inbox",
                                                 run_bot_chat_send_to_other_bot_lands_in_recipient_inbox },
    { "bot_chat_receive_from_human_lands_in_bot_inbox",
                                                 run_bot_chat_receive_from_human_lands_in_bot_inbox },
    { "bot_chat_receive_from_other_bot_via_broadcast",
                                                 run_bot_chat_receive_from_other_bot_via_broadcast },
    { "internal_msg_reaches_allied_bot",         run_internal_msg_reaches_allied_bot         },
    { "internal_msg_skips_sender_self",          run_internal_msg_skips_sender_self          },
    { "internal_msg_skips_non_allied_bot",       run_internal_msg_skips_non_allied_bot       },
    { "internal_msg_skips_inactive_bot_slot",    run_internal_msg_skips_inactive_bot_slot    },
    { "internal_msg_handles_oversized_body",     run_internal_msg_handles_oversized_body     },
    { "internal_msg_null_inputs_are_noop",       run_internal_msg_null_inputs_are_noop       },
    { "alliance_request_flags_addressed",        run_alliance_request_flags_addressed        },
    { "alliance_request_ignores_other_slot",     run_alliance_request_ignores_other_slot     },
    { "phase_lobby_sets_inlobby",                run_phase_lobby_sets_inlobby                },
    { "phase_gameover_resets_in_game",           run_phase_gameover_resets_in_game           },
    { "player_join_appends_lobby_chat",          run_player_join_appends_lobby_chat          },
    { "player_join_self_does_not_announce",      run_player_join_self_does_not_announce      },
    { "player_leave_appends_lobby_chat",         run_player_leave_appends_lobby_chat         },
    { "lobby_settings_clears_balance_proposal",  run_lobby_settings_clears_balance_proposal  },
    { "reset_round_state_preserves_identity",    run_reset_round_state_preserves_identity    },
    { "start_game_preserves_client_flags",       run_start_game_preserves_client_flags       },
    { "return_to_lobby_drops_wbn_keeps_identity", run_return_to_lobby_drops_wbn_keeps_identity },
    { "wbn_lobby_update_deferred_during_rotation", run_wbn_lobby_update_deferred_during_rotation },
    { "wbn_lobby_update_sends_when_not_rotating",  run_wbn_lobby_update_sends_when_not_rotating  },
    { "maprotate_restarts_round_and_rearms",       run_maprotate_restarts_round_and_rearms       },
    { "maprotate_defers_wbn_update_until_key_rotated", run_maprotate_defers_wbn_update_until_key_rotated },
    { "maprotate_gameover_is_not_terminal",        run_maprotate_gameover_is_not_terminal        },
    { "maprotate_boot_does_not_rotate_while_empty", run_maprotate_boot_does_not_rotate_while_empty },
    { "maprotate_vote_return_is_not_terminal",     run_maprotate_vote_return_is_not_terminal     },
    { "wbn_join_keyed_on_reauth",                  run_wbn_join_keyed_on_reauth                  },
    { "wbn_join_anonymous_on_grace",               run_wbn_join_anonymous_on_grace               },
    { "wbn_join_idempotent_reauth_no_double",      run_wbn_join_idempotent_reauth_no_double      },
    { "wbn_join_disconnect_drops",                 run_wbn_join_disconnect_drops                 },
    { "wbn_join_rearm_per_session",                run_wbn_join_rearm_per_session                },
    { "wbn_join_rekey_target_gate",                run_wbn_join_rekey_target_gate                },
    { "join_collision_unverified_no_auth_rejects", run_join_collision_unverified_no_auth_rejects },
    { "join_collision_unverified_will_auth_admits", run_join_collision_unverified_will_auth_admits },
    { "join_collision_verified_rejects_will_auth", run_join_collision_verified_rejects_will_auth },
    { "join_collision_verified_flag_irrelevant",   run_join_collision_verified_flag_irrelevant   },
    { "claim_resolve_free_promotes",               run_claim_resolve_free_promotes               },
    { "claim_resolve_unverified_preempts",         run_claim_resolve_unverified_preempts         },
    { "claim_resolve_verified_keeps_temp",         run_claim_resolve_verified_keeps_temp         },
    { "claim_resolve_free_ignores_holder_flag",    run_claim_resolve_free_ignores_holder_flag    },
    { "remove_player_clears_slot",                 run_remove_player_clears_slot                 },
    { "return_to_lobby_clears_phantom_slot",       run_return_to_lobby_clears_phantom_slot       },
    { "last_human_leave_returns_to_lobby",         run_last_human_leave_returns_to_lobby         },
    { "humanless_round_does_not_autoend",          run_humanless_round_does_not_autoend          },
    { "host_departs_promotes_lowest_human",        run_host_departs_promotes_lowest_human        },
    { "nonhost_departs_keeps_host",                run_nonhost_departs_keeps_host                },
    { "host_reassign_skips_bots",                  run_host_reassign_skips_bots                  },
    { "lobby_reset_clears_host_slot",              run_lobby_reset_clears_host_slot              },
    { "autolock_lobby_stays_open",                 run_autolock_lobby_stays_open                 },
    { "autolock_locks_on_start_wire",              run_autolock_locks_on_start_wire              },
    { "autolock_locks_on_start_inplace",           run_autolock_locks_on_start_inplace           },
    { "autolock_join_blocked_after_start",         run_autolock_join_blocked_after_start         },
    { "no_autolock_stays_open_on_start",           run_no_autolock_stays_open_on_start           },
    { "log_roundtrip_basic",                     run_log_roundtrip_basic                     },
    { "log_roundtrip_snapshot_keeps_chain_synced",
                                                 run_log_roundtrip_snapshot_keeps_chain_synced },
    { "log_roundtrip_lobby_snapshot_is_empty_world",
                                                 run_log_roundtrip_lobby_snapshot_is_empty_world },
    { "log_roundtrip_lobby_mode_drops_world_events",
                                                 run_log_roundtrip_lobby_mode_drops_world_events },
    { "log_path_empty_uses_autobase",            run_log_path_empty_uses_autobase            },
    { "log_path_explicit_file_verbatim",         run_log_path_explicit_file_verbatim         },
    { "log_path_explicit_file_keeps_single_wbv", run_log_path_explicit_file_keeps_single_wbv },
    { "log_path_directory_autonames_inside",     run_log_path_directory_autonames_inside     },
    { "log_path_directory_trailing_slash_no_double",
                                                 run_log_path_directory_trailing_slash_no_double },
    { "log_path_directory_arg_appends_wbv",      run_log_path_directory_arg_appends_wbv      },
    { "alliance_reset_codec_roundtrip",          run_alliance_reset_codec_roundtrip          },
    { "alliance_reset_decoder_rejects_short",    run_alliance_reset_decoder_rejects_short    },
    { "alliance_reset_reapply_publishes_one_event",
                                                 run_alliance_reset_reapply_publishes_one_event },
    { "alliance_reset_apply_rebuilds_alliances", run_alliance_reset_apply_rebuilds_alliances },
    { "treegrow_never_plants_on_deep_sea",       run_treegrow_never_plants_on_deep_sea       },
    { "treegrow_reset_clears_stale_target",      run_treegrow_reset_clears_stale_target      },
    { "starts_pick_cluster_nearest_teammate",    run_starts_pick_cluster_nearest_teammate    },
    { "starts_pick_farthest_when_solo",          run_starts_pick_farthest_when_solo          },
    { "starts_pick_none_when_all_taken",         run_starts_pick_none_when_all_taken         },
    { "starts_batch_reserved_lands_exact",       run_starts_batch_reserved_lands_exact       },
    { "starts_batch_unreserved_avoids_reserved", run_starts_batch_unreserved_avoids_reserved },
    { "starts_batch_stale_reservation_falls_through",
                                                 run_starts_batch_stale_reservation_falls_through },
    { "starts_batch_duplicate_honors_first",     run_starts_batch_duplicate_honors_first     },
    { "starts_batch_null_reservations_place_normally",
                                                 run_starts_batch_null_reservations_place_normally },
    { "starts_batch_solo_random_seed",           run_starts_batch_solo_random_seed           },
    { "starts_batch_teams_cluster_and_separate", run_starts_batch_teams_cluster_and_separate },
    { "starts_batch_team_anchor_jitter_varies",  run_starts_batch_team_anchor_jitter_varies  },
    { "starts_open_ideal_friendly_pill_eligible",
                                                 run_starts_open_ideal_friendly_pill_eligible },
    { "input_redundancy",                        run_input_redundancy                        },
    { "edge_send_predicate",                     run_edge_send_predicate                     },
    { "bases_closest_for_player",                run_bases_closest_for_player                },
    { "base_stock_visibility",                   run_base_stock_visibility                   },
    { "stall_advances_processed_tick",           run_stall_advances_processed_tick           },
    { "stall_mine_late_lays_once",               run_stall_mine_late_lays_once               },
    { "stall_mine_duplicate_not_relaid",         run_stall_mine_duplicate_not_relaid         },
    { "stall_fire_not_harvested",                run_stall_fire_not_harvested                },
    { "stall_never_fires",                       run_stall_never_fires                       },
    { "stall_brief_trough_no_advance",           run_stall_brief_trough_no_advance           },
    { "stall_long_dry_advances",                 run_stall_long_dry_advances                 },
    { "input_catchup",                           run_input_catchup                           },
    { "catchup_ignores_redundant_duplicates",    run_catchup_ignores_redundant_duplicates    },
    { "jitter_buffer_grow",                      run_jitter_buffer_grow                      },
    { "shell_projection",                        run_shell_projection                        },
    { "ping_smoother",                           run_ping_smoother                           },
    { "client_timing",                           run_client_timing                           },
    { "interp_render",                           run_interp_render                           },
    { "interp_respawn_no_death_flash",           run_interp_respawn_no_death_flash           },
    { "snapshot_compaction",                     run_snapshot_compaction                     },
    { "error_smoothing",                         run_error_smoothing                         },
    { "loopback_join",                           run_loopback_join                           },
    { "loopback_join_loss",                      run_loopback_join_loss                      },
    { "loopback_lobby_running_loss",             run_loopback_lobby_running_loss             },
    { "loopback_quiet_lobby_control_loss",       run_loopback_quiet_lobby_control_loss       },
    { "loopback_channel",                        run_loopback_channel                        },
    { "lock_channel",                            run_lock_channel                            },
    { "loopback_map_preview",                    run_loopback_map_preview                    },
    { "loopback_map_upload",                     run_loopback_map_upload                     },
    { "loopback_download_join",                  run_loopback_download_join                  },
    { "loopback_download_midgame",               run_loopback_download_midgame               },
    { "loopback_resync",                         run_loopback_resync                         },
    { "gate1_viewtick_loopback",                 run_gate1_viewtick_loopback                 },
    { "gate1_host_noop",                         run_gate1_host_noop                         },
    { "interp_jitter_e2e",                       run_interp_jitter_e2e                       },
    { "join_version_gate",                       run_join_version_gate                       },
    { "join_rate_limit",                         run_join_rate_limit                         },
    { "cookie_handshake",                        run_cookie_handshake                        },
    { "map_amp_gate",                            run_map_amp_gate                            },
    { "map_resync_cut_and_deliver_once",         run_map_resync_cut_and_deliver_once         },
    { "map_resync_duplicate_request_no_recut",   run_map_resync_duplicate_request_no_recut   },
    { "map_resync_send_gate_holds",              run_map_resync_send_gate_holds              },
    { "map_resync_stale_gen_rejected",           run_map_resync_stale_gen_rejected           },
    { "conn_migration_rehome",                   run_conn_migration_rehome                   },
    { "conn_migration_e2e",                      run_conn_migration_e2e                      },
    { "viewtick_rewind",                         run_viewtick_rewind                         },
    { "viewtick_displayed_tick",                 run_viewtick_displayed_tick                 },
    { "wire_corpus",                             run_wire_corpus                             },
    { "wire_corpus_capture",                     run_wire_corpus_capture                     },
    { "packet_type_names",                       run_packet_type_names                       },
    { "mdns_discovery",                          run_mdns_discovery                          },
    { "client_type_matches_platform",            run_client_type_matches_platform            },
    { "client_type_name_round_trips",            run_client_type_name_round_trips            },
    { "players_oob_index_safe",                  run_players_oob_index_safe                  },
    { "control_oob_player_dropped",              run_control_oob_player_dropped              },
    { "control_overflow_defers_disconnect",      run_control_overflow_defers_disconnect      },
    { "input_gate_taxonomy",                     run_input_gate_taxonomy                     },
    { "addrparse_host_only",                     run_addrparse_host_only                     },
    { "addrparse_host_port",                     run_addrparse_host_port                     },
    { "addrparse_scheme",                        run_addrparse_scheme                        },
    { "addrparse_trailing_path",                 run_addrparse_trailing_path                 },
    { "addrparse_whitespace",                    run_addrparse_whitespace                    },
    { "addrparse_port_bounds",                   run_addrparse_port_bounds                   },
    { "addrparse_bad_port",                      run_addrparse_bad_port                      },
    { "addrparse_empty",                         run_addrparse_empty                         },
#ifdef WB_NETDEBUG
    { "netdebug_commanded_vs_executed",          run_netdebug_commanded_vs_executed          },
    { "netdebug_overshoot_under_loss",           run_netdebug_overshoot_under_loss           },
    { "netdebug_mine_once_under_loss",           run_netdebug_mine_once_under_loss           },
    { "netdebug_error_offset_clamped",           run_netdebug_error_offset_clamped           },
#endif
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
