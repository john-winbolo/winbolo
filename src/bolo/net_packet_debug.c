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
*Name:          Net Packet Debug
*Filename:      net_packet_debug.c
*Purpose:
*  Network packet debug logger implementation.
*********************************************************/

#include "net_packet_debug.h"

#if BOLO_NET_PACKET_DEBUG

#include "netpacks.h"
#include <string.h>
#include <time.h>

/* Max bytes to hex-dump per packet */
#define NET_PKT_DEBUG_HEXDUMP_BYTES 64

static int netPktDebugEnabled = 0;
static FILE *netPktDebugFile = NULL;

void netPacketDebugEnable(int enable) {
  netPktDebugEnabled = enable;
}

int netPacketDebugIsEnabled(void) {
  return netPktDebugEnabled;
}

void netPacketDebugSetFile(FILE *f) {
  netPktDebugFile = f;
}

static FILE *netPktDebugGetOutput(void) {
  return netPktDebugFile ? netPktDebugFile : stderr;
}

static const char *netPktDebugTypeName(BYTE type) {
  switch (type) {
    case BOLOPACKET_INFOREQUEST:        return "INFO_REQUEST";
    case BOLOPACKET_INFORESPONSE:       return "INFO_RESPONSE";
    case BOLOPACKET_PINGREQUEST:        return "PING_REQUEST";
    case BOLOPACKET_PINGRESPONSE:       return "PING_RESPONSE";
    case BOLOPACKET_PASSWORDCHECK:      return "PASSWORD_CHECK";
    case BOLOPACKET_PASSWORDACCEPT:     return "PASSWORD_ACCEPT";
    case BOLOPACKET_PASSWORDFAIL:       return "PASSWORD_FAIL";
    case BOLOPACKET_NAMECHECK:          return "NAME_CHECK";
    case BOLOPACKET_NAMEACCEPT:         return "NAME_ACCEPT";
    case BOLOPACKET_NAMEFAIL:           return "NAME_FAIL";
    case BOLOPACKET_PLAYERDATAREQUEST:  return "PLAYERDATA_REQUEST";
    case BOLOPACKET_PLAYERDATARESPONSE: return "PLAYERDATA_RESPONSE";
    case BOLOPACKET_PLAYERNUMREQUEST:   return "PLAYERNUM_REQUEST";
    case BOLOPACKET_PLAYERNUMRESPONSE:  return "PLAYERNUM_RESPONSE";
    case BOLOPACKET_PLAYERNEWPLAYER:    return "NEW_PLAYER";
    case BOLOPACKET_DATA:               return "DATA";
    case BOLOPACKET_BASESDATAREQUEST:   return "BASES_REQUEST";
    case BOLOPACKET_BASESDATARESPONSE:  return "BASES_RESPONSE";
    case BOLOPACKET_STARTSDATAREQUEST:  return "STARTS_REQUEST";
    case BOLOPACKET_STARTSDATARESPONSE: return "STARTS_RESPONSE";
    case BOLOPACKET_PILLSDATAREQUEST:   return "PILLS_REQUEST";
    case BOLOPACKET_PILLSDATARESPONSE:  return "PILLS_RESPONSE";
    case BOLOPACKET_MAPDATAREQUEST:     return "MAP_REQUEST";
    case BOLOPACKET_MAPDATARESPONSE:    return "MAP_RESPONSE";
    case BOLOPACKET_MESSAGE:            return "MESSAGE";
    case BOLOCHANGENAME_DATA:           return "CHANGE_NAME";
    case BOLOPACKET_VALID:              return "VALID";
    case BOLOPACKET_INVALID:            return "INVALID";
    case BOLOPACKET_PLAYERLEAVE:        return "PLAYER_LEAVE";
    case BOLOPACKET_TIMEREQUEST:        return "TIME_REQUEST";
    case BOLOPACKET_TIMERESPONSE:       return "TIME_RESPONSE";
    case BOLOPACKET_MESSAGE_ALL_PLAYERS:return "MESSAGE_ALL";
    case BOLOPACKET_TOKEN:              return "TOKEN";
    case BOLOCLIENT_DATA:               return "CLIENT_DATA";
    case BOLOLEAVEALLIANCE_DATA:        return "LEAVE_ALLIANCE";
    case BOLOREQUESTALLIANCE_DATA:      return "REQUEST_ALLIANCE";
    case BOLOACCEPTALLIANCE_DATA:       return "ACCEPT_ALLIANCE";
    case BOLOSERVERMESSAGE:             return "SERVER_MESSAGE";
    case BOLOALLOWNEWPLAYERS:           return "ALLOW_PLAYERS";
    case BOLONOALLOWNEWPLAYERS:         return "DISALLOW_PLAYERS";
    case BOLOREJOINREQUEST:             return "REJOIN_REQUEST";
    case BOLOLGMRETURN:                 return "LGM_RETURN";
    case BOLOREQUEST_STARTPOS:          return "START_POS_REQUEST";
    case BOLORESPONSE_STARTPOS:         return "START_POS_RESPONSE";
    case BOLOPACKET_SERVERKEYREQUEST:   return "SERVER_KEY_REQUEST";
    case BOLOPACKET_SERVERKEYRESPONSE:  return "SERVER_KEY_RESPONSE";
    case BOLOPACKET_CLIENTKEY:          return "CLIENT_KEY";
    case BOLOPACKET_PACKETREREQUEST:    return "REREQUEST";
    case BOLOPACKET_PACKETQUIT:         return "QUIT";
    case BOLOPACKET_LGM_OUTWORKING:    return "LGM_WORKING";
    case BOLOPACKET_GAMELOCKED:         return "GAME_LOCKED";
    case BOLOPACKET_MAXPLAYERS:         return "MAX_PLAYERS";
    case BOLOPACKET_RETRANSMITTED_PACKETS: return "RETRANSMITTED";
    case BOLOPACKET_RSACHECK:           return "RSA_CHECK";
    case BOLOPACKET_RSARESPONSE:        return "RSA_RESPONSE";
    case BOLOPACKET_RSAACCEPT:          return "RSA_ACCEPT";
    case BOLOPACKET_RSAFAIL:            return "RSA_FAIL";
    default:                            return "UNKNOWN";
  }
}

