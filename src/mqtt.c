#include "mqtt.h"

#include <math.h>
#include <mosquitto.h>
#include <stdio.h>
#include <string.h>

static struct mosquitto *mosq;
static const struct wsm_cfg *cfg;
static volatile int need_discovery, need_discovery_mm;
static unsigned announced_ch; /* bitmask of channels with discovery sent */
static int announced_base, announced_rain;
static unsigned announced_mm_ch;
static int announced_mm_base, announced_mm_rain;
static char t_state[160], t_avail[160], t_maxmin[160];

void wsm_defaults(struct wsm_cfg *c)
{
    memset(c, 0, sizeof *c);
    snprintf(c->host, sizeof c->host, "localhost");
    c->port = 1883;
    snprintf(c->client_id, sizeof c->client_id, "wsmqtt");
    snprintf(c->prefix, sizeof c->prefix, "weatherhome");
    snprintf(c->disc_prefix, sizeof c->disc_prefix, "homeassistant");
    snprintf(c->dev_id, sizeof c->dev_id, "weatherhome_0040");
    snprintf(c->dev_name, sizeof c->dev_name, "Weather Station");
    c->discovery = 1;
    c->maxmin = 1;
}

static void on_connect(struct mosquitto *m, void *ud, int rc)
{
    (void)ud;
    if (rc == 0) {
        fprintf(stderr, "mqtt: connected\n");
        need_discovery = 1;
        need_discovery_mm = 1;
        mosquitto_publish(m, NULL, t_avail, 6, "online", 1, true);
    } else {
        fprintf(stderr, "mqtt: connect failed: %s\n", mosquitto_connack_string(rc));
    }
}

static void on_disconnect(struct mosquitto *m, void *ud, int rc)
{
    (void)m;
    (void)ud;
    if (rc != 0)
        fprintf(stderr, "mqtt: disconnected (%s), reconnecting\n", mosquitto_strerror(rc));
}

int wsm_start(const struct wsm_cfg *c)
{
    cfg = c;
    snprintf(t_state, sizeof t_state, "%s/state", c->prefix);
    snprintf(t_avail, sizeof t_avail, "%s/status", c->prefix);
    snprintf(t_maxmin, sizeof t_maxmin, "%s/maxmin", c->prefix);

    mosquitto_lib_init();
    mosq = mosquitto_new(c->client_id, true, NULL);
    if (!mosq)
        return -1;
    if (c->user[0])
        mosquitto_username_pw_set(mosq, c->user, c->pass[0] ? c->pass : NULL);
    if (c->tls) {
        int rc = mosquitto_tls_set(mosq, c->cafile[0] ? c->cafile : NULL,
                                   c->cafile[0] ? NULL : "/etc/ssl/certs", NULL, NULL, NULL);
        if (rc != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "mqtt: tls: %s\n", mosquitto_strerror(rc));
            return -1;
        }
    }
    mosquitto_will_set(mosq, t_avail, 7, "offline", 1, true);
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);
    mosquitto_reconnect_delay_set(mosq, 2, 60, true);

    int rc = mosquitto_connect_async(mosq, c->host, c->port, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mqtt: connect %s:%d: %s\n", c->host, c->port, mosquitto_strerror(rc));
        /* the loop thread keeps retrying */
    }
    rc = mosquitto_loop_start(mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "mqtt: loop: %s\n", mosquitto_strerror(rc));
        return -1;
    }
    return 0;
}

static void pub(const char *topic, const char *payload, int retain)
{
    mosquitto_publish(mosq, NULL, topic, (int)strlen(payload), payload, 1, retain);
}

