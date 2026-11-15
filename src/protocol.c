#include "protocol.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

uint8_t ws_cksum(const uint8_t *p, size_t n)
{
    uint8_t s = 0;
    while (n--)
        s += *p++;
    return s;
}

int ws_frame_build(uint8_t *out, const uint8_t *req, size_t n)
{
    if (n == 0 || req[0] != n + 1 || n + 1 > WS_REPORT_SIZE)
        return -1;
    memcpy(out, req, n);
    out[n] = ws_cksum(out, n);
    return (int)(n + 1);
}

int ws_frame_check(const uint8_t *f, size_t n, uint8_t cmd)
{
    if (n < 3 || f[0] < 3 || f[0] > n)
        return -1;
    if (ws_cksum(f, f[0] - 1) != f[f[0] - 1])
        return -2;
    if (f[1] != cmd)
        return -3;
    return 0;
}

static unsigned le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static unsigned long le24(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | ((unsigned long)p[2] << 16);
}

/* Item i of a packed 12-bit array. */
static unsigned pk12(const uint8_t *p, int i)
{
    const uint8_t *q = p + 3 * (i / 2);
    return (i & 1) ? (le16(q + 1) >> 4) : (le16(q) & 0xfff);
}

static double temp_c(unsigned raw)
{
    raw &= 0xfff;
    if (raw >= 0x7fa)
        return NAN;
    double f = ((int)raw - 400) / 10.0;
    return (f - 32.0) / 1.8;
}

static int hum(unsigned raw) { return raw >= 0x7a ? -1 : (int)raw; }

static double scaled12(unsigned raw)
{
    raw &= 0xfff;
    return raw >= 0x7fa ? NAN : raw / 10.0;
}

static double press(unsigned raw) { return raw >= 0x7ffa ? NAN : raw / 10.0; }

int ws_decode_record(const uint8_t *f, size_t n, struct ws_record *r)
{
    if (ws_frame_check(f, n, WS_CMD_RECORD) != 0)
        return -1;
    if (f[0] < 2 + 61 + 1)
        return -1;
    const uint8_t *p = f + 2; /* payload */

    memset(r, 0, sizeof *r);
    memcpy(r->status, p, 5);
    r->in_temp = temp_c(le16(p + 5));
    r->in_hum = hum(p[7]);
    for (int i = 0; i < WS_NCH; i++) {
        r->ch_temp[i] = temp_c(pk12(p + 8, i));
        r->ch_hum[i] = hum(p[20 + i]);
        r->ch_dew[i] = temp_c(pk12(p + 28, i));
        r->ch_feel[i] = temp_c(pk12(p + 40, i));
    }
    r->press_abs = press(le16(p + 52));
    r->press_rel = press(le16(p + 54));
    r->wind = scaled12(pk12(p + 56, 0));
    r->gust = scaled12(pk12(p + 56, 1));
    unsigned dir = le16(p + 59) & 0xfff;
    r->wind_dir = dir >= 0x7fa ? -1 : (int)dir;

    r->has_rain = (p[2] >> 6) & 1;
    r->rain_hour = r->rain_day = r->rain_24h = r->rain_week = r->rain_total = NAN;
    if (r->has_rain && f[0] >= 2 + 73 + 1) {
        r->rain_hour = le16(p + 61) / 10.0;
        r->rain_day = le16(p + 63) / 10.0;
        r->rain_24h = (le24(p + 65) & 0xfffff) / 10.0;
        r->rain_week = (le24(p + 67) >> 4) / 10.0;
        r->rain_total = (le24(p + 70) & 0xfffff) / 10.0;
    }
    return 0;
}

struct jbuf {
    char *b;
    size_t sz, len;
    int first;
};

static void jprintf(struct jbuf *j, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t room = j->len < j->sz ? j->sz - j->len : 0;
    int k = vsnprintf(room ? j->b + j->len : NULL, room, fmt, ap);
    va_end(ap);
    if (k > 0)
        j->len += (size_t)k;
}

static void jnum(struct jbuf *j, const char *key, double v, int prec)
{
    jprintf(j, "%s\"%s\":", j->first ? "" : ",", key);
    j->first = 0;
    if (isnan(v))
        jprintf(j, "null");
    else
        jprintf(j, "%.*f", prec, v);
}

static void jint(struct jbuf *j, const char *key, int v)
{
    jprintf(j, "%s\"%s\":", j->first ? "" : ",", key);
    j->first = 0;
    if (v < 0)
        jprintf(j, "null");
    else
        jprintf(j, "%d", v);
}

int ws_record_json(const struct ws_record *r, char *buf, size_t sz)
{
    struct jbuf j = {buf, sz, 0, 1};
    char k[32];
    jprintf(&j, "{");
    jnum(&j, "indoor_temp", r->in_temp, 1);
    jint(&j, "indoor_humidity", r->in_hum);
    for (int i = 0; i < WS_NCH; i++) {
        if (isnan(r->ch_temp[i]) && r->ch_hum[i] < 0)
            continue; /* channel not connected */
        snprintf(k, sizeof k, "ch%d_temp", i + 1);
        jnum(&j, k, r->ch_temp[i], 1);
        snprintf(k, sizeof k, "ch%d_humidity", i + 1);
        jint(&j, k, r->ch_hum[i]);
        snprintf(k, sizeof k, "ch%d_dewpoint", i + 1);
        jnum(&j, k, r->ch_dew[i], 1);
        snprintf(k, sizeof k, "ch%d_feels_like", i + 1);
        jnum(&j, k, r->ch_feel[i], 1);
    }
    jnum(&j, "pressure_abs", r->press_abs, 1);
    jnum(&j, "pressure_rel", r->press_rel, 1);
    jnum(&j, "wind_speed", r->wind, 1);
    jnum(&j, "wind_gust", r->gust, 1);
    jint(&j, "wind_dir", r->wind_dir);
    if (r->has_rain) {
        jnum(&j, "rain_hour", r->rain_hour, 1);
        jnum(&j, "rain_day", r->rain_day, 1);
        jnum(&j, "rain_24h", r->rain_24h, 1);
        jnum(&j, "rain_week", r->rain_week, 1);
        jnum(&j, "rain_total", r->rain_total, 1);
    }
    jprintf(&j, "}");
    return (int)j.len;
}

