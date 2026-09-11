#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "esp_err.h"
#include "esp_http_server.h"

// Start the embedded HTTP server. Serves the embedded index.html at GET "/".
esp_err_t http_server_start(void);

// Returns the running server handle, or NULL before http_server_start() succeeds.
// Phase 3 uses this to register the WebSocket route on the same server.
httpd_handle_t http_server_get_handle(void);

#endif // HTTP_SERVER_H
