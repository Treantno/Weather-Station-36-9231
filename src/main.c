/* wsmqtt - read a WeatherHome (TMU313X, DevID 0x0040) USB weather station
 * and optionally publish readings to MQTT with Home Assistant discovery. */
#define _POSIX_C_SOURCE 200809L
#include "mqtt.h"
#include "protocol.h"
#include "station.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

static void usage(FILE *f)
{
    fprintf(f,
"usage: wsmqtt [options] <command> [args]\n"
"\n"
"commands:\n"
"  read            print the current reading (add --json for JSON)\n"
"  maxmin          print the station's max/min memory (add --json for JSON)\n"
"  info            print the raw device info reply\n"
"  dump            print raw hex of the live record and max/min frames\n"
"  raw HEX...      send a raw request (len cmd ...; checksum is added)\n"
"  synctime        set the station clock to the local time\n"
"  mqtt            poll the station and publish to MQTT until stopped\n"
"\n"
"options:\n"
"  -c, --config FILE      key=value config file (default /etc/wsmqtt.conf if present)\n"
"  -j, --json             JSON output for 'read'\n"
"  -H, --host HOST        MQTT broker host (default localhost)\n"
"  -p, --port PORT        MQTT broker port (default 1883)\n"
"  -u, --user USER        MQTT username\n"
"  -P, --password PASS    MQTT password (or env WSMQTT_PASSWORD)\n"
"  -t, --topic PREFIX     topic prefix (default weatherhome)\n"
"  -d, --discovery PREFIX HA discovery prefix (default homeassistant)\n"
"      --no-discovery     do not publish HA discovery configs\n"
"      --no-maxmin        do not publish the max/min memory\n"
"      --device-id ID     unique id base (default weatherhome_0040)\n"
"      --tls              use TLS (with --cafile or system CAs)\n"
"      --cafile FILE      CA certificate for TLS\n"
"  -i, --interval SEC     poll interval for 'mqtt' (default 60)\n"
"  -v, --verbose          print each published reading\n"
"  -h, --help\n");
}

static void hexdump(const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++)
        printf("%02x%c", p[i], i + 1 == n ? '\n' : ' ');
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

#define CPY(dst, src) snprintf((dst), sizeof(dst), "%s", (src))

static int set_opt(struct wsm_cfg *m, int *interval, const char *k, const char *v)
{
    if (!strcmp(k, "host")) CPY(m->host, v);
    else if (!strcmp(k, "port")) m->port = atoi(v);
    else if (!strcmp(k, "user")) CPY(m->user, v);
    else if (!strcmp(k, "password")) CPY(m->pass, v);
    else if (!strcmp(k, "client_id")) CPY(m->client_id, v);
    else if (!strcmp(k, "topic")) CPY(m->prefix, v);
    else if (!strcmp(k, "discovery_prefix")) CPY(m->disc_prefix, v);
    else if (!strcmp(k, "discovery")) m->discovery = !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes");
    else if (!strcmp(k, "maxmin")) m->maxmin = !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes");
    else if (!strcmp(k, "device_id")) CPY(m->dev_id, v);
    else if (!strcmp(k, "device_name")) CPY(m->dev_name, v);
    else if (!strcmp(k, "tls")) m->tls = !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes");
    else if (!strcmp(k, "cafile")) CPY(m->cafile, v);
    else if (!strcmp(k, "interval")) *interval = atoi(v);
    else return -1;
    return 0;
}

static int load_config(const char *path, struct wsm_cfg *m, int *interval, int required)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        if (required) {
            fprintf(stderr, "%s: %s\n", path, strerror(errno));
            return -1;
        }
        return 0;
    }
    char line[512];
    int ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *s = trim(line);
        if (!*s || *s == '#')
            continue;
        char *eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr, "%s:%d: expected key=value\n", path, ln);
            continue;
        }
        *eq = 0;
        if (set_opt(m, interval, trim(s), trim(eq + 1)) != 0)
            fprintf(stderr, "%s:%d: unknown key '%s'\n", path, ln, trim(s));
    }
    fclose(f);
    return 0;
}

static ws_station *open_or_die(void)
{
    ws_station *s = ws_open();
    if (!s) {
        fprintf(stderr, "station %04x:%04x not found (or no permission; see udev rule)\n",
                WS_VID, WS_PID);
        exit(1);
    }
    return s;
}

static int read_record(ws_station *s, struct ws_record *r)
{
    static const uint8_t req[] = {0x03, WS_CMD_RECORD};
    uint8_t resp[WS_MAX_FRAME];
    int n = ws_exchange(s, req, sizeof req, resp);
    if (n < 0) {
        fprintf(stderr, "read record: %s\n", ws_error(s));
        return -1;
    }
    if (ws_decode_record(resp, (size_t)n, r) != 0) {
        fprintf(stderr, "read record: short/invalid frame\n");
        return -1;
    }
    return 0;
}

