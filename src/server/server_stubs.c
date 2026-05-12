/*
 * Copyright (c) 1998-2008 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "global.h"
#include "server_lifecycle.h"

void serverInstanceRecordProbeReply(const char *reflexiveIp,
                                    unsigned short reflexivePort) {
  (void)reflexiveIp;
  (void)reflexivePort;
}

/* server_sim.c references these from serverSimInformation. The full
 * implementations live in server_lifecycle.c, which BrainTest doesn't
 * link — report zero so the diagnostic prints fall through cleanly. */
void serverLifecycleGetTickStats(double *outLastMs, double *outEwmaMs) {
  if (outLastMs) *outLastMs = 0.0;
  if (outEwmaMs) *outEwmaMs = 0.0;
}

void serverLifecycleGetSimStats(double *outLastMs, double *outEwmaMs) {
  if (outLastMs) *outLastMs = 0.0;
  if (outEwmaMs) *outEwmaMs = 0.0;
}
