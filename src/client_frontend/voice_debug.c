/*
 * Copyright (c) 1998-2026 John Morrison.
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
 * Name:          Voice Debug
 * Filename:      voice_debug.c
 * Purpose:
 *   The capture-chain recorder. See voice_debug.h for what
 *   it records, what the columns mean, and the two things
 *   that have to be known before the files are read.
 *
 *   Plain C stdio only: no SDL, and no voice_backend.h -
 *   the caller passes the clock reading in, so this file
 *   sees none of the backend's private header.
 *
 *   Sample data is written and read in host byte order,
 *   which is little-endian on every platform the game
 *   builds for; only the WAV header fields are placed byte
 *   by byte.
 *********************************************************/

#if defined(WB_VOICEDEBUG)

#include <opus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "voice_core.h"
#include "voice_debug.h"

/* Canonical PCM WAV header: RIFF/WAVE, a 16 byte fmt chunk, then data. */
#define VOICE_DEBUG_WAV_HEADER 44

/* Where the two length fields sit, for the patch on close. */
#define VOICE_DEBUG_RIFF_SIZE_OFFSET  4
#define VOICE_DEBUG_DATA_SIZE_OFFSET 40

/* Each handle gets its own buffer.  The writes happen on the frame thread,
 * and an unbuffered fwrite per 20 ms frame produces exactly the stutter
 * this file exists to find. */
#define VOICE_DEBUG_BUFFER_BYTES (64 * 1024)

/* The recording directory, and the longest path built inside it: the
 * directory, a separator, and a file name. */
#define VOICE_DEBUG_DIR_MAX  256
#define VOICE_DEBUG_PATH_MAX 512

/* One open WAV, and the sample count its two length fields are patched
 * from when it is closed. */
typedef struct {
    FILE *fp;
    uint32_t samples;
} DebugWav;

/* The fixed taps, in enum order.  VOICE_TAP_REMOTE is not here: it is one
 * file per talker, opened when that talker is first heard. */
static const char *const tapFileNames[VOICE_TAP_REMOTE] = {
    "raw.wav", "gained.wav", "cleaned.wav", "roundtrip.wav"
};

static bool recording = false;

/* Whether voiceDebugStop has been handed to atexit.  Once for the life of
 * the process, however many recordings it makes. */
static bool stopRegistered = false;

static char outDir[VOICE_DEBUG_DIR_MAX];
static DebugWav tapWavs[VOICE_TAP_REMOTE];
static DebugWav remoteWavs[MAX_TANKS];
static FILE *framesCsv = NULL;
static FILE *speakersCsv = NULL;

/* The recorder's own decoder, so the round-trip file is the far end's view
 * and nothing is taken from the playback decoders. */
static VoiceDecoder *roundtripDecoder = NULL;

/* Counters for the summary line. */
static uint32_t framesCaptured = 0;
static uint32_t framesEncoded = 0;
static uint32_t framesOverLimit = 0;

/* Clock reading the next speakers.csv row is due at. */
static uint32_t nextSpeakerRowMs = 0;

/* The bandwidth of the frame that has just been encoded, in kHz, read off
 * the packet by voiceDebugRoundtrip and written by the frames.csv row that
 * follows it.  Cleared by that row, so a frame that never reached the
 * encoder writes 0 rather than the frame before it. */
static int frameBandwidthKhz = 0;

/* The injection source, and how much of its data chunk is left. */
static FILE *injectFp = NULL;
static uint32_t injectBytesLeft = 0;

/*********************************************************
*NAME:          putLe16
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Places a 16 bit WAV header field, least significant byte
*  first, whatever the host's own order is.
*
*ARGUMENTS:
*  p - where to place it
*  v - the value
*********************************************************/
static void putLe16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

