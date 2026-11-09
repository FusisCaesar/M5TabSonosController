# Tab5 control handover

Instructions for an agent bringing up a new firmware project on an M5Stack Tab5. This is how the board is actually controlled. It is not a product spec. The rowing app that discovered these constraints is only mentioned where its code is the working example.

Copy this file into the new repo as `AGENTS.md` or a Cursor rule, and copy the small platform files named at the end. Do not copy the rowing UI.

The unit this was proven on is an ESP32-P4 revision v1.3 (pre-rev3), read by esptool as USB-Serial/JTAG, 16 MB flash, 400 MHz, 40 MHz crystal. A later P4 revision (chip revision 300 or above) is a different M5 board id (`board_M5Tab5X`) and needs the matching PlatformIO board, not `m5stack-tab5-p4`.

## Toolchain

Arduino on the ESP32-P4, built with PlatformIO. There is no hand-written ESP-IDF project, no `sdkconfig`, and no custom partition CSV in the application repo.

```ini
[platformio]
default_envs = tab5
; Windows only, while long paths are off. Delete on Linux/macOS.
core_dir = C:/pio

[env:tab5]
platform = https://github.com/pioarduino/platform-espressif32.git#55.03.312-1
framework = arduino
board = m5stack-tab5-p4
board_build.partitions = default_16MB.csv
board_upload.flash_size = 16MB
monitor_speed = 115200
monitor_filters = esp32_exception_decoder
lib_deps =
    https://github.com/M5Stack/M5Unified.git#0.2.26
    https://github.com/M5Stack/M5GFX.git#0.2.33
```

Facts that took a failed flash to learn:

- Pin both `board_build.partitions = default_16MB.csv` and `board_upload.flash_size = 16MB`. Without the flash size the core silently used a 1.3 MB app partition.
- `m5stack-tab5-p4` is the pre-rev3 (v1.x) board. It already defines `ARDUINO_USB_CDC_ON_BOOT` and `ARDUINO_USB_MODE` (hardware CDC, not TinyUSB). Do not use `esp32-p4-evboard`. That variant treats GPIO45 as SD-card power and enables on-chip LDO channel 4. On a Tab5, GPIO45 is a normal M5-Bus pin and the SD slot is already powered.
- The platform above is Arduino-ESP32 3.3.12 / ESP-IDF 5.5 (the flash log recorded IDF libs 5.5.5 and esptool 5.4.0). The camera driver (`ESP_Video`, SC2356) needs core 3.3.11 or newer. Core 3.2.1 (platform `54.03.21`) has CSI, ISP, and PPA, and no SC2356 driver.
- PSRAM must run at 200 MHz. Arduino-ESP32 3.3.0 cannot. Use 3.3.1 or later. Sprites and framebuffers will not fit in the 512 KB of internal RAM. This board has 32 MB of PSRAM.
- M5Unified 0.2.26 and M5GFX 0.2.33 are the versions that built and ran this firmware (git deps, otherwise unpinned). Floors that matter: M5Unified >= 0.2.23 and M5GFX >= 0.2.30, because older releases miss the ST7121/ST7123 panel and the LCD-reset sequence below. M5Unified 0.2.26 itself requires M5GFX >= 0.2.31. Pin the versions. A floating git HEAD will move.
- The board definition uploads with esptool at 1.5 Mbaud. Application serial is 115200. `monitor_filters = esp32_exception_decoder`.
- `default_16MB.csv` gives two app slots of 6,553,600 bytes (6.25 MB) and a 20 KB NVS. This firmware never mounts the SPIFFS partition that table also contains. OTA was not used. `boot_app0.bin` is still written at `0xe000` because the Arduino uploader does that. Image layout that was flashed: bootloader `0x2000`, partitions `0x8000`, `boot_app0.bin` `0xe000`, application `0x10000`.
- "Core dump data check failed" on boot is harmless. This table has no coredump partition.

Flash from the repo root. On the machine this was developed on, the Tab5 was COM10. A second "USB Serial Device" on COM8 was never the flash or serial port. Confirm the port on another PC.

```powershell
$env:PYTHONIOENCODING='utf-8'; $env:PYTHONUTF8='1'
python -m platformio run -e tab5 -t upload --upload-port COM10
```

Windows:

- `PYTHONUTF8=1` is required. esptool's progress bar prints characters outside cp1252 and the upload dies after a successful compile.
- `core_dir = C:/pio` exists because the Arduino package nests past the 260-character path limit, and enabling long paths needs an administrator. The toolchain extract is about 2 GB. Delete that line if long paths are on, or on Linux/macOS.
- PowerShell does not accept `&&` on older versions. Use `;`.
- One build and one upload at a time. PlatformIO and esptool share `.pio` and the COM port. An overlapped upload left a half-written image and a black screen. Recovery: stop every `pio` and `esptool` process, `python -m platformio run -e tab5 -t clean`, then one upload. The ROM download mode cannot be overwritten by a bad app. If USB stops answering, hold the boot button while resetting.
- "Access denied" or "port is busy" means a leftover esptool, monitor, or serial script still has the port. The compile was fine.
- A one-off `HP_SYS_HP_WDT_RESET` and a dark screen immediately after a flash was a bad reset or a half-written image. One clean upload fixed it. It was not an application bug.

