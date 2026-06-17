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
