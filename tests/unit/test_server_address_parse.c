/*
 * Tests for parseServerAddressPaste() in
 * src/gui/sdl3/dialogs/server_address_parse.h — the helper that splits a value
 * pasted into the manual join dialog's address field into host + port.
 *
 * The dialog routes the parsed pieces into separate input boxes, so the
 * contract checked here is: scheme + trailing path stripped, a valid trailing
 * ":port" split into the port field, and anything ambiguous (no port, an
 * out-of-range or non-numeric port) left verbatim as the host with the port
 * untouched.
 *
 * Also covers the text written back: formatServerAddressHostPort and the
 * Steam "+connect" value from formatSteamConnectString.
 */

#include <string.h>

#include "server_address_parse.h"
#include "test_harness.h"

/* Runs the parser and asserts host/port outputs. expectPort == NULL means the
 * port field must come back empty. Returns 0 on success, 1 on mismatch. */
static int check(const char *in, int expectRet,
                 const char *expectHost, const char *expectPort) {
    char host[256];
    char port[16];
    /* Pre-poison so we catch a parser that forgets to write/clear. */
    strcpy(host, "<UNSET>");
    strcpy(port, "<UNSET>");

    int ret = parseServerAddressPaste(in, host, sizeof(host), port, sizeof(port));
    if (ret != expectRet) {
        UT_FAIL("input \"%s\": ret %d, expected %d", in ? in : "(null)",
                ret, expectRet);
    }
    if (!expectRet) {
        /* On failure host is undefined; only the port-empty guarantee holds. */
        if (port[0] != '\0') {
            UT_FAIL("input \"%s\": failed parse but port = \"%s\"",
                    in ? in : "(null)", port);
        }
        return 0;
    }
    if (strcmp(host, expectHost) != 0) {
        UT_FAIL("input \"%s\": host \"%s\", expected \"%s\"", in, host,
                expectHost);
    }
    const char *wantPort = expectPort ? expectPort : "";
    if (strcmp(port, wantPort) != 0) {
        UT_FAIL("input \"%s\": port \"%s\", expected \"%s\"", in, port,
                wantPort);
    }
    return 0;
}

/* Host-only and ip-only inputs: fill the host, leave the port empty. */
int run_addrparse_host_only(void) {
    if (check("1.2.3.4", 1, "1.2.3.4", NULL)) return 1;
    if (check("myserver.example.com", 1, "myserver.example.com", NULL)) return 1;
    return 0;
}

/* host:port and ip:port split into both fields. */
int run_addrparse_host_port(void) {
    if (check("1.2.3.4:5000", 1, "1.2.3.4", "5000")) return 1;
    if (check("myserver.com:9000", 1, "myserver.com", "9000")) return 1;
    return 0;
}

/* A "scheme://" prefix is stripped, with or without a port. */
int run_addrparse_scheme(void) {
    if (check("winbolo://1.2.3.4:5000", 1, "1.2.3.4", "5000")) return 1;
    if (check("winbolo://host.example.com", 1, "host.example.com", NULL)) return 1;
    if (check("http://1.2.3.4:1234", 1, "1.2.3.4", "1234")) return 1;
    return 0;
}

/* A trailing /path (and any scheme) is dropped, keeping host[:port]. */
int run_addrparse_trailing_path(void) {
    if (check("winbolo://host.example.com:1234/foo/bar", 1,
              "host.example.com", "1234")) return 1;
    if (check("1.2.3.4:5000/join", 1, "1.2.3.4", "5000")) return 1;
    if (check("winbolo://1.2.3.4/", 1, "1.2.3.4", NULL)) return 1;
    return 0;
}

/* Surrounding whitespace is trimmed off the front. */
int run_addrparse_whitespace(void) {
    if (check("   1.2.3.4:5000", 1, "1.2.3.4", "5000")) return 1;
    if (check("\t winbolo://host:42", 1, "host", "42")) return 1;
    return 0;
}

/* Boundary ports: 1 and 65535 split; 0 and 65536 are not a valid port so the
 * whole token stays as the host. */
int run_addrparse_port_bounds(void) {
    if (check("1.2.3.4:1", 1, "1.2.3.4", "1")) return 1;
    if (check("1.2.3.4:65535", 1, "1.2.3.4", "65535")) return 1;
    if (check("1.2.3.4:0", 1, "1.2.3.4:0", NULL)) return 1;
    if (check("1.2.3.4:65536", 1, "1.2.3.4:65536", NULL)) return 1;
    if (check("1.2.3.4:99999", 1, "1.2.3.4:99999", NULL)) return 1;
    return 0;
}

