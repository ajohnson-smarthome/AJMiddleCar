#ifndef STATUS_API_H
#define STATUS_API_H

#include "esp_err.h"
#include "esp_http_server.h"

/* Starts the HTTP server and registers GET /status.
 *
 * NOT bound to the USB interface — httpd_start listens on INADDR_ANY, and IDF's
 * esp_http_server offers no interface-binding option (httpd_config_t has server_port
 * and ctrl_port, nothing address-shaped). This surface is USB-only today only
 * because no other netif exists. Plan 4 adds a station, and from Plan 2 this server
 * carries a car's password: whoever links a radio must reject non-USB peers here
 * first (getsockname on httpd_req_to_sockfd, or httpd_config_t.open_fn). */
esp_err_t status_api_start(void);

/* The running server, so another module can register its handlers on it rather than
 * start a second one. NULL before status_api_start succeeds. */
httpd_handle_t status_api_server(void);

#endif /* STATUS_API_H */
