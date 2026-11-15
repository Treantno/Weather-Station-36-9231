/* Decoder tests against frames captured from a real station (2026-11-15). */
#include "../src/protocol.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #cond);    \
            fails++;                                                           \
        }                                                                      \
    } while (0)
#define NEAR(a, b) CHECK(fabs((a) - (b)) < 0.051)

static const uint8_t rec1[] = {
    0x4c, 0x04, 0x01, 0x01, 0x7f, 0x00, 0x00, 0x8f, 0x04, 0x2e, 0x46, 0xa3, 0x7f, 0xfa, 0xa7, 0x7f,
    0xfa, 0xa7, 0x7f, 0xfa, 0xa7, 0x7f, 0x5c, 0x7a, 0x7a, 0x7a, 0x7a, 0x7a, 0x7a, 0x7a, 0x31, 0xa3,
    0x7f, 0xfa, 0xa7, 0x7f, 0xfa, 0xa7, 0x7f, 0xfa, 0xa7, 0x7f, 0x47, 0xa3, 0x7f, 0xfa, 0xa7, 0x7f,
    0xfa, 0xa7, 0x7f, 0xfa, 0xa7, 0x7f, 0x2b, 0x26, 0xdd, 0x26, 0x00, 0x00, 0x00, 0x8f, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0x10, 0x00, 0x79};

static double f2c(double f) { return (f - 32) / 1.8; }

static void test_record(void)
{
    struct ws_record r;
    CHECK(sizeof rec1 == 76);
    CHECK(ws_frame_check(rec1, sizeof rec1, WS_CMD_RECORD) == 0);
    CHECK(ws_decode_record(rec1, sizeof rec1, &r) == 0);

    NEAR(r.in_temp, f2c(76.7));
    CHECK(r.in_hum == 46);
    NEAR(r.ch_temp[0], f2c(43.8));
    CHECK(r.ch_hum[0] == 92);
    NEAR(r.ch_dew[0], f2c(41.7));
    NEAR(r.ch_feel[0], f2c(43.9));
    for (int i = 1; i < WS_NCH; i++) {
        CHECK(isnan(r.ch_temp[i]));
        CHECK(r.ch_hum[i] == -1);
        CHECK(isnan(r.ch_dew[i]));
        CHECK(isnan(r.ch_feel[i]));
    }
    NEAR(r.press_abs, 977.1);
    NEAR(r.press_rel, 994.9);
    NEAR(r.wind, 0.0);
    NEAR(r.gust, 0.0);
    CHECK(r.wind_dir == 143);
    CHECK(r.has_rain == 1);
    NEAR(r.rain_hour, 0.0);
    NEAR(r.rain_day, 0.0);
    NEAR(r.rain_24h, 0.0);
    NEAR(r.rain_week, 0.0);
    NEAR(r.rain_total, 435.0);

    char js[2048];
    ws_record_json(&r, js, sizeof js);
    CHECK(strstr(js, "\"indoor_temp\":24.8") != NULL);
    CHECK(strstr(js, "\"ch1_humidity\":92") != NULL);
    CHECK(strstr(js, "\"ch2_temp\"") == NULL);
    CHECK(strstr(js, "\"rain_total\":435.0") != NULL);
    CHECK(strstr(js, "\"wind_dir\":143") != NULL);
    printf("%s\n", js);
}

/* Packed fields with non-zero neighbours, to catch nibble mistakes. */
static void test_packing(void)
{
    uint8_t f[76];
    memcpy(f, rec1, sizeof f);
    uint8_t *p = f + 2;
    /* wind 12.3 m/s (123 = 0x07b), gust 45.6 (456 = 0x1c8) */
    p[56] = 0x7b; p[57] = 0x80; p[58] = 0x1c;
    /* rain 24h 1234.5 (12345 = 0x03039), week 6789.0 (67890 = 0x10932) */
    p[65] = 0x39; p[66] = 0x30; p[67] = 0x20; p[68] = 0x93; p[69] = 0x10;
    /* rain hour 1.5, day 25.5 */
    p[61] = 15; p[62] = 0; p[63] = 0xff; p[64] = 0;
    f[75] = ws_cksum(f, 75);

    struct ws_record r;
    CHECK(ws_decode_record(f, sizeof f, &r) == 0);
    NEAR(r.wind, 12.3);
    NEAR(r.gust, 45.6);
    NEAR(r.rain_24h, 1234.5);
    NEAR(r.rain_week, 6789.0);
    NEAR(r.rain_hour, 1.5);
    NEAR(r.rain_day, 25.5);
    NEAR(r.rain_total, 435.0);
}

