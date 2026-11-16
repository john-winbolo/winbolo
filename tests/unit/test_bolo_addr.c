/*
 * Tests for the BoloAddr helpers and bolo_resolve_addr() in
 * src/bolo/public/bolo_addr.h — the one address type for IPv4 and IPv6.
 *
 * The contract checked here: numeric text parses and prints back in the
 * expected form (IPv6 bracketed with a port, mapped IPv4 as plain IPv4, a
 * link-local address with its %scope), the canonical key makes a mapped IPv4
 * address and the plain one the same host while two link-local addresses on
 * different interfaces stay different, the /64 prefix key groups one IPv6
 * network, malformed text is refused, and the resolver reads literals without
 * a network.
 *
 * getaddrinfo and getnameinfo need Winsock started on Windows, so each case
 * that reaches them brackets itself with bolo_net_init / bolo_net_cleanup.
 */

#include <string.h>
#include <SDL3/SDL.h>

#include "bolo_addr.h"
#include "test_harness.h"

/* Parses text with defPort and asserts boloAddrFormat gives want. */
static int expectFormat(const char *text, uint16_t defPort, const char *want) {
    BoloAddr a;
    char buf[BOLO_ADDR_STR_LEN];
    if (!boloAddrParse(text, defPort, &a)) {
        UT_FAIL("parse \"%s\" failed", text);
    }
    if (!boloAddrFormat(&a, buf, sizeof(buf))) {
        UT_FAIL("format of \"%s\" failed", text);
    }
    if (strcmp(buf, want) != 0) {
        UT_FAIL("\"%s\" formatted as \"%s\", expected \"%s\"", text, buf, want);
    }
    return 0;
}

/* True when both addresses give byte-equal canonical keys. */
static int keysEqual(const BoloAddr *a, const BoloAddr *b) {
    uint8_t ka[BOLO_ADDR_KEY_LEN];
    uint8_t kb[BOLO_ADDR_KEY_LEN];
    boloAddrKey(a, ka);
    boloAddrKey(b, kb);
    return memcmp(ka, kb, BOLO_ADDR_KEY_LEN) == 0;
}

/* True when both addresses give byte-equal /64 prefix keys. */
static int prefixKeysEqual(const char *x, const char *y, int *ok) {
    BoloAddr a;
    BoloAddr b;
    uint8_t ka[BOLO_ADDR_KEY_LEN];
    uint8_t kb[BOLO_ADDR_KEY_LEN];
    *ok = boloAddrParse(x, 0, &a) && boloAddrParse(y, 0, &b);
    if (!*ok) {
        return 0;
    }
    boloAddrPrefixKey(&a, ka);
    boloAddrPrefixKey(&b, kb);
    return memcmp(ka, kb, BOLO_ADDR_KEY_LEN) == 0;
}

/* IPv4 text, with and without a port, prints back as host:port. */
int run_boloaddr_roundtrip_v4(void) {
    int rc = 0;
    bolo_net_init();
    if (expectFormat("1.2.3.4", 27500, "1.2.3.4:27500")) rc = 1;
    else if (expectFormat("1.2.3.4:27500", 1, "1.2.3.4:27500")) rc = 1;
    else if (expectFormat("1.2.3.4:5000", 27500, "1.2.3.4:5000")) rc = 1;
    bolo_net_cleanup();
    return rc;
}

/* IPv6 text prints bracketed; a mapped IPv4 address prints as plain IPv4. */
int run_boloaddr_roundtrip_v6(void) {
    int rc = 0;
    bolo_net_init();
    if (expectFormat("2001:db8::1", 27500, "[2001:db8::1]:27500")) rc = 1;
    else if (expectFormat("[2001:db8::1]", 27500, "[2001:db8::1]:27500")) rc = 1;
    else if (expectFormat("[2001:db8::1]:27500", 1, "[2001:db8::1]:27500")) rc = 1;
    else if (expectFormat("::ffff:1.2.3.4", 27500, "1.2.3.4:27500")) rc = 1;
    else if (expectFormat("[::ffff:1.2.3.4]:5000", 1, "1.2.3.4:5000")) rc = 1;
    bolo_net_cleanup();
    return rc;
}

