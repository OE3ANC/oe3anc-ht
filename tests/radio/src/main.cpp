// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/companion.h>
#include <ht/emulator.hpp>
#include <string.h>
#include <zephyr/ztest.h>

using namespace ht;

static void before_test(void *) {
    radio_power(true);
    zassert_ok(radio_start(RadioConfig{}));
}

static void execute(RadioCommand command) {
    if (command.kind == CommandKind::QuickControls) {
        const auto state = radio_snapshot();
        command.expected_generation = state.generation;
        command.expected_revision = state.configuration_revision;
        command.selection = state.selection;
    }
    zassert_ok(radio_submit(command));
    radio_service();
}

ZTEST(radio, test_selection_identity_global_edits_and_tuning_into_vfo) {
    const Selection selected{Operating::Memory, 3, 19};
    zassert_ok(radio_start({}, selected));
    zassert_true(same_selection(radio_snapshot().selection, selected));
    RadioCommand command;
    command.config = radio_snapshot().config;
    strcpy(command.config.callsign, "OE3ANC");
    command.config.transmit_limit_s = 120;
    execute(command);
    zassert_ok(radio_snapshot().command_error);
    zassert_true(same_selection(radio_snapshot().selection, selected));
    command.config.rx_frequency_hz = command.config.tx_frequency_hz = 145500000;
    execute(command);
    zassert_ok(radio_snapshot().command_error);
    zassert_equal(radio_snapshot().selection.operating, Operating::Vfo);
    zassert_equal(radio_snapshot().selection.channel_id, 19);
    zassert_equal(radio_snapshot().selection.bank_id, 3);
}

ZTEST(radio, test_metadata_edit_authorizes_without_rf_retune_or_monitor_cancellation) {
    zassert_ok(radio_start({}, {Operating::Memory, 1, 4}));
    radio_monitor(true);
    radio_service();
    zassert_true(radio_snapshot().monitor_active);
    const auto previous = radio_snapshot();
    RadioCommand edit;
    edit.kind = CommandKind::Edit;
    edit.id = 700;
    edit.expected_generation = previous.generation;
    edit.expected_revision = previous.configuration_revision;
    edit.selection = {Operating::Memory, 0, 4};
    edit.config.rx_frequency_hz = 0; // Metadata does not apply RF fields.
    emulator_fail_next(-EIO);
    execute(edit);
    zassert_ok(radio_snapshot().recall_error);
    zassert_equal(radio_snapshot().selection.bank_id, 0);
    zassert_true(radio_snapshot().monitor_active);
    zassert_equal(radio_snapshot().configuration_revision, previous.configuration_revision + 1);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, previous.config.rx_frequency_hz);
    // The failure remains queued until an actual backend configure is attempted.
    RadioCommand configure;
    configure.config = previous.config;
    execute(configure);
    zassert_equal(radio_snapshot().command_error, -EIO);
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
}

ZTEST(radio, test_metadata_edit_rejects_tx_invalid_identity_and_stale_revision) {
    const auto initial = radio_snapshot();
    RadioCommand edit;
    edit.kind = CommandKind::Edit;
    edit.id = 701;
    edit.expected_generation = initial.generation;
    edit.expected_revision = initial.configuration_revision;
    edit.selection.operating = Operating::Memory; // Missing stable ID.
    execute(edit);
    zassert_equal(radio_snapshot().recall_error, -EINVAL);
    edit.selection.channel_id = 4;
    radio_ptt(true);
    radio_service();
    execute(edit);
    zassert_equal(radio_snapshot().recall_error, -EBUSY);
    zassert_true(emulator_transmitting());
    zassert_equal(radio_snapshot().selection.operating, Operating::Vfo);
    radio_ptt(false);
    radio_service();
    execute(edit);
    zassert_ok(radio_snapshot().recall_error);
    edit.id = 702;
    execute(edit);
    zassert_equal(radio_snapshot().recall_error, -ESTALE);
    zassert_equal(radio_snapshot().selection.channel_id, 4);
}

ZTEST(radio, test_owner_ack_cannot_complete_colliding_ordinary_ui_command_id) {
    RadioCommand previous;
    previous.id = 3;
    execute(previous);
    zassert_ok(radio_snapshot().command_error);
    const auto state = radio_snapshot();
    RadioCommand edit;
    edit.kind = CommandKind::Edit;
    edit.id = 4;
    edit.expected_generation = state.generation;
    edit.expected_revision = state.configuration_revision;
    edit.selection = state.selection;
    RadioCommand ordinary;
    ordinary.id = 4;
    ordinary.config.rx_frequency_hz = ordinary.config.tx_frequency_hz = 145500000;
    zassert_ok(radio_submit(edit));
    zassert_ok(radio_submit(ordinary));
    radio_service();
    zassert_equal(radio_snapshot().recall_id, 4);
    zassert_equal(radio_snapshot().command_id, 3); // UI id4 is still pending.
    radio_service();
    zassert_equal(radio_snapshot().command_id, 4);
    zassert_ok(radio_snapshot().command_error);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 145500000);
    zassert_equal(radio_snapshot().recall_id, 4);
}

ZTEST(radio, test_recall_revision_guards_retained_ack_and_failure_identity) {
    const auto initial = radio_snapshot();
    RadioCommand recall;
    recall.kind = CommandKind::Recall;
    recall.id = 17;
    recall.config = initial.config;
    recall.selection = {Operating::Memory, 1, 2};
    recall.expected_generation = initial.generation;
    recall.expected_revision = initial.configuration_revision;
    execute(recall);
    zassert_ok(radio_snapshot().recall_error);
    zassert_equal(radio_snapshot().recall_id, 17);
    zassert_equal(radio_snapshot().configuration_revision, initial.configuration_revision + 1);
    RadioCommand ordinary;
    ordinary.id = 18;
    ordinary.config = radio_snapshot().config;
    execute(ordinary);
    zassert_equal(radio_snapshot().command_id, 18);
    zassert_equal(radio_snapshot().recall_id, 17);
    zassert_ok(radio_snapshot().recall_error);
    recall.id = 19; // Same old view, already superseded by a successful configure.
    execute(recall);
    zassert_equal(radio_snapshot().recall_error, -ESTALE);
    zassert_equal(radio_snapshot().selection.channel_id, 2);
    recall.expected_revision = radio_snapshot().configuration_revision;
    recall.selection.channel_id = 3;
    emulator_fail_next(-EIO);
    execute(recall);
    zassert_equal(radio_snapshot().recall_error, -EIO);
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    zassert_equal(radio_snapshot().selection.channel_id, 2);
    zassert_false(emulator_transmitting());
}

