#include "station.h"
#include "protocol.h"

#include <hidapi.h> /* pkg-config hidapi-hidraw adds the include dir */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define READ_TIMEOUT_MS 1200
#define RETRIES 3

struct ws_station {
    hid_device *dev;
    char err[160];
};

ws_station *ws_open(void)
{
    if (hid_init() != 0)
        return NULL;
    ws_station *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->dev = hid_open(WS_VID, WS_PID, NULL);
    if (!s->dev) {
        free(s);
        return NULL;
    }
    return s;
}

void ws_close(ws_station *s)
{
    if (!s)
        return;
    hid_close(s->dev);
    free(s);
    hid_exit();
}

const char *ws_error(ws_station *s) { return s->err; }

static int once(ws_station *s, const uint8_t *req, size_t n, uint8_t *resp)
{
    uint8_t out[WS_REPORT_SIZE + 1] = {0}; /* report ID 0 + 64 bytes */
    uint8_t in[WS_REPORT_SIZE];

    if (ws_frame_build(out + 1, req, n) < 0) {
        snprintf(s->err, sizeof s->err, "bad request");
        return -1;
    }
    /* drain stale input */
    while (hid_read_timeout(s->dev, in, sizeof in, 0) > 0)
        ;
    if (hid_write(s->dev, out, sizeof out) < 0) {
        snprintf(s->err, sizeof s->err, "write: %ls", hid_error(s->dev));
        return -1;
    }
    int r = hid_read_timeout(s->dev, resp, WS_REPORT_SIZE, READ_TIMEOUT_MS);
    if (r <= 0) {
        snprintf(s->err, sizeof s->err, "read: %s", r == 0 ? "timeout" : "error");
        return -1;
    }
    if (resp[0] > WS_REPORT_SIZE) {
        r = hid_read_timeout(s->dev, resp + WS_REPORT_SIZE, WS_REPORT_SIZE,
                             READ_TIMEOUT_MS);
        if (r <= 0) {
            snprintf(s->err, sizeof s->err, "read (2nd report): %s",
                     r == 0 ? "timeout" : "error");
            return -1;
        }
    }
    int c = ws_frame_check(resp, WS_MAX_FRAME, req[1]);
    if (c != 0) {
        snprintf(s->err, sizeof s->err, "bad reply (%s)",
                 c == -2 ? "checksum" : c == -3 ? "command mismatch" : "length");
        return -1;
    }
    return resp[0];
}

int ws_exchange(ws_station *s, const uint8_t *req, size_t n, uint8_t *resp)
{
    for (int i = 0; i < RETRIES; i++) {
        memset(resp, 0, WS_MAX_FRAME);
        int r = once(s, req, n, resp);
        if (r > 0)
            return r;
    }
    return -1;
}