/* A non-numeric or empty ":port" is left verbatim, port field empty. */
int run_addrparse_bad_port(void) {
    if (check("host:abc", 1, "host:abc", NULL)) return 1;
    if (check("host:", 1, "host:", NULL)) return 1;
    if (check("host:80x", 1, "host:80x", NULL)) return 1;
    return 0;
}

/* Empty / NULL / whitespace-only / scheme-only inputs fail with an empty port. */
int run_addrparse_empty(void) {
    if (check("", 0, NULL, NULL)) return 1;
    if (check(NULL, 0, NULL, NULL)) return 1;
    if (check("   ", 0, NULL, NULL)) return 1;
    if (check("winbolo://", 0, NULL, NULL)) return 1;
    if (check("winbolo:///path", 0, NULL, NULL)) return 1;
    return 0;
}

/* "[host]:port" and "[host]" give the host without brackets; an invalid port
 * or other text after the bracket leaves the port empty. */
int run_addrparse_bracketed(void) {
    if (check("[2001:db8::1]:27500", 1, "2001:db8::1", "27500")) return 1;
    if (check("[2001:db8::1]", 1, "2001:db8::1", NULL)) return 1;
    if (check("[::1]:0", 1, "::1", NULL)) return 1;
    if (check("[::1]:abc", 1, "::1", NULL)) return 1;
    if (check("[::1]:", 1, "::1", NULL)) return 1;
    if (check("[::1]:65536", 1, "::1", NULL)) return 1;
    if (check("[1.2.3.4]:5000", 1, "1.2.3.4", "5000")) return 1;
    if (check("[]:5000", 0, NULL, NULL)) return 1;
    return 0;
}

/* A bare IPv6 literal (two or more ':') is kept whole, with no port split. */
int run_addrparse_bare_ipv6(void) {
    if (check("2001:db8::1", 1, "2001:db8::1", NULL)) return 1;
    if (check("::1", 1, "::1", NULL)) return 1;
    if (check("2001:db8::1:27500", 1, "2001:db8::1:27500", NULL)) return 1;
    if (check("::ffff:1.2.3.4", 1, "::ffff:1.2.3.4", NULL)) return 1;
    return 0;
}

/* A %scope suffix stays part of the host, bracketed or not. */
int run_addrparse_ipv6_scope(void) {
    if (check("fe80::1%en0", 1, "fe80::1%en0", NULL)) return 1;
    if (check("[fe80::1%en0]:27500", 1, "fe80::1%en0", "27500")) return 1;
    if (check("[fe80::1%2]", 1, "fe80::1%2", NULL)) return 1;
    return 0;
}

/* The scheme and trailing path are stripped around the IPv6 forms too. */
int run_addrparse_ipv6_scheme(void) {
    if (check("winbolo://[2001:db8::1]:27500", 1, "2001:db8::1", "27500")) return 1;
    if (check("winbolo://[2001:db8::1]:27500/x", 1, "2001:db8::1", "27500")) return 1;
    if (check("winbolo://[2001:db8::1]", 1, "2001:db8::1", NULL)) return 1;
    if (check("winbolo://2001:db8::1", 1, "2001:db8::1", NULL)) return 1;
    if (check("  winbolo://[fe80::1%en0]:42", 1, "fe80::1%en0", "42")) return 1;
    return 0;
}

/* Runs formatServerAddressHostPort into a buffer of bufSz bytes and asserts
 * the text. Returns 0 on success, 1 on mismatch. */
static int checkFormat(const char *host, unsigned port, size_t bufSz,
                       const char *expect) {
    char out[64];
    strcpy(out, "<UNSET>");
    formatServerAddressHostPort(host, port, out, bufSz);
    if (strcmp(out, expect) != 0) {
        UT_FAIL("format(\"%s\", %u, %u): \"%s\", expected \"%s\"",
                host ? host : "(null)", port, (unsigned)bufSz, out, expect);
    }
    return 0;
}

/* The text form brackets a host containing ':' and leaves out port 0. Its
 * output parses back to the same host and port. */