ZTEST(radio, test_quick_controls_only_apply_gain_and_squelch_with_tx_exclusion) {
    const Selection selected{Operating::Memory, 0, 4};
    zassert_ok(radio_start({}, selected));
    RadioCommand quick;
    quick.kind = CommandKind::QuickControls;
    quick.config.squelch = 13;
    quick.config.rx_frequency_hz = 0;
    quick.config.mode = static_cast<Mode>(255); // Ignored non-quick fields.
    execute(quick);
    zassert_ok(radio_snapshot().command_error);
    zassert_equal(radio_snapshot().config.squelch, 13);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 430000000);
    zassert_true(same_selection(radio_snapshot().selection, selected));
    quick.config.squelch = 16;
    execute(quick);
    zassert_equal(radio_snapshot().command_error, -EINVAL);
    zassert_equal(radio_snapshot().config.squelch, 13);
    quick.config.squelch = 3;
    radio_ptt(true);
    radio_service();
    execute(quick);
    zassert_equal(radio_snapshot().command_error, -EBUSY);
    zassert_true(emulator_transmitting());
    radio_ptt(false);
    radio_service();
    RadioCommand digital;
    digital.config = radio_snapshot().config;
    digital.config.mode = Mode::M17;
    execute(digital);
    zassert_ok(radio_snapshot().command_error);
    execute(quick);
    zassert_equal(radio_snapshot().command_error, -ENOTSUP);
    zassert_equal(radio_snapshot().config.squelch, 13);
}

ZTEST(radio, test_recall_invalid_identity_and_changed_lifecycle) {
    RadioCommand recall;
    recall.kind = CommandKind::Recall;
    recall.id = 28;
    recall.expected_generation = radio_snapshot().generation;
    recall.expected_revision = radio_snapshot().configuration_revision;
    recall.selection.operating = Operating::Memory;
    execute(recall);
    zassert_equal(radio_snapshot().recall_error, -EINVAL);
    zassert_equal(radio_snapshot().selection.operating, Operating::Vfo);
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    recall.selection.channel_id = 4;
    radio_power(false);
    radio_service();
    radio_power(true);
    radio_service();
    execute(recall);
    zassert_equal(radio_snapshot().recall_error, -ESTALE);
    zassert_equal(radio_snapshot().selection.operating, Operating::Vfo);
}

ZTEST(radio, test_typed_tone_validation_and_independent_polarity) {
    zassert_true(backend_capabilities().dcs);
    RadioCommand change;
    change.config.rx_tone = {ToneKind::Dcs, 0023, true};
    change.config.tx_tone = {ToneKind::Ctcss, 885, false};
    execute(change);
    zassert_ok(radio_snapshot().command_error);
    zassert_true(same_tone(radio_snapshot().config.rx_tone, change.config.rx_tone));
    radio_ptt(true);
    radio_service();
    zassert_true(emulator_transmitting());
    change.config.tx_tone = {ToneKind::Dcs, 0754, false};
    execute(change);
    zassert_equal(radio_snapshot().command_error, -EBUSY);
    radio_ptt(false);
    radio_service();
    execute(change);
    zassert_ok(radio_snapshot().command_error);
    zassert_true(same_tone(radio_snapshot().config.tx_tone, change.config.tx_tone));
    const Tone invalid[] = {{ToneKind::None, 23, false},           {ToneKind::None, 0, true},
                            {ToneKind::Ctcss, 885, true},          {ToneKind::Ctcss, 669, false},
                            {ToneKind::Ctcss, 2542, false},        {ToneKind::Dcs, 01000, false},
                            {static_cast<ToneKind>(255), 0, false}};
    for (const auto &tone : invalid) {
        change.config.tx_tone = tone;
        execute(change);
        zassert_equal(radio_snapshot().command_error, -EINVAL);
        zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
        zassert_equal(radio_snapshot().config.tx_tone.value, 0754);
    }
}

ZTEST(radio, test_simplex_ptt_and_configuration_exclusion) {
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    zassert_false(emulator_transmitting());
    radio_ptt(true);
    radio_service();
    zassert_true(emulator_transmitting());
    RadioCommand change;
    change.id = 42;
    change.config.rx_frequency_hz = change.config.tx_frequency_hz = 145500000;
    execute(change);
    const auto state = radio_snapshot();
    zassert_equal(state.command_error, -EBUSY);
    zassert_equal(state.command_id, 42);
    zassert_equal(state.config.rx_frequency_hz, 430000000);
    radio_ptt(false);
    radio_service();
    zassert_false(emulator_transmitting());
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    execute(change);
    zassert_ok(radio_snapshot().command_error);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 145500000);
}

ZTEST(radio, test_release_survives_full_command_queue) {
    radio_ptt(true);
    radio_service();
    RadioCommand command;
    for (int i = 0; i < 8; ++i) {
        zassert_ok(radio_submit(command));
    }
    zassert_not_equal(radio_submit(command), 0);
    radio_ptt(false);
    radio_service();
    zassert_false(emulator_transmitting());
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
}

ZTEST(radio, test_rapid_release_repress_still_unkeys) {
    radio_ptt(true);
    radio_service();
    emulator_fail_next(-EIO);
    radio_ptt(false);
    radio_ptt(true);
    radio_service();
    // The release must attempt RX (which we fail), even though latest input
    // is pressed again. Merely sampling the latest level would stay keyed.
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    zassert_false(emulator_transmitting());
}

ZTEST(radio, test_startup_failure_is_visible_and_unkeyed) {
    emulator_fail_next(-ETIMEDOUT);
    zassert_equal(radio_start(RadioConfig{}), -ETIMEDOUT);
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    zassert_false(emulator_transmitting());
}

