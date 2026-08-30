#ifndef USB_NET_H
#define USB_NET_H

#include "esp_err.h"
#include "esp_netif.h"

#include "dongle_contract.inc"

/* Brings up the USB NCM class and attaches an lwIP interface to it.
 *
 * The dongle is an endpoint on this wire, not a transparent bridge: it holds
 * USB_NET_ADDR and runs a DHCP server, so a host that plugs in is configured with
 * no help from anything else. That property is what lets the app reach the dongle
 * before the dongle has joined any network — see the design note. */
esp_err_t usb_net_start(void);

/* The interface, for later plans that need to bridge or route through it. */
esp_netif_t *usb_net_netif(void);

#define USB_NET_ADDR DONGLE_HOST
#define USB_NET_MASK "255.255.255.0"

#endif /* USB_NET_H */