## Boot

`M5.begin()` is the only supported way to bring the panel, touch, RTC, IMU, fuel gauge, and GPIO expanders up. Do not bit-bang LCD reset.

```cpp
auto c = M5.config();
c.serial_baudrate = 115200;
c.internal_imu = true;
M5.begin(c);
Serial.setTxTimeoutMs(0);
// create the I2C mutex here, before any other task touches the bus
M5.Power.setExtOutput(true);  // Grove / M5-Bus / side-header 5 V
M5.Ex_I2C.release();           // Port A is I2C until this returns
```

`M5.config()` defaults leave the internal RTC, mic, and speaker enabled. This firmware only overrides baud and IMU. It never calls `Mic.begin()` or `Speaker.begin()`. Pin configs for audio are registered; the codecs are not programmed until those objects start. Leave them off unless the new app needs audio. See the address collision under I2C.

Order that matters:

1. `M5.begin()` detects the board (GPIO32 high, then chip revision: below 300 is Tab5, 300 and above is Tab5X), starts internal I2C, both PI4IOE5V6408 expanders, the panel, touch, RX8130, BMI270, and INA226. Before any Wi-Fi init it rewrites the ESP-Hosted SDIO pins to the Tab5 wiring. A generic P4 board definition points the C6 at the wrong pins, and the rewrite only works if the hosted driver is not already initialised.
2. `Serial.setTxTimeoutMs(0)` immediately after `M5.begin()`. See USB serial. Skipping this freezes the UI whenever a cable is plugged in and nothing is reading.
3. Create the shared I2C mutex before the IMU task, the camera, or `M5.update()`.
4. `M5.Power.setExtOutput(true)` before a Grove peripheral that needs 5 V. On Tab5 that is PI4IOE `0x43` pin 2, `EXT5V_EN`. It also feeds the rear M5-Bus 5 V and the side 2.54 mm header.
5. `M5.Ex_I2C.release()` before using GPIO53/GPIO54 as a UART. Until then, Port A is the external I2C bus.

Do not "fix" `setExtOutput` by forcing the expander direction bit. A reading of M5Unified showed the output latch set while `IO_DIR` bit 2 stayed clear, which looked like 5 V was not enabled. The GPS was already streaming NMEA, so the rail was on. Patch it only if a module is actually unpowered.

LCD reset is PI4IOE `0x43` pin 4, not a native GPIO. M5GFX drives it low, then switches it to an input with pull-up. Leaving it driven high breaks the shared I2C bus (IMU, touch, RTC, fuel gauge, camera SCCB). The same sequence lives in `Power_Class` for Tab5: `IO_DIR` has LCD reset as an input, pull-up enabled.

## One I2C bus

Internal bus: SDA GPIO31, SCL GPIO32. External bus, until released: SDA GPIO53, SCL GPIO54.

M5Unified drives this bus below the ESP-IDF driver's lock. The BMI270 task, `M5.update()` (touch), the RTC, the fuel gauge, and the camera SCCB will corrupt each other unless they share one recursive mutex. Hold it around every `M5.update()`, `M5.Imu.update()`, `M5.Rtc` call, `M5.Display.setBrightness`, and `M5.Power.getBatteryVoltage`.

The camera's ISP task calls `i2c_master_*` on its own. A second I2C controller on the same pins still collides electrically. Linker-wrap the IDF entry points so those calls take the same mutex:

```ini
-Wl,--wrap=i2c_master_transmit
-Wl,--wrap=i2c_master_receive
-Wl,--wrap=i2c_master_transmit_receive
-Wl,--wrap=i2c_master_probe
```

The working wrappers are `src/board.cpp` and `src/board.h` in this repo. The mutex must already exist; the wrappers no-op the lock when it is still null, which is only safe before `board::begin()` returns and before the camera starts.

Addresses M5Unified 0.2.26 actually uses on this board:

| Device | Address | Where |
| --- | --- | --- |
| ES8388 speaker codec | `0x10` | programmed only if `Speaker` starts |
| GT911 touch (units before 2025-10-14) | `0x14` | `0x5D` if INT GPIO23 is low at probe |
| RX8130CE RTC | `0x32` | `RX8130_Class::DEFAULT_ADDRESS` |
| ES7210 microphone ADC | `0x40` | `es7210_i2c_addr` in M5Unified |
| INA226 pack monitor | `0x40` | `INA226_Class::DEFAULT_ADDRESS`; Tab5 power init calls `Ina226.begin()` with no override |
| PI4IOE5V6408 | `0x43` and `0x44` | constructed in `M5.begin` |
| ST7123 / ST7121 touch | `0x55` | `Touch_ST7123::default_addr` |
| BMI270 | `0x69` | `BMI270_Class::DEFAULT_ADDRESS` |

