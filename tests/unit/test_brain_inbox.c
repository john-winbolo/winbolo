/*
 * Tests for the per-tick brain inbox in src/bolo/messages.c. The
 * inbox is what allows a brain to see ALL chat messages addressed to
 * it during a single tick, not just the most recent — the original
 * single-slot newMessage buffer silently overwrote on every arrival
 * so two ally bots chatting on the same tick caused one to be lost.
 * Ally coordination depends on every broadcast surfacing, so this is
 * load-bearing.
 *
 * Four tests:
 *   - brain_inbox_push_peek_fifo:
 *       push N messages, peek each by logical index, confirm FIFO
 *       order and that sender + Pascal-string body round-trip.
 *   - brain_inbox_overflow_drops_oldest:
 *       push BRAIN_INBOX_CAP + 5 messages; verify count caps at
 *       BRAIN_INBOX_CAP and the FIVE OLDEST were dropped, not the
 *       newest.
 *   - brain_inbox_legacy_drain_fifo:
 *       confirm the back-compat messageIsNewMessage /
 *       messageGetNewMessage pair drains the inbox head in FIFO
 *       order (pre-change behavior would have surfaced the newest;
 *       any consumer still using these is now FIFO).
 *   - brain_inbox_clear_resets:
 *       messageInboxClear empties the queue and lets new pushes
 *       start from the beginning.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "messages.h"
#include "test_harness.h"

/* Build a Pascal string from a C string into dst. dst must be at
 * least strlen(src) + 2 bytes. */
static void pascalize(const char *src, char *dst) {
    size_t len = strlen(src);
    if (len > 250) len = 250;
    dst[0] = (char)len;
    memcpy(dst + 1, src, len);
    dst[len + 1] = '\0';
}

/* Read body from a Pascal-stringified buffer into a NUL-terminated
 * C string. */
static void depascalize(const char *p, char *out) {
    size_t len = (size_t)(unsigned char)p[0];
    memcpy(out, p + 1, len);
    out[len] = '\0';
}

int run_brain_inbox_push_peek_fifo(void) {
    MessageState ms;
    messageCreate(&ms);

    UT_ASSERT_MSG(messageInboxCount(&ms) == 0,
                  "freshly-created MessageState should have empty inbox");

    /* Push 5 messages from interleaved senders so we can verify both
     * the from-byte and the body round-trip through the ring. */
    const struct { BYTE from; const char *text; } cases[] = {
        { 3, "hello"          },
        { 1, "from one"       },
        { 7, "shoot pill 17"  },
        { 0, ""               },
        { 15, "last sender"   },
    };
    const int n = (int)(sizeof(cases) / sizeof(cases[0]));
    for (int i = 0; i < n; i++) {
        char pbuf[BRAIN_INBOX_MSG_LEN];
        pascalize(cases[i].text, pbuf);
        messageInboxPush(&ms, cases[i].from, pbuf);
    }
    UT_ASSERT_MSG(messageInboxCount(&ms) == n,
                  "inbox count should equal %d after %d pushes, got %d",
                  n, n, messageInboxCount(&ms));

    /* Peek each in FIFO order (logical index 0 = oldest = first
     * pushed). */
    for (int i = 0; i < n; i++) {
        char  pbuf[BRAIN_INBOX_MSG_LEN];
        BYTE  from = messageInboxPeek(&ms, i, pbuf);
        char  body[256];
        depascalize(pbuf, body);
        UT_ASSERT_MSG(from == cases[i].from,
                      "peek %d: expected sender=%d got %d",
                      i, cases[i].from, from);
        UT_ASSERT_MSG(strcmp(body, cases[i].text) == 0,
                      "peek %d: expected body \"%s\" got \"%s\"",
                      i, cases[i].text, body);
    }

    /* Peek is non-destructive — count still equals n. */
    UT_ASSERT(messageInboxCount(&ms) == n);

    /* Out-of-range peek returns 0 sender + empty body. */
    {
        char  pbuf[BRAIN_INBOX_MSG_LEN];
        BYTE  from = messageInboxPeek(&ms, 999, pbuf);
        UT_ASSERT(from == 0);
        UT_ASSERT(pbuf[0] == '\0');
    }

    messageDestroy(&ms);
    return 0;
}