/* A link-local address keeps its numeric scope id and prints it inside the
 * brackets. Scope 1 may print as an interface name, so only the shape of the
 * text is checked. */
static int linkLocalScopeBody(void) {
    BoloAddr a;
    char buf[BOLO_ADDR_STR_LEN];
    const char *suffix = "]:27500";
    size_t len;
    const struct sockaddr_in6 *s6;

    if (!boloAddrParse("fe80::1%1", 27500, &a)) {
        UT_FAIL("parse \"fe80::1%%1\" failed");
    }
    if (boloAddrFamily(&a) != AF_INET6) {
        UT_FAIL("fe80::1%%1 family %d, expected AF_INET6", boloAddrFamily(&a));
    }
    s6 = (const struct sockaddr_in6 *)&a.ss;
    if (s6->sin6_scope_id != 1) {
        UT_FAIL("fe80::1%%1 scope id %lu, expected 1",
                (unsigned long)s6->sin6_scope_id);
    }
    if (!boloAddrIsLinkLocal(&a)) {
        UT_FAIL("fe80::1%%1 not reported link-local");
    }
    if (!boloAddrFormat(&a, buf, sizeof(buf))) {
        UT_FAIL("format of fe80::1%%1 failed");
    }
    len = strlen(buf);
    if (strncmp(buf, "[fe80::1%", 9) != 0 || len <= strlen(suffix) ||
        strcmp(buf + len - strlen(suffix), suffix) != 0) {
        UT_FAIL("fe80::1%%1 formatted as \"%s\"", buf);
    }

    /* The bracketed form with a port reads the same scope. */
    if (!boloAddrParse("[fe80::1%1]:27500", 1, &a)) {
        UT_FAIL("parse \"[fe80::1%%1]:27500\" failed");
    }
    s6 = (const struct sockaddr_in6 *)&a.ss;
    if (s6->sin6_scope_id != 1 || boloAddrPort(&a) != 27500) {
        UT_FAIL("[fe80::1%%1]:27500 scope %lu port %u",
                (unsigned long)s6->sin6_scope_id, (unsigned)boloAddrPort(&a));
    }
    return 0;
}

int run_boloaddr_link_local_scope(void) {
    int rc;
    bolo_net_init();
    rc = linkLocalScopeBody();
    bolo_net_cleanup();
    return rc;
}

/* 1.2.3.4 and ::ffff:1.2.3.4 are one host: same key, equal with the same
 * port, not equal with different ports, same host either way. */
static int mappedKeyBody(void) {
    BoloAddr v4;
    BoloAddr mapped;
    if (!boloAddrParse("1.2.3.4", 27500, &v4) ||
        !boloAddrParse("::ffff:1.2.3.4", 27500, &mapped)) {
        UT_FAIL("parse failed");
    }
    if (!boloAddrIsV4(&mapped) || boloAddrFamily(&mapped) != AF_INET6) {
        UT_FAIL("::ffff:1.2.3.4 should be AF_INET6 and report IPv4");
    }
    if (!keysEqual(&v4, &mapped)) {
        UT_FAIL("keys differ for 1.2.3.4 and ::ffff:1.2.3.4");
    }
    if (!boloAddrEqual(&v4, &mapped)) {
        UT_FAIL("1.2.3.4 and ::ffff:1.2.3.4 with one port not equal");
    }
    boloAddrSetPort(&mapped, 27501);
    if (boloAddrEqual(&v4, &mapped)) {
        UT_FAIL("equal with different ports");
    }
    if (!boloAddrSameHost(&v4, &mapped)) {
        UT_FAIL("not the same host with different ports");
    }
    return 0;
}

int run_boloaddr_mapped_key_equal(void) {
    int rc;
    bolo_net_init();
    rc = mappedKeyBody();
    bolo_net_cleanup();
    return rc;
}

/* Two link-local addresses on different interfaces differ; a global address
 * ignores a stray scope id. */
