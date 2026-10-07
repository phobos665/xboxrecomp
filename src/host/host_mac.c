/*
 * host_mac.c -- the few AppKit calls SDL3 does not make for us, through the
 * Objective-C runtime so the library stays plain C.
 *
 * All of these run on the main thread (host_sdl.c calls them from there).
 */
#include "host_mac.h"

#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>

typedef id (*msg_id)(id, SEL);
typedef id (*msg_id_ptr)(id, SEL, const char *);
typedef void (*msg_void_long)(id, SEL, long);
typedef void (*msg_void_id)(id, SEL, id);
typedef BOOL (*msg_bool)(id, SEL);
typedef id (*msg_activity)(id, SEL, unsigned long long, id);

static id app(void)
{
    Class cls = objc_getClass("NSApplication");

    return cls ? ((msg_id)objc_msgSend)((id)cls, sel_registerName("sharedApplication")) : nil;
}

void host_mac_set_accessory(void)
{
    id a = app();

    /* NSApplicationActivationPolicyAccessory (1): may have windows, is not in
     * the Dock or the app switcher, and is activated only if the user clicks
     * it. An unbundled program that SDL has not made "regular" is otherwise
     * left at Prohibited, whose windows the window server may not show. */
    if (a)
        ((msg_void_long)objc_msgSend)(a, sel_registerName("setActivationPolicy:"), 1);
}

void host_mac_keep_awake(void)
{
    static id token;
    Class pi_cls = objc_getClass("NSProcessInfo");
    Class str_cls = objc_getClass("NSString");
    id pi, reason;

    if (token || !pi_cls || !str_cls)
        return;
    pi = ((msg_id)objc_msgSend)((id)pi_cls, sel_registerName("processInfo"));
    reason = ((msg_id_ptr)objc_msgSend)((id)str_cls, sel_registerName("stringWithUTF8String:"),
                                        "a game is running");
    /* NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical:
     * no App Nap, no timer coalescing, for a window that is often covered
     * (a background run sits below everything). A napped process has its
     * timers stretched, which would change a run's pacing. */
    token = ((msg_activity)objc_msgSend)(pi, sel_registerName("beginActivityWithOptions:reason:"),
                                         0x00EFFFFFull | 0xFF00000000ull, reason);
    if (token)
        ((msg_id)objc_msgSend)(token, sel_registerName("retain"));
}

void host_mac_order_back(void *nswindow)
{
    /* Behind every other window at its level, without activating anything:
     * the macOS form of SetWindowPos(HWND_BOTTOM, SWP_NOACTIVATE). */
    if (nswindow)
        ((msg_void_id)objc_msgSend)((id)nswindow, sel_registerName("orderBack:"), nil);
}

int host_mac_app_is_active(void)
{
    id a = app();

    return a ? (int)((msg_bool)objc_msgSend)(a, sel_registerName("isActive")) : 0;
}

#else

void host_mac_set_accessory(void) {}
void host_mac_keep_awake(void) {}
void host_mac_order_back(void *nswindow) { (void)nswindow; }
int  host_mac_app_is_active(void) { return 0; }

#endif