int run_addrparse_format_host_port(void) {
    if (checkFormat("1.2.3.4", 27500, 64, "1.2.3.4:27500")) return 1;
    if (checkFormat("host.example.com", 1, 64, "host.example.com:1")) return 1;
    if (checkFormat("2001:db8::1", 27500, 64, "[2001:db8::1]:27500")) return 1;
    if (checkFormat("fe80::1%en0", 65535, 64, "[fe80::1%en0]:65535")) return 1;
    if (checkFormat("1.2.3.4", 0, 64, "1.2.3.4")) return 1;
    if (checkFormat("::1", 0, 64, "[::1]")) return 1;
    if (checkFormat(NULL, 27500, 64, "")) return 1;
    /* Truncated to the buffer, still terminated. */
    if (checkFormat("1.2.3.4", 27500, 4, "1.2")) return 1;
    if (checkFormat("2001:db8::1", 27500, 1, "")) return 1;

    char text[64];
    formatServerAddressHostPort("2001:db8::1", 27500, text, sizeof(text));
    if (check(text, 1, "2001:db8::1", "27500")) return 1;
    formatServerAddressHostPort("1.2.3.4", 5000, text, sizeof(text));
    if (check(text, 1, "1.2.3.4", "5000")) return 1;

    /* The copied join link is "winbolo://" + this text; it parses back. */
    char link[80];
    formatServerAddressHostPort("2001:db8::1", 27500, text, sizeof(text));
    strcpy(link, "winbolo://");
    strcat(link, text);
    if (check(link, 1, "2001:db8::1", "27500")) return 1;
    return 0;
}

/* Runs formatSteamConnectString and asserts the return value and text.
 * Returns 0 on success, 1 on mismatch. */
static int checkSteam(const char *host, unsigned port, int expectRet,
                      const char *expect) {
    char out[128];
    strcpy(out, "<UNSET>");
    int ret = formatSteamConnectString(host, port, out, sizeof(out));
    if (ret != expectRet) {
        UT_FAIL("steam(\"%s\", %u): ret %d, expected %d",
                host ? host : "(null)", port, ret, expectRet);
    }
    if (strcmp(out, expect) != 0) {
        UT_FAIL("steam(\"%s\", %u): \"%s\", expected \"%s\"",
                host ? host : "(null)", port, out, expect);
    }
    return 0;
}

/* The Steam "+connect" value brackets IPv6, refuses an address a friend
 * cannot reach (link-local, loopback, unspecified, empty), always allows a
 * host name, and what follows "+connect " parses back to the same host and
 * port. */
static int steamConnectBody(void) {
    if (checkSteam("1.2.3.4", 27500, 1, "+connect 1.2.3.4:27500")) return 1;
    if (checkSteam("host.example.com", 5000, 1,
                   "+connect host.example.com:5000")) return 1;
    if (checkSteam("2001:db8::1", 27500, 1,
                   "+connect [2001:db8::1]:27500")) return 1;
    if (checkSteam("fe80::1", 27500, 0, "")) return 1;
    if (checkSteam("169.254.1.2", 27500, 0, "")) return 1;
    if (checkSteam("127.0.0.1", 27500, 0, "")) return 1;
    if (checkSteam("::1", 27500, 0, "")) return 1;
    if (checkSteam("0.0.0.0", 27500, 0, "")) return 1;
    if (checkSteam("::", 27500, 0, "")) return 1;
    if (checkSteam("", 27500, 0, "")) return 1;
    if (checkSteam(NULL, 27500, 0, "")) return 1;

    static const char prefix[] = "+connect ";
    char out[128];
    if (!formatSteamConnectString("2001:db8::1", 27500, out, sizeof(out))) {
        UT_FAIL("steam(\"2001:db8::1\") refused");
    }
    if (check(out + sizeof(prefix) - 1, 1, "2001:db8::1", "27500")) return 1;
    if (!formatSteamConnectString("1.2.3.4", 5000, out, sizeof(out))) {
        UT_FAIL("steam(\"1.2.3.4\") refused");
    }
    if (check(out + sizeof(prefix) - 1, 1, "1.2.3.4", "5000")) return 1;
    return 0;
}

int run_addrparse_steam_connect(void) {
    int rc;
    /* boloAddrParse reads an IPv6 literal through getaddrinfo, which needs
     * Winsock started on Windows. */
    bolo_net_init();
    rc = steamConnectBody();
    bolo_net_cleanup();
    return rc;
}
