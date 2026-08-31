#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "lwip/sockets.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"

#include "usb_net.h"

static const char *TAG = "usb_net";

static esp_netif_t *s_netif;

/* lwIP has a frame for the host. This runs on lwIP's linkoutput path, synchronously
 * inside whatever context handed lwIP the packet, and tinyusb_net_send_sync blocks
 * that caller until TinyUSB's task drains the frame or this 100 ms timeout fires — a
 * host that stops reading stalls the whole IP stack for up to 100 ms per packet.
 * That is the accepted cost of a synchronous send, not an oversight. */
static esp_err_t usb_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    return tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(100));
}

/* lwIP is done with a frame we handed it in on_usb_frame. That frame is our copy. */
static void usb_free_rx(void *h, void *buffer)
{
    (void)h;
    free(buffer);
}

/* The host has a frame for us.
 *
 * TinyUSB's buffer is only valid for the duration of this callback, so the frame is
 * copied before it goes to lwIP, which keeps it until usb_free_rx. Copying every
 * frame is the honest version; if it ever shows up in a profile, the fix is a pool,
 * not a borrowed pointer. */
static esp_err_t on_usb_frame(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;
    if (s_netif == NULL || len == 0) {
        return ESP_OK;
    }
    void *copy = malloc(len);
    if (copy == NULL) {
        /* This return value is not a signal — tinyusb_net.h documents it as ignored,
         * and tud_network_recv_cb (tinyusb_net.c) confirms it: it always calls
         * tud_network_recv_renew() and returns true regardless of what we return here.
         * Returning early only decides whether *we* forward the frame; there is no
         * backpressure to add through this path, so don't try to route one through. */
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, buffer, len);
    esp_netif_receive(s_netif, copy, len, NULL);
    return ESP_OK;
}

/* Step-1 header check (esp_netif_types.h / esp_netif.h, IDF 6.0.2): post_attach reads
 * like it belongs inside esp_netif_driver_ifconfig_t, next to the transmit and
 * free-rx callbacks it works alongside. It does not: that struct has no such
 * member — it only carries handle/transmit/transmit_wrap/driver_free_rx_buffer/
 * driver_set_mac_filter, so a driver built that way would not compile.
 * esp_netif_attach() instead casts whatever handle it's given straight to
 * esp_netif_driver_base_t*, and that base struct — post_attach plus a netif
 * back-pointer — is documented (esp_netif_driver.rst,
 * "ESP-NETIF Custom I/O Driver") to be the *first member* of the driver's own struct.
 * esp_eth_netif_glue_t follows exactly this shape, so usb_net_driver_t does too: it is
 * what goes to esp_netif_attach(), while esp_netif_driver_ifconfig_t is instead built
 * inside post_attach and handed to esp_netif_set_driver_config(), same as the eth glue
 * does. */
typedef struct {
    esp_netif_driver_base_t base;
} usb_net_driver_t;

static usb_net_driver_t s_driver;

static esp_err_t usb_post_attach(esp_netif_t *netif, void *args)
{
    usb_net_driver_t *driver = (usb_net_driver_t *)args;
    driver->base.netif = netif;

    /* .handle is retained even though usb_transmit/usb_free_rx both ignore the pointer
     * they're handed — this class has no per-connection state to look up by it. It
     * still has to be the real driver, not a placeholder: esp_netif_set_driver_config
     * below overwrites esp_netif->driver_handle with whatever is here, clobbering the
     * value esp_netif_attach() already set correctly. A dummy would make
     * esp_netif_get_io_driver() hand back garbage to the next caller. */
    esp_netif_driver_ifconfig_t ifcfg = {
        .handle = driver,
        .transmit = usb_transmit,
        .driver_free_rx_buffer = usb_free_rx,
    };
    ESP_RETURN_ON_ERROR(esp_netif_set_driver_config(netif, &ifcfg), TAG,
                        "cannot set the driver config");
    /* Nothing else brings this interface up — there is no link-detect on a USB
     * class device, so it is up from the moment it is attached. */
    esp_netif_action_start(netif, NULL, 0, NULL);

    /* esp_netif_action_start() returns void: esp_netif_start()'s result — including
     * ESP_ERR_ESP_NETIF_DHCPS_START_FAILED — is silently discarded on the way back to
     * us, and esp_netif_start() is not public API we can call directly instead. Check
     * the public status here so a DHCP server that failed to start doesn't look
     * identical to one that succeeded: without this we'd log "usb net up" and the
     * host would sit on a 169.254.x.x self-assigned address with nothing in this log
     * to explain why. */
    esp_netif_dhcp_status_t dhcp_status;
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_get_status(netif, &dhcp_status), TAG,
                        "cannot read the DHCP server status");
    ESP_RETURN_ON_FALSE(dhcp_status == ESP_NETIF_DHCP_STARTED, ESP_FAIL, TAG,
                        "DHCP server did not start");
    return ESP_OK;
}

