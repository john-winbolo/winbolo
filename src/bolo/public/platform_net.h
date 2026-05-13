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

/*********************************************************
*Name:          Platform Network Abstraction
*Filename:      platform_net.h
*Purpose:
*  Thin cross-platform socket abstraction for WinBolo.
*  Consolidates the Windows/POSIX socket API differences
*  so that networking code in bolo/ and server/ can use
*  a single set of types and includes.
*
*  On Windows:  wraps Winsock2
*  On POSIX:    wraps BSD sockets
*********************************************************/

#ifndef PLATFORM_NET_H
#define PLATFORM_NET_H

#include <string.h>  /* memset, strncpy used by helpers below */
#include <stddef.h>  /* size_t */

#ifdef _WIN32
  #include <WinSock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET bolo_socket_t;
  #define BOLO_INVALID_SOCKET INVALID_SOCKET
  #define BOLO_SOCKET_ERROR   SOCKET_ERROR
#else
  /* Save current pack alignment and reset to default before system headers.
   * macOS system headers (netinet/in.h, net/if.h, etc.) use bare #pragma pack()
   * which resets alignment without pop, corrupting any push stack pushed by
   * bolo headers (e.g. brain.h does #pragma pack(push,8)).             */
  #pragma pack(push)
  #pragma pack()
  /* brain.h defines a 2-arg setkey() macro before including us.
   * macOS <unistd.h> declares void setkey(const char *) which the macro
   * expands incorrectly (wrong arg count → compile error).  Save and
   * temporarily suppress the macro around the system includes.         */
  #ifdef setkey
  #  pragma push_macro("setkey")
  #  undef setkey
  #  define BOLO_PLATFORM_NET_RESTORED_SETKEY
  #endif
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <netdb.h>
  #include <fcntl.h>
  #include <sys/ioctl.h>
  /* Restore setkey macro and previous pack alignment */
  #ifdef BOLO_PLATFORM_NET_RESTORED_SETKEY
  #  pragma pop_macro("setkey")
  #  undef BOLO_PLATFORM_NET_RESTORED_SETKEY
  #endif
  #pragma pack(pop)
  typedef int bolo_socket_t;
  #define BOLO_INVALID_SOCKET (-1)
  #define BOLO_SOCKET_ERROR   (-1)
  /* POSIX compatibility aliases used by existing code */
  #ifndef INVALID_SOCKET
  #  define INVALID_SOCKET    (-1)
  #endif
  #ifndef SOCKET_ERROR
  #  define SOCKET_ERROR      (-1)
  #endif
  #ifndef SD_BOTH
  #  define SD_BOTH           SHUT_RDWR
  #endif
  #ifndef SD_SEND
  #  define SD_SEND           SHUT_WR
  #endif
  /* Provide SOCKET as a POSIX alias so existing code compiles unchanged */
  typedef int SOCKET;
  /* On POSIX, closesocket() maps to close() */
  #define closesocket         close
  /* ioctlsocket maps to ioctl on POSIX */
  #define ioctlsocket(s,r,v)  ioctl((s),(r),(v))
  /* FIONBIO is defined in <sys/ioctl.h> on Linux, but just in case: */
  #ifndef FIONBIO
  #  include <sys/filio.h>
  #endif
#endif

/*
 * bolo_net_init() — Initialise the network subsystem.
 *   On Windows: calls WSAStartup(2.0).
 *   On POSIX:   no-op, returns 0.
 * Returns 0 on success, non-zero on failure.
 */
static inline int bolo_net_init(void) {
#ifdef _WIN32
  WSADATA wsaData;
  return WSAStartup(MAKEWORD(2, 0), &wsaData);
#else
  return 0;
#endif
}

/*
 * bolo_net_cleanup() — Shut down the network subsystem.
 *   On Windows: calls WSACleanup().
 *   On POSIX:   no-op.
 */
static inline void bolo_net_cleanup(void) {
#ifdef _WIN32
  WSACleanup();
#endif
}

/* (LAN-IP discovery helper lives in imgui_lobby.cpp directly — it's
 * the only consumer and putting it in this header tickled MSVC's C89
 * strict-mode warnings.) */

#endif /* PLATFORM_NET_H */