ZTEST(radio, test_callsign_required_only_for_m17_transmit) {
    RadioCommand change;
    change.config.mode = Mode::M17;
    change.id = 7;
    execute(change);
    zassert_ok(radio_snapshot().command_error);
    radio_ptt(true);
    radio_service();
    zassert_false(emulator_transmitting());
    zassert_equal(radio_snapshot().ptt_error, -EADDRNOTAVAIL);
    zassert_equal(radio_snapshot().command_id, 7);
    zassert_ok(radio_snapshot().command_error);
    // Editing a callsign while PTT remains held must not unexpectedly key up.
    strcpy(change.config.callsign, "OE3ANC");
    change.id = 8;
    execute(change);
    zassert_equal(radio_snapshot().ptt_error, -EADDRNOTAVAIL);
    zassert_equal(radio_snapshot().command_id, 8);
    zassert_ok(radio_snapshot().command_error);
    zassert_false(emulator_transmitting());
    radio_ptt(false);
    radio_service();
    radio_ptt(true);
    radio_service();
    zassert_true(emulator_transmitting());
}

ZTEST(radio, test_ptt_rejection_survives_command_completion_in_same_tick) {
    RadioCommand change;
    change.config.mode = Mode::M17;
    change.id = 7;
    execute(change);
    radio_ptt(true);
    change.id = 8;
    strcpy(change.config.callsign, "OE3ANC");
    execute(change);
    const auto state = radio_snapshot();
    zassert_equal(state.ptt_error, -EADDRNOTAVAIL);
    zassert_equal(state.command_id, 8);
    zassert_ok(state.command_error);
    zassert_false(emulator_transmitting());
}

ZTEST(radio, test_rejects_bad_configuration_without_interrupting_receive) {
    RadioCommand change;
    change.config.rx_frequency_hz = change.config.tx_frequency_hz = 300000000;
    execute(change);
    zassert_equal(radio_snapshot().command_error, -EINVAL);
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    change.config = {};
    memset(change.config.callsign, 'A', sizeof(change.config.callsign));
    execute(change);
    zassert_equal(radio_snapshot().command_error, -EINVAL);
    change.config = {};
    change.config.mode = static_cast<Mode>(99);
    execute(change);
    zassert_equal(radio_snapshot().command_error, -EINVAL);
    change.config = {};
    change.config.gain = 1;
    execute(change);
    zassert_equal(radio_snapshot().command_error, -ENOTSUP);
    change.config = {};
    change.config.tx_tone = {ToneKind::Ctcss, 100, false};
    execute(change);
    zassert_equal(radio_snapshot().command_error, -EINVAL);
}

ZTEST(radio, test_diagnostics_are_exclusive_and_temporary) {
    RadioCommand command;
    command.kind = CommandKind::WriteRegister;
    command.register_address = 0x40;
    command.register_value = 0xbeef;
    execute(command);
    zassert_equal(radio_snapshot().command_error, -EBUSY);
    command.kind = CommandKind::EnterDiagnostics;
    execute(command);
    zassert_equal(radio_snapshot().phase, RadioPhase::Diagnostics);
    radio_ptt(true);
    radio_service();
    zassert_false(emulator_transmitting());
    command.kind = CommandKind::ExitDiagnostics;
    execute(command);
    zassert_equal(radio_snapshot().command_error, -EBUSY);
    radio_ptt(false);
    radio_service();
    command.kind = CommandKind::WriteRegister;
    execute(command);
    command.kind = CommandKind::ReadRegister;
    execute(command);
    zassert_equal(radio_snapshot().register_value, 0xbeef);
    command.register_address = 0x80;
    execute(command);
    zassert_equal(radio_snapshot().command_error, -EINVAL);
    command.kind = CommandKind::ExitDiagnostics;
    execute(command);
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    command.kind = CommandKind::EnterDiagnostics;
    execute(command);
    command.kind = CommandKind::ReadRegister;
    command.register_address = 0x40;
    execute(command);
    zassert_equal(radio_snapshot().register_value, 0);
}

ZTEST(radio, test_fault_unkeys_and_latches_until_restart) {
    radio_ptt(true);
    radio_service();
    BackendStatus failure;
    failure.error = -ETIMEDOUT;
    emulator_inject(failure);
    radio_service();
    zassert_false(emulator_transmitting());
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    zassert_equal(radio_snapshot().fault, -ETIMEDOUT);
    emulator_inject(BackendStatus{});
    radio_ptt(false);
    radio_service();
    radio_ptt(true);
    radio_service();
    RadioCommand command;
    execute(command);
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    zassert_equal(radio_snapshot().command_error, -ETIMEDOUT);
    zassert_false(emulator_transmitting());
}

ZTEST(radio, test_backend_setup_failure_does_not_commit_configuration) {
    emulator_fail_next(-EIO);
    RadioCommand command;
    command.config.rx_frequency_hz = command.config.tx_frequency_hz = 145500000;
    execute(command);
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 430000000);
    zassert_false(emulator_transmitting());
}

ZTEST(radio, test_received_activity_and_typed_events) {
    RadioCommand command;
    command.config.mode = Mode::M17;
    strcpy(command.config.callsign, "OE3ANC");
    command.id = 7;
    execute(command);
    BackendStatus received;
    received.rx_active = true;
    received.rssi_dbm = -85;
    strcpy(received.callsign, "OE1TEST");
    emulator_inject(received);
    radio_service();
    const auto state = radio_snapshot();
    zassert_true(state.rx_active);
    zassert_equal(state.rssi_dbm, -85);
    zassert_equal(strcmp(state.received_callsign, "OE1TEST"), 0);
    zassert_equal(state.command_id, 7);
    zassert_ok(state.command_error);
}

