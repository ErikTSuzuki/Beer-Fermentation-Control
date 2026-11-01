#ifndef LOCAL_WEB_UI_H
#define LOCAL_WEB_UI_H

#include <stdbool.h>
#include "esp_http_server.h"

// Mesmo servidor do iSpindel, protegido pelo open_fn do receptor. O callback
// confirma que a origem ainda e um cliente associado ao AP com IP do DHCP.
esp_err_t local_web_ui_register(httpd_handle_t server, bool (*localClient)(httpd_req_t *));

#endif