static int scopeKeyBody(void) {
    BoloAddr a;
    BoloAddr b;
    BoloAddr g;
    BoloAddr g2;
    if (!boloAddrParse("fe80::1%1", 27500, &a) ||
        !boloAddrParse("fe80::1%2", 27500, &b)) {
        UT_FAIL("link-local parse failed");
    }
    if (boloAddrEqual(&a, &b)) {
        UT_FAIL("fe80::1%%1 and fe80::1%%2 equal");
    }
    if (keysEqual(&a, &b)) {
        UT_FAIL("fe80::1%%1 and fe80::1%%2 share a key");
    }
    if (!boloAddrParse("2001:db8::1", 27500, &g)) {
        UT_FAIL("2001:db8::1 parse failed");
    }
    g2 = g;
    ((struct sockaddr_in6 *)&g2.ss)->sin6_scope_id = 5;
    if (!keysEqual(&g, &g2)) {
        UT_FAIL("scope id changed the key of a global address");
    }
    if (!boloAddrEqual(&g, &g2)) {
        UT_FAIL("scope id made a global address unequal");
    }
    return 0;
}

int run_boloaddr_scope_in_key(void) {
    int rc;
    bolo_net_init();
    rc = scopeKeyBody();
    bolo_net_cleanup();
    return rc;
}

/* One /64 is one prefix key; IPv4 keeps the whole address. */
static int prefixKeyBody(void) {
    int ok = 0;
    if (!prefixKeysEqual("2001:db8:1:2::1", "2001:db8:1:2:aaaa:bbbb:cccc:dddd",
                         &ok) || !ok) {
        UT_FAIL("one /64 gave two prefix keys (parse ok %d)", ok);
    }
    if (prefixKeysEqual("2001:db8:1:2::1", "2001:db8:1:3::1", &ok) || !ok) {
        UT_FAIL("two /64s gave one prefix key (parse ok %d)", ok);
    }
    if (prefixKeysEqual("1.2.3.4", "1.2.3.5", &ok) || !ok) {
        UT_FAIL("1.2.3.4 and 1.2.3.5 gave one prefix key (parse ok %d)", ok);
    }
    return 0;
}

int run_boloaddr_prefix_key(void) {
    int rc;
    bolo_net_init();
    rc = prefixKeyBody();
    bolo_net_cleanup();
    return rc;
}

/* A default port of 0 is kept and printed; SetPort / Port round trip. */
static int portZeroBody(void) {
    BoloAddr a;
    char buf[BOLO_ADDR_STR_LEN];
    if (!boloAddrParse("1.2.3.4", 0, &a)) {
        UT_FAIL("parse with port 0 failed");
    }
    if (boloAddrPort(&a) != 0) {
        UT_FAIL("port %u, expected 0", (unsigned)boloAddrPort(&a));
    }
    if (!boloAddrFormat(&a, buf, sizeof(buf)) || strcmp(buf, "1.2.3.4:0") != 0) {
        UT_FAIL("port 0 formatted as \"%s\"", buf);
    }
    boloAddrSetPort(&a, 65535);
    if (boloAddrPort(&a) != 65535) {
        UT_FAIL("SetPort 65535 read back %u", (unsigned)boloAddrPort(&a));
    }
    boloAddrSetPort(&a, 27500);
    if (boloAddrPort(&a) != 27500) {
        UT_FAIL("SetPort 27500 read back %u", (unsigned)boloAddrPort(&a));
    }
    return 0;
}

int run_boloaddr_port_zero(void) {
    int rc;
    bolo_net_init();
    rc = portZeroBody();
    bolo_net_cleanup();
    return rc;
}

/* Malformed or non-numeric text is refused and leaves the output cleared. */
static int rejectsBody(void) {
    static const char *const bad[] = {
        "2001:db8::1:27500",
        "",
        "example.com",
        "1.2.3.4:99999",
        "1.2.3.4:abc",
        "1.2.3.4:",
        "[2001:db8::1]x",
        "[2001:db8::1",
        "[]",
        "[::1]:",
    };
    size_t i;
    BoloAddr a;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        boloAddrParse("9.9.9.9", 1, &a);
        if (boloAddrParse(bad[i], 27500, &a)) {
            UT_FAIL("\"%s\" was accepted", bad[i]);
        }
        if (boloAddrFamily(&a) != 0 || a.len != 0) {
            UT_FAIL("\"%s\" rejected but output not cleared", bad[i]);
        }
    }
    boloAddrParse("9.9.9.9", 1, &a);
    if (boloAddrParse(NULL, 27500, &a)) {
        UT_FAIL("NULL was accepted");
    }
    if (boloAddrFamily(&a) != 0) {
        UT_FAIL("NULL rejected but output not cleared");
    }
    return 0;
}