ZTEST(radio, test_peripheral_fault_bypasses_full_queue_and_preserves_first_error) {
    radio_ptt(true);
    radio_service();
    zassert_true(emulator_transmitting());
    RadioCommand command;
    for (unsigned i = 0; i < 8; ++i) {
        zassert_ok(radio_submit(command));
    }
    radio_report_fault(-ETIMEDOUT);
    radio_report_fault(-EIO);
    radio_ptt(true); // Another producer re-presses before the owner handles fault.
    zassert_false(radio_ptt_requested());
    radio_service();
    zassert_false(emulator_transmitting());
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    zassert_equal(radio_snapshot().fault, -ETIMEDOUT);
    radio_ptt(true);
    radio_service();
    zassert_false(emulator_transmitting());
    zassert_equal(radio_snapshot().fault, -ETIMEDOUT);
}

ZTEST(radio, test_split_frequency_inhibit_and_fresh_press) {
    RadioCommand command;
    command.config.rx_frequency_hz = 439075000;
    command.config.tx_frequency_hz = 431475000;
    execute(command);
    zassert_ok(radio_snapshot().command_error);
    zassert_equal(emulator_tuned_frequency(), 439075000);
    radio_ptt(true);
    radio_service();
    zassert_true(emulator_transmitting());
    zassert_equal(emulator_tuned_frequency(), 431475000);
    radio_ptt(false);
    radio_service();
    zassert_equal(emulator_tuned_frequency(), 439075000);
    command.config.tx_inhibit = true;
    execute(command);
    radio_ptt(true);
    radio_service();
    zassert_equal(radio_snapshot().ptt_error, -EPERM);
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    zassert_false(emulator_transmitting());
    zassert_equal(emulator_tuned_frequency(), 439075000);
    command.config.tx_inhibit = false;
    execute(command); // Editing while held cannot unexpectedly enable TX.
    zassert_false(emulator_transmitting());
    radio_ptt(false);
    radio_service();
    radio_ptt(true);
    radio_service();
    zassert_true(emulator_transmitting());
    radio_ptt(false);
    radio_service();
    command.config.tx_frequency_hz = 300000000;
    command.config.tx_inhibit = true;
    execute(command);
    zassert_equal(radio_snapshot().command_error, -EINVAL);
    zassert_equal(emulator_tuned_frequency(), 439075000);
}

ZTEST(radio, test_m17_destination_can_validation_and_copied_config) {
    RadioCommand command;
    command.config.mode = Mode::M17;
    strcpy(command.config.callsign, "OE3ANC");
    command.config.m17.destination = Destination::Station;
    strcpy(command.config.m17.callsign, "OE1TEST");
    command.config.m17.can = 15;
    command.config.m17.rx_can_check = true;
    execute(command);
    zassert_ok(radio_snapshot().command_error);
    command.config.m17.can = 16;
    execute(command);
    zassert_equal(radio_snapshot().command_error, -EINVAL);
    zassert_equal(radio_snapshot().config.m17.can, 15);
    command.config.m17.can = 0;
    strcpy(command.config.m17.callsign, "ALL");
    execute(command);
    zassert_equal(radio_snapshot().command_error, -EINVAL);
    command.config.m17 = {};
    command.config.m17.destination = Destination::Station;
    execute(command);
    zassert_equal(radio_snapshot().command_error, -EINVAL);
    memset(command.config.m17.callsign, 'A', 10);
    execute(command);
    zassert_equal(radio_snapshot().command_error, -EINVAL);
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    zassert_equal(strcmp(radio_snapshot().config.m17.callsign, "OE1TEST"), 0);
    radio_ptt(true);
    radio_service();
    zassert_true(emulator_transmitting());
    command.config.m17 = {};
    execute(command);
    zassert_equal(radio_snapshot().command_error, -EBUSY);
    zassert_equal(radio_snapshot().config.m17.can, 15);
    radio_ptt(false);
    radio_service();
}

ZTEST(radio, test_transmit_limits_warn_expire_and_require_release) {
    const uint16_t limits[] = {60, 120, 180};
    for (uint16_t limit : limits) {
        RadioConfig config;
        config.transmit_limit_s = limit;
        zassert_ok(radio_start(config));
        radio_ptt(true);
        radio_service();
        zassert_true(emulator_transmitting());
        zassert_equal(radio_snapshot().tx_remaining_s, limit);
        k_sleep(K_SECONDS(limit - 11));
        radio_ptt(true);
        radio_service(); // Repeated presses must not restart timing.
        zassert_false(radio_snapshot().tx_warning);
        k_sleep(K_SECONDS(2));
        radio_service();
        zassert_true(radio_snapshot().tx_warning);
        zassert_true(radio_snapshot().tx_remaining_s <= 9);
        RadioCommand command;
        command.config = config;
        for (unsigned i = 0; i < 8; ++i) {
            zassert_ok(radio_submit(command));
        }
        k_sleep(K_SECONDS(9));
        radio_service();
        const auto state = radio_snapshot();
        zassert_equal(state.phase, RadioPhase::Receiving);
        zassert_true(state.tx_timed_out);
        zassert_equal(state.ptt_error, -ETIMEDOUT);
        zassert_ok(state.fault);
        zassert_equal(state.tx_remaining_s, 0);
        zassert_false(state.tx_warning);
        zassert_false(emulator_transmitting());
        // Accepted configuration and repeated held samples cannot clear lockout.
        radio_ptt(true);
        radio_service();
        zassert_true(radio_snapshot().tx_timed_out);
        zassert_false(emulator_transmitting());
        radio_ptt(false);
        radio_service();
        zassert_false(radio_snapshot().tx_timed_out);
        zassert_ok(radio_snapshot().ptt_error);
        radio_ptt(true);
        radio_service();
        zassert_true(emulator_transmitting());
        zassert_equal(radio_snapshot().tx_remaining_s, limit);
    }
}

ZTEST(radio, test_timeout_disabled_and_invalid_values) {
    RadioCommand command;
    command.config.transmit_limit_s = 59;
    execute(command);
    zassert_equal(radio_snapshot().command_error, -EINVAL);
    zassert_equal(radio_snapshot().config.transmit_limit_s, 180);
    command.config.transmit_limit_s = 0;
    execute(command);
    zassert_ok(radio_snapshot().command_error);
    radio_ptt(true);
    radio_service();
    k_sleep(K_SECONDS(200));
    radio_service();
    zassert_true(emulator_transmitting());
    zassert_equal(radio_snapshot().tx_remaining_s, 0);
    zassert_false(radio_snapshot().tx_warning);
    zassert_false(radio_snapshot().tx_timed_out);
}

