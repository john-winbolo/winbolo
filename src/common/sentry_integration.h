/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SENTRY_INTEGRATION_H
#define SENTRY_INTEGRATION_H

int sentryInit(const char *executable_name, int argc, char *argv[]);
void sentryClose(void);

#endif