/*********************************************************
*NAME:          putLe32
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Places a 32 bit WAV header field, least significant byte
*  first, whatever the host's own order is.
*
*ARGUMENTS:
*  p - where to place it
*  v - the value
*********************************************************/
static void putLe32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/*********************************************************
*NAME:          getLe16
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Reads a 16 bit WAV header field.
*
*ARGUMENTS:
*  p - the two bytes to read
*********************************************************/
static uint16_t getLe16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/*********************************************************
*NAME:          getLe32
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Reads a 32 bit WAV header field.
*
*ARGUMENTS:
*  p - the four bytes to read
*********************************************************/
static uint32_t getLe32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/*********************************************************
*NAME:          bandwidthKhz
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Turns an OPUS_BANDWIDTH_* constant into the kHz it stands
*  for, so the column can be read without a table.  Anything
*  else, including the negative return of a packet that could
*  not be read, is 0.
*
*ARGUMENTS:
*  bandwidth - what opus_packet_get_bandwidth returned
*********************************************************/
static int bandwidthKhz(int bandwidth) {
    switch (bandwidth) {
    case OPUS_BANDWIDTH_NARROWBAND:
        return 4;
    case OPUS_BANDWIDTH_MEDIUMBAND:
        return 6;
    case OPUS_BANDWIDTH_WIDEBAND:
        return 8;
    case OPUS_BANDWIDTH_SUPERWIDEBAND:
        return 12;
    case OPUS_BANDWIDTH_FULLBAND:
        return 20;
    default:
        return 0;
    }
}

/*********************************************************
*NAME:          debugPath
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Builds the path of one output file inside the recording
*  directory.
*
*ARGUMENTS:
*  out    - buffer to build into
*  outLen - its size
*  name   - the file name
*********************************************************/
static void debugPath(char *out, size_t outLen, const char *name) {
    snprintf(out, outLen, "%s/%s", outDir, name);
}

/*********************************************************
*NAME:          wavOpen
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Creates a 48 kHz mono S16 WAV and writes its header with
*  both lengths at zero; they are patched by wavClose.
*  Returns whether the file is open.
*
*ARGUMENTS:
*  w    - the handle to fill in
*  path - the file to create
*********************************************************/
static bool wavOpen(DebugWav *w, const char *path) {
    uint8_t hdr[VOICE_DEBUG_WAV_HEADER];

    w->fp = fopen(path, "wb");
    w->samples = 0;
    if (w->fp == NULL) {
        fprintf(stderr, "Voice recorder: cannot create %s\n", path);
        fflush(stderr);
        return false;
    }
    setvbuf(w->fp, NULL, _IOFBF, VOICE_DEBUG_BUFFER_BYTES);

    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "RIFF", 4);
    putLe32(hdr + 4, 0);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    putLe32(hdr + 16, 16);
    putLe16(hdr + 20, 1);                        /* PCM               */
    putLe16(hdr + 22, 1);                        /* mono              */
    putLe32(hdr + 24, VOICE_SAMPLE_RATE);
    putLe32(hdr + 28, VOICE_SAMPLE_RATE * 2u);   /* bytes per second  */
    putLe16(hdr + 32, 2);                        /* bytes per frame   */
    putLe16(hdr + 34, 16);                       /* bits per sample   */
    memcpy(hdr + 36, "data", 4);
    putLe32(hdr + 40, 0);

    if (fwrite(hdr, 1, sizeof(hdr), w->fp) != sizeof(hdr)) {
        fprintf(stderr, "Voice recorder: cannot write %s\n", path);
        fflush(stderr);
        fclose(w->fp);
        w->fp = NULL;
        return false;
    }
    return true;
}

/*********************************************************
*NAME:          wavWrite
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Appends one 20 ms frame, and counts it towards the two
*  lengths.  A closed handle is ignored.
*
*ARGUMENTS:
*  w   - the handle
*  pcm - VOICE_FRAME_SAMPLES mono S16
*********************************************************/
static void wavWrite(DebugWav *w, const int16_t *pcm) {
    if (w->fp == NULL) {
        return;
    }
    if (fwrite(pcm, sizeof(int16_t), VOICE_FRAME_SAMPLES, w->fp) !=
        (size_t)VOICE_FRAME_SAMPLES) {
        return;
    }
    w->samples += VOICE_FRAME_SAMPLES;
}