ZTEST(radio, test_rejected_transmit_does_not_arm_timeout) {
    RadioConfig config;
    config.transmit_limit_s = 60;
    config.mode = Mode::M17;
    zassert_ok(radio_start(config));
    radio_ptt(true);
    radio_service();
    zassert_equal(radio_snapshot().ptt_error, -EADDRNOTAVAIL);
    k_sleep(K_SECONDS(61));
    radio_service();
    zassert_false(radio_snapshot().tx_timed_out);
    zassert_equal(radio_snapshot().tx_remaining_s, 0);
    radio_ptt(false);
    radio_service();
    RadioCommand command;
    command.config = config;
    strcpy(command.config.callsign, "OE3ANC");
    execute(command);
    radio_ptt(true);
    radio_service();
    k_sleep(K_SECONDS(60));
    radio_service();
    zassert_true(radio_snapshot().tx_timed_out); // Same enforcement in M17.
    zassert_false(emulator_transmitting());
}

ZTEST(radio, test_fault_at_timeout_deadline_has_precedence) {
    RadioConfig config;
    config.transmit_limit_s = 60;
    zassert_ok(radio_start(config));
    radio_ptt(true);
    radio_service();
    k_sleep(K_SECONDS(60));
    radio_report_fault(-EPIPE);
    radio_service();
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    zassert_equal(radio_snapshot().fault, -EPIPE);
    zassert_false(radio_snapshot().tx_timed_out);
    zassert_false(emulator_transmitting());
}

ZTEST(radio, test_power_off_unkeys_despite_full_queue_and_rearms_release) {
    radio_ptt(true);
    radio_service();
    zassert_true(emulator_transmitting());
    RadioCommand command;
    command.config.rx_frequency_hz = command.config.tx_frequency_hz = 145500000;
    for (unsigned i = 0; i < 8; ++i) {
        zassert_ok(radio_submit(command));
    }
    const auto old = radio_snapshot();
    radio_power(false);
    zassert_false(radio_ptt_requested());
    radio_service();
    auto state = radio_snapshot();
    zassert_equal(state.phase, RadioPhase::Inactive);
    zassert_false(state.power_active);
    zassert_false(emulator_transmitting());
    zassert_equal(state.tx_remaining_s, 0);
    zassert_equal(state.config.rx_frequency_hz, old.config.rx_frequency_hz);
    zassert_equal(radio_submit(command), -EHOSTDOWN);
    radio_power(true);
    radio_service();
    state = radio_snapshot();
    zassert_equal(state.phase, RadioPhase::Receiving);
    zassert_true(state.power_active);
    zassert_false(emulator_transmitting()); // PTT remained held.
    zassert_true(state.generation > old.generation);
    zassert_equal(state.config.rx_frequency_hz, old.config.rx_frequency_hz);
    radio_ptt(false);
    radio_service();
    radio_ptt(true);
    radio_service();
    zassert_true(emulator_transmitting());
}

ZTEST(radio, test_coalesced_off_on_unkeys_and_requires_fresh_ptt) {
    radio_ptt(true);
    radio_service();
    const auto generation = radio_snapshot().generation;
    radio_power(false);
    radio_power(true);
    radio_service();
    zassert_false(emulator_transmitting());
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    zassert_equal(radio_snapshot().generation, generation + 2);
    radio_ptt(true);
    radio_service();
    zassert_false(emulator_transmitting());
    radio_ptt(false);
    radio_service();
    radio_ptt(true);
    radio_service();
    zassert_true(emulator_transmitting());
}

ZTEST(radio, test_release_repress_while_off_cannot_arm_resume) {
    radio_ptt(true);
    radio_service();
    radio_power(false);
    radio_ptt(false);
    radio_ptt(true);
    radio_monitor(false);
    radio_monitor(true);
    radio_power(true);
    zassert_false(radio_power_requested(), "Unconsumed off edge gates blocking backends");
    zassert_false(radio_ptt_requested());
    radio_service();
    zassert_true(radio_power_requested());
    zassert_false(emulator_transmitting(), "Old release may not arm held PTT at resume");
    zassert_false(radio_snapshot().monitor_active);
    radio_service();
    zassert_false(emulator_transmitting());
    radio_ptt(false);
    radio_service();
    radio_ptt(true);
    radio_service();
    zassert_true(emulator_transmitting());
}

ZTEST(radio, test_auxiliary_release_repress_while_off_cannot_open_monitor_on_resume) {
    radio_monitor(true);
    radio_service();
    zassert_true(radio_snapshot().monitor_active);
    radio_power(false);
    radio_monitor(false);
    radio_monitor(true);
    radio_power(true);
    radio_service();
    zassert_false(radio_snapshot().monitor_active);
    radio_service();
    zassert_false(radio_snapshot().monitor_active);
    radio_monitor(false);
    radio_service();
    radio_monitor(true);
    radio_service();
    zassert_true(radio_snapshot().monitor_active);
}

ZTEST(radio, test_inactive_start_does_not_initialize_backend) {
    radio_power(false);
    emulator_fail_next(-EIO);
    RadioConfig config;
    config.rx_frequency_hz = config.tx_frequency_hz = 145500000;
    zassert_ok(radio_start(config));
    zassert_equal(radio_snapshot().phase, RadioPhase::Inactive);
    zassert_equal(radio_snapshot().config.rx_frequency_hz, 145500000);
    radio_service();
    zassert_equal(radio_snapshot().fault, 0);
    radio_power(true);
    radio_service();
    // The still-pending backend init error proves no init happened while off.
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    zassert_equal(radio_snapshot().fault, -EIO);
}

