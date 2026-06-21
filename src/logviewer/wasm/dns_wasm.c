/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * dns_wasm.c - No-op DNS for WASM build
 *
 * Replaces dns.c — DNS lookups are not available in the browser sandbox.
 * All functions are no-ops; lv_dnsLookup just copies the IP to the host buffer.
 */

#include "lv_global.h"
#include "dns.h"
#include <string.h>

bool lv_dnsCreate(void) {
    return TRUE;
}

bool lv_dnsSetEnabled(bool set) {
    (void)set;
    return FALSE; /* DNS not available in WASM */
}

void lv_dnsShutdown(void) {
}

void lv_dnsLookup(char *ip, char *host, size_t host_size) {
    strncpy(host, ip, host_size - 1);
    host[host_size - 1] = '\0';
}
