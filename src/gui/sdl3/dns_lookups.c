/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/********************************************************
*Name:          dnsLookups
*Filename:      dns_lookups.c
*Purpose:
*  Cross-platform DNS lookup thread using SDL3.
*  Ported from win32/dnslookups.c.
*********************************************************/

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4005)
#include <crtdbg.h>
#pragma warning(pop)
#endif
#include <SDL3/SDL.h>
#include <string.h>
#include "global.h"
#include "platform_net.h"
#include "../dnsLookups.h"
#include "client_sim.h"

static SDL_Mutex *hDnsMutex = NULL;
static SDL_Thread *hDnsThread = NULL;
static dnsList dnsProcessing;
static dnsList dnsWaiting;
static bool dnsShouldRun;
static bool dnsFinished;
static ClientSim *dnsClientSim = NULL;

static int SDLCALL dnsLookupsThreadFunc(void *data);

bool dnsLookupsCreate(ClientSim *cs) {
  bool returnValue;

  returnValue = TRUE;
  dnsClientSim = cs;
  dnsProcessing = NULL;
  dnsWaiting = NULL;
  dnsShouldRun = TRUE;
  dnsFinished = FALSE;

  hDnsMutex = SDL_CreateMutex();
  if (hDnsMutex == NULL) {
    returnValue = FALSE;
  }

  if (returnValue == TRUE) {
    hDnsThread = SDL_CreateThread(dnsLookupsThreadFunc, "DnsLookups", NULL);
    if (hDnsThread == NULL) {
      SDL_DestroyMutex(hDnsMutex);
      hDnsMutex = NULL;
      returnValue = FALSE;
    }
  }
  return returnValue;
}

void dnsLookupsDestroy(void) {
  dnsList del;

  if (hDnsMutex != NULL) {
    dnsShouldRun = FALSE;
    while (dnsFinished == FALSE) {
      SDL_Delay(DNS_SHUTDOWN_SLEEP_TIME);
    }
    SDL_WaitThread(hDnsThread, NULL);
    hDnsThread = NULL;

    SDL_LockMutex(hDnsMutex);
    while (NonEmpty(dnsProcessing)) {
      del = dnsProcessing;
      dnsProcessing = dnsProcessing->next;
      Dispose(del);
    }
    while (NonEmpty(dnsWaiting)) {
      del = dnsWaiting;
      dnsWaiting = dnsWaiting->next;
      Dispose(del);
    }
    SDL_UnlockMutex(hDnsMutex);

    SDL_DestroyMutex(hDnsMutex);
    hDnsMutex = NULL;
  }
}

void dnsLookupsAddRequest(char *ip, void *func) {
  dnsList add;

  if (dnsShouldRun == TRUE) {
    New(add);
    strcpy(add->ip, ip);
    add->func = func;
    SDL_LockMutex(hDnsMutex);
    add->next = dnsWaiting;
    dnsWaiting = add;
    SDL_UnlockMutex(hDnsMutex);
  }
}

static int SDLCALL dnsLookupsThreadFunc(void *data) {
  char dest[512];
  dnsList q;

  (void)data;

  while (dnsShouldRun == TRUE) {
    SDL_LockMutex(hDnsMutex);
    dnsProcessing = dnsWaiting;
    dnsWaiting = NULL;
    SDL_UnlockMutex(hDnsMutex);

    while (NonEmpty(dnsProcessing) && dnsShouldRun == TRUE) {
      q = dnsProcessing;
      {
        struct sockaddr_in lookupAddr;
        struct hostent *hd;
        lookupAddr.sin_addr.s_addr = inet_addr(q->ip);
        hd = gethostbyaddr((const char *)&lookupAddr.sin_addr, sizeof(struct in_addr), AF_INET);
        if (hd == NULL) {
          strcpy(dest, q->ip);
        } else {
          strcpy(dest, hd->h_name);
        }
      }
      netProcessedDnsLookup(dnsClientSim, q->ip, dest);
      dnsProcessing = q->next;
      Dispose(q);
    }
    SDL_Delay(DNS_THREAD_SLEEP_TIME);
  }
  dnsFinished = TRUE;
  return 0;
}

int dnsLookupsRun(void) {
  return dnsLookupsThreadFunc(NULL);
}
