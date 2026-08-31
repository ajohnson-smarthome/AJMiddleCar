#ifndef RELAY_TCP_H
#define RELAY_TCP_H

#include "esp_err.h"

/* The car's whole REST surface (config, calibration, status, firmware upload), carried
 * between the phone and the car a byte at a time. See relay_udp.h and
 * docs/superpowers/specs/2026-08-30-dongle-api-design.md, "The relay" — the NAPT path was
 * checked against lwIP's source and does not work for this topology, so the dongle listens
 * on its own address and, for every accepted connection, opens its own connection toward the
 * car and pumps bytes between the two. It never parses a request, a header, a
 * Content-Length or a firmware image; it only moves them — growing a parser here would be
 * the wrong direction for a byte pump to go.
 *
 * The destination is never a constant: it is the gateway address the station's join
 * produced (wifi_sta_gateway()), read fresh every pass, because the dongle knows no car.
 *
 * Needs no guard of its own, but not because of its bind address. The listener is pinned to
 * the USB interface with SO_BINDTODEVICE (usb_net_bind_socket), and THAT is what keeps it
 * from ever being reachable from the car's side of the dongle; an accepted connection
 * inherits the pin from the listener. Binding DONGLE_HOST would not: lwIP is a weak-host
 * stack — ip4_input walks NETIF_FOREACH and accepts a packet on any netif whose address
 * matches the destination — so before the pin, a SYN from a station on the car's network
 * would be delivered to this listener exactly as one from the USB wire is. It would not have
 * gotten further than that, though: usb_net.c's ip.gw comment and api_guard.h both document
 * that this interface has no gateway, so the SYN-ACK to an off-link peer fails at
 * etharp_output with ERR_RTE (etharp.c:851) before the handshake can complete — accept()
 * never returns for that peer, so no slot is ever taken. The delivery-layer premise holds; a
 * completed connection taking every slot in the pool never followed from it. relay_udp.h's
 * equivalent claim about its four-slot session table IS accurate, because a UDP session is
 * created on the first datagram received, with no handshake to fail first. Unlike the
 * dongle's own HTTP server (status_api.c), which esp_http_server gives no bind-address or
 * bind-interface control over at all and which api_guard.c therefore has to screen connection
 * by connection. */

/* Starts the relay task. Safe to call right after wifi_sta_start(): the task waits on its
 * own for wifi_sta_gateway() to succeed before opening any socket, so this does not need to
 * wait for a join to finish and never blocks its caller. */
esp_err_t relay_tcp_start(void);

#endif /* RELAY_TCP_H */
