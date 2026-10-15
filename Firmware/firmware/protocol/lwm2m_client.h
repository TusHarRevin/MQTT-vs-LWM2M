#ifndef PROTOCOL_LWM2M_CLIENT_H
#define PROTOCOL_LWM2M_CLIENT_H

#include "security/dtls.h"

struct _lwm2m_object_t; /* Wakaama's lwm2m_object_t */

typedef struct {
    const char *endpoint; /* registration endpoint name, usually the device ID */
    const char *host;
    uint16_t port;
    bool use_dtls;
    dtls_config_t dtls;
    /* Must include at least Security (0), Server (1) and Device (3).
     * Wakaama's examples/client/object_*.c are a good starting point. */
    struct _lwm2m_object_t **objects;
    uint16_t object_count;
} lwm2m_client_config_t;

/* Single client instance. All functions return FW_ERR_UNSUPPORTED when built
 * without Wakaama. */
int lwm2m_client_start(const lwm2m_client_config_t *cfg);
/* Runs the Wakaama state machine and handles incoming CoAP; blocks up to timeout_ms. */
int lwm2m_client_poll(uint32_t timeout_ms);
bool lwm2m_client_is_registered(void);
/* Tell the server a resource changed, e.g. "/3303/0/5700". */
int lwm2m_client_notify(const char *uri_path);
void lwm2m_client_stop(void);

#endif /* PROTOCOL_LWM2M_CLIENT_H */