The published Tab5 pin table lists the BMI270 at `0x68` and the INA226 at `0x41`. This firmware's library does not. IMU samples and pack voltage both come back through the defaults above, so use `0x69` and `0x40` with M5Unified 0.2.26. Do not start `M5.Mic` while reading the INA226: the library also places the ES7210 at `0x40`, and the two drivers will fight.

Expander `0x43` pins, low bit = P0, from `Power_Class` for Tab5:

| Pin | Name | This firmware |
| --- | --- | --- |
| P0 | antenna select, low = internal, high = external MMCX | left to the boot default |
| P1 | speaker amp enable (`NS4150B`) | off unless `Speaker` starts |
| P2 | `EXT5V_EN` | on, via `setExtOutput(true)` |
| P4 | LCD reset | input + pull-up after reset pulse |
| P5 | touch reset | owned by M5GFX |
| P6 | camera reset | owned by M5GFX / ESP_Video |
| P7 | headphone detect | input |

Expander `0x44`:

| Pin | Name | This firmware |
| --- | --- | --- |
| P0 | C6 Wi-Fi/BLE power | owned by M5.Power |
| P3 | USB-A 5 V (`USB5V_EN`) | off; USB-A is a host port |
| P4 | power-off pulse | not used |
| P5 | `nCHG_QC_EN` | left to M5.Power |
| P6 | charge status | input |
| P7 | charge enable | left to M5.Power |

Charge current, if a new app sets it, is 0, 500, or 1000 mA through those expander pins. There is no AXP PMIC. This firmware never calls `setChargeCurrent`.

Other pins that are easy to reuse by mistake:

- Camera MCLK is GPIO36. GPIOs above 36 can have LDO power issues on a bare P4 devkit; on the Tab5 they are already assigned. Do not treat them as free.
- RS485 transceiver (not used here): RX GPIO21, TX GPIO20, direction GPIO34.
- ESP32-C6 SDIO: CLK GPIO12, CMD GPIO13, D0 GPIO11, D1 GPIO10, D2 GPIO9, D3 GPIO8, reset GPIO15, C6 IO2 on GPIO14. Do not use these for anything else.
- Do not put a second device on GPIO31/GPIO32. Those pins are also broken out on the M5-Bus and they are the internal I2C bus.

## Display

The panel is a 5-inch MIPI-DSI framebuffer in PSRAM, native portrait 720×1280. It is not a SPI ILI panel. There is no SPI init sequence to copy.

M5GFX 0.2.33 autodetects three generations. Check the sticker on the back if you care which one you have; the library already branches:

- Before 2025-10-14: ILI9881C, touch GT911 at `0x14`.
- From 2025-10-14: ST7123, touch at `0x55`.
- From 2026-04-28: ST7121, touch still at `0x55`. Lane rate 900 Mbps; the others use 1040 Mbps.

Early firmware and old M5GFX builds do not light the newer panels. Updating the library is the fix, not a custom driver.

Bring-up details the application relies on and must not redo:

- Touch INT GPIO23 is driven high before the probe, so a GT911 answers at `0x14` rather than `0x5D`, then released to an input.
- Board identity at probe time is "both PI4IOE `0x43` and `0x44` ACK".
- The MIPI DSI PHY LDO is channel 3 at 2500 mV. The camera uses that same rail. See Camera.
- ST7123 porch timings are load-bearing for touch, not just for the picture. `vsync_back_porch + vsync_pulse_width` stays 10 or the image shifts. Cutting `vsync_front_porch` (220) makes the touch controller stop. Leave the panel config to M5GFX.
- Backlight is LEDC PWM on GPIO22, channel 7, 44100 Hz. `M5.Display.setBrightness(0..255)` under the I2C lock. A default of 200 is readable. Floor used by the rowing UI was 20.

Landscape is a rotation, not a second panel. Native is portrait. This UI uses `M5.Display.setRotation(flip ? 1 : 3)` so a 180° mount stays landscape. Rotation 1 was the first sketch that came up the right way; rotation 3 is that view turned 180°. Any PPA blit has to apply the same rotation as `Panel_FrameBufferBase::display()`, including `offset_rotation`, or sprites land mirrored or off-screen.

There is no LVGL in this project. Drawing is an M5GFX `M5Canvas` sprite, 16-bit, allocated in PSRAM (`setPsram(true)`), then copied into the DSI framebuffer by the Pixel Processing Accelerator.