int ws_decode_maxmin(const uint8_t *f, size_t n, const struct ws_record *live,
                     struct ws_maxmin *m)
{
    if (ws_frame_check(f, n, WS_CMD_MAXMIN) != 0 || f[0] < 2 + 113 + 1)
        return -1;
    const uint8_t *p = f + 2; /* payload */

    memset(m, 0, sizeof *m);
    m->in_temp[0] = temp_c(pk12(p, 0));
    m->in_temp[1] = temp_c(pk12(p, 1));
    m->in_hum[0] = hum(p[3]);
    m->in_hum[1] = hum(p[4]);
    for (int i = 0; i < WS_NCH; i++) {
        for (int k = 0; k < 2; k++) {
            m->ch_temp[i][k] = temp_c(pk12(p + 5, 2 * i + k));
            m->ch_hum[i][k] = hum(p[29 + 2 * i + k]);
            m->ch_dew[i][k] = temp_c(pk12(p + 45, 2 * i + k));
            m->ch_feel[i][k] = temp_c(pk12(p + 69, 2 * i + k));
        }
    }
    m->press_abs[0] = press(le16(p + 93));
    m->press_abs[1] = press(le16(p + 95));
    m->press_rel[0] = press(le16(p + 97));
    m->press_rel[1] = press(le16(p + 99));
    m->wind_max = scaled12(pk12(p + 101, 0));
    m->gust_max = scaled12(pk12(p + 101, 1));

    m->has_rain = live ? live->has_rain : 1;
    m->rain_hour_max = m->rain_day_max = m->rain_24h_max = m->rain_week_max = NAN;
    if (m->has_rain) {
        m->rain_hour_max = le16(p + 104) / 10.0;
        m->rain_day_max = le16(p + 106) / 10.0;
        m->rain_24h_max = (le24(p + 108) & 0xfffff) / 10.0;
        m->rain_week_max = (le24(p + 110) >> 4) / 10.0;
    }
    return 0;
}

static void jpair_t(struct jbuf *j, const char *base, const double v[2])
{
    char k[40];
    snprintf(k, sizeof k, "%s_max", base);
    jnum(j, k, v[0], 1);
    snprintf(k, sizeof k, "%s_min", base);
    jnum(j, k, v[1], 1);
}

static void jpair_h(struct jbuf *j, const char *base, const int v[2])
{
    char k[40];
    snprintf(k, sizeof k, "%s_max", base);
    jint(j, k, v[0]);
    snprintf(k, sizeof k, "%s_min", base);
    jint(j, k, v[1]);
}

int ws_maxmin_json(const struct ws_maxmin *m, char *buf, size_t sz)
{
    struct jbuf j = {buf, sz, 0, 1};
    char k[32];
    jprintf(&j, "{");
    jpair_t(&j, "indoor_temp", m->in_temp);
    jpair_h(&j, "indoor_humidity", m->in_hum);
    for (int i = 0; i < WS_NCH; i++) {
        if (isnan(m->ch_temp[i][0]) && m->ch_hum[i][0] < 0)
            continue; /* channel never seen */
        snprintf(k, sizeof k, "ch%d_temp", i + 1);
        jpair_t(&j, k, m->ch_temp[i]);
        snprintf(k, sizeof k, "ch%d_humidity", i + 1);
        jpair_h(&j, k, m->ch_hum[i]);
        snprintf(k, sizeof k, "ch%d_dewpoint", i + 1);
        jpair_t(&j, k, m->ch_dew[i]);
        snprintf(k, sizeof k, "ch%d_feels_like", i + 1);
        jpair_t(&j, k, m->ch_feel[i]);
    }
    jpair_t(&j, "pressure_abs", m->press_abs);
    jpair_t(&j, "pressure_rel", m->press_rel);
    jnum(&j, "wind_speed_max", m->wind_max, 1);
    jnum(&j, "wind_gust_max", m->gust_max, 1);
    if (m->has_rain) {
        jnum(&j, "rain_hour_max", m->rain_hour_max, 1);
        jnum(&j, "rain_day_max", m->rain_day_max, 1);
        jnum(&j, "rain_24h_max", m->rain_24h_max, 1);
        jnum(&j, "rain_week_max", m->rain_week_max, 1);
    }
    jprintf(&j, "}");
    return (int)j.len;
}

void ws_synctime_req(uint8_t req[9], int year, int mon, int day, int hour,
                     int min, int sec, int msec)
{
    int frac = (msec + 62) / 125;
    if (frac > 7)
        frac = 7;
    req[0] = 0x0a;
    req[1] = WS_CMD_SYNCTIME;
    req[2] = (uint8_t)frac;
    req[3] = (uint8_t)sec;
    req[4] = (uint8_t)min;
    req[5] = (uint8_t)hour;
    req[6] = (uint8_t)day;
    req[7] = (uint8_t)mon;
    req[8] = (uint8_t)(year - 2000);
}
