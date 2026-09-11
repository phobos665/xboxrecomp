/*
 * hle_d3d8.c -- Direct3D 8 functions replaced by name.
 *
 * The route the DOAXBV port proved: replace D3D8 at its API and translate to
 * the host's graphics API, rather than emulate the NV2A beneath it. Burnout 2
 * supports doing that wholesale -- no game function reads the D3D device
 * global itself (110 references inside the D3D library, none outside), and
 * only one game function pushes raw GPU methods through BeginPush.
 *
 * This file starts with the display-filter setters because their correct
 * behaviour on a PC is to do nothing: they tune the TV encoder's flicker
 * filter, which a monitor does not have. That makes them the safe first proof
 * of the whole path -- signature lookup, generated thunk, dispatch, stack
 * clean-up -- before any replacement whose behaviour could be wrong.
 *
 * Each logs its first call, so a run shows the replacement was reached.
 */
#include <stdio.h>
#include "hle.h"

static void first_call(int *seen, const char *name, uint32_t arg)
{
    if (!*seen) {
        *seen = 1;
        fprintf(stderr, "[HLE] %s(0x%X) replaced by name\n", name, arg);
        fflush(stderr);
    }
}

/* void D3DDevice_SetFlickerFilter(DWORD Filter) -- TV encoder only. */
HLE_EXPORT(D3DDevice_SetFlickerFilter)
{
    static int seen;
    first_call(&seen, "D3DDevice_SetFlickerFilter", HLE_ARG(0));
    HLE_RETURN(0);
}

/* void D3DDevice_SetSoftDisplayFilter(BOOL Enable) -- TV encoder only. */
HLE_EXPORT(D3DDevice_SetSoftDisplayFilter)
{
    static int seen;
    first_call(&seen, "D3DDevice_SetSoftDisplayFilter", HLE_ARG(0));
    HLE_RETURN(0);
}