`canvas.pushSprite` of a large PSRAM sprite onto the rotated framebuffer measured about 2 fps (a plot redraw of 434 ms). A `pushSprite` with a transparent colour key also leaves the old pixels on screen. Fill the sprite with the real background.

The working blit is `src/display_fb.cpp`:

- `PPA_OPERATION_SRM`, blocking, RGB565 in and out, scale 1.
- `op.byte_swap = true`. M5GFX canvases store RGB565 big-endian. The panel framebuffer is little-endian.
- The framebuffer pointer `_lines_buffer` is protected in M5GFX. Reach it with an empty derived class and a member pointer. Do not fork M5GFX for this.
- If the PPA client fails to register, or the blit fails, fall back to `pushSprite`.
- The PPA writes the framebuffer behind the CPU cache. Before `readRect` (screenshots), call `esp_cache_msync(buffer, size, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED)`. Skipping it returns a stale or torn shot.
- One writer. Capture camera frames on another task and push them into the framebuffer from the UI task. Two writers were not made safe. There is one live framebuffer, not a swap chain, so a blit can tear; that was accepted.

On this RISC-V core, casting packed file bytes to `int32_t*` faults. `memcpy` each field. `std::vector` and other default allocators land in internal RAM and will not hold a map or a full-frame sprite. Use `heap_caps_malloc` / `heap_caps_realloc` with `MALLOC_CAP_SPIRAM`. A full-frame 1280×720 RGB565 buffer is about 1.8 MB. A 1280×564 UI canvas (about 1.44 MB) allocated. Log and skip the feature if the allocation returns null. Do not fall a large image back to internal RAM.

`readRect` works because M5GFX sets the panel readable. Screenshot bytes from `readRect` are byte-swapped relative to the panel. Decode them as big-endian RGB565 (`v = (data[2*i] << 8) | data[2*i+1]`). A raw dump viewed as little-endian looks wrong.

## Touch

The application never talks to the controller. After `M5.begin()`:

```cpp
M5.Touch.setHoldThresh(800);          // product choice, not a controller limit
auto t = M5.Touch.getDetail();        // inside the I2C lock, via M5.update()
```

Coordinates are in the rotated display space (1280×720 when landscape). A drag threshold of about 12 px was enough to separate a tap from a drag. M5Unified also synthesises BtnA/B/C from a touch strip along the native 1280 edge. This UI ignores those and draws its own buttons.

`M5.update()` shares the bus with the IMU task. Call it only while holding the I2C mutex.

The on-screen keyboard in this repo is drawn into the caller's canvas. It is not a hardware keyboard. The Tab5 has none.

## USB serial

The console is the ESP32-P4 native USB Serial/JTAG CDC. Baud in software is 115200. USB CDC ignores it. Set it anyway so monitors and scripts agree.

Opening the port with DTR or RTS high resets the chip. pyserial does that on open and on close. Every host tool goes through this, which is `tools/tab5serial.py`:

```python
ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = timeout
ser.dtr = False
ser.rts = False
ser.open()
```

An intentional reset that worked: `dtr=False`, `rts=True` for 0.2 s, then `rts=False`.

After `ESP.restart()` or an RTS reset, USB CDC re-enumerates. Reopen the port. Reading the log on the handle that issued the reset returns nothing.

`Serial.setTxTimeoutMs(0)` is mandatory. With the cable plugged in and no reader, the hardware CDC still sees start-of-frame and treats the host as connected. `Serial.printf` then blocks on the TX timeout. A 300-byte status line splits into 64-byte packets and stalled the UI for about a second every few seconds. The screen was smooth only while a PC tool held the port open, and also smooth with the cable unplugged. Timeout 0 drops the print instead of blocking.

Binary transfers have to raise the timeout again. At 0 the USB driver drops bytes that do not fit in the TX buffer. This firmware uses 200 ms for screenshots and file downloads, then sets it back to 0.

Host-side rules:

- Pass the COM port explicitly. Nothing here scans for it.
- `python -c` from the repo root does not import `tab5serial`. Set `PYTHONPATH=tools` or insert `tools` on `sys.path`.
- Rapid separate `python tools\send.py` processes drop bytes. One open port and about 0.5 s between commands works. Four separate keystrokes of the same command advanced the UI once.
- PowerShell double quotes eat `$` and backticks, and `|` is a pipe. A Wi-Fi password containing either never reached the device. Single-quote the argument: `python tools\send.py COM10 'W SSID|PASSWORD'`.

The rowing firmware's command set is a product protocol, not something the new app must implement. Copy the pattern if you want screenshots and files:

