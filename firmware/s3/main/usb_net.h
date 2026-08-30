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

/* Pin an already-created socket to the USB interface, so that only traffic which actually
 * ARRIVED on that wire can ever reach it.
 *
 * This is not what bind() does, and the difference is the whole reason this exists. bind()
 * sets only the pcb's local address; lwIP is a weak-host stack, so ip4_input walks
 * NETIF_FOREACH and accepts a packet on any netif whose address matches the destination. A
 * datagram sent from the car's network to DONGLE_HOST is therefore delivered to a socket
 * bound to DONGLE_HOST exactly as one from the USB wire is. The interface filter is a
 * separate pcb field — netif_idx — and SO_BINDTODEVICE is the only way to set it through the
 * socket API (lwip/api/sockets.c, SO_BINDTODEVICE -> netif_find() ->
 * tcp_bind_netif/udp_bind_netif). Once it is set, udp_input (udp.c:139-140) and tcp_input
 * (tcp_in.c:267-268, 301-302, 331-332) discard anything that arrived on another netif before
 * it can reach the socket at all.
 *
 * Call it on a listening or unconnected socket, before bind() — the same order lwIP's own
 * ping client uses (components/lwip/apps/ping/ping_sock.c). A TCP listener's netif_idx is
 * inherited by every connection it accepts (tcp_in.c:711), so pinning the listener pins the
 * whole conversation.
 *
 * Returns ESP_ERR_INVALID_STATE before usb_net_start() has succeeded (there is no interface
 * to name yet), and ESP_FAIL if the option is refused. Both are fail-closed conditions for a
 * caller that relies on this for isolation: a socket this call did not pin is reachable from
 * every interface the dongle has. */
esp_err_t usb_net_bind_socket(int fd);

#define USB_NET_ADDR DONGLE_HOST
#define USB_NET_MASK "255.255.255.0"

#endif /* USB_NET_H */
