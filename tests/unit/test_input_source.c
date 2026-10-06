/* Controller assistance follows actual input without changing the saved
 * auto-slowdown choice. Exercise the production input-source tracker with
 * synthetic SDL events and polled Steam Input activity. */
#include "input_source.h"
#include "test_harness.h"

/* Gamepad stubs in test_stubs.c. */
extern bool gamepadStubConnected;
extern bool gamepadStubActivity;

int run_input_source_autoslow(void) {
    SDL_Event ev = {0};
    gamepadStubConnected = true;
    gamepadStubActivity = false;
    inputSourceInit();

    /* UI hints may default to controller, but an untouched attached pad
     * must not override an explicit Off preference, including at restart. */
    UT_ASSERT(inputSourceCurrent() == INPUT_SOURCE_GAMEPAD);
    UT_ASSERT(!inputSourceAutoSlowdown(false));
    UT_ASSERT(inputSourceAutoSlowdown(true));

    ev.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    ev.gaxis.value = 100;
    inputSourceUpdate(&ev);
    UT_ASSERT_MSG(!inputSourceAutoSlowdown(false), "idle stick drift enabled slowdown");

    ev.gaxis.value = 20000;
    inputSourceUpdate(&ev);
    UT_ASSERT(inputSourceAutoSlowdown(false));
    /* Releasing the stick must keep the assist so the tank slows down. */
    ev.gaxis.value = 0;
    inputSourceUpdate(&ev);
    inputSourceTick();
    UT_ASSERT(inputSourceAutoSlowdown(false));

    /* Pointer input never steers the tank: a Deck trackpad, a touchscreen
     * tap or a bumped mouse must not drop the assist mid-drive. */
    ev = (SDL_Event){0};
    ev.type = SDL_EVENT_MOUSE_MOTION;
    ev.motion.xrel = 5.0f;
    inputSourceUpdate(&ev);
    ev.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    inputSourceUpdate(&ev);
    ev.type = SDL_EVENT_MOUSE_WHEEL;
    inputSourceUpdate(&ev);
    inputSourceNoteKeyboard();
    UT_ASSERT(inputSourceCurrent() == INPUT_SOURCE_KEYBOARD);
    UT_ASSERT_MSG(inputSourceAutoSlowdown(false), "pointer input dropped the assist");

    ev.type = SDL_EVENT_KEY_DOWN;
    inputSourceUpdate(&ev);
    UT_ASSERT_MSG(!inputSourceAutoSlowdown(false), "keyboard did not restore Off");
    UT_ASSERT(inputSourceAutoSlowdown(true));

    /* Steam Input has no SDL gamepad events. */
    gamepadStubActivity = true;
    inputSourceTick();
    UT_ASSERT(inputSourceAutoSlowdown(false));
    gamepadStubActivity = false;

    gamepadStubConnected = false;
    UT_ASSERT_MSG(!inputSourceAutoSlowdown(false), "unplug did not restore Off");
    UT_ASSERT(inputSourceAutoSlowdown(true));
    /* Replugging alone must not bring the assist back. */
    inputSourceTick();
    gamepadStubConnected = true;
    UT_ASSERT_MSG(!inputSourceAutoSlowdown(false), "replug enabled slowdown");
    ev.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    inputSourceUpdate(&ev);
    UT_ASSERT(inputSourceAutoSlowdown(false));

    inputSourceInit();
    UT_ASSERT_MSG(!inputSourceAutoSlowdown(false), "restart enabled slowdown");

    gamepadStubConnected = false;
    inputSourceInit();
    return 0;
}
