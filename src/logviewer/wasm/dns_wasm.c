/*
 * dns_wasm.c - No-op DNS for WASM build
 *
 * Replaces dns.c — DNS lookups are not available in the browser sandbox.
 * All functions are no-ops; dnsLookup just copies the IP to the host buffer.
 */

#include "global.h"
#include "dns.h"
#include <string.h>

bool dnsCreate(void) {
    return TRUE;
}

bool dnsSetEnabled(bool set) {
    (void)set;
    return FALSE; /* DNS not available in WASM */
}

void dnsShutdown(void) {
}

void dnsLookup(char *ip, char *host, size_t host_size) {
    strncpy(host, ip, host_size - 1);
    host[host_size - 1] = '\0';
}