| What | How | Why it is shaped this way |
| --- | --- | --- |
| Screenshot | Device prints `SCREENSHOT <w> <h>` then `w*h*2` raw bytes, one row at a time, after `fb::syncForRead()` and with the TX timeout raised | `readRect` is big-endian RGB565 |
| Upload | Header `U <path> <size> <crc32hex>`, device prints `[upload] READY`, host sends 240-byte chunks, device replies `0x06` after each, CRC is `esp_rom_crc32_le` / `zlib.crc32` | USB CDC RX overflows on a bulk write. 240 stays under that buffer |
| Download | Device prints `[fetch] SIZE <n>`. Host sends `N` (next 4096 bytes), `R` (resend), or `Q`. Each chunk is followed by a little-endian CRC32 | The link drops bytes. Retry a bad chunk |
| Injected touch | `T x y` tap, `P x y` hold, `Z x0 y0 x1 y1` drag, in rotated pixels | Lets a host script drive the UI without a finger |

`tools/send.py` sends one character raw and a longer command plus a newline. It waits for a line matching `^\[\w+\] (OK|ERR)` and ignores status lines. Touch commands exit immediately.

## Storage

Only the microSD. This firmware never mounts SPIFFS, LittleFS, or FFat. Small settings go in NVS through Arduino `Preferences`.

SDMMC 4-bit, not SPI. SPI on these pins returned error 3 (card not answering). Error 13 would have meant "card answered, no filesystem".

| Signal | GPIO |
| --- | --- |
| DAT0 | 39 |
| DAT1 | 40 |
| DAT2 | 41 |
| DAT3 | 42 |
| CLK | 43 |
| CMD | 44 |

```cpp
SD_MMC.setPowerChannel(-1);  // do not enable LDO4 or drive GPIO45
SD_MMC.setPins(43, 44, 39, 40, 41, 42);
SD_MMC.begin("/sdcard", false, false, 20000, maxOpenFiles);
```

`setPowerChannel(-1)` is required even on `m5stack-tab5-p4`, which has no SD-power pin of its own. The generic ESP32-P4 Arduino variant enables LDO channel 4. The Tab5 powers the slot itself. Keep the call.

Gold contacts face the same way as the screen. Upside down looks exactly like a driver failure (timeout `0x107`, no OCR).

FAT32 only. exFAT is compiled out of this Arduino FatFs. A 59 GB card once mounted as a 39 MB leftover partition. Format on device with `f_mkfs` on `"0:"` and no partition number, which writes a new MBR and one FAT32 volume (32 KB clusters were used). `f_fdisk` is unnecessary. Before formatting, `f_getfree("0:")` and check that the volume size equals `SD_MMC.totalBytes()`, so drive 0 is actually this card. Then `ESP.restart()`. USB CDC drops across reboot, and the card needs a remount. Windows will not FAT32-format a card larger than 32 GB from the GUI. Refuse the format while a file is open.

NVS is 20 KB, shared by every `Preferences` namespace. Open it read-write. A read-only `begin` logged a harmless `nvs_open` error and did not create the namespace. `putBytes` of a large blob fails. There is no second NVS partition. Logs go on the SD card. Moving the partition table wipes this 20 KB.

The open-file budget has to cover every file the app holds at once. `SD_MMC.begin`'s last argument is that count. Uploads and downloads in this firmware are refused while a logging session has files open.

## Wi-Fi and BLE

The P4 has no radio. Wi-Fi 6 and BLE are an ESP32-C6-MINI over SDIO, on the pins listed above. M5Unified calls `hostedSetPins(12, 13, 11, 10, 9, 8, 15)` before the hosted driver initialises. C6 power is expander `0x44` P0. Antenna select is expander `0x43` P0 (low = internal). This firmware never toggled the antenna switch.

Use the Arduino `WiFi` class and the bundled `BLEDevice`. Do not add another NimBLE library. Arduino-ESP32 3.3.12's P4 config already sets `CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE` and `CONFIG_BT_NIMBLE_ROLE_CENTRAL`.

Constraints that showed up on this board:

- 2.4 GHz only. `WiFi.status()` 1 after a 20 s attempt was "not found", which included a 5 GHz-only access point. Status 4 was usually a bad password.
- Keep the radio off until something asks. It draws enough that this app does not hold it up for the whole boot.
- `WiFi.begin` on the UI thread froze the display for up to 20 s. Call it off the draw path, then poll `WiFi.status()`. This firmware does that from the main loop with a 20 s timeout (`src/net.cpp`) and only writes credentials to NVS after `WL_CONNECTED`.
- `WiFi.scanNetworks(true)` is async. `WIFI_SCAN_RUNNING` is -1, `WIFI_SCAN_FAILED` is -2. One consumer at a time. A second `scanComplete` / `scanDelete` wipes the first consumer's results. This app scans at most every 15 s.
- Do not `hostedDeinit` while Wi-Fi or BLE is still up. They share hosted init. The heart-rate task guards Wi-Fi teardown with a "BLE is active" flag.
- `BLEDevice::init` and `BLEClient::connect` block. Run them on their own task. Heart Rate Service `0x180D`, measurement `0x2A37`, worked against a chest strap and a Garmin watch broadcasting HR. A scan on this C6 found 25 devices. ANT+ is not available.
- This Arduino build has TLS 1.2 only: `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3` is unset in the prebuilt ESP32-P4 libraries, while `CONFIG_MBEDTLS_SSL_PROTO_TLS1_2` and the full CA bundle are on. The chip and current ESP-IDF can do TLS 1.3, but this firmware cannot until those libraries are rebuilt with the option enabled. A host that offers only TLS 1.3 has to be fetched as plain HTTP, or not at all. Spotify and SoundCloud still accept TLS 1.2. Certificate checks were turned off for one HTTPS tile server that did work. A TLS-plus-decode task needed about 12 KB of stack here; the silent failure that looked like a stack overflow was a logic bug (each request issued twice). Measure high-water before growing the stack. Do not drop a TLS task to the default 2–4 KB without measuring.