int run_boloaddr_parse_rejects(void) {
    int rc;
    bolo_net_init();
    rc = rejectsBody();
    bolo_net_cleanup();
    return rc;
}

/* The text "::ffff:" never appears in either formatter's output. */
static int noMappedTextBody(void) {
    static const char *const mapped[] = {
        "::ffff:1.2.3.4",
        "::ffff:127.0.0.1",
        "[::ffff:10.0.0.1]:5000",
        "::ffff:0.0.0.0",
    };
    size_t i;
    for (i = 0; i < sizeof(mapped) / sizeof(mapped[0]); i++) {
        BoloAddr a;
        char buf[BOLO_ADDR_STR_LEN];
        if (!boloAddrParse(mapped[i], 27500, &a)) {
            UT_FAIL("parse \"%s\" failed", mapped[i]);
        }
        if (!boloAddrFormat(&a, buf, sizeof(buf)) || strstr(buf, "::ffff:")) {
            UT_FAIL("\"%s\" formatted as \"%s\"", mapped[i], buf);
        }
        if (!boloAddrFormatHost(&a, buf, sizeof(buf)) ||
            strstr(buf, "::ffff:")) {
            UT_FAIL("\"%s\" host formatted as \"%s\"", mapped[i], buf);
        }
    }
    return 0;
}

int run_boloaddr_no_mapped_text(void) {
    int rc;
    bolo_net_init();
    rc = noMappedTextBody();
    bolo_net_cleanup();
    return rc;
}

/* Unspecified, loopback and IPv4 classification. */
static int classifyBody(void) {
    BoloAddr a;
    boloAddrClear(&a);
    if (!boloAddrIsUnspecified(&a)) UT_FAIL("empty not unspecified");
    if (!boloAddrParse("0.0.0.0", 1, &a) || !boloAddrIsUnspecified(&a))
        UT_FAIL("0.0.0.0 not unspecified");
    if (!boloAddrParse("::", 1, &a) || !boloAddrIsUnspecified(&a))
        UT_FAIL(":: not unspecified");
    if (!boloAddrParse("1.2.3.4", 1, &a) || boloAddrIsUnspecified(&a))
        UT_FAIL("1.2.3.4 unspecified");

    if (!boloAddrParse("127.0.0.1", 1, &a) || !boloAddrIsLoopback(&a))
        UT_FAIL("127.0.0.1 not loopback");
    if (!boloAddrParse("::1", 1, &a) || !boloAddrIsLoopback(&a))
        UT_FAIL("::1 not loopback");
    if (!boloAddrParse("::ffff:127.0.0.1", 1, &a) || !boloAddrIsLoopback(&a))
        UT_FAIL("::ffff:127.0.0.1 not loopback");
    if (!boloAddrParse("2001:db8::1", 1, &a) || boloAddrIsLoopback(&a))
        UT_FAIL("2001:db8::1 loopback");

    if (!boloAddrParse("1.2.3.4", 1, &a) || !boloAddrIsV4(&a))
        UT_FAIL("1.2.3.4 not IPv4");
    if (!boloAddrParse("::ffff:1.2.3.4", 1, &a) || !boloAddrIsV4(&a))
        UT_FAIL("::ffff:1.2.3.4 not IPv4");
    if (!boloAddrParse("2001:db8::1", 1, &a) || boloAddrIsV4(&a))
        UT_FAIL("2001:db8::1 reported IPv4");

    if (!boloAddrParse("169.254.1.1", 1, &a) || !boloAddrIsLinkLocal(&a))
        UT_FAIL("169.254.1.1 not link-local");
    if (!boloAddrParse("2001:db8::1", 1, &a) || boloAddrIsLinkLocal(&a))
        UT_FAIL("2001:db8::1 link-local");
    return 0;
}

