/*
 * Matching a saved audio device name against the devices present
 * (src/client_frontend/voice_core.c). Device ids are handed out per run and
 * do not survive a restart, so the player's choice is persisted as a display
 * name and resolved at startup. This covers the resolving on its own, with no
 * audio device anywhere near it: where a name that is there lands, and -1 -
 * which the caller reads as "use the system default" - for every way it can
 * be absent.
 */
#include <stddef.h>

#include "voice_core.h"
#include "test_harness.h"

int run_voice_device_resolve(void) {
    static const char *const names[] = {
        "Built-in Microphone",
        "USB Headset",
        "HDMI Audio",
    };
    /* A device the platform would not give a name for sits in the list as a
     * hole, and must be stepped over rather than stopped on. */
    static const char *const withHole[] = {
        "Built-in Microphone",
        NULL,
        "USB Headset",
    };
    const int count = (int)(sizeof(names) / sizeof(names[0]));

    /* Present, in the middle of the list. */
    UT_ASSERT(voiceDeviceResolveName("USB Headset", names, count) == 1);

    /* Index 0 is a match like any other, not the no-match answer. */
    UT_ASSERT(voiceDeviceResolveName("Built-in Microphone", names, count) == 0);

    /* And the last entry is reached. */
    UT_ASSERT(voiceDeviceResolveName("HDMI Audio", names, count) == 2);

    /* Not present - unplugged, or a prefs file from another machine. */
    UT_ASSERT(voiceDeviceResolveName("Studio Interface", names, count) == -1);

    /* Nothing saved, either way of saying it. */
    UT_ASSERT(voiceDeviceResolveName("", names, count) == -1);
    UT_ASSERT(voiceDeviceResolveName(NULL, names, count) == -1);

    /* No devices at all. */
    UT_ASSERT(voiceDeviceResolveName("USB Headset", names, 0) == -1);

    /* The whole name or nothing: a prefix is a different device. */
    UT_ASSERT(voiceDeviceResolveName("USB", names, count) == -1);
    UT_ASSERT(voiceDeviceResolveName("USB Headset 2", names, count) == -1);

    /* A hole is skipped, and the entries past it are still reached. */
    UT_ASSERT(voiceDeviceResolveName("USB Headset", withHole, 3) == 2);
    UT_ASSERT(voiceDeviceResolveName("Studio Interface", withHole, 3) == -1);

    return 0;
}