/*********************************************************
*NAME:          wavClose
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Patches the RIFF and data lengths from the sample count
*  and closes the file.  Safe on a handle that was never
*  opened.
*
*ARGUMENTS:
*  w - the handle
*********************************************************/
static void wavClose(DebugWav *w) {
    uint8_t field[4];
    uint32_t dataBytes;

    if (w->fp == NULL) {
        return;
    }
    dataBytes = w->samples * (uint32_t)sizeof(int16_t);
    if (fseek(w->fp, VOICE_DEBUG_RIFF_SIZE_OFFSET, SEEK_SET) == 0) {
        putLe32(field, (VOICE_DEBUG_WAV_HEADER - 8u) + dataBytes);
        fwrite(field, 1, sizeof(field), w->fp);
    }
    if (fseek(w->fp, VOICE_DEBUG_DATA_SIZE_OFFSET, SEEK_SET) == 0) {
        putLe32(field, dataBytes);
        fwrite(field, 1, sizeof(field), w->fp);
    }
    fclose(w->fp);
    w->fp = NULL;
    w->samples = 0;
}

/*********************************************************
*NAME:          csvOpen
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Creates one CSV in the recording directory and writes its
*  header row.  Returns the file, or NULL.
*
*ARGUMENTS:
*  name   - the file name
*  header - the header row, without its newline
*********************************************************/
static FILE *csvOpen(const char *name, const char *header) {
    char path[VOICE_DEBUG_PATH_MAX];
    FILE *fp;

    debugPath(path, sizeof(path), name);
    fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "Voice recorder: cannot create %s\n", path);
        fflush(stderr);
        return NULL;
    }
    setvbuf(fp, NULL, _IOFBF, VOICE_DEBUG_BUFFER_BYTES);
    fprintf(fp, "%s\n", header);
    return fp;
}

/*********************************************************
*NAME:          closeEverything
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Closes every file the recording opened, patching the WAV
*  lengths on the way, and releases the round-trip decoder.
*  The injection source is left alone: it is not part of the
*  recording, and the run that is ending still holds it so
*  the microphone is not opened in its place.
*
*ARGUMENTS:
*  (none)
*********************************************************/
static void closeEverything(void) {
    int i;

    for (i = 0; i < VOICE_TAP_REMOTE; i++) {
        wavClose(&tapWavs[i]);
    }
    for (i = 0; i < MAX_TANKS; i++) {
        wavClose(&remoteWavs[i]);
    }
    if (framesCsv != NULL) {
        fclose(framesCsv);
        framesCsv = NULL;
    }
    if (speakersCsv != NULL) {
        fclose(speakersCsv);
        speakersCsv = NULL;
    }
    voiceDecoderDestroy(roundtripDecoder);
    roundtripDecoder = NULL;
}

/*********************************************************
*NAME:          voiceDebugStart
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Opens every WAV and both CSVs in dir, which must already
*  exist, and creates the round-trip decoder.  Anything that
*  will not open takes the whole recording down rather than
*  leaving a set of files with a hole in it.
*
*ARGUMENTS:
*  dir - the directory to record into
*********************************************************/
bool voiceDebugStart(const char *dir) {
    char path[VOICE_DEBUG_PATH_MAX];
    int i;

    if (recording) {
        return true;
    }
    if (dir == NULL || dir[0] == '\0') {
        return false;
    }
    if (strlen(dir) >= sizeof(outDir)) {
        fprintf(stderr, "Voice recorder: %s is too long a directory name\n",
                dir);
        fflush(stderr);
        return false;
    }

    memset(tapWavs, 0, sizeof(tapWavs));
    memset(remoteWavs, 0, sizeof(remoteWavs));
    framesCaptured = 0;
    framesEncoded = 0;
    framesOverLimit = 0;
    nextSpeakerRowMs = 0;
    frameBandwidthKhz = 0;
    snprintf(outDir, sizeof(outDir), "%s", dir);

    for (i = 0; i < VOICE_TAP_REMOTE; i++) {
        debugPath(path, sizeof(path), tapFileNames[i]);
        if (!wavOpen(&tapWavs[i], path)) {
            closeEverything();
            return false;
        }
    }

    framesCsv = csvOpen("frames.csv",
                        "frame,ms,inputLevel,rmsPostAec,clipped,micOpen,"
                        "sending,encodedLen,overWireLimit,bandwidth");
    speakersCsv = csvOpen("speakers.csv",
                          "ms,player,played,concealed,lateDropped,evicted,"
                          "queued");
    if (framesCsv == NULL || speakersCsv == NULL) {
        closeEverything();
        return false;
    }

    roundtripDecoder = voiceDecoderCreate();
    if (roundtripDecoder == NULL) {
        fprintf(stderr, "Voice recorder: cannot create the round-trip "
                        "decoder\n");
        fflush(stderr);
        closeEverything();
        return false;
    }

    recording = true;

    /* main has return paths that never reach voiceCleanup - quitting from
     * the welcome screen, which returns FALSE out of gameFrontStart, is the
     * ordinary one - and a recording that ended on one of those left every
     * WAV with both length fields at zero and no player would open it.
     * voiceDebugStop does nothing when there is no recording, so the call
     * from voiceCleanup and this one cannot both do the work. */
    if (!stopRegistered) {
        stopRegistered = (atexit(voiceDebugStop) == 0);
    }

    fprintf(stderr, "Voice recorder: recording to %s\n", outDir);
    fflush(stderr);
    return true;
}

