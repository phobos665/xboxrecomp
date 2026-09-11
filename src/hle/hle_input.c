/*
 * hle_input.c -- XAPI controller input replaced by name.
 *
 * Adapted from doaxbv-re (https://github.com/NoRain211/doaxbv-re,
 * recomp-runtime/input_adapter.c), GPL-3.0.
 *
 * The game's own XAPI enumerates pads through the USB stack, which needs an
 * emulated OHCI controller that finds a device, runs its SETUP transfers and
 * reports it. Ours starts and never gets that far: Burnout 2 calls
 * XGetDeviceChanges 13,172 times, is never told a pad arrived, and so never
 * calls XInputOpen or XInputGetState -- "Press START" cannot be pressed.
 *
 * doaxbv-re's answer, reused: replace the XAPI input functions and keep a
 * small model with a pad on port 0 (input_model.c), sampled from the host's
 * XInput pad 0 and keyboard (input_host.c).
 *
 * Differences from doaxbv-re:
 *  - keyed by XDK function name, not one title's addresses;
 *  - the gamepad device type comes from the title's XDK symbols by name
 *    (HLE_IMPORT_VAR), where doaxbv-re hard-codes its title's address. A
 *    title passes this pointer to say a call is about pads; memory units
 *    (g_DeviceType_MU) and the rest get "nothing connected";
 *  - XInputSetState also completes the request's status word, which a title
 *    may poll;
 *  - a lock, since nothing guarantees one guest thread.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "input_model.h"
#include "input_host.h"

HLE_IMPORT_VAR(g_DeviceType_Gamepad);

enum {
    CAPABILITIES_SIZE = 25u,          /* XINPUT_CAPABILITIES */
    STATE_SIZE = 22u,                 /* XINPUT_STATE */
    FEEDBACK_LEFT_MOTOR = 0x42u,      /* after the 66-byte feedback header */
    FEEDBACK_RIGHT_MOTOR = 0x44u,
    ERR_SUCCESS = 0u,
    ERR_INVALID_PARAMETER = 0x57u,
    ERR_DEVICE_NOT_CONNECTED = 0x48Fu,
};

static RecompInputModel g_model;
static CRITICAL_SECTION g_lock;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_lock);
    recomp_input_reset(&g_model, 1u);        /* one pad, on port 0 */
    fprintf(stderr, "[INPUT] XAPI input replaced by name: pad on port 0 "
                    "(XInput pad 0, keyboard%s); gamepad type 0x%08X\n",
            getenv("RECOMP_FAKE_INPUT") ? ", scripted presses" : "",
            hle_var_g_DeviceType_Gamepad);
    if (!hle_var_g_DeviceType_Gamepad)
        fprintf(stderr, "[INPUT] g_DeviceType_Gamepad is not named in this "
                        "title's XDK symbols: no pad will be reported\n");
    fflush(stderr);
    return TRUE;
}

static void lock(void)
{
    InitOnceExecuteOnce(&g_once, init, NULL, NULL);
    EnterCriticalSection(&g_lock);
}

static void unlock(void) { LeaveCriticalSection(&g_lock); }

static int is_gamepad(uint32_t type)
{
    return hle_var_g_DeviceType_Gamepad != 0u &&
           type == hle_var_g_DeviceType_Gamepad;
}

/* DWORD XGetDevices(PXPP_DEVICE_TYPE type) -- bitmask of connected ports. */
HLE_EXPORT(XGetDevices)
{
    uint32_t type = HLE_ARG(0), mask = 0u;

    lock();
    if (is_gamepad(type))
        mask = recomp_input_get_devices(&g_model);
    unlock();
    HLE_RETURN(mask);
}

/* BOOL XGetDeviceChanges(type, DWORD *insertions, DWORD *removals) */
HLE_EXPORT(XGetDeviceChanges)
{
    uint32_t type = HLE_ARG(0), ins_va = HLE_ARG(1), rem_va = HLE_ARG(2);
    uint32_t insertions = 0u, removals = 0u;
    bool changed = false;

    lock();
    if (is_gamepad(type))
        changed = recomp_input_get_device_changes(&g_model, &insertions, &removals);
    unlock();
    if (ins_va) HLE_MEM32(ins_va) = insertions;
    if (rem_va) HLE_MEM32(rem_va) = removals;
    HLE_RETURN(changed ? 1u : 0u);
}

