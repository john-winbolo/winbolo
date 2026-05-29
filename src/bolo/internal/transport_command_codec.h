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

/* Stack-allocation upper bound for any single ClientCommand's wire
 * encoding. Largest variants are CMD_LOBBY_SET_MAP (header + 1 +
 * 256 = 265) and CMD_CHAT (header + 1 + 128 = 137). 512 leaves
 * comfortable headroom below the UDP datagram cap. */
#define COMMAND_MAX_WIRE_BYTES 512

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
