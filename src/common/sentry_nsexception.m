/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Uncaught Objective-C exceptions on macOS.
 *
 * sentry-native's inproc backend only sees the SIGABRT that the Objective-C
 * runtime raises after an uncaught NSException. The crash event it writes
 * is titled "SIGABRT: Abort" and carries the stack of the abort call, not of
 * the throw. AppKit also catches and rethrows exceptions inside
 * -[NSApplication nextEventMatchingMask:untilDate:inMode:dequeue:], which
 * removes the throw frames from that stack entirely, so such a report
 * names neither the exception nor the code that raised it.
 *
 * The exception does reach the runtime's terminate handler: SDL calls
 * nextEventMatchingMask: itself rather than running [NSApp run], so there
 * is no AppKit run loop above it to swallow the exception, and it
 * propagates out of AppKit into C code with nothing to catch it. The
 * terminate handler calls the function installed with
 * NSSetUncaughtExceptionHandler and then calls abort(). That function runs
 * in ordinary (non-signal) context on the throwing thread, so it can read
 * the exception's name and reason and the return addresses the runtime
 * recorded when the exception was raised (the rethrow reuses the same
 * object, so those are still the throw site), and put them on the Sentry
 * scope. The on_crash hook then runs for the SIGABRT, on sentry's own
 * handler thread while the crashing thread is held, and rewrites the
 * crash event's exception type and value from fixed buffers filled a
 * moment earlier, so the Sentry issue is
 * titled by the NSException instead of "Abort". The hook only touches
 * fixed buffers: it must not call into Foundation from a signal handler.
 *
 * Only the inproc backend runs on_crash on macOS (sentry.h: it "does not
 * work with crashpad on macOS"), so CMake adds this file only for that
 * backend.
 */

#import <Foundation/Foundation.h>
#include <sentry.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "sentry_nsexception.h"

/* Sentry drops a tag whose value is over 200 characters. */
static char s_name[200];
static char s_reason[2048];
/* Set once by the first thread whose exception goes uncaught. A second
 * thread throwing in the same instant leaves the buffers alone, so the
 * crash event cannot mix one thread's name with another's reason. */
static atomic_bool s_claimed = false;
static atomic_bool s_filled = false;
static NSUncaughtExceptionHandler *s_previous = NULL;

/* Copies `src` into `dst`, ending with "..." when it does not fit. */
static void copyString(char *dst, size_t cap, NSString *src) {
    const char *utf8 = src ? [src UTF8String] : NULL;
    if (!utf8) utf8 = "";
    int n = snprintf(dst, cap, "%s", utf8);
    if (n < 0) {
        dst[0] = '\0';
    } else if ((size_t)n >= cap && cap > 4) {
        strcpy(dst + cap - 4, "...");
    }
}

static void uncaughtHandler(NSException *exception) {
    bool expected = false;
    if (atomic_compare_exchange_strong(&s_claimed, &expected, true)) {
        @autoreleasepool {
            copyString(s_name, sizeof(s_name), exception.name);
            copyString(s_reason, sizeof(s_reason), exception.reason);

            /* callStackSymbols is the stack at the point the exception was
             * raised, which is the one piece of information the SIGABRT
             * report cannot recover. */
            NSString *stack = [exception.callStackSymbols componentsJoinedByString:@"\n"];
            const char *stackUtf8 = stack ? [stack UTF8String] : NULL;
            if (!stackUtf8) stackUtf8 = "";

            sentry_set_tag("nsexception.name", s_name);
            sentry_set_extra("nsexception.reason", sentry_value_new_string(s_reason));
            sentry_set_extra("nsexception.stack", sentry_value_new_string(stackUtf8));

            fprintf(stderr, "Uncaught Objective-C exception %s: %s\n%s\n",
                    s_name, s_reason, stackUtf8);
            fflush(stderr);
        }
        atomic_store(&s_filled, true);
    }

    if (s_previous) s_previous(exception);
}

static sentry_value_t onCrash(const sentry_ucontext_t *uctx,
                              sentry_value_t event, void *closure) {
    (void)uctx;
    (void)closure;
    /* Only the SIGABRT that follows the recorded exception is that
     * exception. Any other signal arriving while the buffers are filled is
     * its own crash and keeps its own title. (The hook runs on sentry's
     * handler thread, not the crashing one, so it cannot also check that
     * the abort came from the thread that threw.) */
    if (!atomic_load(&s_filled)) return event;

    sentry_value_t values = sentry_value_get_by_key(
        sentry_value_get_by_key(event, "exception"), "values");
    sentry_value_t exc = sentry_value_get_by_index(values, 0);
    if (sentry_value_is_null(exc)) return event;
    const char *type = sentry_value_as_string(sentry_value_get_by_key(exc, "type"));
    if (!type || strcmp(type, "SIGABRT") != 0) return event;

    sentry_value_set_by_key(exc, "type", sentry_value_new_string(s_name));
    sentry_value_set_by_key(exc, "value", sentry_value_new_string(s_reason));
    return event;
}

void sentryNSExceptionAttach(struct sentry_options_s *options) {
    sentry_options_set_on_crash(options, onCrash, NULL);
}

void sentryNSExceptionInstall(void) {
    s_previous = NSGetUncaughtExceptionHandler();
    NSSetUncaughtExceptionHandler(uncaughtHandler);
}