/* Send one discovery config. dclass/sclass/unit/icon may be NULL. */
static void disc(const char *st, const char *key, const char *name, const char *unit,
                 const char *dclass, const char *sclass, const char *icon)
{
    char topic[256], p[1400];
    int n;
    snprintf(topic, sizeof topic, "%s/sensor/%s/%s/config", cfg->disc_prefix,
             cfg->dev_id, key);
    n = snprintf(p, sizeof p,
                 "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"object_id\":\"%s_%s\","
                 "\"state_topic\":\"%s\",\"value_template\":\"{{ value_json.%s }}\","
                 "\"availability_topic\":\"%s\"",
                 name, cfg->dev_id, key, cfg->dev_id, key, st, key, t_avail);
    if (unit)
        n += snprintf(p + n, sizeof p - n, ",\"unit_of_measurement\":\"%s\"", unit);
    if (dclass)
        n += snprintf(p + n, sizeof p - n, ",\"device_class\":\"%s\"", dclass);
    if (sclass)
        n += snprintf(p + n, sizeof p - n, ",\"state_class\":\"%s\"", sclass);
    if (icon)
        n += snprintf(p + n, sizeof p - n, ",\"icon\":\"%s\"", icon);
    snprintf(p + n, sizeof p - n,
             ",\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\","
             "\"manufacturer\":\"WeatherHome\",\"model\":\"TMU313X (ID0040)\"}}",
             cfg->dev_id, cfg->dev_name);
    pub(topic, p, 1);
}

static void send_discovery(const struct ws_record *r, int force)
{
    const char *M = "measurement";
    if (force) {
        announced_base = announced_rain = 0;
        announced_ch = 0;
    }
    if (!announced_base) {
        disc(t_state, "indoor_temp", "Indoor temperature", "°C", "temperature", M, NULL);
        disc(t_state, "indoor_humidity", "Indoor humidity", "%", "humidity", M, NULL);
        disc(t_state, "pressure_abs", "Absolute pressure", "hPa", "atmospheric_pressure", M, NULL);
        disc(t_state, "pressure_rel", "Relative pressure", "hPa", "atmospheric_pressure", M, NULL);
        disc(t_state, "wind_speed", "Wind speed", "m/s", "wind_speed", M, NULL);
        disc(t_state, "wind_gust", "Wind gust", "m/s", "wind_speed", M, NULL);
        disc(t_state, "wind_dir", "Wind direction", "°", NULL, M, "mdi:compass-outline");
        announced_base = 1;
    }
    for (int i = 0; i < WS_NCH; i++) {
        if (announced_ch & (1u << i))
            continue;
        if (isnan(r->ch_temp[i]) && r->ch_hum[i] < 0)
            continue;
        char k[32], nm[64];
        snprintf(k, sizeof k, "ch%d_temp", i + 1);
        snprintf(nm, sizeof nm, "Channel %d temperature", i + 1);
        disc(t_state, k, nm, "°C", "temperature", M, NULL);
        snprintf(k, sizeof k, "ch%d_humidity", i + 1);
        snprintf(nm, sizeof nm, "Channel %d humidity", i + 1);
        disc(t_state, k, nm, "%", "humidity", M, NULL);
        snprintf(k, sizeof k, "ch%d_dewpoint", i + 1);
        snprintf(nm, sizeof nm, "Channel %d dew point", i + 1);
        disc(t_state, k, nm, "°C", "temperature", M, NULL);
        snprintf(k, sizeof k, "ch%d_feels_like", i + 1);
        snprintf(nm, sizeof nm, "Channel %d feels like", i + 1);
        disc(t_state, k, nm, "°C", "temperature", M, NULL);
        announced_ch |= 1u << i;
    }
    if (!announced_rain && r->has_rain) {
        disc(t_state, "rain_hour", "Rain last hour", "mm", "precipitation", M, NULL);
        disc(t_state, "rain_day", "Rain today", "mm", "precipitation", "total_increasing", NULL);
        disc(t_state, "rain_24h", "Rain last 24h", "mm", "precipitation", M, NULL);
        disc(t_state, "rain_week", "Rain this week", "mm", "precipitation", "total_increasing", NULL);
        disc(t_state, "rain_total", "Rain total", "mm", "precipitation", "total_increasing", NULL);
        announced_rain = 1;
    }
}

int wsm_publish(const struct ws_record *r)
{
    char js[2048];
    if (cfg->discovery) {
        int force = need_discovery;
        need_discovery = 0;
        send_discovery(r, force);
    }
    ws_record_json(r, js, sizeof js);
    pub(t_state, js, 0);
    return 0;
}

