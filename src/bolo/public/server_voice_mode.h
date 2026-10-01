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
 *Name:          Server Voice Mode
 *Filename:      server_voice_mode.h
 *Author:        John Morrison
 *Purpose:
 *  How a server handles the voice its clients send it.
 *  Fixed when the server starts, and read by the paths
 *  that advertise the server.
 *
 *  Not the client's own capture mode — that is VoiceMode
 *  in src/gui/voice.h, and it is about one player's
 *  microphone rather than about the server.
 *********************************************************/

#ifndef SERVER_VOICE_MODE_H
#define SERVER_VOICE_MODE_H

/* Values are ordered so a zero-initialized server config, a server whose
 * advertisement predates the field, and any value that cannot be parsed all
 * mean ON — which is what every server did before this setting existed. */
typedef enum {
    serverVoiceOn        = 0,   /* zero = the default everywhere */
    serverVoiceOff       = 1,
    serverVoiceProximity = 2    /* not implemented; treated as on */
} ServerVoiceMode;

#endif /* SERVER_VOICE_MODE_H */
