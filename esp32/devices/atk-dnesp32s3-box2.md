# ALIENTEK ATK-DNESP32S3-BOX2

This profile runs Muse's avatar, push-to-talk, live voice calls, a two-key
menu, BLE setup, Wi-Fi and the home-network tunnel on the
**ATK-DNESP32S3-BOX2** (Wi-Fi version). It is not the profile for the V1.1
box ([setup](atk-dnesp32s3-box.md)), the BOX0 or the BOX3, which use other
pins.

The port is experimental: it runs on one unit, and the checklist at the end
says what has and hasn't been checked there.

The box has an ESP32-S3 with 16 MB flash and 8 MB octal PSRAM, a 240×320
ST7789 LCD on an 8-bit i80 bus with a PWM backlight (GPIO21), an ES8389 codec
for the speaker and mic, a battery read through a divider on GPIO1, an SC7A20
accelerometer, and a 16-bit XL9555 I/O expander (0x20) that holds the power
latch, the speaker amplifier, the analog and 4G rails, the charger lines and
three of the keys. ALIENTEK publishes no BSP for it; the pins, the panel's
tuning table, the expander's lines and the battery table follow xiaozhi-esp32's
[atk-dnesp32s3-box2-wifi board](https://github.com/78/xiaozhi-esp32/tree/main/main/boards/alientek/atk-dnesp32s3-box2-wifi).
The accelerometer (I2C 0x19, WHO_AM_I 0x11) and key Q's role were found on a
unit; xiaozhi doesn't use either.

## Build and flash

After activating ESP-IDF v6.0.3, set your SDK token in the build directory's
`sdkconfig` (see [`../AGENTS.md`](../AGENTS.md)), then:

```sh
tools/muse/board.sh build atk-box2
tools/muse/board.sh flash atk-box2
```

The tools expect the chip's own USB Serial/JTAG (`303a:1001`) on the USB-C
port. To keep the factory firmware, back up the whole flash first and store it
outside version control:

```sh
python -m esptool --chip esp32s3 -p PORT read-flash 0 0x1000000 atk-box2-factory.bin
```

### Options

The overlay keeps the avatar's and audio's static buffers (about 18 KB) in
PSRAM (`CONFIG_MUSE_STATIC_BUFFERS_IN_PSRAM`), for the internal RAM Link and
Muse's sessions need. These are off unless the build's `sdkconfig` (or
`idf.py -B build-muse-atk-dnesp32s3-box2 menuconfig`) turns them on:

- `CONFIG_MUSE_REALTIME_VOICE=y`: live voice calls (see "Live voice calls" in
  [`../AGENTS.md`](../AGENTS.md)).
- `CONFIG_MUSE_TAP_MS`: the longest press of M that's a tap, 300 ms by default.
  With live calls, 2000 makes any press under 2 s start or end a call.
- `CONFIG_MUSE_HIDE_REPLY_CAPTIONS=y`: only Muse's animation while it thinks
  and answers, with no transcript or reply text. A voice note's reply is text
  only (Muse doesn't speak it), so with this on it's read in the Muse app;
  live calls are heard.
- `CONFIG_MUSE_CLOCK=y`: the time and IP address in the status line. The time
  comes from `CONFIG_MUSE_CLOCK_NTP_SERVER` (`pool.ntp.org`) once on Wi-Fi, in
  `CONFIG_MUSE_CLOCK_TZ` (a POSIX TZ string, US Pacific time by default; for
  example `UTC0`, or `CST-8` for China). With `CONFIG_MUSE_WEATHER`, once
  Muse has said where you are, the time is that place's instead, from the
  UTC offset Open-Meteo gives with its weather.
- `CONFIG_MUSE_WEATHER=y`: a line under the status line with the location and
  weather, such as `San Jose 18°C Cloudy`. Twice a day the box asks Muse where
  you are, as a typed message that shows in the Muse app's chat; a press of M
  takes priority and the question waits for the next try. If Muse can't tell,
  the location comes from the network's public IP (ipinfo.io), marked `(IP)`,
  and failing that from `CONFIG_MUSE_WEATHER_FALLBACK` (`San Jose`), marked
  `(default)`. The weather comes from Open-Meteo every 30 minutes, in °C
  (`CONFIG_MUSE_WEATHER_FAHRENHEIT=y` for °F). City names with accented
  letters show boxes: the line's font is Montserrat.
- `CONFIG_MUSE_CLOCK_SCREEN=y` (needs `CONFIG_MUSE_CLOCK`): a clock screen
  after the factory firmware's, on key Q. A tap goes from Muse to the clock,
  then to screen off, then back to Muse; a 2 s hold still powers off. It
  shows the weekday, date and time with seconds, and with `CONFIG_MUSE_WEATHER`
  the place, an icon and word for the sky, the temperature, today's high and
  low (landscape only) and the humidity, with Muse at 64 px in the corner.
  Beside the humidity (under it in portrait) are Wi-Fi bars for the signal
  and the battery's level, green while charging and red at 15% or less, or
  `USB` with no battery reading.
  The clock doesn't sleep. M, the menu and an image from Muse take the screen
  while they need it, and the clock comes back 3 s after Muse is idle.
  Checked on a unit in landscape and portrait.

## Controls

- M (XL9555 P0.7, active high): talk. A tap (under `CONFIG_MUSE_TAP_MS`)
  starts or ends a live call, or only wakes the screen; held longer it records
  a voice note from the start of the press. Also pairing confirmation, and
  select in the menu.
- R (BOOT, GPIO0): opens the menu and steps down it. Held at reset it enters
  the ROM bootloader, as on any S3.
- L (XL9555 P0.5): turns the speaker on and off.
- Q (XL9555 P0.6, listed by xiaozhi but unused there): power. A tap turns
  the screen off or on (with `CONFIG_MUSE_CLOCK_SCREEN`, shows the clock
  first), a 2 s hold powers off. Its resting level is read at
  boot, so either polarity works.

## Screen

- Landscape, 320×240, the way the box sits on a desk. Muse is 192 px wide
  either way up, three screen pixels to each of its 64. The picture turns with
  the box within a second: turned over, or stood on either side for portrait,
  240×320, where the screen is laid out again for its shape (the tall compact
  layout). The accelerometer is read twice a second, a turn
  counts after two readings, and the log says each one (`board: box on its
  left side (x -1003, y 12 mg)`). Lying flat keeps it as it is. The turned
  layout comes from the board (`turned` in `muse_board.h`): an open menu
  stays open, and a downloaded image goes.
- The key icons sit along the keys' edge, M in the middle and R at its end,
  and move with it: along the bottom turned over, down a side in portrait.
- Checked on a unit: all four ways up. Portrait takes the panel's own
  address order on one side and both mirrors on the other (`WAYS` in the
  board file).
- Captions show Chinese, Japanese and Korean (`CONFIG_MUSE_CJK_FONT`).
- LVGL draws through two 40-line strips in PSRAM (`DRAW_BUF_LINES`, 50 KB),
  which the i80 bus sends by DMA. In internal RAM even 32 lines left Link and
  Muse's sessions too little (`int=12K/4K` in the heartbeat) and Muse couldn't
  connect. DMA from PSRAM moves whole 64-byte blocks, so every redrawn area is
  widened to 16-pixel columns and an even number of rows (`round_area`).

## Power and limits

- Power off, on battery, drives CHG_CTRL low and opens the power latch
  (XL9555 P1.2), as xiaozhi does. On USB power the chip keeps running, so it
  falls back to deep sleep with the backlight and amplifier off; R, or M or Q
  through the expander's interrupt on GPIO2, wakes it.
- The battery percentage comes from xiaozhi's table of raw ADC counts; no
  voltage is reported. Charging is the charger's CHRG line (P1.7). While
  charging, the reading is raised by the charge current.
- Up to eight Wi-Fi networks are kept; the box joins whichever saved one is in
  range. Add one in the Muse app, or with the `wifi.ssid`, `wifi.pass` and
  `wifi.connect` console commands.
- Not integrated: the TF card slot, the USB-A host port (VBUS_EN stays off)
  and the BOX2-4G's modem.

## Hardware verification

Checked on one unit, powered over USB:

- Boot reports the board name and 8 MB PSRAM, with no panic or reset loop.
- BLE pairing with the Muse app (with an SDK token in the build), Wi-Fi and
  the Muse session. With 16-line draw strips the heartbeat read
  `int=34K/23K min=19K` once Muse connected.
- The speaker plays the built-in MP3 (console key `m`) and the
  `>test.loopback` tones; the mic records voice notes that reach Muse.
- Keys M, R, L and Q as above.
- The picture turns with the box, all four ways up, and the key icons sit
  along the keys' edge each way.

Still to check:

1. The 40-line strips in PSRAM: the picture whole, with no `Draw bitmap
   failed` in the log, and internal RAM through a live call and a voice note
   (`min=` in the heartbeat should stay above about 8K).
2. A boot on battery: the board stays on after the power key is released.
3. Playback after the audio has rested on battery (asleep, unplugged) and
   reopened: xiaozhi keeps the ES8389's output open for fear that reopening
   the duplex bus leaves it silent on IDF 6. On USB the audio never rests.
4. The menu: volume changes and is kept across a restart.
5. Battery: the percentage and the charging state.
6. Power off on battery, and which key turns it back on; on USB, deep sleep
   and waking on R, M and Q.