ZTEST(radio, test_power_cycle_preserves_fault_and_pending_fault_blocks_resume) {
    radio_report_fault(-EPIPE);
    radio_service();
    radio_power(false);
    radio_service();
    zassert_equal(radio_snapshot().phase, RadioPhase::Inactive);
    zassert_equal(radio_snapshot().fault, -EPIPE);
    radio_power(true);
    radio_service();
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    zassert_equal(radio_snapshot().fault, -EPIPE);
    zassert_false(emulator_transmitting());
    zassert_ok(radio_start({}));
    radio_power(false);
    radio_service();
    emulator_fail_next(-EIO);
    radio_report_fault(-ETIMEDOUT);
    radio_power(true);
    radio_service();
    zassert_equal(radio_snapshot().fault, -ETIMEDOUT);
    // Resume must not consume the backend operation before observing the fault.
    zassert_equal(radio_start({}), -EIO);
}

ZTEST(radio, test_monitor_and_diagnostics_cancel_on_inactive_transition) {
    radio_monitor(true);
    radio_service();
    zassert_true(radio_snapshot().monitor_active);
    radio_power(false);
    radio_service();
    zassert_false(radio_snapshot().monitor_active);
    radio_power(true);
    radio_service();
    zassert_false(radio_snapshot().monitor_active); // Auxiliary button held.
    radio_monitor(false);
    radio_service();
    RadioCommand command;
    command.kind = CommandKind::EnterDiagnostics;
    execute(command);
    zassert_equal(radio_snapshot().phase, RadioPhase::Diagnostics);
    radio_power(false);
    radio_service();
    radio_power(true);
    radio_service();
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    radio_monitor(true);
    radio_service();
    zassert_true(radio_snapshot().monitor_active);
}

ZTEST(radio, test_shutdown_sequence_time_and_raw_resume_intent) {
    const auto before = radio_snapshot();
    radio_power(false);
    radio_power(true);
    zassert_true(radio_power_on_intent());
    zassert_false(radio_power_requested());
    radio_service();
    auto state = radio_snapshot();
    zassert_equal(state.shutdown_sequence, before.shutdown_sequence + 1);
    zassert_true(state.shutdown_ms <= k_uptime_get());
    const auto sequence = state.shutdown_sequence;
    const auto at = state.shutdown_ms;
    zassert_ok(radio_start(state.config));
    zassert_equal(radio_snapshot().shutdown_sequence, sequence);
    zassert_equal(radio_snapshot().shutdown_ms, at);
    zassert_false(radio_idle_lock(true));
    radio_power(false);
    radio_service();
    zassert_true(radio_idle_lock(true));
    radio_idle_unlock();
    radio_power(false);
    radio_power(true); // newer raw on hidden by pending off edge
    zassert_true(radio_power_on_intent());
    zassert_false(radio_power_requested());
    zassert_false(radio_idle_lock(true));
}

ZTEST(radio, test_controller_origin_fault_is_atomically_visible_without_pending_bridge) {
    zassert_ok(radio_latched_fault());
    BackendStatus status;
    status.error = -EPIPE;
    emulator_inject(status);
    radio_service();
    zassert_equal(radio_snapshot().fault, -EPIPE);
    zassert_ok(radio_pending_fault());
    zassert_equal(radio_latched_fault(), -EPIPE);
    radio_power(false);
    radio_service();
    zassert_equal(radio_latched_fault(), -EPIPE);
    zassert_ok(radio_start({}));
    zassert_ok(radio_latched_fault());
}

static RadioCommand companion_command(bool enabled) {
    const auto state = radio_snapshot();
    RadioCommand command;
    command.kind = CommandKind::CompanionMode;
    command.expected_generation = state.generation;
    command.expected_revision = state.configuration_revision;
    command.companion_enabled = enabled;
    command.companion_disconnected = !enabled;
    return command;
}

ZTEST(radio, test_companion_ownership_blocks_ptt_and_requires_fresh_release) {
    execute(companion_command(true));
    zassert_ok(radio_snapshot().command_error);
    zassert_true(radio_snapshot().companion_mode);
    zassert_true(ht_companion_enabled());
    const auto sequence = radio_ptt_press_sequence();
    radio_ptt(true);
    radio_service();
    zassert_false(radio_ptt_requested());
    zassert_equal(radio_ptt_press_sequence(), sequence);
    zassert_false(emulator_transmitting());
    uint8_t byte;
    zassert_equal(ht_companion_read(&byte, 1), -ENOTSUP);
    auto unconfirmed = companion_command(false);
    unconfirmed.companion_disconnected = false;
    execute(unconfirmed);
    zassert_equal(radio_snapshot().command_error, -EPERM);
    zassert_true(ht_companion_enabled());
    execute(companion_command(false));
    zassert_ok(radio_snapshot().command_error);
    zassert_false(ht_companion_enabled());
    radio_ptt(true);
    radio_service();
    zassert_false(emulator_transmitting(), "Exiting mode cannot rearm a held press");
    radio_ptt(false);
    radio_service();
    radio_ptt(true);
    radio_service();
    zassert_true(emulator_transmitting());
    radio_ptt(false);
    radio_service();
}

ZTEST(radio, test_companion_rejects_stale_tx_diagnostics_and_resets_on_restart) {
    auto stale = companion_command(true);
    ++stale.expected_revision;
    execute(stale);
    zassert_equal(radio_snapshot().command_error, -ESTALE);
    radio_ptt(true);
    radio_service();
    execute(companion_command(true));
    zassert_equal(radio_snapshot().command_error, -EBUSY);
    radio_ptt(false);
    radio_service();
    RadioCommand diagnostic;
    diagnostic.kind = CommandKind::EnterDiagnostics;
    execute(diagnostic);
    execute(companion_command(true));
    zassert_equal(radio_snapshot().command_error, -EBUSY);
    zassert_ok(radio_start({}));
    execute(companion_command(true));
    zassert_ok(radio_snapshot().command_error);
    radio_power(false);
    radio_service();
    zassert_true(radio_snapshot().companion_mode, "Power cycling retains UART ownership");
    radio_power(true);
    radio_service();
    radio_ptt(true);
    radio_service();
    zassert_false(emulator_transmitting());
    zassert_ok(radio_start({}));
    zassert_false(radio_snapshot().companion_mode);
    zassert_false(ht_companion_enabled());
}

ZTEST_SUITE(radio, nullptr, nullptr, before_test, nullptr, nullptr);

