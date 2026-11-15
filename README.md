# wsmqtt — WeatherHome USB weather station → MQTT

A Linux CLI that reads the USB weather station supported by the Windows
**WeatherHome** app (HID `1130:0829`, reports itself as `TMU313X USB R/W64`,
WeatherHome DevID `0x0040`). It publishes readings to MQTT, with Home Assistant
auto-discovery.

The protocol was reverse-engineered from the WeatherHome binaries; see
[PROTOCOL.md](PROTOCOL.md).

## Build

```sh
sudo apt install build-essential pkg-config libhidapi-dev libmosquitto-dev
make
make test
sudo make install         # binary, udev rule, systemd unit, /etc/wsmqtt.conf
sudo udevadm control --reload && sudo udevadm trigger
```

Your user needs to be in the `plugdev` group (or be logged in at the console)
to use the station without root.

## Usage

```sh
wsmqtt read               # human-readable current values
wsmqtt read --json        # one JSON object
wsmqtt maxmin             # the station's recorded max/min values (--json too)
wsmqtt info               # raw device info frame
wsmqtt dump               # raw live-record and max/min frames (hex)
wsmqtt raw 03 04          # send any request; checksum is appended
wsmqtt synctime           # set the station clock from this machine
wsmqtt -H broker -u user -P pass mqtt -v   # publish every 60 s
```

Example output:

```
Indoor temperature     24.8 °C
Indoor humidity        46 %
CH1 temperature        6.6 °C
CH1 humidity           92 %
CH1 dew point          5.4 °C
CH1 feels like         6.6 °C
Pressure (abs)         977.1 hPa
Pressure (rel)         994.9 hPa
Wind speed             0.0 m/s
Wind gust              0.0 m/s
Wind direction         143°
Rain hour              0.0 mm
...
Rain total             435.0 mm
```

## MQTT

| Topic | Content |
|---|---|
| `weatherhome/state` | JSON reading, e.g. `{"indoor_temp":24.8,"indoor_humidity":46,"ch1_temp":6.6,…,"rain_total":435.0}` |
| `weatherhome/maxmin` | retained JSON max/min memory, e.g. `{"indoor_temp_max":33.0,"indoor_temp_min":15.0,…,"wind_gust_max":18.2}` |
| `weatherhome/status` | `online` / `offline` (retained, LWT) |
| `homeassistant/sensor/weatherhome_0040/<key>/config` | retained HA discovery configs |

Units are metric: °C, %, hPa, m/s, degrees, mm. Missing sensors are `null`.
The max/min values are whatever the station has recorded since its memory was
last cleared on the console; there are no timestamps. Turn them off with
`--no-maxmin` or `maxmin = no`.
Remote channels (CH1–CH8) are included only when a sensor reports on them.
Their discovery configs are sent the first time each one appears.

Configure with `/etc/wsmqtt.conf` (see `contrib/wsmqtt.conf.example`) or with
command-line options (`wsmqtt --help`). Options override the file. The password
can also come from `WSMQTT_PASSWORD`.

## Run as a service

```sh
sudo systemctl enable --now wsmqtt
journalctl -u wsmqtt -f
```

The service reconnects to the broker on its own and reopens the station if it
is unplugged.
