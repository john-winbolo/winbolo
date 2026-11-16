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
 *Name:          Transport Command Codec
 *Filename:      transport_command_codec.h
 *Purpose:
 *  Per-variant encoder/decoder table for ClientCommand.
 *  Encoders run on the UDP client side and produce a
 *  complete wire packet (8-byte header + variant body)
 *  ready for udpClientSendTo. Decoders run on the UDP
 *  server side and build a ClientCommand the dispatcher
 *  consumes via serverSimApplyCommand.
 *
 *  Co-located so the client (encode) and server (decode)
 *  sides of each command sit next to each other.
 *********************************************************/

#ifndef TRANSPORT_COMMAND_CODEC_H
#define TRANSPORT_COMMAND_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "client_command.h"
#include "transport_udp_internal.h"  /* UDP_MAX_SEND, PACKET_HEADER_SIZE */

/* Stack-allocation upper bound for any single ClientCommand's wire
 * encoding, and the size decoders are handed. The largest variant the
 * fields allow is CMD_SET_SCRIPT_LIST, which carries a whole script list:
 * header(8) + cmdSeq(4) + count(1) +
 * CMD_SCRIPT_LIST_MAX * (1 + CMD_SCRIPT_LIST_FILE_LEN - 1) =
 * 13 + 10 * 128 = 1293. Behind it are CMD_LOBBY_SET_MAP (header + 1 +
 * 256 = 265) and CMD_CHAT (header + 1 + 128 = 137).
 *
 * 1400, the receive buffer size UDP_MAX_PAYLOAD, rather than 1293, so a
 * decoder's buffer holds anything one received datagram can carry. It is
 * not what a sender may build: COMMAND_MAX_SEND_BYTES below is. This is a
 * stack bound and not a queue bound: two of these buffers exist at a time,
 * one in buildInputPacket's caller and one in the drain loop. */
#define COMMAND_MAX_WIRE_BYTES 1400

/* The largest encoded command that fits one PACKET_COMMAND_TICK on its own.
 * The queue drain in transport_udp_client.c packs entries behind an 8-byte
 * header and a count byte, each entry behind a 2-byte length, and holds the
 * datagram to UDP_MAX_SEND, so one entry alone may be 1232 - 8 - 1 - 2 =
 * 1221 bytes.
 *
 * Only CMD_SET_SCRIPT_LIST can reach it. Its fields allow 1293 bytes, so its
 * encoder refuses a list longer than this, the way it refuses a count past
 * CMD_SCRIPT_LIST_MAX: ten names may hold 1198 bytes between them, not the
 * 1270 the fields would take. A command the drain could never send would sit
 * at the head of the queue for good and hold every later command behind it. */
#define COMMAND_MAX_SEND_BYTES (UDP_MAX_SEND - PACKET_HEADER_SIZE - 1 - 2)

/* Encode a ClientCommand into a complete wire packet (header +
 * body). On success writes `buf[0..*outLen)` and returns true.
 * Returns false on:
 *   - cmd == NULL or buf == NULL or outLen == NULL
 *   - cmd->type out of range or unsupported (e.g. CMD_NONE)
 *   - bufCap < required size
 * The header is packed with the matching PACKET_* type and
 * sequence 0; the per-command sequence rides in a 4-byte cmdSeq
 * slot immediately after the header (filled from cmd->cmdSeq). */
bool commandCodecEncode(const ClientCommand *cmd,
                        uint8_t *buf, size_t bufCap, size_t *outLen);

/* Decode a wire packet into a ClientCommand. Reads the packet-type
 * byte at buf[2], looks up the matching decoder, fills *cmd.
 * Returns false on:
 *   - buf == NULL or cmd == NULL
 *   - len shorter than header + cmdSeq slot
 *   - buf[0..1] != BOLO_NEW_MAGIC_0/_1
 *   - packet type isn't backed by a ClientCommand variant
 *   - decoder's bounds check fails
 * The cmdSeq slot at offset PACKET_HEADER_SIZE is read into
 * cmd->cmdSeq; the header itself is not surfaced. */
bool commandCodecDecode(const uint8_t *buf, size_t len,
                        ClientCommand *cmd);

#endif /* TRANSPORT_COMMAND_CODEC_H */