esp_netif_t *usb_net_netif(void)
{
    return s_netif;
}

/* See usb_net.h for why an interface pin is a different thing from a bind, and why the
 * relays depend on this one rather than on their bind address. The name is the lwIP netif
 * name esp_netif_get_netif_impl_name reports, which its own documentation names as the value
 * to hand setsockopt for exactly this purpose; struct ifreq's ifr_name is NETIF_NAMESIZE (6)
 * bytes, the size that call is documented to write into. */
esp_err_t usb_net_bind_socket(int fd)
{
    if (s_netif == NULL) {
        ESP_LOGE(TAG, "cannot pin fd %d: the USB interface is not up yet", fd);
        return ESP_ERR_INVALID_STATE;
    }
    struct ifreq iface = { 0 };
    ESP_RETURN_ON_ERROR(esp_netif_get_netif_impl_name(s_netif, iface.ifr_name), TAG,
                        "cannot read the USB interface name");
    /* esp_netif_get_netif_impl_name's IDF 6.0.2 implementation (esp_netif_lwip.c:2770-2775)
     * calls netif_index_to_name() but discards its NULL-on-failure return and always reports
     * ESP_OK back to us, so a lookup failure does not fail the ESP_RETURN_ON_ERROR above — it
     * just leaves iface.ifr_name untouched, still all-zero from the initializer. That matters
     * here more than a normal ignored error would: lwIP's own SO_BINDTODEVICE handler
     * (sockets.c, the SO_BINDTODEVICE case) treats ifr_name[0] == 0 as "no netif" and calls
     * tcp_bind_netif(pcb, NULL), which CLEARS any pin rather than leaving one in place or
     * failing the call — setsockopt returns 0 either way. An empty name is therefore worse
     * than a failed lookup: it would make this function report ESP_OK while silently handing
     * back an unpinned socket, which is the one outcome a caller relying on fail-closed
     * isolation cannot tell apart from success. Caught explicitly rather than trusted to
     * setsockopt, the same way ping_sock.c:271-274 checks its own name lookup before using it. */
    if (iface.ifr_name[0] == '\0') {
        ESP_LOGE(TAG, "cannot pin fd %d: the USB interface has no name", fd);
        return ESP_FAIL;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &iface, sizeof(iface)) != 0) {
        ESP_LOGE(TAG, "SO_BINDTODEVICE(%s) on fd %d: errno %d", iface.ifr_name, fd, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t usb_net_start(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "cannot init esp_netif");

    esp_netif_ip_info_t ip = { 0 };
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(USB_NET_ADDR, &ip.ip), TAG, "bad address");
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(USB_NET_MASK, &ip.netmask), TAG, "bad netmask");
    /* ip.gw is deliberately left at its zero-init default (0.0.0.0), not USB_NET_ADDR.
     * This interface must never become the host's default route — the whole point of
     * the dongle is that plugging it in costs the phone nothing; it keeps its own
     * Wi-Fi/cellular default route untouched. The DHCP-server option calls below back
     * this up at the wire level, not just in this config struct.
     *
     * LOAD-BEARING BEYOND USABILITY, and this is the record of it. api_guard.c can only
     * check that a connection was ADDRESSED to USB_NET_ADDR, not which wire delivered it
     * (see api_guard.h — esp_http_server gives no way to pin its listener to an interface).
     * What actually stops a station on the car's network from completing a handshake to
     * USB_NET_ADDR:8080 is this zero: the SYN-ACK is routed by source address (IDF's
     * ip4_route_src_hook) onto this netif, and etharp_output then has no gateway through
     * which to reach an off-link destination, so the reply is dropped and the handshake
     * never finishes. Give this interface a gateway one day and that stops being true, and
     * api_guard silently degrades to an address check with nothing behind it. If a gateway
     * is ever wanted here, api_guard needs a real interface filter first. */

    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.if_key = "USBNCM";
    base.if_desc = "usb";
    base.ip_info = &ip;
    /* A DHCP server, not a client: this interface serves the host rather than
     * asking anyone for an address. AUTOUP so it comes up without a link event. */
    base.flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP);
    base.get_ip_event = 0;
    base.lost_ip_event = 0;

    esp_netif_config_t cfg = {
        .base = &base,
        /* Left NULL here, same as ESP_NETIF_DEFAULT_ETH(): the driver isn't ready at
         * esp_netif_new() time. usb_post_attach() wires the real transmit/free-rx
         * pair in via esp_netif_set_driver_config() once esp_netif_attach() runs it —
         * see usb_net_driver_t above for why the *attach* handle is a different,
         * esp_netif_driver_base_t-shaped pointer from this one. */
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };

    /* Local, not s_netif, until attach has actually succeeded. on_usb_frame starts
     * firing the moment tinyusb_net_init() returns, below, and until esp_netif_attach()
     * has run usb_post_attach() -> esp_netif_set_driver_config(), this netif's
     * driver_free_rx_buffer is still NULL. A frame arriving in that window would sail
     * straight past an `s_netif != NULL` guard, reach esp_netif_free_rx_buffer with a
     * NULL driver_free_rx_buffer, and crash on a null function-pointer call. Publishing
     * s_netif only once attach has succeeded closes that window instead of narrowing
     * it, and also means a failure anywhere below never leaves s_netif pointing at a
     * live-but-never-attached netif for usb_net_netif() to hand out. */
    esp_netif_t *netif = esp_netif_new(&cfg);
    ESP_RETURN_ON_FALSE(netif != NULL, ESP_FAIL, TAG, "cannot create the netif");

    /* Refuse to be the host's gateway or DNS server — on top of leaving ip.gw at
     * 0.0.0.0 above. The app only ever needs to reach USB_NET_ADDR itself: an
     * on-link route to this /24, never a default route through it. Without this, a
     * host that plugs in loses its real uplink outright — the DHCP server's router
     * option names the dongle as gateway, the OS ranks the wired link above
     * Wi-Fi, installs a default route through a device with no uplink, and every
     * packet (DNS included) goes into a black hole. That is not a corner case: it
     * is what a bare ESP_NETIF_DHCP_SERVER config does by default, and it inverts
     * the one property this product exists to provide.
     *
     * Must land before the DHCP server starts: esp_netif_dhcps_option(..., SET, ...)
     * refuses once dhcps_status is ESP_NETIF_DHCP_STARTED (esp_netif_lwip.c:2581-2583),
     * and that only happens once esp_netif_attach() below runs usb_post_attach() ->
     * esp_netif_action_start(). esp_netif->dhcps itself already exists here, though —
     * it's allocated inside esp_netif_new() whenever ESP_NETIF_DHCP_SERVER is set
     * (esp_netif_lwip.c:864-873) — so calling it now, ahead of TinyUSB and attach, is
     * valid and is the earliest point it can run.
     *
     * DNS caveat, not fixable from here: IDF's DHCP server
     * (components/lwip/apps/dhcpserver/dhcpserver.c:479-503) always emits a DNS
     * option in every OFFER/ACK, falling back to this interface's own address
     * whenever no real DNS server is configured via esp_netif_set_dns_info() — which
     * this firmware never calls. Disabling ESP_NETIF_DOMAIN_NAME_SERVER below matches
     * dhcps_dns's own factory default (already 0x00 — off) and is set explicitly for
     * the same belt-and-braces reason as ip.gw, and in case that fallback ever
     * changes upstream, but it does not remove the option from the wire in this IDF
     * version. The router option has no equivalent gap: it is independently gated on
     * both the offer bit cleared here and on ip.gw being unset (dhcpserver.c:466-476),
     * so either one alone would already stop it — this is genuine belt and braces,
     * not just documentation. */
    uint8_t offer = 0;      /* dhcps_offer_t: no bits set — offer neither */
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
                                                ESP_NETIF_ROUTER_SOLICITATION_ADDRESS,
                                                &offer, sizeof(offer)), TAG,
                        "cannot disable the router option");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET,
                                                ESP_NETIF_DOMAIN_NAME_SERVER,
                                                &offer, sizeof(offer)), TAG,
                        "cannot disable the DNS option");

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG,
                        "cannot install the TinyUSB driver");

    tinyusb_net_config_t net_cfg = {
        .on_recv_callback = on_usb_frame,
    };
    ESP_RETURN_ON_ERROR(esp_read_mac(net_cfg.mac_addr, ESP_MAC_WIFI_STA), TAG,
                        "cannot read the MAC");
    ESP_RETURN_ON_ERROR(tinyusb_net_init(&net_cfg), TAG, "cannot init the NCM class");

    /* The netif's own MAC must differ from the one the host sees on its side of the
     * wire, or both ends answer to the same address. Set the locally-administered bit
     * on ours to force that. `|=` sets the bit unconditionally rather than toggling
     * it — "set", not "flip" — which only reads as the same operation because an
     * Espressif OUI never starts with that bit already on; the code is written for
     * what it does, not for what happens to coincide with it today.
     *
     * net_cfg.mac_addr — the address the host sees, above — borrows the real, per-device
     * station MAC from the efuse block. This used to say "a later plan that brings up a
     * station must revisit it". That plan is this one, and the answer is that the sharing
     * is harmless: the address collision would matter only if the two interfaces could ever
     * appear on one L2 segment, and they cannot. The USB wire and the car's Wi-Fi are
     * separate segments with nothing bridging them — no NAPT, no forwarding, no promiscuous
     * mode; both relays operate at the socket layer, opening their own connection toward the
     * car rather than passing frames between netifs, which is the whole reason the relay
     * exists (see relay_udp.h / relay_tcp.h and the design note's NAPT section). No ARP
     * request for this MAC ever crosses from one to the other, so nothing on either side
     * sees two devices answering to one address.
     *
     * Two things this does NOT claim, so they are not mistaken for settled. It is a privacy
     * exposure, unchanged by this plan: the station's probe requests and association carry
     * the same efuse MAC that the USB host sees, so the two are trivially correlatable. And
     * it stops being safe the moment anything bridges the two segments — a future plan that
     * adds forwarding must give the USB side a MAC of its own first. */
    uint8_t our_mac[6];
    memcpy(our_mac, net_cfg.mac_addr, sizeof(our_mac));
    our_mac[0] |= 0x02;
    ESP_RETURN_ON_ERROR(esp_netif_set_mac(netif, our_mac), TAG, "cannot set the MAC");

    s_driver.base.post_attach = usb_post_attach;
    ESP_RETURN_ON_ERROR(esp_netif_attach(netif, &s_driver), TAG, "cannot attach");

    /* Only now: attach succeeded, so usb_post_attach already ran, driver_free_rx_buffer
     * is installed, and the DHCP server is confirmed started. on_usb_frame's
     * `s_netif == NULL` guard is safe from this point on. */
    s_netif = netif;

    ESP_LOGI(TAG, "usb net up on %s", USB_NET_ADDR);
    return ESP_OK;
}
