#ifndef CFG_API_H
#define CFG_API_H

#include "esp_err.h"

// Register GET and POST for every config domain in the generated table
// (contract/car-api.json -> cfg_table.inc). Call after http_server_start().
esp_err_t cfg_api_start(void);

#endif // CFG_API_H
