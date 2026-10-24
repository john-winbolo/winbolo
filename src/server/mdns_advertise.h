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

/*********************************************************
 *Name:          mDNS Advertiser
 *Filename:      mdns_advertise.h
 *Purpose:
 *  Lifecycle hooks for the inline mDNS service advertiser
 *  (_winbolo._udp.local). The host opens a multicast socket
 *  at startup, answers discovery queries from the server tick
 *  (no thread of its own), and tears the socket down at
 *  shutdown. mdns.h is deliberately kept out of this header so
 *  the lifecycle's only caller (server_lifecycle.c) doesn't
 *  inherit the all-static-inline header's unused-function noise.
 *********************************************************/

#ifndef MDNS_ADVERTISE_H
#define MDNS_ADVERTISE_H

#include <stdbool.h>
#include <stddef.h>

/* ServerSim is opaque here — the poll hook only passes it through to the
 * server getters it reads. The struct tag (not a fresh typedef) is used so
 * this header stays compatible with server_sim.h under C99. */
struct ServerSim;

/* Open the IPv4 mDNS socket (join 224.0.0.251 on MDNS_PORT) and remember
 * gamePort for the SRV record. No-op safe to leave un-called; if the open
 * fails the poll/stop hooks below simply do nothing. */
void transportUdpServerStartMdnsAdvertiser(unsigned short gamePort);

/* Non-blocking drain of incoming _winbolo._udp.local queries, answering
 * each from a fresh snapshot of current server state. A cheap no-op when
 * the advertiser was never started. Call once per server tick. */
void transportUdpServerPollMdnsAdvertiser(struct ServerSim *sim);

/* Close the mDNS socket and clear state. Safe to call when never started
 * or already stopped. */
void transportUdpServerStopMdnsAdvertiser(void);

/* A platform's own advertiser, for a system that keeps multicast to itself
 * (iOS, where raw mDNS sockets need Apple's multicast entitlement but the
 * system's Bonjour API may register a service). Once one is set, the hooks
 * above register the service through it instead of opening a socket: the
 * first poll after start calls start with the instance label (no service
 * suffix), the game port, the name of the interface the advertised address
 * belongs to ("" when unknown) and the TXT pairs; later polls call update
 * with the pairs when they have changed, at most once a second; stop calls
 * stop. keys and values are count NUL-terminated strings, valid only for
 * the call. The calls come from the server tick, under the threads mutex.
 * start returns false when the registration could not be made; the
 * advertiser then stays off until the next start. NULL restores the
 * raw-socket advertiser. Set it once at startup, before a server starts. */
typedef struct {
  bool (*start)(const char *instance, unsigned short port, const char *ifName,
                const char *const *keys, const char *const *values,
                size_t count);
  void (*update)(const char *const *keys, const char *const *values,
                 size_t count);
  void (*stop)(void);
} MdnsPlatformAdvertiser;
void mdnsAdvertiseSetPlatform(const MdnsPlatformAdvertiser *advertiser);

/* A platform's hold on incoming multicast while the raw-socket advertiser
 * has its socket open (Android, whose Wi-Fi driver drops multicast unless
 * the app holds its multicast lock, which costs battery to hold at other
 * times). Called with true once the socket has opened and with false as it
 * closes, on the thread that starts or stops the server; not called for a
 * platform advertiser. NULL, the default, calls nothing. Set it once at
 * startup, before a server starts. */
void mdnsAdvertiseSetMulticastHold(void (*hold)(bool on));

#endif /* MDNS_ADVERTISE_H */
