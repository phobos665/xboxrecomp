/*
 * xbox_fault_route.h - the runtime's fault route (see xbox_fault_route.c).
 *
 * A title installs it: recomp_fault_install(xbox_fault_route, crash_report).
 */
#ifndef XBOX_FAULT_ROUTE_H
#define XBOX_FAULT_ROUTE_H

#include "platform/recomp_fault.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the fault was a deliberate trap and has been serviced: resume. */
int xbox_fault_route(recomp_fault *f);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_FAULT_ROUTE_H */
