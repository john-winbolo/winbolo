/* Controller assistance follows actual input without changing the saved
 * auto-slowdown choice. Exercise the production input-source tracker with
 * synthetic SDL events and polled Steam Input activity. */
#include "input_source.h"
#include "input_gamepad.h"
#include "test_harness.h"

static bool controllerConnected;
static bool controllerActivity;

bool inputGamepadRealControllerConnected(void) { return controllerConnected; }
bool inputGamepadActivityDetected(void) { return controllerActivity; }

int run_input_source_autoslow(void) {
    SDL_Event ev = {0};
    controllerConnected = true;
    controllerActivity = false;
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

    ev.type = SDL_EVENT_KEY_DOWN;
    inputSourceUpdate(&ev);
    UT_ASSERT_MSG(!inputSourceAutoSlowdown(false), "keyboard did not restore Off");
    UT_ASSERT(inputSourceAutoSlowdown(true));

    /* Steam Input has no SDL gamepad events. */
    controllerActivity = true;
    inputSourceTick();
    UT_ASSERT(inputSourceAutoSlowdown(false));
    controllerActivity = false;
    ev.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    inputSourceUpdate(&ev);
    UT_ASSERT_MSG(!inputSourceAutoSlowdown(false), "mouse did not restore Off");

    ev.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    inputSourceUpdate(&ev);
    UT_ASSERT(inputSourceAutoSlowdown(false));
    controllerConnected = false;
    UT_ASSERT_MSG(!inputSourceAutoSlowdown(false), "unplug did not restore Off");
    UT_ASSERT(inputSourceAutoSlowdown(true));

    controllerConnected = true;
    inputSourceInit();
    UT_ASSERT_MSG(!inputSourceAutoSlowdown(false), "restart enabled slowdown");

    controllerConnected = false;
    inputSourceInit();
    return 0;
}
