/* USB HID transport to the station (hidapi). */
#ifndef WS_STATION_H
#define WS_STATION_H

#include <stddef.h>
#include <stdint.h>

typedef struct ws_station ws_station;

ws_station *ws_open(void);
void ws_close(ws_station *s);

/* Send request (len byte first, no checksum, n bytes) and receive the reply
 * frame into resp (WS_MAX_FRAME bytes). Retries on failure.
 * Returns reply length, or negative on error. */
int ws_exchange(ws_station *s, const uint8_t *req, size_t n, uint8_t *resp);

const char *ws_error(ws_station *s);

#endif