void netPacketDebugLog(netPacketSide side, netPacketDir dir,
                       const BYTE *buff, int len, const char *extraInfo) {
  FILE *out;
  int i, dumpLen;
  const char *sideStr;
  const char *dirStr;
  const char *typeName;
  BYTE pktType;
  BYTE verMaj, verMin, verRev;

  if (!netPktDebugEnabled || len <= 0 || buff == NULL) {
    return;
  }

  out = netPktDebugGetOutput();
  sideStr = (side == NET_PKT_SIDE_CLIENT) ? "CLI" : "SRV";
  dirStr = (dir == NET_PKT_DIR_RECV) ? "RECV" : "SEND";

  /* Extract header fields if packet is large enough */
  if (len >= 8) {
    verMaj = buff[BOLO_VERSION_MAJORPOS];
    verMin = buff[BOLO_VERSION_MINORPOS];
    verRev = buff[BOLO_VERSION_REVISIONPOS];
    pktType = buff[BOLOPACKET_REQUEST_TYPEPOS];
    typeName = netPktDebugTypeName(pktType);
    fprintf(out, "[PKT] %s %s type=%d(%s) len=%d ver=%d.%d.%02x",
            sideStr, dirStr, pktType, typeName, len,
            verMaj, verMin, verRev);
  } else {
    fprintf(out, "[PKT] %s %s len=%d (too short for header)",
            sideStr, dirStr, len);
  }

  if (extraInfo) {
    fprintf(out, " [%s]", extraInfo);
  }
  fprintf(out, "\n");

  /* Hex dump */
  dumpLen = len;
  if (dumpLen > NET_PKT_DEBUG_HEXDUMP_BYTES) {
    dumpLen = NET_PKT_DEBUG_HEXDUMP_BYTES;
  }

  fprintf(out, "      hex(%d/%d): ", dumpLen, len);
  for (i = 0; i < dumpLen; i++) {
    fprintf(out, "%02x ", buff[i]);
    if ((i + 1) % 16 == 0 && i + 1 < dumpLen) {
      fprintf(out, "\n                  ");
    }
  }
  if (dumpLen < len) {
    fprintf(out, "...");
  }
  fprintf(out, "\n");
  fflush(out);
}

#endif /* BOLO_NET_PACKET_DEBUG */
