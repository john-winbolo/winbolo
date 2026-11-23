/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SENTRY_NSEXCEPTION_H
#define SENTRY_NSEXCEPTION_H

/* Uncaught Objective-C exception reporting for the sentry-native inproc
 * backend on macOS. See sentry_nsexception.m for why both calls exist
 * and the order they must run in. */

struct sentry_options_s;

/* Sets the on_crash hook on `options`. Call before sentry_init(). */
void sentryNSExceptionAttach(struct sentry_options_s *options);

/* Installs the NSUncaughtExceptionHandler. Call after sentry_init()
 * has returned 0, because the handler writes to the Sentry scope. */
void sentryNSExceptionInstall(void);

#endif