static int read_maxmin(ws_station *s, const struct ws_record *live, struct ws_maxmin *m)
{
    static const uint8_t req[] = {0x03, WS_CMD_MAXMIN};
    uint8_t resp[WS_MAX_FRAME];
    int n = ws_exchange(s, req, sizeof req, resp);
    if (n < 0) {
        fprintf(stderr, "read max/min: %s\n", ws_error(s));
        return -1;
    }
    if (ws_decode_maxmin(resp, (size_t)n, live, m) != 0) {
        fprintf(stderr, "read max/min: short/invalid frame\n");
        return -1;
    }
    return 0;
}

static void fmt_t(char *b, size_t n, double v)
{
    if (isnan(v)) snprintf(b, n, "--");
    else snprintf(b, n, "%.1f °C", v);
}
static void fmt_h(char *b, size_t n, int v)
{
    if (v < 0) snprintf(b, n, "--");
    else snprintf(b, n, "%d %%", v);
}
static void fmt_v(char *b, size_t n, double v, const char *unit)
{
    if (isnan(v)) snprintf(b, n, "--");
    else snprintf(b, n, "%.1f %s", v, unit);
}
static void row(const char *label, const char *mx, const char *mn)
{
    printf("%-22s %-14s %s\n", label, mx, mn);
}
static void row_t(const char *label, const double v[2])
{
    char a[24], b[24];
    fmt_t(a, sizeof a, v[0]);
    fmt_t(b, sizeof b, v[1]);
    row(label, a, b);
}
static void row_h(const char *label, const int v[2])
{
    char a[24], b[24];
    fmt_h(a, sizeof a, v[0]);
    fmt_h(b, sizeof b, v[1]);
    row(label, a, b);
}
static void row_v(const char *label, const double v[2], const char *unit)
{
    char a[24], b[24];
    fmt_v(a, sizeof a, v[0], unit);
    fmt_v(b, sizeof b, v[1], unit);
    row(label, a, b);
}
static void row_max(const char *label, double v, const char *unit)
{
    char a[24];
    fmt_v(a, sizeof a, v, unit);
    row(label, a, "");
}

static void print_maxmin(const struct ws_maxmin *m)
{
    char l[48];
    row("", "max", "min");
    row_t("Indoor temperature", m->in_temp);
    row_h("Indoor humidity", m->in_hum);
    for (int i = 0; i < WS_NCH; i++) {
        if (isnan(m->ch_temp[i][0]) && m->ch_hum[i][0] < 0)
            continue;
        snprintf(l, sizeof l, "CH%d temperature", i + 1); row_t(l, m->ch_temp[i]);
        snprintf(l, sizeof l, "CH%d humidity", i + 1);    row_h(l, m->ch_hum[i]);
        snprintf(l, sizeof l, "CH%d dew point", i + 1);   row_t(l, m->ch_dew[i]);
        snprintf(l, sizeof l, "CH%d feels like", i + 1);  row_t(l, m->ch_feel[i]);
    }
    row_v("Pressure (abs)", m->press_abs, "hPa");
    row_v("Pressure (rel)", m->press_rel, "hPa");
    row_max("Wind speed", m->wind_max, "m/s");
    row_max("Wind gust", m->gust_max, "m/s");
    if (m->has_rain) {
        row_max("Rain hour", m->rain_hour_max, "mm");
        row_max("Rain day", m->rain_day_max, "mm");
        row_max("Rain 24h", m->rain_24h_max, "mm");
        row_max("Rain week", m->rain_week_max, "mm");
    }
}

static void prt_t(const char *label, double v)
{
    if (isnan(v)) printf("%-22s --\n", label);
    else printf("%-22s %.1f °C\n", label, v);
}
static void prt_h(const char *label, int v)
{
    if (v < 0) printf("%-22s --\n", label);
    else printf("%-22s %d %%\n", label, v);
}
static void prt_v(const char *label, double v, const char *unit)
{
    if (isnan(v)) printf("%-22s --\n", label);
    else printf("%-22s %.1f %s\n", label, v, unit);
}

