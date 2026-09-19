/*
 * input_bindings.h -- host devices and per-control bindings, from a config
 * file, for all four Xbox controller ports.
 *
 * One place decides what the four guest pads read: which host device stands
 * in for each port, and which key or pad control drives each of the title's
 * 24 inputs. Both input paths use it -- the XAPI replacement in src/hle and
 * the USB gamepad in src/usb -- so a binding changed once applies to a title
 * whichever way it reads its pads.
 *
 * The file is JSON, written by `py -3 -m tools.input_ui`; see
 * docs/technical/input-binding.md for the format and the search order. With
 * no file present the built-in defaults are what the runtime always did:
 * XInput pad 0 plus the keyboard on port 1, XInput pad N on port N.
 */
#ifndef XBOXRECOMP_INPUT_BINDINGS_H
#define XBOXRECOMP_INPUT_BINDINGS_H

#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load the config, once. Safe to call from anywhere and from any thread that
 * reaches it first; later calls do nothing. Logs one line naming the file it
 * read (or the built-in defaults) and the device on each port. */
void recomp_bindings_init(void);

/* The config file actually read, or NULL when the built-in defaults are in
 * use. Valid after recomp_bindings_init(). */
const char *recomp_bindings_source_path(void);

/* A human name for what drives this port: "XInput pad 0 + keyboard",
 * "keyboard", "XInput pad 2", "nothing". Never NULL. */
const char *recomp_bindings_device_name(unsigned port);

/* Bitmask of ports whose device is present right now, bit 0 = port 1. A
 * keyboard port is always present; an XInput port is present when that pad
 * answers, or when the port also has keyboard bindings. Re-checks XInput at
 * most a few times a second, so a pad plugged in mid-game shows up. */
unsigned recomp_bindings_present_mask(void);

/* Sample one port through its bindings. Returns 0 (and a neutral pad) when
 * the port has no device. */
int recomp_bindings_sample(unsigned port, XBOX_GAMEPAD *out);

#ifdef __cplusplus
}
#endif

#endif
