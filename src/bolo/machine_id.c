/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* This computer's random id and the salt answers (machine_id.h). */

#if defined(_WIN32)
#  define _CRT_RAND_S   /* rand_s */
#endif

#include "machine_id.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#if defined(_WIN32)
#  include <direct.h>   /* _mkdir */
#else
#  include <sys/stat.h> /* mkdir */
#  include <sys/types.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

/* ── SHA-256 (FIPS 180-4) ─────────────────────────────────────────────── */

typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t  buf[64];
    size_t   have;
} Sha256;

static const uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256Block(Sha256 *c, const uint8_t *p) {
    uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^
                      (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t S0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        t1 = h + S1 + ch + kSha256K[i] + w[i];
        t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256Init(Sha256 *c) {
    static const uint32_t iv[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memcpy(c->h, iv, sizeof(iv));
    c->bytes = 0;
    c->have = 0;
}

static void sha256Update(Sha256 *c, const uint8_t *p, size_t n) {
    c->bytes += n;
    while (n > 0) {
        size_t take = 64 - c->have;
        if (take > n) take = n;
        memcpy(c->buf + c->have, p, take);
        c->have += take;
        p += take;
        n -= take;
        if (c->have == 64) {
            sha256Block(c, c->buf);
            c->have = 0;
        }
    }
}

static void sha256Final(Sha256 *c, uint8_t out[32]) {
    uint64_t bits = c->bytes * 8u;
    uint8_t pad = 0x80, zero = 0, len[8];
    int i;
    sha256Update(c, &pad, 1);
    while (c->have != 56) sha256Update(c, &zero, 1);
    for (i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256Update(c, len, 8);
    for (i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)c->h[i];
    }
}

void machineSha256(const void *data, size_t len,
                   uint8_t out[MACHINE_HASH_LEN]) {
    Sha256 c;
    sha256Init(&c);
    sha256Update(&c, (const uint8_t *)data, len);
    sha256Final(&c, out);
}

void machineIdHashSalt(const uint8_t id[MACHINE_ID_LEN],
                       const uint8_t salt[MACHINE_SALT_LEN],
                       uint8_t out[MACHINE_HASH_LEN]) {
    Sha256 c;
    sha256Init(&c);
    sha256Update(&c, id, MACHINE_ID_LEN);
    sha256Update(&c, salt, MACHINE_SALT_LEN);
    sha256Final(&c, out);
}

/* ── Random bytes ─────────────────────────────────────────────────────── */

bool machineRandomBytes(uint8_t *out, size_t n) {
    if (out == NULL) return false;
#if defined(_WIN32)
    {
        size_t i = 0;
        while (i < n) {
            unsigned int v;
            size_t k;
            if (rand_s(&v) != 0) return false;
            for (k = 0; k < 4 && i < n; k++) out[i++] = (uint8_t)(v >> (8 * k));
        }
        return true;
    }
#elif defined(__APPLE__)
    arc4random_buf(out, n);
    return true;
#else
    {
        int fd = open("/dev/urandom", O_RDONLY);
        size_t off = 0;
        if (fd < 0) return false;
        while (off < n) {
            ssize_t r = read(fd, out + off, n - off);
            if (r <= 0) {
                close(fd);
                return false;
            }
            off += (size_t)r;
        }
        close(fd);
        return true;
    }
#endif
}

/* ── Salt memory ──────────────────────────────────────────────────────── */

bool machineSaltMemorySeen(const MachineSaltMemory *m,
                           const uint8_t salt[MACHINE_SALT_LEN]) {
    int i;
    if (m == NULL || salt == NULL) return false;
    for (i = 0; i < m->count; i++) {
        if (memcmp(m->salt[i], salt, MACHINE_SALT_LEN) == 0) return true;
    }
    return false;
}

void machineSaltMemoryAdd(MachineSaltMemory *m,
                          const uint8_t salt[MACHINE_SALT_LEN]) {
    if (m == NULL || salt == NULL) return;
    if (m->next < 0 || m->next >= MACHINE_SALT_KEEP) m->next = 0;
    memcpy(m->salt[m->next], salt, MACHINE_SALT_LEN);
    m->next = (m->next + 1) % MACHINE_SALT_KEEP;
    if (m->count < MACHINE_SALT_KEEP) m->count++;
}

bool machineSaltAnswer(MachineSaltMemory *m, const uint8_t id[MACHINE_ID_LEN],
                       const uint8_t salt[MACHINE_SALT_LEN],
                       uint8_t out[MACHINE_HASH_LEN]) {
    if (m == NULL || id == NULL || salt == NULL || out == NULL) return false;
    if (machineSaltMemorySeen(m, salt)) return false;
    machineSaltMemoryAdd(m, salt);
    machineIdHashSalt(id, salt, out);
    return true;
}

/* ── The id file ──────────────────────────────────────────────────────── */

/* A salt file longer than this many records is written again with the
 * newest MACHINE_SALT_KEEP. */
#define MACHINE_SALT_FILE_MAX  (MACHINE_SALT_KEEP * 8)

static SDL_SpinLock      s_lock;
static bool              s_inited;
static bool              s_haveId;
static uint8_t           s_id[MACHINE_ID_LEN];
static MachineSaltMemory s_salts;
static char              s_dir[512];
static char              s_saltPath[600];
static long              s_saltRecords;  /* records in the salt file */

static bool machineMakeDir(const char *path) {
    char tmp[512];
    size_t i, n;
    if (path == NULL || path[0] == '\0') return false;
    SDL_strlcpy(tmp, path, sizeof(tmp));
    n = strlen(tmp);
    while (n > 1 && (tmp[n - 1] == '/' || tmp[n - 1] == '\\')) tmp[--n] = '\0';
    for (i = 1; i <= n; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\' || tmp[i] == '\0') {
            char c = tmp[i];
            if (i > 0 && tmp[i - 1] == ':') continue;   /* "C:" */
            tmp[i] = '\0';
#if defined(_WIN32)
            (void)_mkdir(tmp);
#else
            (void)mkdir(tmp, 0755);
#endif
            tmp[i] = c;
        }
    }
    return true;
}

/* True when a file can be made in dir. */
static bool machineDirWritable(const char *dir) {
    char probe[600];
    FILE *f;
    if (!machineMakeDir(dir)) return false;
    SDL_snprintf(probe, sizeof(probe), "%s/machine_id.probe", dir);
    f = fopen(probe, "ab");
    if (f == NULL) return false;
    fclose(f);
    (void)remove(probe);
    return true;
}

static bool machineReadId(const char *path, uint8_t out[MACHINE_ID_LEN]) {
    FILE *f = fopen(path, "rb");
    size_t got;
    if (f == NULL) return false;
    got = fread(out, 1, MACHINE_ID_LEN, f);
    fclose(f);
    return got == MACHINE_ID_LEN;
}

static void machinePickDir(char *out, size_t sz) {
    const char *env = getenv("WINBOLO_MACHINE_ID_DIR");
    out[0] = '\0';
    if (env != NULL && env[0] != '\0') {
        SDL_strlcpy(out, env, sz);
        (void)machineMakeDir(out);
        return;
    }
#if defined(_WIN32)
    {
        const char *pd = getenv("ProgramData");
        SDL_snprintf(out, sz, "%s\\WinBolo",
                     (pd != NULL && pd[0] != '\0') ? pd : "C:\\ProgramData");
        (void)machineMakeDir(out);
    }
#elif defined(__APPLE__)
    SDL_strlcpy(out, "/Users/Shared/WinBolo", sz);
    (void)machineMakeDir(out);
#else
    if (machineDirWritable("/var/lib/winbolo")) {
        SDL_strlcpy(out, "/var/lib/winbolo", sz);
    } else {
        char *pref = SDL_GetPrefPath("WinBolo", "WinBolo");
        if (pref != NULL) {
            size_t n;
            SDL_strlcpy(out, pref, sz);
            SDL_free(pref);
            n = strlen(out);
            if (n > 0 && out[n - 1] == '/') out[n - 1] = '\0';
        }
    }
#endif
}

/* The id from the file, or a new one written to it. Two programs that make
 * one at the same time: the first file made wins ("x" mode), and the
 * other reads it. */
static bool machineLoadId(const char *dir, uint8_t out[MACHINE_ID_LEN]) {
    char path[600];
    uint8_t fresh[MACHINE_ID_LEN];
    int tries;
    SDL_snprintf(path, sizeof(path), "%s/machine_id", dir);
    if (machineReadId(path, out)) return true;
    if (!machineRandomBytes(fresh, sizeof(fresh))) return false;
    {
        FILE *f = fopen(path, "wbx");
        if (f != NULL) {
            bool ok = fwrite(fresh, 1, sizeof(fresh), f) == sizeof(fresh);
            ok = (fclose(f) == 0) && ok;
            if (ok) {
                memcpy(out, fresh, sizeof(fresh));
                return true;
            }
        }
    }
    /* Another program writes it now, or the folder is read-only. */
    for (tries = 0; tries < 20; tries++) {
        if (machineReadId(path, out)) return true;
        SDL_Delay(10);
    }
    /* No file: an id for this program only (it matches no other). */
    fprintf(stderr, "[machine] no id file in %s; this program's id matches "
                    "no other program\n", dir);
    memcpy(out, fresh, sizeof(fresh));
    return true;
}

static void machineLoadSalts(void) {
    FILE *f;
    uint8_t rec[MACHINE_SALT_LEN];
    if (s_saltPath[0] == '\0') return;
    f = fopen(s_saltPath, "rb");
    if (f == NULL) return;
    while (fread(rec, 1, sizeof(rec), f) == sizeof(rec)) {
        machineSaltMemoryAdd(&s_salts, rec);
        s_saltRecords++;
    }
    fclose(f);
}

/* The salt goes at the end of the salt file; a long file is written again
 * with the newest salts. */
static void machineSaveSalt(const uint8_t salt[MACHINE_SALT_LEN]) {
    FILE *f;
    if (s_saltPath[0] == '\0') return;
    if (s_saltRecords >= MACHINE_SALT_FILE_MAX) {
        int i, start = s_salts.count < MACHINE_SALT_KEEP ? 0 : s_salts.next;
        f = fopen(s_saltPath, "wb");
        if (f == NULL) return;
        for (i = 0; i < s_salts.count; i++) {
            (void)fwrite(s_salts.salt[(start + i) % MACHINE_SALT_KEEP], 1,
                         MACHINE_SALT_LEN, f);
        }
        fclose(f);
        s_saltRecords = s_salts.count;
        return;
    }
    f = fopen(s_saltPath, "ab");
    if (f == NULL) return;
    if (fwrite(salt, 1, MACHINE_SALT_LEN, f) == MACHINE_SALT_LEN) {
        s_saltRecords++;
    }
    fclose(f);
}

static void machineInitLocked(void) {
    if (s_inited) return;
    s_inited = true;
    memset(&s_salts, 0, sizeof(s_salts));
    machinePickDir(s_dir, sizeof(s_dir));
    if (s_dir[0] == '\0') {
        s_haveId = machineRandomBytes(s_id, sizeof(s_id));
        return;
    }
    s_haveId = machineLoadId(s_dir, s_id);
    SDL_snprintf(s_saltPath, sizeof(s_saltPath), "%s/machine_salts", s_dir);
    machineLoadSalts();
}

bool machineIdInit(void) {
    bool have;
    SDL_LockSpinlock(&s_lock);
    machineInitLocked();
    have = s_haveId;
    SDL_UnlockSpinlock(&s_lock);
    return have;
}

bool machineIdGet(uint8_t out[MACHINE_ID_LEN]) {
    bool have;
    if (out == NULL) return false;
    SDL_LockSpinlock(&s_lock);
    machineInitLocked();
    have = s_haveId;
    if (have) memcpy(out, s_id, MACHINE_ID_LEN);
    SDL_UnlockSpinlock(&s_lock);
    return have;
}

static MachineSaltObserver s_saltObserver;

bool machineIdAnswerSalt(const uint8_t salt[MACHINE_SALT_LEN],
                         uint8_t out[MACHINE_HASH_LEN]) {
    bool ok;
    MachineSaltObserver obs;
    if (salt == NULL || out == NULL) return false;
    SDL_LockSpinlock(&s_lock);
    machineInitLocked();
    ok = s_haveId && machineSaltAnswer(&s_salts, s_id, salt, out);
    if (ok) machineSaveSalt(salt);
    obs = s_saltObserver;
    SDL_UnlockSpinlock(&s_lock);
    if (ok && obs != NULL) obs(salt);
    return ok;
}

void machineIdUseGiven(const uint8_t id[MACHINE_ID_LEN],
                       const MachineSaltMemory *salts) {
    if (id == NULL) return;
    SDL_LockSpinlock(&s_lock);
    s_inited = true;
    s_haveId = true;
    memcpy(s_id, id, MACHINE_ID_LEN);
    if (salts != NULL) {
        s_salts = *salts;
    } else {
        memset(&s_salts, 0, sizeof(s_salts));
    }
    /* No files: the program that gave the id keeps them. */
    s_dir[0] = '\0';
    s_saltPath[0] = '\0';
    s_saltRecords = 0;
    SDL_UnlockSpinlock(&s_lock);
}

void machineIdSaltSnapshot(MachineSaltMemory *out) {
    if (out == NULL) return;
    SDL_LockSpinlock(&s_lock);
    machineInitLocked();
    *out = s_salts;
    SDL_UnlockSpinlock(&s_lock);
}

void machineIdNoteSalt(const uint8_t salt[MACHINE_SALT_LEN]) {
    if (salt == NULL) return;
    SDL_LockSpinlock(&s_lock);
    machineInitLocked();
    if (!machineSaltMemorySeen(&s_salts, salt)) {
        machineSaltMemoryAdd(&s_salts, salt);
        machineSaveSalt(salt);
    }
    SDL_UnlockSpinlock(&s_lock);
}

void machineIdSetSaltObserver(MachineSaltObserver fn) {
    SDL_LockSpinlock(&s_lock);
    s_saltObserver = fn;
    SDL_UnlockSpinlock(&s_lock);
}

const char *machineIdDir(void) {
    return s_dir;
}