/*********************************************************
*NAME:          voiceDebugStop
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Ends the recording: patches the WAV lengths, closes every
*  file, and prints what the run saw.  Does nothing when
*  there is no recording, so a second call and an exit after
*  the injection ran out are both harmless.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceDebugStop(void) {
    if (!recording) {
        return;
    }
    recording = false;
    closeEverything();
    fprintf(stderr,
            "Voice recorder: %u frames captured, %u encoded, %u over the "
            "wire limit\n",
            framesCaptured, framesEncoded, framesOverLimit);
    fflush(stderr);
}

/*********************************************************
*NAME:          voiceDebugIsRecording
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns whether a recording is running.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceDebugIsRecording(void) {
    return recording;
}

/*********************************************************
*NAME:          voiceDebugTap
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Writes one frame to the file for that stage of the chain.
*  A remote talker's file is created the first time anything
*  is heard from them, so a quiet game leaves no empty files
*  behind.
*
*ARGUMENTS:
*  tap    - which stage the frame came from
*  player - the talker, for VOICE_TAP_REMOTE only
*  pcm    - VOICE_FRAME_SAMPLES mono S16
*********************************************************/
void voiceDebugTap(VoiceTap tap, int player, const int16_t *pcm) {
    char path[VOICE_DEBUG_PATH_MAX];
    char name[64];

    if (!recording || pcm == NULL) {
        return;
    }
    if (tap == VOICE_TAP_REMOTE) {
        if (player < 0 || player >= MAX_TANKS) {
            return;
        }
        if (remoteWavs[player].fp == NULL) {
            snprintf(name, sizeof(name), "remote-%d.wav", player);
            debugPath(path, sizeof(path), name);
            if (!wavOpen(&remoteWavs[player], path)) {
                return;
            }
        }
        wavWrite(&remoteWavs[player], pcm);
        return;
    }
    /* VOICE_TAP_RAW is zero, so the one bound covers the whole enum. */
    if (tap >= VOICE_TAP_REMOTE) {
        return;
    }
    wavWrite(&tapWavs[tap], pcm);
}

/*********************************************************
*NAME:          voiceDebugFrameStats
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Writes one frames.csv row.  Called once per captured
*  frame, including a frame that was never encoded, which is
*  the one a hole in the audio is found in.
*
*  The last column is the bandwidth Opus chose for the frame,
*  in kHz: 4 narrowband, 6 mediumband, 8 wideband, 12
*  superwideband, 20 fullband, and 0 for a frame that was
*  never encoded.  voiceDebugRoundtrip read it off the packet
*  just before this call, and this row clears it.
*
*ARGUMENTS:
*  nowMs         - the backend clock, wall clock rather than
*                  audio time
*  inputLevel    - the meter's 0..1 reading after mic gain
*  rmsPostAec    - the reading the open-mic decision uses
*  clipped       - samples the mic gain pushed out of range
*  micOpen       - whether the microphone counted as open
*  sending       - whether the frame was being transmitted
*  encodedLen    - encoded bytes, 0 when it was not encoded
*  overWireLimit - encodedLen is past what one segment holds
*********************************************************/
void voiceDebugFrameStats(uint32_t nowMs, float inputLevel, float rmsPostAec,
                          int clipped, bool micOpen, bool sending,
                          int encodedLen, bool overWireLimit) {
    if (!recording || framesCsv == NULL) {
        return;
    }
    fprintf(framesCsv, "%u,%u,%.6f,%.6f,%d,%d,%d,%d,%d,%d\n", framesCaptured,
            nowMs, (double)inputLevel, (double)rmsPostAec, clipped,
            micOpen ? 1 : 0, sending ? 1 : 0, encodedLen,
            overWireLimit ? 1 : 0, frameBandwidthKhz);
    frameBandwidthKhz = 0;
    framesCaptured++;
    if (encodedLen > 0) {
        framesEncoded++;
    }
    if (overWireLimit) {
        framesOverLimit++;
    }
}

