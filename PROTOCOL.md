# WeatherHome USB protocol (DevID 0x0040)

Reverse-engineered from `WeatherHome.exe`, `USBDriver.dll` and `ID0040.dll`
(installed in `C:\Program Files (x86)\WeatherHome`), and verified against a live
station that reports itself as `TMU313X USB R/W64`.

## USB

| | |
|---|---|
| Class | HID (no report descriptor quirks, report ID 0) |
| VID / PID | `0x1130` / `0x0829` (bcdDevice `0x0100`) |
| Report size | 64 bytes each way (65 including report ID 0 on write) |

## Framing

Each request and reply is a frame:

```
[len] [cmd] [payload ...] [cksum]
```

* `len` is the length of the whole frame, including `len` and `cksum`.
* `cksum = (sum of frame[0 .. len-2]) & 0xFF`.
* A request is padded with zeros to 64 bytes and written as one report.
* The reply arrives in one 64-byte report. If `reply[0] > 0x40`, a second
  report follows; append it (maximum reply is 128 bytes).
* The reply's `cmd` byte equals the request's `cmd`, and its checksum must be valid.
* The original driver drains pending input before every request, waits ~50 ms
  for the write and up to 1200 ms for each read.

## Commands

| Name | Request (before cksum) | Notes |
|---|---|---|
| ReadDeviceInfo | `03 01` | reply e.g. `0b 01 0a 40 00 fb 63 00 40 00` (contains DevID 0x0040) |
| ReadFatInfo | `04 02 00` | history memory info |
| ReadBlockInfo | `07 02 01 addrLo addrHi n` | |
| ReadHisData | `07 02 02 addrLo addrHi n` | |
| ReadHisInfo | `04 02 03` | |
| SyncTime | `0A 83 frac sec min hour day month year-2000` | `frac` = (ms+62)/125, capped at 7; see *Clock* |
| ReadRecord (live) | `03 04` | 76-byte reply, see below |
| ReadMaxMin | `03 05` | 116-byte reply |
| ReadAlarm / WriteAlarm | `len 06 …` / `len 86 …` | |
| ClearHistory / ClearMaxMin | `04 87 00` / `04 87 01` | |
| ReadParam / WriteParam | `len 08 …` / `len 88 …` | |
| ReadGraph | `05 09 type channel` | |
| ReadCalib / WriteCalib | `len 0A …` / `len 8A …` | |

## Clock

* SyncTime sets the console clock to exactly the fields sent. WeatherHome
  sends the PC's **local** time (`GetLocalTime`), not UTC.
* The reply is `[len] 83 status …`, where `status == 0` means accepted.
* There is **no command to read the clock back**. No other reply carries the
  current time, so a sync can only be checked on the console display.
* WeatherHome resyncs automatically: its timer (`WeatherHome.exe` @ `0x41ecc0`)
  sends SyncTime whenever the local hour differs from the last successfully
  synced hour. That covers startup, each hour, and DST switches.
* The firmware has time-format, time-zone, DST and "RCC Receive" settings in
  the ReadParam frame (`03 08`, 27-byte reply). These are not decoded. Units
  without a DCF77 receiver depend entirely on the host for the time.

## Live record (`03 04`)

Offsets are into the **payload** (frame offset − 2). LE = little-endian.

| Off | Size | Field | Encoding |
|---|---|---|---|
| 0 | 5 | status flags | byte0/byte1: per-channel bitmasks, byte2: misc flags (bit 6 = rain/wind sensor present) |
| 5 | 2 | indoor temperature | `T12 = LE16 & 0xFFF` |
| 7 | 1 | indoor humidity | `H8` |
| 8 | 12 | ch1..ch8 temperature | 8 × packed 12-bit `T12` |
| 20 | 8 | ch1..ch8 humidity | 8 × `H8` |
| 28 | 12 | ch1..ch8 dew point | 8 × packed 12-bit `T12` |
| 40 | 12 | ch1..ch8 feels-like | 8 × packed 12-bit `T12` |
| 52 | 2 | absolute pressure | `LE16 / 10` hPa (`>= 0x7FFA` invalid) |
| 54 | 2 | relative pressure | `LE16 / 10` hPa |
| 56 | 3 | wind speed, gust | packed 12-bit pair, `/10` m/s (`>= 0x7FA` invalid) |
| 59 | 2 | wind direction | `LE16 & 0xFFF` degrees (`>= 0x7FA` invalid) |
| 61 | 2 | rain hour | `LE16 / 10` mm — only if status byte2 bit 6 |
| 63 | 2 | rain day | `LE16 / 10` mm |
| 65 | 5 | rain 24 h, rain week | packed 20-bit pair, `/10` mm |
| 70 | 3 | rain total | `LE24 & 0xFFFFF`, `/10` mm |