ZTEST(radio, test_monitor_hold_release_ptt_and_retune_rearming) {
    zassert_true(backend_capabilities().fm_monitor);
    const auto original = radio_snapshot().config;
    radio_monitor(true);
    radio_service();
    zassert_true(radio_snapshot().monitor_active);
    zassert_true(radio_snapshot().rx_active);
    zassert_false(emulator_transmitting());
    zassert_equal(radio_snapshot().config.squelch, original.squelch);
    RadioCommand command;
    command.config = original;
    command.config.rx_frequency_hz = command.config.tx_frequency_hz = 145500000;
    execute(command);
    zassert_false(radio_snapshot().monitor_active);
    radio_service(); // Still held after retune: never reactivate.
    zassert_false(radio_snapshot().monitor_active);
    radio_monitor(false);
    radio_service();
    radio_monitor(true);
    radio_service();
    zassert_true(radio_snapshot().monitor_active);
    radio_ptt(true);
    radio_service();
    zassert_false(radio_snapshot().monitor_active);
    zassert_true(emulator_transmitting());
    radio_ptt(false);
    radio_service();
    zassert_false(radio_snapshot().monitor_active);
    radio_monitor(false);
    radio_service();
    radio_monitor(true);
    radio_service();
    zassert_true(radio_snapshot().monitor_active);
    radio_monitor(false);
    radio_service();
    zassert_false(radio_snapshot().monitor_active);
    zassert_false(radio_snapshot().rx_active);
}

ZTEST(radio, test_monitor_excludes_m17_diagnostics_and_inhibited_ptt) {
    radio_monitor(true);
    radio_service();
    RadioCommand command;
    command.kind = CommandKind::EnterDiagnostics;
    execute(command);
    zassert_equal(radio_snapshot().phase, RadioPhase::Diagnostics);
    zassert_false(radio_snapshot().monitor_active);
    command.kind = CommandKind::ExitDiagnostics;
    execute(command);
    zassert_false(radio_snapshot().monitor_active);
    radio_monitor(false);
    radio_service();
    command.kind = CommandKind::Configure;
    command.config.mode = Mode::M17;
    execute(command);
    radio_monitor(true);
    radio_service();
    zassert_false(radio_snapshot().monitor_active);
    zassert_false(radio_snapshot().rx_active);
    command.config.mode = Mode::Fm;
    command.config.tx_inhibit = true;
    execute(command);
    radio_service();
    zassert_false(radio_snapshot().monitor_active); // M17 press was consumed.
    radio_monitor(false);
    radio_service();
    radio_monitor(true);
    radio_service();
    zassert_true(radio_snapshot().monitor_active);
    radio_ptt(true);
    radio_service();
    zassert_false(radio_snapshot().monitor_active);
    zassert_equal(radio_snapshot().ptt_error, -EPERM);
    zassert_false(emulator_transmitting());
    radio_ptt(false);
    radio_service();
    zassert_false(radio_snapshot().monitor_active);
}

ZTEST(radio, test_monitor_release_survives_queues_and_rapid_repress) {
    radio_monitor(true);
    radio_service();
    RadioCommand command;
    command.kind = CommandKind::ReadRegister;
    for (unsigned i = 0; i < 8; ++i) {
        zassert_ok(radio_submit(command));
    }
    radio_monitor(false);
    radio_service();
    zassert_false(radio_snapshot().monitor_active);
    radio_monitor(true);
    radio_service();
    zassert_true(radio_snapshot().monitor_active);
    emulator_fail_next(-EIO);
    radio_monitor(false);
    radio_monitor(true);
    radio_service();
    // Failing the restore proves the release edge was not lost to latest level.
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    zassert_equal(radio_snapshot().fault, -EIO);
    zassert_false(radio_snapshot().monitor_active);
    zassert_false(emulator_transmitting());
    radio_monitor(false);
    radio_monitor(true);
    radio_service();
    zassert_false(radio_snapshot().monitor_active);
    zassert_ok(radio_start({}));
    radio_monitor(true);
    radio_service();
    zassert_true(radio_snapshot().monitor_active);
    radio_report_fault(-EPIPE);
    radio_service();
    zassert_equal(radio_snapshot().phase, RadioPhase::Fault);
    zassert_equal(radio_snapshot().fault, -EPIPE);
    zassert_false(radio_snapshot().monitor_active);
}

ZTEST(radio, test_ptt_press_observation_survives_service_and_restart) {
    const uint32_t marker = radio_ptt_press_sequence();
    radio_ptt(true);
    radio_ptt(false);
    zassert_equal(radio_ptt_press_sequence(), uint32_t(marker + 1));
    radio_service();
    zassert_false(emulator_transmitting());
    zassert_equal(radio_ptt_press_sequence(), uint32_t(marker + 1));
    zassert_ok(radio_start({}));
    zassert_equal(radio_ptt_press_sequence(), uint32_t(marker + 1));
    radio_ptt(false);
    zassert_equal(radio_ptt_press_sequence(), uint32_t(marker + 1));
}

ZTEST(radio, test_coalesced_ptt_tap_consumes_monitor_without_keying) {
    radio_monitor(true);
    radio_service();
    zassert_true(radio_snapshot().monitor_active);
    radio_ptt(true);
    radio_ptt(false);
    radio_service();
    zassert_equal(radio_snapshot().phase, RadioPhase::Receiving);
    zassert_false(emulator_transmitting());
    zassert_false(radio_snapshot().monitor_active);
    radio_service();
    zassert_false(radio_snapshot().monitor_active); // Held auxiliary input stays consumed.
    radio_monitor(false);
    radio_service();
    // A new monitor press batched with the PTT tap must be consumed too.
    radio_monitor(true);
    radio_ptt(true);
    radio_ptt(false);
    radio_service();
    zassert_false(radio_snapshot().monitor_active);
    zassert_false(emulator_transmitting());
    radio_monitor(false);
    radio_service();
    radio_monitor(true);
    radio_service();
    zassert_true(radio_snapshot().monitor_active);
}

