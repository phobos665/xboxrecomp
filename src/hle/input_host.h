/*
 * input_host.h -- the host controller behind the replaced XAPI input.
 *
 * Adapted from doaxbv-re (https://github.com/NoRain211/doaxbv-re,
 * recomp-runtime/input_host_win32.h), GPL-3.0.
 */
#ifndef XBOXRECOMP_INPUT_HOST_H
#define XBOXRECOMP_INPUT_HOST_H

#include <stdbool.h>
#include "input_model.h"

/* Guest ordering of the analog button bytes. */
enum {
    HOST_ANALOG_A = 0, HOST_ANALOG_B, HOST_ANALOG_X, HOST_ANALOG_Y,
    HOST_ANALOG_BLACK, HOST_ANALOG_WHITE, HOST_ANALOG_LTRIG, HOST_ANALOG_RTRIG,
};

/* One sample of XInput pad 0, the keyboard, and any RECOMP_FAKE_INPUT press,
 * as the Xbox gamepad the title reads. Always succeeds: with nothing
 * connected or pressed it is a neutral pad. */
bool recomp_input_host_sample(RecompInputGamepad *gamepad);

#endif