static void print_human(const struct ws_record *r)
{
    char l[48];
    prt_t("Indoor temperature", r->in_temp);
    prt_h("Indoor humidity", r->in_hum);
    for (int i = 0; i < WS_NCH; i++) {
        if (isnan(r->ch_temp[i]) && r->ch_hum[i] < 0)
            continue;
        snprintf(l, sizeof l, "CH%d temperature", i + 1); prt_t(l, r->ch_temp[i]);
        snprintf(l, sizeof l, "CH%d humidity", i + 1);    prt_h(l, r->ch_hum[i]);
        snprintf(l, sizeof l, "CH%d dew point", i + 1);   prt_t(l, r->ch_dew[i]);
        snprintf(l, sizeof l, "CH%d feels like", i + 1);  prt_t(l, r->ch_feel[i]);
    }
    prt_v("Pressure (abs)", r->press_abs, "hPa");
    prt_v("Pressure (rel)", r->press_rel, "hPa");
    prt_v("Wind speed", r->wind, "m/s");
    prt_v("Wind gust", r->gust, "m/s");
    if (r->wind_dir < 0) printf("%-22s --\n", "Wind direction");
    else printf("%-22s %d°\n", "Wind direction", r->wind_dir);
    if (r->has_rain) {
        prt_v("Rain hour", r->rain_hour, "mm");
        prt_v("Rain day", r->rain_day, "mm");
        prt_v("Rain 24h", r->rain_24h, "mm");
        prt_v("Rain week", r->rain_week, "mm");
        prt_v("Rain total", r->rain_total, "mm");
    }
}

static int cmd_simple(ws_station *s, const uint8_t *req, size_t n, const char *label)
{
    uint8_t resp[WS_MAX_FRAME];
    int r = ws_exchange(s, req, n, resp);
    if (r < 0) {
        fprintf(stderr, "%s: %s\n", label, ws_error(s));
        return 1;
    }
    if (label[0])
        printf("%-8s ", label);
    hexdump(resp, r);
    return 0;
}

static int cmd_raw(ws_station *s, int argc, char **argv)
{
    uint8_t req[WS_REPORT_SIZE];
    size_t n = 0;
    for (int i = 0; i < argc && n < sizeof req - 1; i++) {
        char *e;
        unsigned long v = strtoul(argv[i], &e, 16);
        if (*e || v > 255) {
            fprintf(stderr, "bad hex byte '%s'\n", argv[i]);
            return 2;
        }
        req[n++] = (uint8_t)v;
    }
    if (n < 2 || req[0] != n + 1) {
        fprintf(stderr, "first byte must be the frame length incl. checksum (%zu)\n", n + 1);
        return 2;
    }
    return cmd_simple(s, req, n, "");
}

static int cmd_synctime(ws_station *s)
{
    struct timeval tv;
    struct tm tm;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    uint8_t req[9];
    ws_synctime_req(req, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                    tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000));
    uint8_t resp[WS_MAX_FRAME];
    if (ws_exchange(s, req, sizeof req, resp) < 0) {
        fprintf(stderr, "synctime: %s\n", ws_error(s));
        return 1;
    }
    printf("station clock set to %04d-%02d-%02d %02d:%02d:%02d\n", tm.tm_year + 1900,
           tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return 0;
}

