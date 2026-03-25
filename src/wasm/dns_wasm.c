/*
 * dns_wasm.c - No-op DNS lookups for WASM build
 */

#include <string.h>
#include "../bolo/global.h"
#include "../bolo/client_sim.h"

bool dnsLookupsCreate(ClientSim *cs)  { (void)cs; return TRUE; }
void dnsLookupsDestroy(void) { }
void dnsLookupsAddRequest(char *ip, void *func) {
  (void)ip; (void)func;
}