**Packed 12-bit arrays**: item *i* (0-based) sits at byte `3*(i/2)`. Even
items are `LE16(p) & 0xFFF`, and odd items are `LE16(p+1) >> 4`.

**Packed 20-bit pair** at `p`: first = `LE24(p) & 0xFFFFF`, second = `LE24(p+2) >> 4`.

**`T12` temperature**: values `>= 0x7FA` are sentinels (no sensor / error).
Otherwise `°F = (raw − 400) / 10`. The station works natively in °F, and
WeatherHome converts with `(°F − 32) / 1.8`.

**`H8` humidity**: percent; values `>= 0x7A` (122) are invalid.

The rain slot order comes from the app's string table: Event, Rate, Hour, Day,
24 Hour, Week, Month, Total. The live record fills Hour, Day, 24 Hour, Week
and Total.

Native units, confirmed from the unit converters in `ID0040.dll`: °F, %, hPa
(÷33.8639 → inHg), m/s (Beaufort table 0.3/1.6/3.4/5.5/7.9/…), mm (÷25.4 → in).

## Max/min memory (`03 05`)

The reply is 116 bytes (113-byte payload). Fields use the same encodings as the
live record. Each pair is **max first, then min**. The station stores no
timestamps. The layout is fixed: WeatherHome skips fields according to the
live record's status flags, but their offsets never move.

| Off | Size | Field | Encoding |
|---|---|---|---|
| 0 | 3 | indoor temperature max, min | packed 12-bit pair, `T12` |
| 3 | 2 | indoor humidity max, min | 2 × `H8` |
| 5 | 24 | ch1..ch8 temperature max, min | 8 × packed 12-bit pair |
| 29 | 16 | ch1..ch8 humidity max, min | 8 × (`H8`, `H8`) |
| 45 | 24 | ch1..ch8 dew point max, min | 8 × packed 12-bit pair |
| 69 | 24 | ch1..ch8 feels-like max, min | 8 × packed 12-bit pair |
| 93 | 8 | abs pressure max, min; rel pressure max, min | 4 × `LE16 / 10` hPa |
| 101 | 3 | wind speed max, gust max | packed 12-bit pair, `/10` m/s |
| 104 | 2 | rain hour max | `LE16 / 10` mm |
| 106 | 2 | rain day max | `LE16 / 10` mm |
| 108 | 5 | rain 24 h max, rain week max | packed 20-bit pair, `/10` mm |

Channel *i* uses the bytes at `5 + 3i`, `29 + 2i`, `45 + 3i` and `69 + 3i`.
Example values from 2026-11-15: indoor 33.0 / 15.0 °C, 72 / 17 %; ch1
40.7 / −16.6 °C, 99 / 16 %; abs 1033.9 / 951.1 hPa, rel 1043.9 / 956.0 hPa;
wind 13.2 m/s, gust 18.2 m/s; rain max hour 7.8, day 24.6, 24 h 51.6, week
96.3 mm.

### Example (2026-11-15)

```
4c 04 01 01 7f 00 00 8f 04 2e 46 a3 7f fa a7 7f fa a7 7f fa a7 7f 5c 7a 7a 7a
7a 7a 7a 7a 31 a3 7f fa a7 7f fa a7 7f fa a7 7f 47 a3 7f fa a7 7f fa a7 7f fa
a7 7f 2b 26 dd 26 00 00 00 8f 00 00 00 00 00 00 00 00 00 00 fe 10 00 79
```

indoor 76.7 °F / 24.8 °C, 46 % · ch1 43.8 °F / 6.6 °C, 92 %, dew 41.7 °F,
feels 43.9 °F · 977.1 / 994.9 hPa · wind 0.0, gust 0.0 m/s from 143° · rain total 435.0 mm.