int run_boloaddr_classify(void) {
    int rc;
    bolo_net_init();
    rc = classifyBody();
    bolo_net_cleanup();
    return rc;
}

/* A hand-built sockaddr_in / sockaddr_in6 equals the parsed text form. */
static int fromSockaddrBody(void) {
    struct sockaddr_in sin;
    struct sockaddr_in6 sin6;
    BoloAddr fromSa;
    BoloAddr fromText;

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(27500);
    if (inet_pton(AF_INET, "1.2.3.4", &sin.sin_addr) != 1) {
        UT_FAIL("inet_pton AF_INET failed");
    }
    if (!boloAddrFromSockaddr((const struct sockaddr *)&sin,
                              (socklen_t)sizeof(sin), &fromSa)) {
        UT_FAIL("FromSockaddr sockaddr_in failed");
    }
    if (!boloAddrParse("1.2.3.4:27500", 1, &fromText) ||
        !boloAddrEqual(&fromSa, &fromText)) {
        UT_FAIL("sockaddr_in differs from \"1.2.3.4:27500\"");
    }
    if (boloAddrFromSockaddr((const struct sockaddr *)&sin,
                             (socklen_t)(sizeof(sin) - 1), &fromSa)) {
        UT_FAIL("short sockaddr_in accepted");
    }

    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = htons(27500);
    if (inet_pton(AF_INET6, "2001:db8::1", &sin6.sin6_addr) != 1) {
        UT_FAIL("inet_pton AF_INET6 failed");
    }
    if (!boloAddrFromSockaddr((const struct sockaddr *)&sin6,
                              (socklen_t)sizeof(sin6), &fromSa)) {
        UT_FAIL("FromSockaddr sockaddr_in6 failed");
    }
    if (!boloAddrParse("[2001:db8::1]:27500", 1, &fromText) ||
        !boloAddrEqual(&fromSa, &fromText)) {
        UT_FAIL("sockaddr_in6 differs from \"[2001:db8::1]:27500\"");
    }
    return 0;
}

int run_boloaddr_from_sockaddr(void) {
    int rc;
    bolo_net_init();
    rc = fromSockaddrBody();
    bolo_net_cleanup();
    return rc;
}

/* The resolver reads loopback literals, in brackets too, and a failed call
 * leaves the output as it was. No DNS name is looked up. */
static int resolveBody(void) {
    static const char *const hosts[] = { "127.0.0.1", "::1", "[::1]" };
    size_t i;
    BoloAddr a;
    BoloAddr before;

    for (i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++) {
        boloAddrClear(&a);
        if (bolo_resolve_addr(hosts[i], 27500, AF_UNSPEC, &a) != 0) {
            UT_FAIL("resolve \"%s\" failed", hosts[i]);
        }
        if (!boloAddrIsLoopback(&a)) {
            UT_FAIL("resolve \"%s\" not loopback", hosts[i]);
        }
        if (boloAddrPort(&a) != 27500) {
            UT_FAIL("resolve \"%s\" port %u", hosts[i],
                    (unsigned)boloAddrPort(&a));
        }
    }

    if (!boloAddrParse("9.9.9.9:1", 0, &a)) {
        UT_FAIL("sentinel parse failed");
    }
    before = a;
    if (bolo_resolve_addr("", 27500, AF_UNSPEC, &a) == 0) {
        UT_FAIL("empty host resolved");
    }
    if (!boloAddrEqual(&a, &before)) {
        UT_FAIL("empty host changed the output");
    }
    if (bolo_resolve_addr(NULL, 27500, AF_UNSPEC, &a) == 0) {
        UT_FAIL("NULL host resolved");
    }
    if (!boloAddrEqual(&a, &before)) {
        UT_FAIL("NULL host changed the output");
    }
    return 0;
}

int run_boloaddr_resolve_literals(void) {
    int rc;
    bolo_net_init();
    rc = resolveBody();
    bolo_net_cleanup();
    return rc;
}

