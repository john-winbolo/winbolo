/*
 * WS↔UDP relay 0x01 metadata-frame parser.
 *
 * The frame arrives from outside the trust boundary (the relay) before any
 * game traffic, so the parser must tolerate every truncation and hostile
 * length field without over-reading. These tests drive
 * transportUdpParseProxyMeta directly (it compiles on every platform; only
 * the consume site in transport_udp_client.c is emscripten-gated):
 *
 *   roundtrip        — a well-formed frame decodes every field.
 *   truncation_safe  — every prefix of a full frame parses without
 *                      over-read; fields past the cut stay at defaults.
 *   oversized_name   — a name longer than the storage truncates into
 *                      out->name but is skipped at its wire length, so the
 *                      wbn/country/prefs after it decode correctly
 *                      (regression: the parse used to clamp the length
 *                      before advancing, desyncing everything after a
 *                      >63-byte name).
 *   prefs_clamps     — a prefsLen field larger than the payload clamps to
 *                      the bytes present; larger than the storage truncates
 *                      to capacity; result stays NUL-terminated.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "internal/transport_udp_internal.h"
#include "test_harness.h"

/* Build a well-formed frame: type + nameLen + name + wbn + country +
 * prefsLen(BE) + prefs. Returns the total length. */
static int buildFrame(uint8_t *buf, const char *name, int nameLen,
                      uint8_t wbnFlag, const char *country,
                      const uint8_t *prefs, int prefsLen,
                      int prefsLenField) {
    int pos = 0;
    buf[pos++] = PROXY_META_FRAME_TYPE;
    buf[pos++] = (uint8_t)nameLen;
    memcpy(buf + pos, name, (size_t)nameLen);
    pos += nameLen;
    buf[pos++] = wbnFlag;
    buf[pos++] = (uint8_t)country[0];
    buf[pos++] = (uint8_t)country[1];
    buf[pos++] = (uint8_t)((prefsLenField >> 8) & 0xFF);
    buf[pos++] = (uint8_t)(prefsLenField & 0xFF);
    if (prefs != NULL && prefsLen > 0) {
        memcpy(buf + pos, prefs, (size_t)prefsLen);
        pos += prefsLen;
    }
    return pos;
}

int run_proxy_meta_parse_roundtrip(void) {
    uint8_t frame[256];
    const char *json = "{\"gunsight\":true}";
    int jsonLen = (int)strlen(json);
    int len = buildFrame(frame, "WebPlayer", 9, 1, "GB",
                         (const uint8_t *)json, jsonLen, jsonLen);

    ProxyMetaFrame m;
    memset(&m, 0xAA, sizeof(m));  /* prove every field is written */
    transportUdpParseProxyMeta(frame, len, &m);

    UT_ASSERT(strcmp(m.name, "WebPlayer") == 0);
    UT_ASSERT(m.wbn == true);
    UT_ASSERT(strcmp(m.country, "GB") == 0);
    UT_ASSERT(m.prefsLen == jsonLen);
    UT_ASSERT(strcmp(m.prefs, json) == 0);
    return 0;
}

int run_proxy_meta_parse_truncation_safe(void) {
    uint8_t frame[256];
    const char *json = "{\"autoscroll\":false}";
    int jsonLen = (int)strlen(json);
    int full = buildFrame(frame, "Trunc", 5, 1, "SE",
                          (const uint8_t *)json, jsonLen, jsonLen);

    int cut;
    for (cut = 0; cut <= full; cut++) {
        ProxyMetaFrame m;
        memset(&m, 0xAA, sizeof(m));
        transportUdpParseProxyMeta(frame, cut, &m);

        /* Whatever was reachable must be internally consistent; anything
         * past the cut must be at its zeroed default. The strings must be
         * NUL-terminated at every cut. */
        UT_ASSERT(memchr(m.name, '\0', sizeof(m.name)) != NULL);
        UT_ASSERT(m.country[2] == '\0');
        UT_ASSERT(m.prefsLen >= 0 && m.prefsLen < (int)sizeof(m.prefs));
        UT_ASSERT(m.prefs[m.prefsLen] == '\0');
        if (cut < full) {
            /* The prefs body is the last field, so any cut short of the
             * full frame must deliver fewer prefs bytes than the intact
             * frame carries. */
            UT_ASSERT_MSG(m.prefsLen < jsonLen,
                          "cut=%d delivered prefsLen=%d (full body is %d)",
                          cut, m.prefsLen, jsonLen);
        }
    }
    return 0;
}

int run_proxy_meta_parse_oversized_name(void) {
    /* 200-byte name — longer than ProxyMetaFrame.name (PACKET_MAX_PLAYER_NAME).
     * The copy must truncate but the walk must skip all 200 bytes so the
     * fields after it decode at their true offsets. */
    char longName[200];
    memset(longName, 'N', sizeof(longName));
    const char *json = "{\"k\":1}";
    int jsonLen = (int)strlen(json);

    uint8_t frame[512];
    int len = buildFrame(frame, longName, (int)sizeof(longName), 1, "JP",
                         (const uint8_t *)json, jsonLen, jsonLen);

    ProxyMetaFrame m;
    transportUdpParseProxyMeta(frame, len, &m);

    UT_ASSERT((int)strlen(m.name) == (int)sizeof(m.name) - 1);
    UT_ASSERT(m.name[0] == 'N');
    UT_ASSERT_MSG(m.wbn == true, "wbn flag misparsed after oversized name");
    UT_ASSERT_MSG(strcmp(m.country, "JP") == 0,
                  "country misparsed after oversized name: '%s'", m.country);
    UT_ASSERT_MSG(m.prefsLen == jsonLen && strcmp(m.prefs, json) == 0,
                  "prefs misparsed after oversized name");
    return 0;
}

int run_proxy_meta_parse_prefs_clamps(void) {
    uint8_t frame[2048];

    /* prefsLen field claims more bytes than the datagram carries: clamp to
     * what is actually present. */
    {
        const char *json = "{\"partial\":true}";
        int jsonLen = (int)strlen(json);
        int len = buildFrame(frame, "Clamp", 5, 0, "DE",
                             (const uint8_t *)json, jsonLen,
                             /*prefsLenField=*/60000);
        ProxyMetaFrame m;
        transportUdpParseProxyMeta(frame, len, &m);
        UT_ASSERT(m.wbn == false);
        UT_ASSERT(m.prefsLen == jsonLen);
        UT_ASSERT(strcmp(m.prefs, json) == 0);
    }

    /* Body bigger than the storage: truncate to capacity, NUL-terminated. */
    {
        uint8_t big[1500];
        memset(big, 'J', sizeof(big));
        int len = buildFrame(frame, "Big", 3, 1, "FR",
                             big, (int)sizeof(big), (int)sizeof(big));
        ProxyMetaFrame m;
        transportUdpParseProxyMeta(frame, len, &m);
        UT_ASSERT(m.prefsLen == (int)sizeof(m.prefs) - 1);
        UT_ASSERT(m.prefs[m.prefsLen] == '\0');
        UT_ASSERT(m.prefs[0] == 'J');
    }
    return 0;
}