int run_brain_inbox_overflow_drops_oldest(void) {
    MessageState ms;
    messageCreate(&ms);

    /* Push CAP + 5 distinct messages. After saturation each new push
     * must displace the OLDEST entry — the most recent CAP entries
     * are what survives. */
    const int total = BRAIN_INBOX_CAP + 5;
    for (int i = 0; i < total; i++) {
        char pbuf[BRAIN_INBOX_MSG_LEN];
        char body[32];
        snprintf(body, sizeof(body), "msg-%d", i);
        pascalize(body, pbuf);
        messageInboxPush(&ms, (BYTE)(i % 16), pbuf);
    }

    UT_ASSERT_MSG(messageInboxCount(&ms) == BRAIN_INBOX_CAP,
                  "saturated inbox should hold exactly BRAIN_INBOX_CAP=%d, got %d",
                  BRAIN_INBOX_CAP, messageInboxCount(&ms));

    /* Logical index 0 should now be msg-5 (msgs 0..4 dropped). */
    char  pbuf[BRAIN_INBOX_MSG_LEN];
    char  body[64];
    messageInboxPeek(&ms, 0, pbuf);
    depascalize(pbuf, body);
    UT_ASSERT_MSG(strcmp(body, "msg-5") == 0,
                  "oldest survivor should be msg-5, got \"%s\"", body);

    /* Logical index CAP-1 = newest = msg-(total-1). */
    messageInboxPeek(&ms, BRAIN_INBOX_CAP - 1, pbuf);
    depascalize(pbuf, body);
    char expectedLast[32];
    snprintf(expectedLast, sizeof(expectedLast), "msg-%d", total - 1);
    UT_ASSERT_MSG(strcmp(body, expectedLast) == 0,
                  "newest should be %s, got \"%s\"", expectedLast, body);

    messageDestroy(&ms);
    return 0;
}

int run_brain_inbox_legacy_drain_fifo(void) {
    MessageState ms;
    messageCreate(&ms);

    /* messageIsNewMessage returns false on empty inbox. */
    UT_ASSERT(!messageIsNewMessage(&ms));

    /* Push 3, drain via legacy API, confirm FIFO + per-call decrement. */
    const char *bodies[] = { "first", "second", "third" };
    const BYTE  senders[] = { 4, 8, 12 };
    for (int i = 0; i < 3; i++) {
        char pbuf[BRAIN_INBOX_MSG_LEN];
        pascalize(bodies[i], pbuf);
        messageInboxPush(&ms, senders[i], pbuf);
    }
    UT_ASSERT(messageIsNewMessage(&ms));

    for (int i = 0; i < 3; i++) {
        char dest[BRAIN_INBOX_MSG_LEN];
        BYTE from = messageGetNewMessage(&ms, dest, NULL);
        char body[64];
        depascalize(dest, body);
        UT_ASSERT_MSG(from == senders[i],
                      "drain %d: expected sender=%d got %d",
                      i, senders[i], from);
        UT_ASSERT_MSG(strcmp(body, bodies[i]) == 0,
                      "drain %d: expected \"%s\" got \"%s\"",
                      i, bodies[i], body);
    }
    /* Fully drained. */
    UT_ASSERT(!messageIsNewMessage(&ms));
    UT_ASSERT(messageInboxCount(&ms) == 0);

    /* Drain when empty: returns 0 + empty string, no crash. */
    {
        char dest[BRAIN_INBOX_MSG_LEN] = { 'x' };
        BYTE from = messageGetNewMessage(&ms, dest, NULL);
        UT_ASSERT(from == 0);
        UT_ASSERT(dest[0] == '\0');
    }

    messageDestroy(&ms);
    return 0;
}

int run_brain_inbox_clear_resets(void) {
    MessageState ms;
    messageCreate(&ms);

    for (int i = 0; i < 5; i++) {
        char pbuf[BRAIN_INBOX_MSG_LEN];
        pascalize("x", pbuf);
        messageInboxPush(&ms, (BYTE)i, pbuf);
    }
    UT_ASSERT(messageInboxCount(&ms) == 5);

    messageInboxClear(&ms);
    UT_ASSERT_MSG(messageInboxCount(&ms) == 0,
                  "clear should empty the inbox, got count=%d",
                  messageInboxCount(&ms));
    UT_ASSERT(!messageIsNewMessage(&ms));

    /* Post-clear push lands at logical index 0. */
    char pbuf[BRAIN_INBOX_MSG_LEN];
    pascalize("after clear", pbuf);
    messageInboxPush(&ms, 9, pbuf);
    UT_ASSERT(messageInboxCount(&ms) == 1);
    char peek[BRAIN_INBOX_MSG_LEN];
    BYTE from = messageInboxPeek(&ms, 0, peek);
    char body[64];
    depascalize(peek, body);
    UT_ASSERT(from == 9);
    UT_ASSERT(strcmp(body, "after clear") == 0);

    messageDestroy(&ms);
    return 0;
}
