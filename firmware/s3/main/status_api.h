#ifndef STATUS_API_H
#define STATUS_API_H

#include "esp_err.h"

/* Starts the HTTP server and registers GET /status.
 *
 * NOT bound to the USB interface — httpd_start listens on INADDR_ANY, and IDF's
 * esp_http_server offers no interface-binding option (httpd_config_t has server_port
 * and ctrl_port, nothing address-shaped). This surface is USB-only today only
 * because no other netif exists. Plan 3 adds a station, and from Plan 2 this server
 * carries a car's password: whoever links a radio must reject non-USB peers here
 * first (getsockname on httpd_req_to_sockfd, or httpd_config_t.open_fn). */
esp_err_t status_api_start(void);

#endif /* STATUS_API_H */