/* HANDLE XInputOpen(type, DWORD port, DWORD slot, PXINPUT_POLLING_PARAMETERS) */
HLE_EXPORT(XInputOpen)
{
    uint32_t type = HLE_ARG(0), port = HLE_ARG(1), slot = HLE_ARG(2), handle = 0u;
    static int said;

    lock();
    if (is_gamepad(type) && slot == 0u)
        handle = recomp_input_open(&g_model, port);
    unlock();
    if (handle && !said) {
        said = 1;
        fprintf(stderr, "[INPUT] XInputOpen port %u -> handle 0x%08X\n", port, handle);
        fflush(stderr);
    }
    HLE_RETURN(handle);
}

/* VOID XInputClose(HANDLE) */
HLE_EXPORT(XInputClose)
{
    lock();
    recomp_input_close(&g_model, HLE_ARG(0));
    unlock();
    HLE_RETURN(0);
}

/* DWORD XInputGetCapabilities(HANDLE, PXINPUT_CAPABILITIES) */
HLE_EXPORT(XInputGetCapabilities)
{
    uint32_t handle = HLE_ARG(0), out = HLE_ARG(1), packet;
    RecompInputGamepad pad;
    uint32_t result = ERR_DEVICE_NOT_CONNECTED;

    if (!out)
        HLE_RETURN(ERR_INVALID_PARAMETER);
    lock();
    if (recomp_input_get_state(&g_model, handle, &packet, &pad)) {
        memset(HLE_PTR(out), 0, CAPABILITIES_SIZE);
        *(uint8_t *)HLE_PTR(out) = 1u;         /* XINPUT_DEVSUBTYPE_GC_GAMEPAD */
        result = ERR_SUCCESS;
    }
    unlock();
    HLE_RETURN(result);
}

/* DWORD XInputGetState(HANDLE, PXINPUT_STATE) */
HLE_EXPORT(XInputGetState)
{
    uint32_t handle = HLE_ARG(0), out = HLE_ARG(1), packet;
    RecompInputGamepad sampled, pad;
    uint32_t result = ERR_DEVICE_NOT_CONNECTED;
    uint8_t *p;
    uint32_t i;

    if (!out)
        HLE_RETURN(ERR_INVALID_PARAMETER);
    recomp_input_host_sample(&sampled);
    lock();
    recomp_input_set_gamepad(&g_model, handle, &sampled);
    if (recomp_input_get_state(&g_model, handle, &packet, &pad)) {
        p = (uint8_t *)HLE_PTR(out);
        memset(p, 0, STATE_SIZE);
        memcpy(p + 0, &packet, 4);                       /* dwPacketNumber */
        memcpy(p + 4, &pad.buttons, 2);                  /* wButtons */
        for (i = 0; i < RECOMP_INPUT_ANALOG_BUTTON_COUNT; i++)
            p[6 + i] = pad.analog_buttons[i];            /* bAnalogButtons */
        memcpy(p + 14, &pad.thumb_lx, 2);
        memcpy(p + 16, &pad.thumb_ly, 2);
        memcpy(p + 18, &pad.thumb_rx, 2);
        memcpy(p + 20, &pad.thumb_ry, 2);
        result = ERR_SUCCESS;
    }
    unlock();
    HLE_RETURN(result);
}

/* DWORD XInputSetState(HANDLE, PXINPUT_FEEDBACK) -- rumble. Recorded, and the
 * request's status (the header's first word) completed at once: on hardware
 * it reads ERROR_IO_PENDING until the motors take the value. */
HLE_EXPORT(XInputSetState)
{
    uint32_t handle = HLE_ARG(0), fb = HLE_ARG(1), result = ERR_INVALID_PARAMETER;

    if (fb) {
        uint16_t left = *(uint16_t *)HLE_PTR(fb + FEEDBACK_LEFT_MOTOR);
        uint16_t right = *(uint16_t *)HLE_PTR(fb + FEEDBACK_RIGHT_MOTOR);
        lock();
        result = recomp_input_set_feedback(&g_model, handle, left, right)
                 ? ERR_SUCCESS : ERR_DEVICE_NOT_CONNECTED;
        unlock();
        HLE_MEM32(fb) = result;
    }
    HLE_RETURN(result);
}
