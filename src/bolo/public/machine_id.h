/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Machine id
 *Filename:      machine_id.h
 *Purpose:
 *  A random id for this computer, shared by every WinBolo
 *  program on it, so a server can find two Bot Hosts (or a
 *  Bot Host and the server) that run on one computer. The
 *  id never leaves the computer: a program answers a salt
 *  from the server with SHA-256(id || salt).
 *
 *  The id is a 32-byte file, machine_id, in:
 *    Windows: C:\ProgramData\WinBolo
 *    macOS:   /Users/Shared/WinBolo
 *    Linux:   /var/lib/winbolo when it is writable, else
 *             the user data folder
 *  WINBOLO_MACHINE_ID_DIR names another folder (tests).
 *
 *  A program answers each salt once only: it keeps the
 *  last MACHINE_SALT_KEEP salts it answered (in memory and
 *  in the file machine_salts beside the id), and a salt it
 *  answered before gets no answer.
 *********************************************************/

#ifndef MACHINE_ID_H
#define MACHINE_ID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MACHINE_ID_LEN     32
#define MACHINE_SALT_LEN   16
#define MACHINE_HASH_LEN   32
#define MACHINE_SALT_KEEP  64

/* SHA-256 of len bytes. */
void machineSha256(const void *data, size_t len,
                   uint8_t out[MACHINE_HASH_LEN]);

/* SHA-256(id || salt). */
void machineIdHashSalt(const uint8_t id[MACHINE_ID_LEN],
                       const uint8_t salt[MACHINE_SALT_LEN],
                       uint8_t out[MACHINE_HASH_LEN]);

/* n random bytes from the operating system. False when there are none. */
bool machineRandomBytes(uint8_t *out, size_t n);

/* The salts a program answered, the newest MACHINE_SALT_KEEP. */
typedef struct {
    uint8_t salt[MACHINE_SALT_KEEP][MACHINE_SALT_LEN];
    int     count;   /* 0..MACHINE_SALT_KEEP */
    int     next;    /* the place for the next one */
} MachineSaltMemory;

bool machineSaltMemorySeen(const MachineSaltMemory *m,
                           const uint8_t salt[MACHINE_SALT_LEN]);
void machineSaltMemoryAdd(MachineSaltMemory *m,
                          const uint8_t salt[MACHINE_SALT_LEN]);

/* The answer to salt from id and the memory m: false (no answer) when m
 * holds the salt; else the hash, and m holds the salt. */
bool machineSaltAnswer(MachineSaltMemory *m, const uint8_t id[MACHINE_ID_LEN],
                       const uint8_t salt[MACHINE_SALT_LEN],
                       uint8_t out[MACHINE_HASH_LEN]);

/* This computer's id: read from the file, or made and written when there
 * is none. Loads once, with the salt file; call it at program start so two
 * programs on the computer both answer a new salt. False when there is no
 * id (no random bytes). */
bool machineIdInit(void);
bool machineIdGet(uint8_t out[MACHINE_ID_LEN]);

/* The answer to a server's salt for this computer: false (no answer) when
 * there is no id or this program answered the salt before. The salt goes
 * in the salt file. */
bool machineIdAnswerSalt(const uint8_t salt[MACHINE_SALT_LEN],
                         uint8_t out[MACHINE_HASH_LEN]);

/* For a program that cannot read the id file (the Bot Host child in its
 * file sandbox, bothost_child.h): use this id and these answered salts,
 * with no files. The program that gave them keeps the files. */
void machineIdUseGiven(const uint8_t id[MACHINE_ID_LEN],
                       const MachineSaltMemory *salts);
/* A copy of the answered salts, to give to such a program. */
void machineIdSaltSnapshot(MachineSaltMemory *out);
/* A salt such a program answered: kept in memory and in the salt file, so
 * no program on this computer answers it again. */
void machineIdNoteSalt(const uint8_t salt[MACHINE_SALT_LEN]);
/* Called (outside the lock) after each salt this program answers. NULL:
 * none. */
typedef void (*MachineSaltObserver)(const uint8_t salt[MACHINE_SALT_LEN]);
void machineIdSetSaltObserver(MachineSaltObserver fn);

/* The folder of the id file, after machineIdInit ("" when none). */
const char *machineIdDir(void);

#ifdef __cplusplus
}
#endif

#endif /* MACHINE_ID_H */
