# M5Tab Sonos Controller

A touch-screen Sonos remote for the [M5Stack Tab5](https://docs.m5stack.com/en/core/Tab5). Hang it on the wall, tap a room, and play music. It also browses Spotify and plays YouTube on your speakers, and it needs no phone, no PC and no cloud service of its own once it is set up.

![Tab5](https://img.shields.io/badge/board-M5Stack%20Tab5-blue) ![ESP32-P4](https://img.shields.io/badge/chip-ESP32--P4-orange) ![License MIT](https://img.shields.io/badge/license-MIT-green)

> **Status:** a hobby project that works well on the author's unit. It has been tested on one Tab5 and one Sonos system. Expect rough edges, and please open an issue if you find one.

<p align="center">
  <img src="docs/images/now-playing-youtube.png" alt="Now playing a YouTube video on a Sonos room" width="720">
</p>

## What it does

- **Sonos control.** Finds your speakers on the local network, shows every room, and controls the one you pick: play, pause, next, previous, volume, now playing with album art, the room's queue, and your Sonos favorites.
- **Rooms.** Tapping a room only selects it. Grouping is explicit: a **+** adds a room to the playing group and a **-** takes it out.
- **Spotify.** Search Spotify with an on-screen keyboard, open songs, albums, playlists and artists before playing (an artist gives its albums and radio), play now or add to the Sonos queue, and browse your own playlists. Playback runs through your Sonos system, not through the Tab5.
- **YouTube.** Search YouTube and play a video's audio on the speaker, with no helper PC. The now-playing screen shows YouTube, the title, the channel and the thumbnail. Star videos and pick them later from a **Starred** list with thumbnails. See [YouTube, and why it is fragile](#youtube-and-why-it-is-fragile).
- **Setup without a computer.** On first start the Tab5 opens its own Wi-Fi network and a small web page where you choose your home Wi-Fi.

## What you need

- An **M5Stack Tab5** (ESP32-P4, 1280x720 display). This project was proven on the earlier hardware revision (chip revision v1.3). The install page has a second button for a newer unit (chip revision 300 and above). That build has not been run on hardware yet. See [docs/tab5-control.md](docs/tab5-control.md).
- A **Sonos** system on the same network. The firmware talks to the speakers' local control port, so the Tab5 must be on the same subnet. It was tested against a household of seven rooms running current Sonos software.
- A **2.4 GHz Wi-Fi** network. The Tab5's radio does not use 5 GHz.
- A computer with a USB-C cable, once, to flash it.
- Optional, for Spotify: a Spotify account that is already linked in the Sonos app, and a free Spotify developer app (see below).

## Install

### Flash from your browser

Chrome or Edge on a computer, and a USB-C cable that carries data.

1. Open the [install page](https://fusiscaesar.github.io/M5TabSonosController/).
2. Plug the Tab5 into the computer.
3. Press **Tab5 (original, tested)** for the earlier hardware, or **Tab5 newer revision (experimental)** for a unit with chip revision 300 or above. Choose the port when the browser asks.
4. If no port appears, hold the boot button while resetting the Tab5, then try again.
5. If the screen stays black, flash the other button. A wrong pick can be flashed over.

The page asks whether to erase the tablet. Erase on a first install. Leave the data in place when you are updating and want to keep the saved Wi-Fi network and the Spotify sign-in.

The same two images are in [M5Burner](https://docs.m5stack.com/en/download). Install the app, then search for **M5Tab Sonos Controller**. Pick the entry that matches the hardware revision.

### Build it yourself

1. Install [PlatformIO](https://platformio.org/install) (the CLI or the VS Code extension) and Python 3.
2. Clone this repository.
3. Plug the Tab5 into your computer over USB-C.
4. Build and flash:

   ```powershell
   python -m platformio run -e tab5 -t upload
   ```

   If more than one serial port is present, add `--upload-port COMx` (Windows) or `--upload-port /dev/ttyACMx` (Linux/macOS).

   For a newer Tab5 (chip revision 300 and above) use `-e tab5x` instead of `-e tab5`. That build has not been run on hardware yet.

**Windows notes.** Set `PYTHONUTF8=1` first, or the upload fails after a successful build:

```powershell
$env:PYTHONUTF8 = 1
python -m platformio run -e tab5 -t upload
```

`platformio.ini` sets `core_dir = C:/pio` so the Arduino toolchain does not hit Windows' 260-character path limit. If you have long paths enabled, or you are on Linux or macOS, delete that line. The first build downloads a large toolchain (around 2 GB).

If the upload cannot find the board, hold the boot button while resetting it. More build and flashing details are in [docs/tab5-control.md](docs/tab5-control.md).

## First start

1. The screen asks you to join a Wi-Fi network named like `TabSonos...`. Join it with your phone.
2. Open `http://192.168.4.1` (most phones open it for you), choose your home network and type the password.
3. The Tab5 joins your network, the setup network closes, and the room list appears. The Tab5 remembers the network.

You can also set the network over USB serial with `W ssid|password`. See [Serial commands](#serial-commands).

## Using it

- **Left side:** the rooms. Tap a room to control it. Use **+** and **-** to group or ungroup rooms.
- **Middle:** what is playing, with album art, title, artist, the source (Spotify, Radio, YouTube and so on) and the clock.
- **Bottom:** previous, play/pause, next, and a volume slider.
- **Top right:** **Browse** (Spotify and YouTube), **Queue** (the selected room's queue) and **Favorites** (your Sonos favorites).

### Spotify

Tap **Browse**, then **Spotify**. Tap the search field to open the keyboard, type, and press **Search**.

- The triangle plays the item now. The plus adds it to the Sonos queue.
- Tapping a song, album, playlist or artist opens it, so you can see what is inside before you play.
- **My playlists** lists your own playlists.

| Search | Open an item |
| --- | --- |
| ![Spotify search results](docs/images/spotify-search.png) | ![Spotify detail page](docs/images/spotify-detail.png) |

### YouTube

Tap **Browse**, then **YouTube**, search, and tap a row to play it. The star on a row, or the star next to the title on the playing screen, saves the video. The **Starred** chip lists your saved videos with thumbnails. Tap a row to play it, or its star to remove it. Up to 24 videos are kept, and they survive a reboot.

| Search | Starred |
| --- | --- |
| ![YouTube search results](docs/images/youtube-search.png) | ![Starred YouTube videos with thumbnails](docs/images/youtube-starred.png) |

## Spotify setup

Spotify needs a one-time sign-in so the Tab5 can browse your account. Nothing is stored in this repository. Your sign-in lives only in the Tab5's own flash memory.

1. In the Sonos app, add Spotify as a music service and sign in.
2. Create a free app at the [Spotify developer dashboard](https://developer.spotify.com/dashboard), while signed in to the Spotify account you use. Add this redirect URI to the app: `http://127.0.0.1:8888/callback`.
3. With the Tab5 connected over USB, run:

   ```powershell
   pip install pyserial
   python tools/spotify_auth.py --client-id YOUR_CLIENT_ID --port COM12
   ```

   Your browser opens, you approve, and the script sends the sign-in to the Tab5 over USB. Leave `--client-secret` out unless the dashboard shows one.

To sign out later, send `X` over serial.

Spotify app rules apply. An app in development mode is limited to a small list of approved users, so add your own account there.

## YouTube, and why it is fragile

YouTube has no official way to play its audio on a Sonos speaker, and the speaker cannot play a YouTube address by itself. This project does it like this:

1. When you tap a result, the Tab5 asks YouTube's internal player endpoint (InnerTube) for the video's address, pretending to be a mobile YouTube app.
2. YouTube refuses to serve the speaker directly, because Sonos does not send the byte-range requests YouTube requires. So the Tab5 acts as a small web server on port 8080. The speaker is told to play `http://<tab5>:8080/yt/...`, and the Tab5 downloads the video's MP4 file from YouTube into a 6 MB buffer in its memory. It then pulls the audio track out of that file and sends it to the speaker as a plain AAC stream, the way an internet radio station would. (Handing over the MP4 itself works for about twelve minutes. The speaker then runs out of its sample table and stops, so a long video would cut off.)
3. The Tab5 must stay powered and on the network while YouTube plays.

What that means in practice:

- It uses an **undocumented** interface. YouTube can change or block it at any time, and it may stop working without warning. The client names and versions it presents are in a table in `src/youtube.cpp`, and you can try a different one at runtime with the `Yc` serial command, without reflashing.
- Some videos will not play, for example age-restricted or region-locked ones, and videos where YouTube asks the client to prove it is not a bot. The status line says "No audio allowed for this video".
- Live streams have not worked in testing.
- The Tab5 downloads the combined 360p MP4, because the audio-only format dropped out on the speaker in testing. It only passes the audio on, but the whole file is read, so a long video uses its video bytes too.
- It is played as a radio-style stream, so the speaker shows no length and cannot seek.
- Using it may go against YouTube's Terms of Service. It is meant for personal use at home. You are responsible for how you use it.

## Serial commands

The Tab5 listens on its USB serial port (115200 baud). Open it **without** toggling DTR/RTS, because the ESP32-P4's built-in USB resets the chip when they change. `tools/tab5serial.py` has an `open_port` helper that does this.

| Command | What it does |
| --- | --- |
| `W ssid\|password` | Join a Wi-Fi network. `W` alone joins the stored one. |
| `F` | Forget the stored Wi-Fi network. |
| `T` | Test the TLS connection to Spotify. |
| `S spotify\|client_id\|refresh_token\|secret` | Store the Spotify sign-in (used by `tools/spotify_auth.py`). |
| `X` | Sign out of Spotify. |
| `Y <videoId>` | Resolve and play a YouTube video. |
| `Yq <words>` | Search YouTube. |
| `Yk` | Star or unstar the playing YouTube video. |
| `Yc name\|version\|ua` | Use a different YouTube client for the player request. |
| `Ys` | Seek the speaker to five minutes (a test command, no effect on the radio-style stream). |
| `P` | Send a screenshot of the screen over serial (used by `tools/screenshot.py`). |
| `U x y` | Inject a tap at screen position x, y (1280x720). |

## How it works

| Part | Where | Notes |
| --- | --- | --- |
| Display and touch | `src/ui.cpp`, `src/display_fb.cpp`, `src/font_ui.h` | M5GFX. The screen is one 1280x720 off-screen canvas pushed to the panel. |
| Wi-Fi and setup page | `src/net.cpp` | Station mode, plus a captive-portal access point for setup. |
| Sonos | `src/sonos.cpp` | SSDP discovery, then UPnP/SOAP on port 1400: transport, rendering, queue, favorites, zone topology. |
| Spotify | `src/services.cpp` | Web API over HTTPS. Items are handed to Sonos as queue entries. |
| YouTube | `src/youtube.cpp` | InnerTube search and player calls, a relay server, a read-ahead buffer, and the starred list in NVS. |
| Board setup | `src/board.cpp` | A guard for the shared I2C bus. |
| Tools | `tools/` | `spotify_auth.py` (one-time Spotify sign-in), `tab5serial.py` (serial helper), `screenshot.py` (takes a screenshot of the Tab5 over USB, with optional taps first; needs Pillow), `make_font.py` (rebuilds `font_ui.h`). |

Network work runs on its own task, away from the drawing code, so the screen stays responsive.

The hardware notes in [docs/tab5-control.md](docs/tab5-control.md) were written while bringing the board up. They cover the display, touch, the shared I2C bus, USB serial, memory, power and a list of failure modes. They are useful if you are starting your own Tab5 project.

## Troubleshooting

- **Black screen or no serial output after flashing.** Run one clean upload with nothing else holding the port. Stop any other PlatformIO or serial program first.
- **"Access denied" or "port busy" when uploading.** Another program still has the serial port open.
- **No rooms appear.** The Tab5 and the speakers must be on the same network and subnet. A guest network or a client-isolation setting hides the speakers.
- **Wi-Fi will not connect.** Use a 2.4 GHz network. Send `F` over serial to forget the network and start the setup page again.
- **YouTube says "No audio allowed" for everything.** YouTube may have changed something. Check the serial log, and see the `Yc` command.

## Contributing

Issues and pull requests are welcome. If you add support for another Tab5 hardware revision, or for another service, a note in the pull request about what you tested on is very helpful.

## Credits

Built with [M5Unified](https://github.com/M5Stack/M5Unified), [M5GFX](https://github.com/M5Stack/M5GFX), [ArduinoJson](https://arduinojson.org/) and the [pioarduino](https://github.com/pioarduino/platform-espressif32) platform.

## Disclaimer

This is an independent project. It is not affiliated with or endorsed by Sonos, Spotify, Google or M5Stack. Sonos, Spotify, YouTube and M5Stack are trademarks of their owners. YouTube playback relies on an unofficial interface and may break or be restricted at any time.

## License

[MIT](LICENSE). You may use, copy, modify and distribute this project for any purpose, including commercial use, as long as the copyright and license notice stay with it.