/*********************************************************
*NAME:          voiceDebugRoundtrip
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Decodes the frame that has just been encoded and writes
*  it to the round-trip file - what the far end would hear,
*  less the network.  A decode that fails is dropped rather
*  than written, so the file never holds noise the chain
*  never produced.
*
*  Also reads the bandwidth out of the packet for the
*  frames.csv row that follows.  Taken above the decode, so a
*  frame that will not decode still says what it was encoded
*  at.  Nothing asks the encoder for a bandwidth, so this is
*  what it chose from the bitrate.
*
*ARGUMENTS:
*  packet - the encoded frame
*  len    - its length in bytes
*********************************************************/
void voiceDebugRoundtrip(const uint8_t *packet, int len) {
    int16_t pcm[VOICE_FRAME_SAMPLES];

    if (!recording || roundtripDecoder == NULL || packet == NULL || len <= 0) {
        return;
    }
    frameBandwidthKhz = bandwidthKhz(opus_packet_get_bandwidth(packet));
    if (voiceDecoderDecode(roundtripDecoder, packet, len, pcm) < 0) {
        return;
    }
    wavWrite(&tapWavs[VOICE_TAP_ROUNDTRIP], pcm);
}

/*********************************************************
*NAME:          voiceDebugSpeakerStatsDue
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns true at most once a second, and only while
*  recording.  The talker table belongs to the caller, so it
*  asks first and walks its talkers only when a row is due.
*
*ARGUMENTS:
*  nowMs - the backend clock
*********************************************************/
bool voiceDebugSpeakerStatsDue(uint32_t nowMs) {
    if (!recording) {
        return false;
    }
    if (nowMs < nextSpeakerRowMs) {
        return false;
    }
    nextSpeakerRowMs = nowMs + 1000;
    return true;
}

/*********************************************************
*NAME:          voiceDebugSpeakerRow
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Writes one speakers.csv row for one remote talker.
*
*ARGUMENTS:
*  nowMs  - the backend clock
*  player - the talker
*  stats  - their cumulative jitter buffer counters
*  queued - frames still waiting on their playback stream
*********************************************************/
void voiceDebugSpeakerRow(uint32_t nowMs, int player,
                          const VoiceSpeakerStats *stats, int queued) {
    if (!recording || speakersCsv == NULL || stats == NULL) {
        return;
    }
    fprintf(speakersCsv, "%u,%d,%u,%u,%u,%u,%d\n", nowMs, player,
            stats->played, stats->concealed, stats->lateDropped,
            stats->evicted, queued);
}