static void sleep_interruptible(int sec)
{
    for (int i = 0; i < sec * 10 && !stop; i++) {
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
}

static int cmd_mqtt(struct wsm_cfg *m, int interval, int verbose)
{
    if (interval < 5)
        interval = 5;
    if (wsm_start(m) != 0)
        return 1;
    ws_station *s = NULL;
    int failures = 0;
    while (!stop) {
        if (!s) {
            s = ws_open();
            if (!s) {
                if (failures++ % 10 == 0)
                    fprintf(stderr, "station not found, retrying\n");
                sleep_interruptible(10);
                continue;
            }
            fprintf(stderr, "station opened\n");
            failures = 0;
        }
        struct ws_record r;
        struct ws_maxmin mm;
        if (read_record(s, &r) == 0) {
            wsm_publish(&r);
            if (verbose) {
                char js[2048];
                ws_record_json(&r, js, sizeof js);
                printf("%s\n", js);
                fflush(stdout);
            }
            /* a failed max/min read is not fatal; the next round retries */
            if (m->maxmin && read_maxmin(s, &r, &mm) == 0)
                wsm_publish_maxmin(&mm);
        } else {
            ws_close(s); /* reopen on next round (USB unplug etc.) */
            s = NULL;
        }
        sleep_interruptible(interval);
    }
    ws_close(s);
    wsm_stop();
    return 0;
}

int main(int argc, char **argv)
{
    struct wsm_cfg m;
    wsm_defaults(&m);
    int interval = 60, json = 0, verbose = 0;
    const char *cfgfile = NULL;

    /* pre-scan for --config so CLI options override the file */
    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config"))
            cfgfile = argv[i + 1];
    if (load_config(cfgfile ? cfgfile : "/etc/wsmqtt.conf", &m, &interval, cfgfile != NULL))
        return 2;
    const char *envpw = getenv("WSMQTT_PASSWORD");
    if (envpw)
        CPY(m.pass, envpw);

    enum { O_NODISC = 256, O_DEVID, O_TLS, O_CAFILE, O_NOMAXMIN };
    static const struct option lo[] = {
        {"config", 1, 0, 'c'},  {"json", 0, 0, 'j'},       {"host", 1, 0, 'H'},
        {"port", 1, 0, 'p'},    {"user", 1, 0, 'u'},       {"password", 1, 0, 'P'},
        {"topic", 1, 0, 't'},   {"discovery", 1, 0, 'd'},  {"no-discovery", 0, 0, O_NODISC},
        {"device-id", 1, 0, O_DEVID}, {"tls", 0, 0, O_TLS}, {"cafile", 1, 0, O_CAFILE},
        {"interval", 1, 0, 'i'}, {"verbose", 0, 0, 'v'},   {"help", 0, 0, 'h'},
        {"no-maxmin", 0, 0, O_NOMAXMIN},
        {0, 0, 0, 0}};
    int o;
    while ((o = getopt_long(argc, argv, "c:jH:p:u:P:t:d:i:vh", lo, NULL)) != -1) {
        switch (o) {
        case 'c': break;
        case 'j': json = 1; break;
        case 'H': CPY(m.host, optarg); break;
        case 'p': m.port = atoi(optarg); break;
        case 'u': CPY(m.user, optarg); break;
        case 'P': CPY(m.pass, optarg); break;
        case 't': CPY(m.prefix, optarg); break;
        case 'd': CPY(m.disc_prefix, optarg); break;
        case O_NODISC: m.discovery = 0; break;
        case O_NOMAXMIN: m.maxmin = 0; break;
        case O_DEVID: CPY(m.dev_id, optarg); break;
        case O_TLS: m.tls = 1; break;
        case O_CAFILE: CPY(m.cafile, optarg); m.tls = 1; break;
        case 'i': interval = atoi(optarg); break;
        case 'v': verbose = 1; break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (optind >= argc) {
        usage(stderr);
        return 2;
    }
    /* GNU getopt permutes, so options may also follow the command */
    const char *cmd = argv[optind++];

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (!strcmp(cmd, "mqtt"))
        return cmd_mqtt(&m, interval, verbose);

    static const char *const known[] = {"read", "maxmin", "info", "dump", "raw", "synctime"};
    int ok = 0;
    for (size_t i = 0; i < sizeof known / sizeof *known; i++)
        ok |= !strcmp(cmd, known[i]);
    if (!ok) {
        fprintf(stderr, "unknown command '%s'\n", cmd);
        usage(stderr);
        return 2;
    }

    ws_station *s = open_or_die();
    int rc = 0;
    if (!strcmp(cmd, "read")) {
        struct ws_record r;
        rc = read_record(s, &r) ? 1 : 0;
        if (!rc) {
            if (json) {
                char js[2048];
                ws_record_json(&r, js, sizeof js);
                printf("%s\n", js);
            } else {
                print_human(&r);
            }
        }
    } else if (!strcmp(cmd, "maxmin")) {
        struct ws_record r;
        struct ws_maxmin mm;
        /* the live record's status flags tell whether rain data is valid */
        rc = (read_record(s, &r) || read_maxmin(s, &r, &mm)) ? 1 : 0;
        if (!rc) {
            if (json) {
                char js[4096];
                ws_maxmin_json(&mm, js, sizeof js);
                printf("%s\n", js);
            } else {
                print_maxmin(&mm);
            }
        }
    } else if (!strcmp(cmd, "info")) {
        static const uint8_t req[] = {0x03, WS_CMD_DEVINFO};
        rc = cmd_simple(s, req, sizeof req, "devinfo");
    } else if (!strcmp(cmd, "dump")) {
        static const uint8_t r1[] = {0x03, WS_CMD_RECORD}, r2[] = {0x03, WS_CMD_MAXMIN};
        rc = cmd_simple(s, r1, sizeof r1, "record") | cmd_simple(s, r2, sizeof r2, "maxmin");
    } else if (!strcmp(cmd, "raw")) {
        rc = cmd_raw(s, argc - optind, argv + optind);
    } else if (!strcmp(cmd, "synctime")) {
        rc = cmd_synctime(s);
    } else {
        fprintf(stderr, "unknown command '%s'\n", cmd);
        usage(stderr);
        rc = 2;
    }
    ws_close(s);
    return rc;
}