/* "localhost" with the game's hint resolves to a loopback address, and to
 * an IPv4 one whenever this machine's localhost has an IPv4 address, so a
 * server that listens on IPv4 only is reached by name. */
static int resolvePreferBody(void) {
    BoloAddr a;
    BoloAddr v4;
    if (bolo_resolve_addr("localhost", 27500, BOLO_RESOLVE_PREFER, &a) != 0) {
        UT_FAIL("resolve \"localhost\" failed");
    }
    if (!boloAddrIsLoopback(&a) || boloAddrPort(&a) != 27500) {
        UT_FAIL("\"localhost\" did not give a loopback address on 27500");
    }
    if (bolo_resolve_addr("localhost", 27500, AF_INET, &v4) == 0 &&
        boloAddrIsV4(&v4)) {
        if (!boloAddrIsV4(&a)) {
            UT_FAIL("\"localhost\" has an IPv4 address but resolved to IPv6");
        }
    } else {
        SDL_Log("  boloaddr_resolve_prefer_v4: localhost has no IPv4 address "
                "here; only the loopback check ran");
    }
    return 0;
}

int run_boloaddr_resolve_prefer_v4(void) {
    int rc;
    bolo_net_init();
    rc = resolvePreferBody();
    bolo_net_cleanup();
    return rc;
}

/* boloAddrToFamily: AF_INET6 maps IPv4 and copies IPv6; AF_INET copies IPv4,
 * unmaps a mapped address and refuses any other IPv6; in and out may be the
 * same object. */
static int toFamilyBody(void) {
    BoloAddr v4, v6, mapped, out;
    char buf[BOLO_ADDR_STR_LEN];
    const uint8_t *b;
    static const uint8_t wantMapped[16] = { 0, 0, 0, 0, 0, 0, 0, 0,
                                            0, 0, 0xff, 0xff, 1, 2, 3, 4 };

    if (!boloAddrParse("1.2.3.4:27500", 0, &v4) ||
        !boloAddrParse("[2001:db8::1]:27501", 0, &v6) ||
        !boloAddrParse("[::ffff:5.6.7.8]:27502", 0, &mapped)) {
        UT_FAIL("parse of the test addresses failed");
    }

    /* IPv4 on an IPv6 socket: ::ffff:1.2.3.4, same port, same host. */
    if (!boloAddrToFamily(&v4, AF_INET6, &out)) {
        UT_FAIL("IPv4 -> AF_INET6 refused");
    }
    b = boloAddrV6Bytes(&out);
    if (boloAddrFamily(&out) != AF_INET6 || b == NULL ||
        memcmp(b, wantMapped, 16) != 0) {
        UT_FAIL("IPv4 -> AF_INET6 is not ::ffff:1.2.3.4");
    }
    if (boloAddrPort(&out) != 27500 || !boloAddrEqual(&out, &v4) ||
        out.len != (socklen_t)sizeof(struct sockaddr_in6)) {
        UT_FAIL("IPv4 -> AF_INET6 lost the port, the host or the length");
    }
    if (!boloAddrFormat(&out, buf, sizeof(buf)) ||
        strcmp(buf, "1.2.3.4:27500") != 0) {
        UT_FAIL("mapped form printed as \"%s\"", buf);
    }

    /* IPv6 on an IPv6 socket: copied. */
    if (!boloAddrToFamily(&v6, AF_INET6, &out) ||
        boloAddrFamily(&out) != AF_INET6 || !boloAddrEqual(&out, &v6)) {
        UT_FAIL("IPv6 -> AF_INET6 was not a copy");
    }

    /* IPv4 on an IPv4 socket: copied. */
    if (!boloAddrToFamily(&v4, AF_INET, &out) ||
        boloAddrFamily(&out) != AF_INET || !boloAddrEqual(&out, &v4)) {
        UT_FAIL("IPv4 -> AF_INET was not a copy");
    }

    /* Mapped on an IPv4 socket: plain AF_INET 5.6.7.8, same port. */
    if (!boloAddrToFamily(&mapped, AF_INET, &out) ||
        boloAddrFamily(&out) != AF_INET ||
        out.len != (socklen_t)sizeof(struct sockaddr_in) ||
        boloAddrPort(&out) != 27502 || !boloAddrEqual(&out, &mapped)) {
        UT_FAIL("mapped -> AF_INET did not give plain 5.6.7.8:27502");
    }

    /* Plain IPv6 on an IPv4 socket: refused, output cleared. */
    out = v4;
    if (boloAddrToFamily(&v6, AF_INET, &out)) {
        UT_FAIL("IPv6 -> AF_INET was accepted");
    }
    if (boloAddrFamily(&out) != 0) {
        UT_FAIL("a refused conversion left an address in the output");
    }

    /* An empty address and an unknown family are refused. */
    boloAddrClear(&out);
    if (boloAddrToFamily(&out, AF_INET6, &out)) {
        UT_FAIL("an empty address converted");
    }
    if (boloAddrToFamily(&v4, AF_UNSPEC, &out)) {
        UT_FAIL("AF_UNSPEC accepted as a socket family");
    }

    /* In place: in and out the same object. */
    out = v4;
    if (!boloAddrToFamily(&out, AF_INET6, &out) ||
        boloAddrFamily(&out) != AF_INET6 || !boloAddrEqual(&out, &v4)) {
        UT_FAIL("in-place IPv4 -> AF_INET6 failed");
    }
    if (!boloAddrToFamily(&out, AF_INET, &out) ||
        boloAddrFamily(&out) != AF_INET || !boloAddrEqual(&out, &v4)) {
        UT_FAIL("in-place mapped -> AF_INET failed");
    }
    return 0;
}