## YouTube

Browse has a YouTube switch next to Spotify. Search uses the same keyboard. A row has a title, channel, length, and one play button. There is no plus and no drill-down. More asks YouTube for the next page. The address is resolved when you tap play, not when the list arrives, because those addresses expire.

This is not a Sonos music service and there is no sign-in. The Tab5 posts to YouTube's own InnerTube player endpoint, picks a plain (no cipher) address, and then relays the audio itself. The speaker is given `http://<tab5>:8080/yt/N` and a second Tab5 task fetches the real `googlevideo.com` file in 256 KB closed ranges into a 6 MB PSRAM ring, up to 4 MB ahead of the speaker, and the relay serves the speaker from that ring. The buffer keeps audio going through a slow Wi-Fi moment or a search running at the same time. A direct address does not work: YouTube answers 403 to a fetch without a closed `Range`, and the Sonos never sends one, so the speaker stopped within seconds. The relay also answers the speaker's own `Range` requests (it reconnects every few seconds), and each new pick gets a new `N` that cuts off the previous stream.

What was measured on 9 Nov 2026: itag 18 (combined 360p MP4) plays on the speaker through the relay. AAC itag 140 is DASH fragmented MP4 and the speaker drops it. The `ANDROID_VR` client gives itag 18 addresses that return 403 even with a range, so the firmware moves on to the `ANDROID` client, which works. HLS manifests are not used. Some videos come back `LOGIN_REQUIRED` ("Sign in to confirm you're not a bot") and will not play. Age-gated, region-locked, and live videos may not resolve. The relay needs the Tab5 to stay on and on the same network as the speaker. YouTube can change this without notice.

Long videos: handing the speaker the MP4 itself stopped playback after about 12 minutes, every time, at the same place. The speaker loads the audio sample table in 32 KB pieces. At sample 32768 (about 760 s at 44.1 kHz) it closed the stream, re-read the table piece at that sample, and never reconnected. Serving that piece instantly (a copy of the file start kept in PSRAM) and answering its extra connections in separate tasks changed nothing, so the fault is in the speaker's MP4 handling. The relay now does not send the MP4. It keeps the first 6 MB of the file (the `moov` index is about 1.6 MB for an hour), reads the audio track's `stsc`, `stco`, `stsz` and the AudioSpecificConfig from `esds`, then walks the interleaved `mdat` as it comes out of the ring and sends each audio sample behind a 7-byte ADTS header. The speaker is told `x-rincon-mp3radio://<tab5>:8080/yt/N` with a `audio/aac` stream and no length. A reconnect continues at the next unsent sample. Verified past 14 minutes on a one-hour video. A file whose `moov` sits after `mdat`, or an audio type other than AAC-LC, is not handled and the relay answers 502. The seek test command no longer applies, because a radio stream cannot seek. When it does, the fix is a serial command, not a reflash.

Starred videos: every search row has a star, and the playing screen shows a star beside the title while YouTube plays. The **Starred** chip next to YouTube lists them, newest first, with thumbnails. Tap a row to play it, or its star to remove it. Up to 24 are kept in NVS (namespace `ytstar`) and survive a reboot. Thumbnails (`i.ytimg.com` `mqdefault.jpg`, over plain http) are downloaded on the network task when it is idle and decoded to 160x90 when the list is open.

Serial, on the USB CDC port opened with DTR and RTS low:

- `Y` plus a video id resolves and plays it on the selected room.
- `Yq` plus words runs the same search the screen uses.
- `Ys` seeks the speaker to five minutes. It no longer works, because the speaker gets a radio-style stream.
- `Yk` stars or unstars the video that is playing.
- `Yc name|version|ua` stores one override client in NVS (namespace `yt`). It is tried first. Example: `Yc ANDROID_VR|1.65.10|com.google.android.apps.youtube.vr.oculus/1.65.10 (Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip`

The player response uses its own 512 KB PSRAM buffer. It does not share the Spotify one. Playback is checked about six seconds after the speaker starts. If it is not playing, the screen says the speaker refused the stream.

