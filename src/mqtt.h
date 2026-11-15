/* MQTT publishing with Home Assistant discovery (libmosquitto). */
#ifndef WS_MQTT_H
#define WS_MQTT_H

#include "protocol.h"

struct wsm_cfg {
    char host[128];
    int port;
    char user[64];
    char pass[128];
    char client_id[64];
    char prefix[96];      /* state/availability topic prefix */
    char disc_prefix[64]; /* "homeassistant" */
    char dev_id[48];      /* unique id base */
    char dev_name[64];
    int discovery;
    int maxmin; /* also poll and publish the max/min memory */
    int tls;
    char cafile[192];
};

void wsm_defaults(struct wsm_cfg *c);
int wsm_start(const struct wsm_cfg *c);
/* Publish one reading (discovery is sent automatically when needed). */
int wsm_publish(const struct ws_record *r);
/* Publish the max/min memory to <prefix>/maxmin (retained). */
int wsm_publish_maxmin(const struct ws_maxmin *m);
void wsm_set_online(int online);
void wsm_stop(void);

#endif