int run_boloaddr_to_family(void) {
    int rc;
    bolo_net_init();
    rc = toFamilyBody();
    bolo_net_cleanup();
    return rc;
}

/* boloAddrAny: the all-zero address of each family with the port, and an
 * empty address for any other family. */
static int anyBody(void) {
    BoloAddr a;
    char buf[BOLO_ADDR_STR_LEN];

    boloAddrAny(AF_INET6, 27500, &a);
    if (boloAddrFamily(&a) != AF_INET6 || boloAddrPort(&a) != 27500 ||
        !boloAddrIsUnspecified(&a) || boloAddrIsV4(&a) ||
        a.len != (socklen_t)sizeof(struct sockaddr_in6)) {
        UT_FAIL("AF_INET6 any is not [::]:27500");
    }
    if (!boloAddrFormat(&a, buf, sizeof(buf)) ||
        strcmp(buf, "[::]:27500") != 0) {
        UT_FAIL("AF_INET6 any printed as \"%s\"", buf);
    }

    boloAddrAny(AF_INET, 0, &a);
    if (boloAddrFamily(&a) != AF_INET || boloAddrPort(&a) != 0 ||
        !boloAddrIsUnspecified(&a) ||
        a.len != (socklen_t)sizeof(struct sockaddr_in)) {
        UT_FAIL("AF_INET any is not 0.0.0.0:0");
    }
    if (!boloAddrFormat(&a, buf, sizeof(buf)) ||
        strcmp(buf, "0.0.0.0:0") != 0) {
        UT_FAIL("AF_INET any printed as \"%s\"", buf);
    }

    boloAddrAny(AF_UNSPEC, 27500, &a);
    if (boloAddrFamily(&a) != 0) {
        UT_FAIL("AF_UNSPEC any is not empty");
    }
    return 0;
}

int run_boloaddr_any(void) {
    int rc;
    bolo_net_init();
    rc = anyBody();
    bolo_net_cleanup();
    return rc;
}

/* boloAddrV4: the 4 bytes of a plain or mapped IPv4 address; 0 and a zeroed
 * output for IPv6 and for an empty address. */