## GPS on Grove Port A

Port A HY2.0-4P: black GND, red 5 V, yellow GPIO53, white GPIO54. The M5 GPS v1.1 (AT6668 / ATGM336H) is 115200 baud and 10 Hz capable. The older v1.0 (AT6558) is 9600. Grove cables are not consistent about which pin is TX.

Sequence: `setExtOutput(true)`, `Ex_I2C.release()`, then a UART on `Serial1` with a 4096-byte RX buffer. Scan baud 115200, 9600, 38400, 57600, and both pin orders `(RX 54, TX 53)` then `(RX 53, TX 54)`. Three good NMEA checksums within 1.5 s count as found. Then try to move the receiver to 115200 and 10 Hz (5 Hz if it stays at 9600). Send CASIC (`PCAS01` / `PCAS02` / `PCAS03`), u-blox, and MediaTek (`PMTK251` / `PMTK220`) so an AT6668, a u-blox, or an MTK unit all take the rate. Ask for RMC and GGA only. If the port is silent for 5 s, scan again.

TinyGPSPlus 1.1.0 does not commit RMC speed or location unless the sentence status is `A`. GGA still updates satellite count and HDOP with no fix. Requiring `speed.isUpdated()` in the same pass as a GGA makes a locked receiver look dead, and a later GGA clears a good fix. Hold the last speed across a sentence that does not carry one. The working version of that logic is `onSentence` in `src/gps_nav.cpp`.

The module's fix LED blinks only with a lock. Indoors: 0 satellites, HDOP 25.5, NMEA still flowing. That is a sky view, not a dead UART.

## Camera

Front SC2356 (2 MP, 1600×1200) on MIPI-CSI. Init that worked, in `src/camera.cpp`:

- Take the existing internal bus: `i2c_master_get_bus_handle(M5.In_I2C.getPort())`. The camera must not create its own I2C bus. SCCB is GPIO31/GPIO32, plus MCLK GPIO36.
- `csi.begin(cam, true)`. The second argument skips powering the MIPI PHY LDO. The display already set channel 3 to 2.5 V. Powering it again was avoided on purpose.
- `setFormat(ESP_VIDEO_FORMAT_RGB565)`, three capture buffers.
- Scale, rotate, and mirror with the same PPA SRM operation as the display blit. The sensor does not match the rotated landscape display, so the preview needs a 90°-class rotation. Mirror is a separate flag. This app mirrors by default and XORs that with the display flip so the preview stays upright.
- Capture task: 8 KB stack, core 1, priority 3, below the IMU task. Stream only while the preview is on screen. Stop the sensor rather than tearing down CSI. Frames are drawn from the UI task.
- About 19–23 fps after the PPA scale. The stock full-frame software path was 1–2 fps.

Green picture, then a camera that will not restart:

The prebuilt AWB tuning asks the ISP colour-correction matrix for gains outside ±4 (blue around 6.3 indoors). The hardware rejects the whole update. White balance never applies, the Bayer image stays green, and `startCapture` fails with a CCM error, so a later start looks frozen. Muting log tags `ISP_CCM`, `isp_video`, `esp_video`, and `ISP` hid this. Leave them on while bringing the camera up.

Fix: `--wrap=esp_isp_ccm_configure`, set the config's `saturation` flag, and scale any matrix row whose peak is above 3.99 down to that limit. Clamping one coefficient without the flag left the picture green. The wrap in `src/camera.cpp` is the one that worked. A pink tint was the same failure in different light.

A failed `startCapture` leaves buffers queued, and every later start fails too. Retry with `capture.requestBuffer(3)` and then `startCapture()`, a few times, with a short delay. This firmware tries three times.

## Power

USB-C is the device port: power, programming, and the CDC console. USB-A is a host port whose VBUS stays off unless expander `0x44` P3 is turned on. This firmware never turns it on.

There is no light sleep, deep sleep, or power-off call. The chip runs until it is unplugged or reset.

The pack is 2S (an NP-F550 in the rear bay on this unit). `M5.Power.getBatteryVoltage()` is `Ina226.getBusVoltage() * 1000` millivolts, and it has to be called under the I2C lock. Charge current reads negative on the shunt; the library negates it.

Do not use `getBatteryLevel()` as the on-screen percentage. With no pack, on USB, the sense node collapses near 0 V and also sits on the charger rail, so a naive 0–100 clamp flaps between empty and full. `BAT 0%` for a moment after boot was a settle glitch, not an empty pack.

The heuristic that settled, polled every 5 s:

- Below 5.5 V: USB, no pack. Show that, not 0%.
- From 6.0 V to 8.8 V: a real 2S pack. Trust it after 2 samples.
- A missing pack also floats on the 8.4 V charger rail between collapses. A reading at or above 8.1 V has to persist for 6 samples (about 30 s) before it is treated as a pack.
- Percentage is per cell, `packMv / 2`, empty at 3.30 V, full at 4.15 V.

