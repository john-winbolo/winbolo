/*
 * Pins bolo_detect_client_type() and bolo_client_type_name(): the
 * self-reported client platform that rides every JOIN. The baseline
 * harness deliberately normalizes the "clientType" field away (it
 * varies with the build host), so this is the only place that asserts
 * the detection is actually right for the platform under test.
 *
 * Strategy: an independent oracle re-derives the expected CLIENT_TYPE_*
 * from the same compile-time platform macros the production function in
 * util.c uses, and we assert agreement. Because the unit-test binary
 * links steam_wrapper_stub.c, steam_is_steam_deck() is always false
 * here, so the Linux arm resolves to CLIENT_TYPE_LINUX (never
 * CLIENT_TYPE_STEAMDECK) — the oracle matches that.
 */
#include "util.h"          /* bolo_detect_client_type / bolo_client_type_name */
#include "player_flags.h"  /* CLIENT_TYPE_* */
#include "test_harness.h"

#include <string.h>

/* Independent oracle — keep in sync with bolo_detect_client_type() in
 * src/bolo/util.c. iOS desktop unit tests don't build (the whole unit
 * suite is gated to the desktop branch in CMake), but the arm is kept
 * for parity with the production ladder. */
static uint8_t expected_client_type(void) {
#if defined(__EMSCRIPTEN__)
    return CLIENT_TYPE_WEB;
#elif defined(__APPLE__)
    #if TARGET_OS_IOS
        return CLIENT_TYPE_IOS;
    #else
        return CLIENT_TYPE_MACOS;
    #endif
#elif defined(__ANDROID__)
    return CLIENT_TYPE_ANDROID;
#elif defined(_WIN32)
    return CLIENT_TYPE_WINDOWS;
#elif defined(__linux__)
    /* steam_is_steam_deck() is stubbed false in the test binary. */
    return CLIENT_TYPE_LINUX;
#else
    return CLIENT_TYPE_UNKNOWN;
#endif
}

int run_client_type_matches_platform(void) {
    uint8_t detected = bolo_detect_client_type();
    uint8_t expected = expected_client_type();

    UT_ASSERT_MSG(detected == expected,
                  "bolo_detect_client_type()=%u but this platform expects %u",
                  (unsigned)detected, (unsigned)expected);

    /* The value must be a real enumerator, never the COUNT sentinel or
     * anything past it. */
    UT_ASSERT_MSG(detected < CLIENT_TYPE_COUNT,
                  "detected client type %u is out of range (>= COUNT %u)",
                  (unsigned)detected, (unsigned)CLIENT_TYPE_COUNT);

    /* On any platform the unit suite actually builds for, detection must
     * resolve to a concrete platform — UNKNOWN would mean the macro
     * ladder fell through. */
    UT_ASSERT_MSG(detected != CLIENT_TYPE_UNKNOWN,
                  "client type resolved to UNKNOWN on a supported build host");

    return 0;
}

int run_client_type_name_round_trips(void) {
    /* Every concrete enumerator has a non-empty, distinct name, and the
     * detected type names itself. */
    static const struct {
        uint8_t type;
        const char *name;
    } table[] = {
        { CLIENT_TYPE_WINDOWS,   "Windows"   },
        { CLIENT_TYPE_LINUX,     "Linux"     },
        { CLIENT_TYPE_MACOS,     "macOS"     },
        { CLIENT_TYPE_IOS,       "iOS"       },
        { CLIENT_TYPE_ANDROID,   "Android"   },
        { CLIENT_TYPE_STEAMDECK, "Steam Deck"},
        { CLIENT_TYPE_WEB,       "Web"       },
    };
    size_t i;
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        const char *name = bolo_client_type_name(table[i].type);
        UT_ASSERT_MSG(name != NULL && name[0] != '\0',
                      "client type %u has an empty name", (unsigned)table[i].type);
        UT_ASSERT_MSG(strcmp(name, table[i].name) == 0,
                      "client type %u name=\"%s\" expected \"%s\"",
                      (unsigned)table[i].type, name, table[i].name);
    }

    /* The detected platform names itself consistently with the table. */
    uint8_t detected = bolo_detect_client_type();
    const char *detected_name = bolo_client_type_name(detected);
    UT_ASSERT_MSG(detected_name != NULL && detected_name[0] != '\0',
                  "detected client type %u has an empty name",
                  (unsigned)detected);

    return 0;
}
