/* host_mac.h -- AppKit pieces of the host shell (host_mac.c); no-ops off Apple. */
#ifndef XBOXRECOMP_HOST_MAC_H
#define XBOXRECOMP_HOST_MAC_H

void host_mac_set_accessory(void);
void host_mac_keep_awake(void);
void host_mac_order_back(void *nswindow);
int  host_mac_app_is_active(void);

#endif