ZTEST(radio, test_quick_controls_reject_stale_revision_generation_and_selection) {
    RadioCommand quick;
    quick.kind = CommandKind::QuickControls;
    quick.id = 911;
    auto state = radio_snapshot();
    quick.expected_generation = state.generation;
    quick.expected_revision = state.configuration_revision;
    quick.selection = state.selection;
    quick.config.squelch = 9;
    RadioCommand tune;
    tune.id = 910;
    tune.config = state.config;
    tune.config.rx_frequency_hz = tune.config.tx_frequency_hz = 145500000;
    zassert_ok(radio_submit(tune));
    zassert_ok(radio_submit(quick));
    radio_service();
    radio_service();
    zassert_equal(radio_snapshot().command_error, -ESTALE);
    zassert_equal(radio_snapshot().config.squelch, 4);
    zassert_equal(emulator_tuned_frequency(), 145500000);
    quick.expected_revision = radio_snapshot().configuration_revision;
    quick.selection = {Operating::Memory, 0, 5};
    zassert_ok(radio_submit(quick));
    radio_service();
    zassert_equal(radio_snapshot().command_error, -ESTALE);
    quick.selection = radio_snapshot().selection;
    zassert_ok(radio_start({}));
    quick.expected_revision = radio_snapshot().configuration_revision;
    zassert_ok(radio_submit(quick));
    radio_service();
    zassert_equal(radio_snapshot().command_error, -ESTALE);
    zassert_equal(radio_snapshot().config.squelch, 4);
}

ZTEST(radio, test_quick_controls_noop_preserves_monitor_revision_and_rejects_tx) {
    radio_monitor(true);
    radio_service();
    const auto before = radio_snapshot();
    zassert_true(before.monitor_active);
    RadioCommand quick;
    quick.kind = CommandKind::QuickControls;
    quick.config = before.config;
    execute(quick);
    zassert_ok(radio_snapshot().command_error);
    zassert_equal(radio_snapshot().configuration_revision, before.configuration_revision);
    zassert_true(radio_snapshot().monitor_active);
    radio_ptt(true);
    radio_service();
    execute(quick);
    zassert_equal(radio_snapshot().command_error, -EBUSY);
    radio_ptt(false);
    radio_service();
}

static RadioCommand limit_command(uint16_t seconds) {
    const auto state = radio_snapshot();
    RadioCommand command;
    command.kind = CommandKind::TransmitLimit;
    command.id = 930;
    command.expected_generation = state.generation;
    command.expected_revision = state.configuration_revision;
    command.selection = state.selection;
    command.config.transmit_limit_s = seconds;
    return command;
}

ZTEST(radio, test_limit_command_updates_only_global_metadata_without_rf_work) {
    zassert_ok(radio_start({}, {Operating::Memory, 1, 4}));
    radio_monitor(true);
    radio_service();
    const auto before = radio_snapshot();
    auto limit = limit_command(60);
    limit.config.rx_frequency_hz = 0; // Only the named field is used.
    emulator_fail_next(-EIO);
    execute(limit);
    const auto after = radio_snapshot();
    zassert_ok(after.command_error);
    zassert_equal(after.config.transmit_limit_s, 60);
    zassert_true(same_selection(before.selection, after.selection));
    zassert_true(same_operating(before.config, after.config));
    zassert_true(after.monitor_active);
    zassert_equal(after.config.gain, before.config.gain);
    zassert_equal(strcmp(after.config.callsign, before.config.callsign), 0);
    zassert_equal(after.configuration_revision, before.configuration_revision + 1);
    execute(limit_command(60));
    zassert_ok(radio_snapshot().command_error);
    zassert_equal(radio_snapshot().configuration_revision, after.configuration_revision);
    zassert_true(radio_snapshot().monitor_active);
    RadioCommand configure;
    configure.config = after.config;
    execute(configure);
    zassert_equal(radio_snapshot().command_error,
                  -EIO); // The backend failure was never consumed by limit edits.
}

ZTEST(radio, test_limit_command_rejects_invalid_stale_and_busy_transactions) {
    const uint16_t invalid[] = {1, 59, 61, 181, UINT16_MAX};
    for (auto seconds : invalid) {
        execute(limit_command(seconds));
        zassert_equal(radio_snapshot().command_error, -EINVAL);
    }
    auto stale = limit_command(120);
    ++stale.expected_generation;
    execute(stale);
    zassert_equal(radio_snapshot().command_error, -ESTALE);
    stale = limit_command(120);
    ++stale.expected_revision;
    execute(stale);
    zassert_equal(radio_snapshot().command_error, -ESTALE);
    stale = limit_command(120);
    stale.selection = {Operating::Memory, 0, 4};
    execute(stale);
    zassert_equal(radio_snapshot().command_error, -ESTALE);
    radio_ptt(true);
    radio_service();
    execute(limit_command(120));
    zassert_equal(radio_snapshot().command_error, -EBUSY);
    zassert_true(emulator_transmitting());
    radio_ptt(false);
    radio_service();
    RadioCommand diagnostics;
    diagnostics.kind = CommandKind::EnterDiagnostics;
    execute(diagnostics);
    execute(limit_command(120));
    zassert_equal(radio_snapshot().command_error, -EBUSY);
    zassert_equal(radio_snapshot().config.transmit_limit_s, 180);
    radio_power(false);
    radio_service();
    zassert_equal(radio_submit(limit_command(120)), -EHOSTDOWN);
}

ZTEST(radio, test_limit_command_held_timeout_requires_release_before_change) {
    RadioConfig config;
    config.transmit_limit_s = 60;
    zassert_ok(radio_start(config));
    radio_ptt(true);
    radio_service();
    k_sleep(K_SECONDS(60));
    radio_service();
    zassert_true(radio_snapshot().tx_timed_out);
    zassert_false(emulator_transmitting());
    execute(limit_command(0));
    zassert_equal(radio_snapshot().command_error, -EBUSY);
    zassert_equal(radio_snapshot().config.transmit_limit_s, 60);
    radio_ptt(false);
    radio_service();
    execute(limit_command(0));
    zassert_ok(radio_snapshot().command_error);
    radio_ptt(true);
    radio_service();
    k_sleep(K_SECONDS(60));
    radio_service();
    zassert_true(emulator_transmitting());
    zassert_false(radio_snapshot().tx_warning);
    radio_ptt(false);
    radio_service();
}
