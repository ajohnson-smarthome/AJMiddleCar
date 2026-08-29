#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"

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

/* Step-1 header check (esp_netif_types.h / esp_netif.h, IDF 6.0.2): the brief drafted
 * post_attach as a field of esp_netif_driver_ifconfig_t. That struct has no such
 * member — it only carries handle/transmit/transmit_wrap/driver_free_rx_buffer/
 * driver_set_mac_filter, so that would not compile. esp_netif_attach() instead casts
 * whatever handle it's given straight to esp_netif_driver_base_t*, and that base
 * struct — post_attach plus a netif back-pointer — is documented (esp_netif_driver.rst,
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

    esp_netif_driver_ifconfig_t ifcfg = {
        .handle = (void *)1,        /* no driver object: the class is a singleton */
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

esp_err_t usb_net_start(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "cannot init esp_netif");

    esp_netif_ip_info_t ip = { 0 };
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(USB_NET_ADDR, &ip.ip), TAG, "bad address");
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(USB_NET_ADDR, &ip.gw), TAG, "bad gateway");
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(USB_NET_MASK, &ip.netmask), TAG, "bad netmask");

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
     * wire, or both ends answer to the same address. Flip the locally-administered
     * bit for ours.
     *
     * net_cfg.mac_addr — the address the host sees, above — is a different, still-open
     * concern: it borrows the real, per-device station MAC from the efuse block.
     * Borrowing it costs nothing today because this firmware never brings up a
     * station. A later plan that does must revisit it — a station and this USB
     * interface on the same MAC would be two interfaces claiming one address. */
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