static int v4Body(void) {
    BoloAddr a;
    struct in_addr out;
    static const uint8_t want[4] = { 10, 20, 30, 40 };
    static const uint8_t zero[4] = { 0, 0, 0, 0 };

    if (!boloAddrParse("10.20.30.40:1", 0, &a) || !boloAddrV4(&a, &out) ||
        memcmp(&out, want, 4) != 0) {
        UT_FAIL("plain IPv4 did not give 10.20.30.40");
    }
    if (!boloAddrParse("::ffff:10.20.30.40", 1, &a) || !boloAddrV4(&a, &out) ||
        memcmp(&out, want, 4) != 0) {
        UT_FAIL("mapped IPv4 did not give 10.20.30.40");
    }
    memset(&out, 0xff, sizeof(out));
    if (!boloAddrParse("2001:db8::1", 1, &a) || boloAddrV4(&a, &out) ||
        memcmp(&out, zero, 4) != 0) {
        UT_FAIL("IPv6 gave an IPv4 address or left the output set");
    }
    memset(&out, 0xff, sizeof(out));
    boloAddrClear(&a);
    if (boloAddrV4(&a, &out) || memcmp(&out, zero, 4) != 0) {
        UT_FAIL("an empty address gave an IPv4 address");
    }
    return 0;
}

int run_boloaddr_v4(void) {
    int rc;
    bolo_net_init();
    rc = v4Body();
    bolo_net_cleanup();
    return rc;
}

/* boloAddrFromV4: an AF_INET address with the 4 bytes and the port, equal
 * to the parsed text form. */
static int fromV4Body(void) {
    BoloAddr a, text;
    struct in_addr v4;
    static const uint8_t bytes[4] = { 192, 0, 2, 7 };

    memcpy(&v4, bytes, 4);
    boloAddrFromV4(v4, 40000, &a);
    if (boloAddrFamily(&a) != AF_INET || boloAddrPort(&a) != 40000 ||
        a.len != (socklen_t)sizeof(struct sockaddr_in)) {
        UT_FAIL("FromV4 did not give an AF_INET address on port 40000");
    }
    if (!boloAddrParse("192.0.2.7:40000", 0, &text) ||
        !boloAddrEqual(&a, &text)) {
        UT_FAIL("FromV4 differs from \"192.0.2.7:40000\"");
    }
    return 0;
}

int run_boloaddr_from_v4(void) {
    int rc;
    bolo_net_init();
    rc = fromV4Body();
    bolo_net_cleanup();
    return rc;
}

/* For one family, the routing lookup either finds nothing and leaves an
 * empty address, or finds an address of that family with port 0 that is not
 * loopback, link-local or unspecified. Both outcomes pass, so the case holds
 * on a machine with no network. */
static int localForRouteFamily(int family, const char *name) {
    BoloAddr a;
    memset(&a, 0xab, sizeof(a));
    if (!boloAddrLocalForRoute(family, &a)) {
        if (boloAddrFamily(&a) != 0) {
            UT_FAIL("%s: no route, but the address was not cleared", name);
        }
        SDL_Log("  boloaddr_local_for_route: no usable %s address here", name);
        return 0;
    }
    if (boloAddrFamily(&a) != family) {
        UT_FAIL("%s: found an address of another family", name);
    }
    if (boloAddrPort(&a) != 0) {
        UT_FAIL("%s: found address has port %u, expected 0", name,
                (unsigned)boloAddrPort(&a));
    }
    if (boloAddrIsLoopback(&a) || boloAddrIsLinkLocal(&a) ||
        boloAddrIsUnspecified(&a)) {
        UT_FAIL("%s: found a loopback, link-local or unspecified address",
                name);
    }
    return 0;
}

static int localForRouteBody(void) {
    BoloAddr a;
    if (localForRouteFamily(AF_INET, "IPv4")) return 1;
    if (localForRouteFamily(AF_INET6, "IPv6")) return 1;
    memset(&a, 0xab, sizeof(a));
    if (boloAddrLocalForRoute(AF_UNSPEC, &a) || boloAddrFamily(&a) != 0) {
        UT_FAIL("AF_UNSPEC was not refused with an empty address");
    }
    if (boloAddrLocalForRoute(AF_INET, NULL)) {
        UT_FAIL("a NULL output was not refused");
    }
    return 0;
}

int run_boloaddr_local_for_route(void) {
    int rc;
    bolo_net_init();
    rc = localForRouteBody();
    bolo_net_cleanup();
    return rc;
}
