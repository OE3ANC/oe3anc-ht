// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/ui_text.hpp>
#include <errno.h>
#include <string.h>
#include <zephyr/ztest.h>
using namespace ht;

ZTEST(ui_text, test_physical_digit_groups_and_timeout) {
    const char *groups[] = {" 0",   ".,?!-/1", "ABC2",  "DEF3", "GHI4",
                            "JKL5", "MNO6",    "PQRS7", "TUV8", "WXYZ9"};
    TextEditor editor;
    for (unsigned d = 0; d < 10; ++d) {
        zassert_ok(editor.begin(TextKind::Name));
        int64_t now = 100;
        for (const char *p = groups[d]; *p; ++p, now += 10) {
            zassert_ok(editor.digit('0' + d, now));
            zassert_equal(editor.length(), 1);
            zassert_equal(editor.text()[0], *p);
        }
        zassert_ok(editor.digit('0' + d, now));
        zassert_equal(editor.text()[0], groups[d][0]);
        editor.advance(now + 749);
        zassert_true(editor.pending());
        editor.advance(now + 750);
        zassert_false(editor.pending());
        zassert_ok(editor.digit('0' + d, now + 750));
        zassert_equal(editor.length(), 2);
    }
}

ZTEST(ui_text, test_commit_same_digit_and_middle_edit) {
    TextEditor editor;
    zassert_ok(editor.begin(TextKind::Name));
    zassert_ok(editor.digit('2', 100));
    editor.finish();
    zassert_ok(editor.digit('2', 110));
    zassert_equal(strcmp(editor.text(), "AA"), 0);
    zassert_ok(editor.digit('3', 120));
    zassert_equal(strcmp(editor.text(), "AAD"), 0);
    editor.move(-1);
    editor.move(-1);
    zassert_false(editor.pending());
    zassert_equal(editor.cursor(), 1);
    zassert_ok(editor.literal('Z'));
    zassert_equal(strcmp(editor.text(), "AZAD"), 0);
    editor.erase();
    zassert_equal(strcmp(editor.text(), "AAD"), 0);
    for (unsigned i = 0; i < 10; ++i) {
        editor.move(-1);
    }
    editor.erase();
    zassert_equal(strcmp(editor.text(), "AAD"), 0);
    zassert_equal(editor.cursor(), 0);
    for (unsigned i = 0; i < 10; ++i) {
        editor.move(1);
    }
    zassert_equal(editor.cursor(), editor.length());
}

ZTEST(ui_text, test_bounds_normalization_and_transactional_begin) {
    TextEditor editor;
    zassert_ok(editor.begin(TextKind::Name, "12345678901234567890123"));
    zassert_ok(editor.digit('2', 100));
    zassert_ok(editor.digit('2', 110)); // Can cycle last character at capacity.
    zassert_equal(editor.text()[23], 'B');
    zassert_equal(editor.literal('x'), -ENOSPC);
    zassert_equal(editor.begin(TextKind::Name, "1234567890123456789012345"), -ENOSPC);
    zassert_equal(editor.length(), 24);
    zassert_equal(editor.begin(TextKind::Callsign, "bad!"), -EINVAL);
    zassert_equal(editor.length(), 24);
    zassert_equal(editor.begin(static_cast<TextKind>(255)), -EINVAL);
    zassert_equal(editor.begin(TextKind::Name, nullptr), -EINVAL);
    zassert_ok(editor.begin(TextKind::Callsign, "oe3anc/P"));
    zassert_equal(strcmp(editor.text(), "OE3ANC/P"), 0);
    zassert_equal(editor.literal(' '), -EINVAL);
    zassert_ok(editor.literal('1'));
    zassert_equal(editor.literal('2'), -ENOSPC);
    zassert_ok(editor.begin(TextKind::Frequency));
    zassert_equal(editor.literal('a'), -EINVAL);
    zassert_ok(editor.digit('2', 100));
    zassert_ok(editor.digit('2', 110));
    zassert_equal(strcmp(editor.text(), "22"), 0);
    zassert_false(editor.pending());
    zassert_ok(editor.begin(TextKind::ChannelNumber));
    zassert_ok(editor.digit('2', 100));
    zassert_ok(editor.digit('5', 110));
    zassert_ok(editor.digit('6', 120));
    zassert_equal(strcmp(editor.text(), "256"), 0);
    zassert_equal(editor.literal('7'), -ENOSPC);
    zassert_equal(editor.literal('.'), -EINVAL);
}

ZTEST(ui_text, test_producer_timestamps_and_clock_reversal) {
    TextEditor editor;
    zassert_ok(editor.begin(TextKind::Callsign));
    zassert_ok(editor.digit('2', 100));
    zassert_ok(editor.digit('2', 500));
    zassert_equal(strcmp(editor.text(), "B"), 0);
    editor.advance(1249);
    zassert_true(editor.pending());
    zassert_ok(editor.digit('2', 1250));
    zassert_equal(strcmp(editor.text(), "BA"), 0);
    zassert_ok(editor.digit('2', 50)); // Reversed clock commits instead of cycling.
    zassert_equal(strcmp(editor.text(), "BAA"), 0);
    zassert_equal(editor.digit('2', -1), -EINVAL);
    zassert_equal(editor.digit('x', 100), -EINVAL);
    zassert_equal(strcmp(editor.text(), "BAA"), 0);
}

ZTEST(ui_text, test_callsign_keypad_filter) {
    TextEditor editor;
    zassert_ok(editor.begin(TextKind::Callsign));
    const char *group = ".-/1";
    for (unsigned i = 0; i < 4; ++i) {
        zassert_ok(editor.digit('1', 100 + i));
        zassert_equal(editor.text()[0], group[i]);
    }
    zassert_ok(editor.digit('0', 110));
    zassert_equal(strcmp(editor.text(), "10"), 0);
    zassert_equal(editor.literal('!'), -EINVAL);
}

ZTEST(ui_text, test_frequency_hertz_boundaries_and_errors) {
    uint32_t hz = 123;
    zassert_ok(ui_parse_frequency("145.500001", hz));
    zassert_equal(hz, 145500001);
    zassert_ok(ui_parse_frequency("4294.967295", hz));
    zassert_equal(hz, UINT32_MAX);
    const char *bad[] = {"", ".", "1..2", "-145", "145a", "145.1234567"};
    for (auto *text : bad) {
        zassert_equal(ui_parse_frequency(text, hz), -EINVAL);
        zassert_equal(hz, UINT32_MAX);
    }
    const char *overflow[] = {"4294.967296", "4295", "999999999999999999999999"};
    for (auto *text : overflow) {
        zassert_equal(ui_parse_frequency(text, hz), -ERANGE);
        zassert_equal(hz, UINT32_MAX);
    }
    zassert_equal(ui_parse_frequency(nullptr, hz), -EINVAL);
    zassert_ok(ui_parse_frequency(".5", hz));
    zassert_equal(hz, 500000);
    zassert_ok(ui_parse_frequency("145.", hz));
    zassert_equal(hz, 145000000);
}

ZTEST_SUITE(ui_text, nullptr, nullptr, nullptr, nullptr, nullptr);