/*********************************************************
*NAME:          voiceDebugInjectOpen
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Opens a WAV to read in place of the microphone.  Walks the
*  RIFF chunks for fmt and data, and takes only 48 kHz mono
*  S16 PCM - the format the whole chain runs at.  Anything
*  else is refused with a line saying what was found, rather
*  than played as noise.  Returns whether it is open.
*
*ARGUMENTS:
*  wavPath - the file to read
*********************************************************/
bool voiceDebugInjectOpen(const char *wavPath) {
    uint8_t riff[12];
    uint8_t chunk[8];
    uint8_t fmt[16];
    uint16_t format = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t rate = 0;
    uint32_t size;
    long skip;
    bool haveFmt = false;
    FILE *fp;

    if (injectFp != NULL) {
        return true;
    }
    if (wavPath == NULL || wavPath[0] == '\0') {
        return false;
    }
    fp = fopen(wavPath, "rb");
    if (fp == NULL) {
        fprintf(stderr, "Voice recorder: cannot open %s\n", wavPath);
        fflush(stderr);
        return false;
    }
    if (fread(riff, 1, sizeof(riff), fp) != sizeof(riff) ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "Voice recorder: %s is not a RIFF/WAVE file\n",
                wavPath);
        fflush(stderr);
        fclose(fp);
        return false;
    }

    /* Chunk sizes are odd-padded, so an odd one is followed by a byte that
     * belongs to neither chunk. */
    while (fread(chunk, 1, sizeof(chunk), fp) == sizeof(chunk)) {
        size = getLe32(chunk + 4);
        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (size < sizeof(fmt) ||
                fread(fmt, 1, sizeof(fmt), fp) != sizeof(fmt)) {
                fprintf(stderr, "Voice recorder: %s has a short fmt chunk\n",
                        wavPath);
                fflush(stderr);
                fclose(fp);
                return false;
            }
            format = getLe16(fmt);
            channels = getLe16(fmt + 2);
            rate = getLe32(fmt + 4);
            bits = getLe16(fmt + 14);
            haveFmt = true;
            skip = (long)(size - sizeof(fmt)) + (long)(size & 1u);
            if (skip > 0 && fseek(fp, skip, SEEK_CUR) != 0) {
                break;
            }
        } else if (memcmp(chunk, "data", 4) == 0) {
            if (!haveFmt) {
                fprintf(stderr,
                        "Voice recorder: %s has no fmt chunk before its "
                        "data\n",
                        wavPath);
                fflush(stderr);
                fclose(fp);
                return false;
            }
            if (format != 1 || channels != 1 || rate != VOICE_SAMPLE_RATE ||
                bits != 16) {
                fprintf(stderr,
                        "Voice recorder: %s is format %u, %u channel, %u Hz, "
                        "%u bit; need PCM (1), 1 channel, %d Hz, 16 bit\n",
                        wavPath, (unsigned)format, (unsigned)channels,
                        (unsigned)rate, (unsigned)bits, VOICE_SAMPLE_RATE);
                fflush(stderr);
                fclose(fp);
                return false;
            }
            injectFp = fp;
            injectBytesLeft = size;
            return true;
        } else {
            skip = (long)size + (long)(size & 1u);
            if (fseek(fp, skip, SEEK_CUR) != 0) {
                break;
            }
        }
    }

    fprintf(stderr, "Voice recorder: %s has no data chunk\n", wavPath);
    fflush(stderr);
    fclose(fp);
    return false;
}

/*********************************************************
*NAME:          voiceDebugInjectIsOpen
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns whether the microphone is being read from a file.
*  Stays true once the file has run out, so nothing opens a
*  recording device in its place afterwards.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceDebugInjectIsOpen(void) {
    return injectFp != NULL;
}

/*********************************************************
*NAME:          voiceDebugInjectRead
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Hands over the next frame of the injected file, padding a
*  short final frame with silence.  At the end of the file it
*  returns false and stops the recording, which is what ends
*  the run: there is no looping, because two recordings of
*  the same file have to line up.
*
*ARGUMENTS:
*  pcm - VOICE_FRAME_SAMPLES mono S16 to fill
*********************************************************/
bool voiceDebugInjectRead(int16_t *pcm) {
    const size_t frameBytes = sizeof(int16_t) * VOICE_FRAME_SAMPLES;
    size_t want;
    size_t got;

    if (injectFp == NULL || pcm == NULL) {
        return false;
    }
    want = frameBytes;
    if ((uint32_t)want > injectBytesLeft) {
        want = (size_t)injectBytesLeft;
    }
    got = (want > 0) ? fread(pcm, 1, want, injectFp) : 0;
    if (got == 0) {
        injectBytesLeft = 0;
        voiceDebugStop();
        return false;
    }
    injectBytesLeft -= (uint32_t)got;
    if (got < frameBytes) {
        memset((uint8_t *)pcm + got, 0, frameBytes - got);
    }
    return true;
}

#endif /* WB_VOICEDEBUG */
