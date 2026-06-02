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

#ifdef __cplusplus
extern "C" {
#endif

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
int run_command_codec_roundtrip_variants(void);
int run_command_codec_cmdseq_slot(void);
int run_command_codec_bounds_checks(void);
int run_command_rejected_parks_name_codes(void);
int run_command_rejected_ignores_other_slot(void);
int run_command_rejected_clear_resets_both_fields(void);
int run_lobby_add_bot_rejects_empty_brain_path(void);
int run_lobby_add_bot_rejects_ai_none(void);
int run_lobby_add_bot_rejects_not_in_lobby(void);
int run_lobby_add_bot_rejects_non_host_sender(void);
int run_command_queue_first_submit_drains(void);
int run_command_queue_second_submit_does_not_drain(void);
int run_command_queue_ack_drains_pending_tail(void);
int run_command_queue_ack_clearing_queue_skips_drain(void);
int run_game_vote_state_codec_back_to_lobby(void);
int run_game_vote_state_codec_surrender_passed(void);
int run_game_vote_state_decoder_rejects_short(void);
int run_vote_surrender_rejects_unassigned_caller(void);
int run_vote_surrender_starts_for_real_team_caller(void);
int run_vote_surrender_rejects_three_active_teams(void);
int run_vote_toggle_standalone_no_does_nothing(void);
int run_vote_toggle_invalid_mode_dropped(void);
int run_vote_toggle_yes_opens_vote(void);
int run_vote_toggle_no_during_running_records_answered(void);
int run_lobby_set_team_clamps_max_tanks(void);
int run_lobby_set_team_accepts_max_legal(void);
int run_lobby_set_team_accepts_unassigned(void);
int run_lobby_set_team_clamps_above_max(void);
int run_server_text_codec_via_chat_wire(void);
int run_md5_rfc1321_vectors(void);
int run_md5_streaming_matches_oneshot(void);
int run_md5_block_boundaries(void);
int run_ranked_flag_persists_with_one_player(void);
int run_ranked_shape_gate(void);
int run_lobby_lock_bit_lookup(void);
int run_lobby_lock_rejects_settings(void);
int run_lobby_lock_mask_roundtrip(void);
int run_upload_cap_enforced(void);
int run_map_field_clamps_evil(void);
int run_map_field_clamps_passthrough(void);
int run_map_field_clamps_angry_start(void);
int run_map_reload_rollback(void);
int run_upload_busy_predicate(void);
int run_upload_filename_safe(void);
int run_lobby_time_minutes_valid(void);
int run_lobby_bot_name_rejects_reserved_prefix(void);
int run_lobby_bot_name_rejects_disallowed_char(void);
int run_lobby_bot_name_rejects_collision(void);
int run_lobby_bot_name_skip_does_not_bypass_validator_prefix(void);
int run_lobby_bot_name_skip_does_not_bypass_validator_control(void);
int run_lobby_bot_name_skip_excludes_self_from_uniqueness(void);
int run_lobby_bot_name_accepts_clean_unique(void);
int run_lobby_map_list_chunked(void);
int run_lobby_map_search_chunked(void);
int run_wbn_bearer_state(void);
int run_wbn_rekey_codec(void);
int run_wbn_news_parse(void);
int run_wbn_country_cache(void);
int run_brain_crash_log_writes_file(void);
int run_brain_crash_log_falls_back_to_luaptr(void);
int run_brain_inbox_push_peek_fifo(void);
int run_brain_inbox_overflow_drops_oldest(void);
int run_brain_inbox_legacy_drain_fifo(void);
int run_brain_inbox_clear_resets(void);
int run_bolo_rand_golden_sequence(void);

/* Lobby runtime fixes (test_lobby_runtime_fixes.c). */
int run_countdown_abort_publishes_phase(void);
int run_lobby_auto_unready_clears_humans_keeps_bots(void);
int run_mapdir_root_fallback(void);

/* Bot chat routing (test_bot_chat_routing.c) — exercises serverSimApplyCommand
 * with a bot's slot as senderSlot and inspects the recipient ClientSim's
 * brain inbox. Confirms bots send and receive through the same CMD_CHAT
 * dispatcher arm + CTRL_CHAT subscriber funnel as humans. */
int run_bot_chat_send_to_human_lands_in_human_inbox(void);
int run_bot_chat_send_to_other_bot_lands_in_recipient_inbox(void);
int run_bot_chat_receive_from_human_lands_in_bot_inbox(void);
int run_bot_chat_receive_from_other_bot_via_broadcast(void);

/* Internal brain-message routing (test_brain_internal_msg_routing.c) —
 * pins botManagerDeliverInternalMessage: bot brains sending with
 * messagedest=0 fan out into allied bot inboxes only, never the chat
 * wire, never a human's newswire. */
int run_internal_msg_reaches_allied_bot(void);
int run_internal_msg_skips_sender_self(void);
int run_internal_msg_skips_non_allied_bot(void);
int run_internal_msg_skips_inactive_bot_slot(void);
int run_internal_msg_handles_oversized_body(void);
int run_internal_msg_null_inputs_are_noop(void);

/* Subscriber-arm orphan regressions (test_subscriber_arm_orphans.c).
 * Each captures a side effect that was orphaned when its standalone
 * PACKET_* handler was superseded by the unified control bus. */
int run_alliance_request_flags_addressed(void);
int run_alliance_request_ignores_other_slot(void);
int run_phase_lobby_sets_inlobby(void);
int run_phase_gameover_resets_in_game(void);
int run_player_join_appends_lobby_chat(void);
int run_player_join_self_does_not_announce(void);
int run_player_leave_appends_lobby_chat(void);
int run_lobby_settings_clears_balance_proposal(void);

/* Identity-survives-reset regressions (test_identity_survives_reset.c).
 * Connection identity (name, country, clientType, clientFlags incl.
 * PLAYER_FLAG_BOT) must survive the per-round world reset; it was twice
 * dropped on the networked game-start path. */
int run_reset_round_state_preserves_identity(void);
int run_start_game_preserves_client_flags(void);
int run_return_to_lobby_drops_wbn_keeps_identity(void);

/* WBN session-rotation guard (test_wbn_session_rotation.c). The next
 * round's map must never be reported to WBN while the just-finished
 * round's server_key is still live. */
int run_wbn_lobby_update_deferred_during_rotation(void);
int run_wbn_lobby_update_sends_when_not_rotating(void);

/* Lobby/leave cleanup (test_lobby_reset_cleanup.c). Removed slots clear
 * (no phantom re-announce), and the last human leaving a running game
 * returns the server to the lobby. */
int run_remove_player_clears_slot(void);
int run_last_human_leave_returns_to_lobby(void);
int run_humanless_round_does_not_autoend(void);

/* CTRL_ALLIANCE_RESET batched alliance event (test_alliance_reset.c).
 * Replaces the O(N²) per-pair CTRL_ALLIANCE_ACCEPT burst that overflowed
 * the host's reliable control queue at game start with 16 players. */
int run_alliance_reset_codec_roundtrip(void);
int run_alliance_reset_decoder_rejects_short(void);
int run_alliance_reset_reapply_publishes_one_event(void);
int run_alliance_reset_apply_rebuilds_alliances(void);

/* Reliable control-event queue regression tests (test_control_event_queue.c).
 * Each captures a specific bug that shipped during the
 * reliable-control-events rollout. */
int run_queue_init_is_valid(void);
int run_queue_enqueue_advances_nextSeq(void);
int run_queue_ack_advance_within_range(void);
int run_queue_stale_ack_above_nextSeq(void);
int run_queue_wipe_resets_both_seqs(void);
int run_queue_enqueue_into_empty_after_wipe(void);
int run_queue_hasspace_at_capacity(void);
int run_log_roundtrip_basic(void);
int run_log_roundtrip_snapshot_keeps_chain_synced(void);
int run_log_roundtrip_lobby_snapshot_is_empty_world(void);
int run_log_roundtrip_lobby_mode_drops_world_events(void);

/* Build a ready-to-tick ServerSim from the embedded Everard Island map
 * with one player added at slot 0. Caller is responsible for
 * serverSimDestroy. Returns NULL on failure. */
struct ServerSim *ut_make_running_sim(const char *player_name);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_UNITTEST_HARNESS_H */