/* Discovery for one max/min pair: "<key>_max" and "<key>_min". */
static void disc_pair(const char *key, const char *name, const char *unit,
                      const char *dclass)
{
    char k[48], nm[96];
    snprintf(k, sizeof k, "%s_max", key);
    snprintf(nm, sizeof nm, "%s max", name);
    disc(t_maxmin, k, nm, unit, dclass, NULL, "mdi:arrow-collapse-up");
    snprintf(k, sizeof k, "%s_min", key);
    snprintf(nm, sizeof nm, "%s min", name);
    disc(t_maxmin, k, nm, unit, dclass, NULL, "mdi:arrow-collapse-down");
}

static void send_discovery_mm(const struct ws_maxmin *m, int force)
{
    const char *U = "mdi:arrow-collapse-up";
    if (force) {
        announced_mm_base = announced_mm_rain = 0;
        announced_mm_ch = 0;
    }
    if (!announced_mm_base) {
        disc_pair("indoor_temp", "Indoor temperature", "°C", "temperature");
        disc_pair("indoor_humidity", "Indoor humidity", "%", "humidity");
        disc_pair("pressure_abs", "Absolute pressure", "hPa", "atmospheric_pressure");
        disc_pair("pressure_rel", "Relative pressure", "hPa", "atmospheric_pressure");
        disc(t_maxmin, "wind_speed_max", "Wind speed max", "m/s", "wind_speed", NULL, U);
        disc(t_maxmin, "wind_gust_max", "Wind gust max", "m/s", "wind_speed", NULL, U);
        announced_mm_base = 1;
    }
    for (int i = 0; i < WS_NCH; i++) {
        if (announced_mm_ch & (1u << i))
            continue;
        if (isnan(m->ch_temp[i][0]) && m->ch_hum[i][0] < 0)
            continue;
        char k[32], nm[64];
        snprintf(k, sizeof k, "ch%d_temp", i + 1);
        snprintf(nm, sizeof nm, "Channel %d temperature", i + 1);
        disc_pair(k, nm, "°C", "temperature");
        snprintf(k, sizeof k, "ch%d_humidity", i + 1);
        snprintf(nm, sizeof nm, "Channel %d humidity", i + 1);
        disc_pair(k, nm, "%", "humidity");
        snprintf(k, sizeof k, "ch%d_dewpoint", i + 1);
        snprintf(nm, sizeof nm, "Channel %d dew point", i + 1);
        disc_pair(k, nm, "°C", "temperature");
        snprintf(k, sizeof k, "ch%d_feels_like", i + 1);
        snprintf(nm, sizeof nm, "Channel %d feels like", i + 1);
        disc_pair(k, nm, "°C", "temperature");
        announced_mm_ch |= 1u << i;
    }
    if (!announced_mm_rain && m->has_rain) {
        disc(t_maxmin, "rain_hour_max", "Rain hour max", "mm", "precipitation", NULL, U);
        disc(t_maxmin, "rain_day_max", "Rain day max", "mm", "precipitation", NULL, U);
        disc(t_maxmin, "rain_24h_max", "Rain 24h max", "mm", "precipitation", NULL, U);
        disc(t_maxmin, "rain_week_max", "Rain week max", "mm", "precipitation", NULL, U);
        announced_mm_rain = 1;
    }
}

int wsm_publish_maxmin(const struct ws_maxmin *m)
{
    char js[4096];
    if (cfg->discovery) {
        int force = need_discovery_mm;
        need_discovery_mm = 0;
        send_discovery_mm(m, force);
    }
    ws_maxmin_json(m, js, sizeof js);
    pub(t_maxmin, js, 1); /* retained: changes rarely */
    return 0;
}

void wsm_set_online(int online)
{
    pub(t_avail, online ? "online" : "offline", 1);
}

void wsm_stop(void)
{
    if (!mosq)
        return;
    wsm_set_online(0);
    mosquitto_disconnect(mosq);
    mosquitto_loop_stop(mosq, false);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    mosq = NULL;
}
