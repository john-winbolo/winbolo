/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nat_portmap.h"

#include <string.h>

#ifdef BOLO_PORTMAP

#include <SDL3/SDL.h>
#include <plum/plum.h>

#include "../common/wb_log.h"

/* libplum's mapping callback receives only the int handle, so we keep a
 * small static side-table mapping handles back to their NatPortMap. */
typedef struct {
  int         mappingId;
  NatPortMap *map;
  bool        inUse;
} PortMapSlot;

#define PORTMAP_MAX_SLOTS 4

static PortMapSlot  slots[PORTMAP_MAX_SLOTS];
static SDL_Mutex   *slotsMutex;
static bool         plumInited;

static void plumLogCb(plum_log_level_t level, const char *message);
static void plumMappingCb(int id, plum_state_t state,
                          const plum_mapping_t *mapping);

static void plumInitOnce(void) {
  if (plumInited) {
    return;
  }
  if (!slotsMutex) {
    slotsMutex = SDL_CreateMutex();
  }
  plum_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.log_level    = PLUM_LOG_LEVEL_INFO;
  cfg.log_callback = plumLogCb;
  if (plum_init(&cfg) == 0) {
    plumInited = true;
  }
}

static void plumLogCb(plum_log_level_t level, const char *message) {
  const char *msg = message ? message : "(null)";
  switch (level) {
    case PLUM_LOG_LEVEL_FATAL:
    case PLUM_LOG_LEVEL_ERROR:
      WB_LOG_ERROR(WB_LOG_CAT_NET, "[libplum] %s", msg);
      break;
    case PLUM_LOG_LEVEL_WARN:
      WB_LOG_WARN(WB_LOG_CAT_NET, "[libplum] %s", msg);
      break;
    case PLUM_LOG_LEVEL_INFO:
      WB_LOG_INFO(WB_LOG_CAT_NET, "[libplum] %s", msg);
      break;
    case PLUM_LOG_LEVEL_DEBUG:
      WB_LOG_DEBUG(WB_LOG_CAT_NET, "[libplum] %s", msg);
      break;
    case PLUM_LOG_LEVEL_VERBOSE:
    default:
      WB_LOG_TRACE(WB_LOG_CAT_NET, "[libplum] %s", msg);
      break;
  }
}

static PortMapSlot *findSlotByMappingId(int id) {
  for (int i = 0; i < PORTMAP_MAX_SLOTS; ++i) {
    if (slots[i].inUse && slots[i].mappingId == id) {
      return &slots[i];
    }
  }
  return NULL;
}

static void plumMappingCb(int id, plum_state_t state,
                          const plum_mapping_t *mapping) {
  if (!slotsMutex) {
    return;
  }
  SDL_LockMutex(slotsMutex);
  PortMapSlot *slot = findSlotByMappingId(id);
  if (!slot || !slot->map) {
    SDL_UnlockMutex(slotsMutex);
    return;
  }
  NatPortMap *out = slot->map;
  if (state == PLUM_STATE_SUCCESS && mapping) {
    SDL_strlcpy(out->externalIp, mapping->external_host,
                sizeof(out->externalIp));
    out->externalPort = mapping->external_port;
    /* libplum 0.5.x doesn't expose which protocol family negotiated the
     * mapping (UPnP / NAT-PMP / PCP), so use a generic marker. */
    SDL_strlcpy(out->method, "Plum", sizeof(out->method));
    out->mapped = true;
  } else if (state == PLUM_STATE_FAILURE || state == PLUM_STATE_DESTROYED) {
    out->mapped     = false;
    out->method[0]  = '\0';
  }
  SDL_UnlockMutex(slotsMutex);
}

void natPortMapRequest(unsigned short internalPort, NatPortMap *out) {
  if (!out) {
    return;
  }

  plumInitOnce();
  if (!plumInited || !slotsMutex) {
    memset(out, 0, sizeof(*out));
    out->internalPort = internalPort;
    return;
  }

  SDL_LockMutex(slotsMutex);

  for (int i = 0; i < PORTMAP_MAX_SLOTS; ++i) {
    if (slots[i].inUse && slots[i].map == out) {
      SDL_UnlockMutex(slotsMutex);
      return;
    }
  }

  PortMapSlot *slot = NULL;
  for (int i = 0; i < PORTMAP_MAX_SLOTS; ++i) {
    if (!slots[i].inUse) {
      slot = &slots[i];
      break;
    }
  }
  if (!slot) {
    SDL_UnlockMutex(slotsMutex);
    memset(out, 0, sizeof(*out));
    out->internalPort = internalPort;
    return;
  }

  memset(out, 0, sizeof(*out));
  out->internalPort = internalPort;

  plum_mapping_t req;
  memset(&req, 0, sizeof(req));
  req.protocol      = PLUM_IP_PROTOCOL_UDP;
  req.internal_port = internalPort;

  int id = plum_create_mapping(&req, plumMappingCb);
  if (id < 0) {
    SDL_UnlockMutex(slotsMutex);
    return;
  }

  slot->mappingId = id;
  slot->map       = out;
  slot->inUse     = true;
  SDL_UnlockMutex(slotsMutex);
}

void natPortMapRelease(NatPortMap *map) {
  if (!map || !slotsMutex) {
    return;
  }

  SDL_LockMutex(slotsMutex);
  int releaseId = -1;
  for (int i = 0; i < PORTMAP_MAX_SLOTS; ++i) {
    if (slots[i].inUse && slots[i].map == map) {
      releaseId        = slots[i].mappingId;
      slots[i].inUse   = false;
      slots[i].map     = NULL;
      slots[i].mappingId = 0;
      break;
    }
  }
  SDL_UnlockMutex(slotsMutex);

  if (releaseId >= 0) {
    plum_destroy_mapping(releaseId);
  }

  unsigned short keep = map->internalPort;
  memset(map, 0, sizeof(*map));
  map->internalPort = keep;
}

void natPortMapRenewIfNeeded(NatPortMap *map) {
  /* libplum renews mappings internally; this entry point exists so callers
   * don't need to know which library backs the wrapper. */
  (void)map;
}

#else /* !BOLO_PORTMAP */

void natPortMapRequest(unsigned short internalPort, NatPortMap *out) {
  if (!out) {
    return;
  }
  memset(out, 0, sizeof(*out));
  out->internalPort = internalPort;
}

void natPortMapRelease(NatPortMap *map)       { (void)map; }
void natPortMapRenewIfNeeded(NatPortMap *map) { (void)map; }

#endif