The RTC is the RX8130CE, kept by the on-board supercap. Its interrupt does not land on a P4 GPIO directly; a small PMS150G sits in between. Set the clock from NTP when Wi-Fi is up, or from a GPS time sentence. A year of 2024 or later means the RTC was already set. Store UTC in logs if you also show a local zone.

## Tasks and memory

Internal RAM is 512 KB. A late rowing build used about 17% of it and about 35% of the 6.25 MB app slot. PSRAM is the place for sprites, tracks, and map geometry.

What this firmware pinned, and why:

| Task | Core | Priority | Stack | Why |
| --- | --- | --- | --- | --- |
| IMU | 0 | `configMAX_PRIORITIES - 2` | (see `imu_stroke.cpp`) | 100 Hz. A few overruns when the camera starts were accepted. Missing the 10 ms slot is counted, not retried into a backlog |
| GPS | 0 | 3 | | UART parse off the UI |
| Logger | 0 | 2 | 6 KB | SD writes off the UI |
| Map render | 0 | 1 | | Drawing the map on the UI loop froze it for 170–380 ms. After the move, the UI gap max was 23 ms and the IMU stayed at 100 Hz |
| Camera | 1 | 3 | 8 KB | Capture only. The UI task is the one that writes the framebuffer |
| BLE heart rate | unpinned | 1 | 6 KB | `connect` blocks |

Keep framebuffer writes on one task. Move heavy work off the UI task, onto a core that still leaves the IMU its slots. The UI frame period here is 50 ms. The main loop ends in `vTaskDelay(10)`.

## What to copy, and what to leave

Copy into a new Tab5 project, then delete the rowing-specific calls:

- `src/board.h` and `src/board.cpp` — the recursive I2C mutex and the four `i2c_master_*` wraps.
- `src/display_fb.h` and `src/display_fb.cpp` — PPA blit and the cache sync before `readRect`.
- The `__wrap_esp_isp_ccm_configure` function at the bottom of `src/camera.cpp`, plus the `csi.begin(cam, true)` and `requestBuffer(3)` retry, if the new app uses the camera.
- `tools/tab5serial.py` — open the port without resetting the chip.
- The SD `begin` sequence in `src/logger.cpp` (`setPowerChannel(-1)`, pins, 20 MHz, FAT32 format guard).

Leave the rowing screens, metrics, sessions, maps, and the `case/` CAD files. The serial command letters (`N`, `M`, `G`, and the rest) are that product's remote control. Keep the screenshot, upload, and download framing if you want the same host tools.

## Failure modes, so they are not rediscovered

| What you see | What it was |
| --- | --- |
| UI stutter only while USB is plugged in and no program has the port | Missing `Serial.setTxTimeoutMs(0)` |
| Black screen after a flash, or `HP_SYS_HP_WDT_RESET` once | Overlapping upload or a bad reset. One clean flash. Not application code |
| "Core dump data check failed" | No coredump partition. Ignore |
| COM port access denied | Another esptool or script still has it |
| esptool dies after a good compile, `UnicodeEncodeError` | Set `PYTHONUTF8=1` |
| SD timeout, no OCR, card is inserted | Card upside down (contacts must face the screen), or `setPowerChannel` left at the dev-board default |
| SD error 3 | SPI mode. Use SDMMC 4-bit |
| Card mounts tiny on a large card | Leftover partition. `f_mkfs("0:")` as FAT32, then restart |
| Green camera, then `startCapture` never succeeds | ISP CCM gains outside ±4. The saturation wrap above. Do not mute `ISP_CCM` |
| Camera init fails the second time | Buffer queue left dirty. `requestBuffer(3)` before retrying |
| Touch dead, picture shifted | ST7123 porch timings changed, or LCD reset left driven high (I2C then dies too) |
| Landscape blit at about 2 fps | `pushSprite` from PSRAM. Use the PPA path |
| Screenshot looks byte-swapped | `readRect` is big-endian RGB565. Also call `fb::syncForRead()` first |
| GPS NMEA flows, fix never sticks | TinyGPSPlus drops RMC speed unless status is `A`, and a GGA clears it if you require `speed.isUpdated()` |
| GPS silent | 5 V not on, Port A still in I2C mode, or the other pin order. Scan both |
| Wi-Fi associates and the UI is frozen | `WiFi.begin` was called on the draw path |
| `BAT` flaps 0% and 100% on USB with no pack | Sense node collapses and also sits on the 8.4 V charger rail. Use the 5.5 V / 8.1 V heuristic |
| `nvs_open` error at boot | `Preferences::begin` was read-only |
| Unaligned load exception while parsing a file | Cast of packed bytes to `int32_t*`. `memcpy` |
| Allocation of a framebuffer fails, heap still looks fine | It was aimed at internal RAM. Use PSRAM |