static const uint8_t mm1[] = {
    0x74, 0x05, 0x22, 0xe5, 0x3d, 0x48, 0x11, 0xac, 0x55, 0x1a, 0xfa, 0xa7, 0x7f, 0xfa, 0xa7, 0x7f,
    0xfa, 0xa7, 0x7f, 0xfa, 0xa7, 0x7f, 0xfa, 0xa7, 0x7f, 0xfa, 0xa7, 0x7f, 0xfa, 0xa7, 0x7f, 0x63,
    0x10, 0x7a, 0x7a, 0x7a, 0x7a, 0x7a, 0x7a, 0x7a, 0x7a, 0x7a, 0x7a, 0x7a, 0x7a, 0x7a, 0x7a, 0x38,
    0x34, 0x17, 0xfa, 0xa7, 0x7f, 0xfa, 0xa7, 0x7f, 0xfa, 0xa7, 0x7f, 0xfa, 0xa7, 0x7f, 0xfa, 0xa7,
    0x7f, 0xfa, 0xa7, 0x7f, 0xfa, 0xa7, 0x7f, 0xac, 0x55, 0x1a, 0xfa, 0xa7, 0x7f, 0xfa, 0xa7, 0x7f,
    0xfa, 0xa7, 0x7f, 0xfa, 0xa7, 0x7f, 0xfa, 0xa7, 0x7f, 0xfa, 0xa7, 0x7f, 0xfa, 0xa7, 0x7f, 0x63,
    0x28, 0x27, 0x25, 0xc7, 0x28, 0x58, 0x25, 0x84, 0x60, 0x0b, 0x4e, 0x00, 0xf6, 0x00, 0x04, 0x02,
    0x30, 0x3c, 0x00, 0x76};

static void test_maxmin(void)
{
    struct ws_maxmin m;
    CHECK(sizeof mm1 == 116);
    CHECK(ws_decode_maxmin(mm1, sizeof mm1, NULL, &m) == 0);

    NEAR(m.in_temp[0], f2c(91.4));
    NEAR(m.in_temp[1], f2c(59.0));
    CHECK(m.in_hum[0] == 72 && m.in_hum[1] == 17);
    NEAR(m.ch_temp[0][0], f2c(105.2));
    NEAR(m.ch_temp[0][1], f2c(2.1));
    CHECK(m.ch_hum[0][0] == 99 && m.ch_hum[0][1] == 16);
    NEAR(m.ch_dew[0][0], f2c(68.0));
    NEAR(m.ch_dew[0][1], f2c(-2.9));
    NEAR(m.ch_feel[0][0], f2c(105.2));
    NEAR(m.ch_feel[0][1], f2c(2.1));
    for (int i = 1; i < WS_NCH; i++) {
        CHECK(isnan(m.ch_temp[i][0]) && isnan(m.ch_temp[i][1]));
        CHECK(m.ch_hum[i][0] == -1 && m.ch_hum[i][1] == -1);
        CHECK(isnan(m.ch_dew[i][0]) && isnan(m.ch_feel[i][1]));
    }
    NEAR(m.press_abs[0], 1033.9);
    NEAR(m.press_abs[1], 951.1);
    NEAR(m.press_rel[0], 1043.9);
    NEAR(m.press_rel[1], 956.0);
    NEAR(m.wind_max, 13.2);
    NEAR(m.gust_max, 18.2);
    CHECK(m.has_rain);
    NEAR(m.rain_hour_max, 7.8);
    NEAR(m.rain_day_max, 24.6);
    NEAR(m.rain_24h_max, 51.6);
    NEAR(m.rain_week_max, 96.3);

    /* live status without the rain sensor flag hides rain */
    struct ws_record live;
    memset(&live, 0, sizeof live);
    CHECK(ws_decode_maxmin(mm1, sizeof mm1, &live, &m) == 0);
    CHECK(!m.has_rain && isnan(m.rain_week_max));

    char js[4096];
    CHECK(ws_decode_maxmin(mm1, sizeof mm1, NULL, &m) == 0);
    ws_maxmin_json(&m, js, sizeof js);
    CHECK(strstr(js, "\"indoor_temp_max\":33.0") != NULL);
    CHECK(strstr(js, "\"ch1_humidity_min\":16") != NULL);
    CHECK(strstr(js, "\"ch2_temp_max\"") == NULL);
    CHECK(strstr(js, "\"wind_gust_max\":18.2") != NULL);
    CHECK(strstr(js, "\"rain_week_max\":96.3") != NULL);
    printf("%s\n", js);

    /* a live-record frame must be rejected */
    CHECK(ws_decode_maxmin(rec1, sizeof rec1, NULL, &m) != 0);
}

static void test_frames(void)
{
    uint8_t out[16];
    static const uint8_t req[] = {0x03, 0x04};
    CHECK(ws_frame_build(out, req, 2) == 3);
    CHECK(out[2] == 0x07);

    /* device info reply captured from the station */
    static const uint8_t di[] = {0x0b, 0x01, 0x0a, 0x40, 0x00, 0xfb, 0x63, 0x00, 0x40, 0x00, 0xf4};
    CHECK(ws_frame_check(di, sizeof di, 0x01) == 0);
    CHECK(ws_frame_check(di, sizeof di, 0x04) == -3);
    uint8_t bad[sizeof di];
    memcpy(bad, di, sizeof di);
    bad[10] ^= 1;
    CHECK(ws_frame_check(bad, sizeof bad, 0x01) == -2);

    struct ws_record r;
    CHECK(ws_decode_record(di, sizeof di, &r) != 0);

    uint8_t st[9];
    ws_synctime_req(st, 2026, 11, 15, 13, 45, 30, 999);
    static const uint8_t exp[] = {0x0a, 0x83, 7, 30, 45, 13, 15, 11, 26};
    CHECK(memcmp(st, exp, 9) == 0);
}

int main(void)
{
    test_frames();
    test_record();
    test_packing();
    test_maxmin();
    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
