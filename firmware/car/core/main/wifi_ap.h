#ifndef WIFI_AP_H
#define WIFI_AP_H

#include "esp_err.h"

// Bring up a WiFi softAP. If password is empty (""), the AP is open; otherwise
// WPA2-PSK (password must be >= 8 chars). Initializes netif + default event loop.
// NVS must already be initialized (esp_wifi stores calibration there).
esp_err_t wifi_ap_start(const char *ssid, const char *password);

#endif // WIFI_AP_H
