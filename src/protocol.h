/* WeatherHome (DevID 0x0040) station protocol: framing and decoding.
 * Pure C, no I/O. See PROTOCOL.md. */
#ifndef WS_PROTOCOL_H
#define WS_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define WS_VID 0x1130
#define WS_PID 0x0829

#define WS_REPORT_SIZE 64
#define WS_MAX_FRAME 128
#define WS_NCH 8

enum {
    WS_CMD_DEVINFO = 0x01,
    WS_CMD_HISTORY = 0x02,
    WS_CMD_RECORD = 0x04,
    WS_CMD_MAXMIN = 0x05,
    WS_CMD_SYNCTIME = 0x83,
};

/* Decoded live record. Temperatures in degC, pressure hPa, wind m/s,
 * rain mm. Invalid / absent values are NAN (doubles) or -1 (ints). */
struct ws_record {
    uint8_t status[5];
    double in_temp;
    int in_hum;
    double ch_temp[WS_NCH];
    int ch_hum[WS_NCH];
    double ch_dew[WS_NCH];
    double ch_feel[WS_NCH];
    double press_abs, press_rel;
    double wind, gust;
    int wind_dir;
    int has_rain;
    double rain_hour, rain_day, rain_24h, rain_week, rain_total;
};

/* Max/min memory (ReadMaxMin). Index [0] = max, [1] = min. Same units and
 * invalid markers as ws_record. The station stores no timestamps. */
struct ws_maxmin {
    double in_temp[2];
    int in_hum[2];
    double ch_temp[WS_NCH][2];
    int ch_hum[WS_NCH][2];
    double ch_dew[WS_NCH][2];
    double ch_feel[WS_NCH][2];
    double press_abs[2], press_rel[2];
    double wind_max, gust_max;
    int has_rain;
    double rain_hour_max, rain_day_max, rain_24h_max, rain_week_max;
};

/* Checksum over n bytes. */
uint8_t ws_cksum(const uint8_t *p, size_t n);

/* Build a request frame: copies req (len byte first, without checksum) into
 * out and appends the checksum. out must hold req[0] bytes.
 * Returns frame length or -1 if req[0] does not match n+1. */
int ws_frame_build(uint8_t *out, const uint8_t *req, size_t n);

/* Validate a reply frame. Returns 0 if OK, -1 bad length, -2 bad checksum,
 * -3 command mismatch. */
int ws_frame_check(const uint8_t *f, size_t n, uint8_t cmd);

/* Decode a full ReadRecord reply frame (starting with the len byte). */
int ws_decode_record(const uint8_t *f, size_t n, struct ws_record *r);

/* Render as a JSON object. Returns bytes written (like snprintf). */
int ws_record_json(const struct ws_record *r, char *buf, size_t sz);

/* Decode a full ReadMaxMin reply frame. live (may be NULL) supplies the
 * status flags: without it, rain is assumed present. */
int ws_decode_maxmin(const uint8_t *f, size_t n, const struct ws_record *live,
                     struct ws_maxmin *m);

/* JSON with keys like "indoor_temp_max", "ch1_humidity_min", "wind_speed_max". */
int ws_maxmin_json(const struct ws_maxmin *m, char *buf, size_t sz);

/* Build a SyncTime request body (without checksum, 9 bytes). */
void ws_synctime_req(uint8_t req[9], int year, int mon, int day, int hour,
                     int min, int sec, int msec);

#endif
