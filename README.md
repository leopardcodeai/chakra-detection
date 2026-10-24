# Chakra Detection

**Strike a singing bowl. The room takes its colour.**

An open-source app for Espressif's [ESP-Mosaico](https://mosaico.espressif.com) (ESP32-S31, 2.16" 480 x 480
touch screen, microphone). It hears a singing bowl, a tuning fork or a hummed tone, shows the chakra with its
symbol, note and frequency, and colours the room: the screen itself turns the chakra's colour, and your lamps
follow (Philips Hue, Home Assistant, WiZ). Made for sound healing journeys: put it in the middle of the room
and play.

<p align="center"><img src="docs/demo.gif" width="360" alt="The app hearing seven bowls, root to crown: the screen turns red, orange, yellow, green, blue, indigo and violet"></p>

| Ready | A bowl sounds | Crown | Settings |
|---|---|---|---|
| ![Ready](docs/screen_ready.png) | ![Heart, F3 at 174.4 Hz](docs/screen_heart.png) | ![Crown, B3](docs/screen_crown.png) | ![Lamps, brightness, sensitivity, Flow or Strike, mapping, language](docs/screen_settings.png) |

## What it does

- **Hears the tone, not the noise.** A colour changes only for a tone that is loud enough, carries most of its
  energy on its harmonics and holds its pitch. Drums, rattles, rain sticks and wind chimes in the wind mostly
  leave the colour alone.
- **Shows it like a tuner.** Chakra name, Sanskrit name, note, frequency to a tenth of a hertz and cents off
  the note; a ring around the symbol fills the closer the tone sits to its note.
- **Colours the room.** The whole screen fades to the chakra's colour, so it works without any lamp. Tick
  the lamps that should follow; lamps that are not paired yet are shown greyed.
- **Flow or Strike.** Flow (default) waits 0.4 s for a steady tone and fades softly, made for a journey.
  Strike reacts in 0.13 s to every strike.
- **Three ways to map a tone to a chakra.** Notes (C root to B crown, A = 440 Hz or 432 Hz), Solfeggio
  (396 to 963 Hz), or **your own bowls**: strike each of your seven bowls once and the app learns them.
- **English and German**, switched in the settings. Hold the top bar for 3 s to close the app.

![From a singing bowl to the colour of the room](docs/how-it-works.png)

The symbols are the project's own artwork, drawn from geometry (petal count and inner figure of each
chakra): ![The seven chakra symbols](docs/symbols.png)

## Tested against real recordings

`tests/analyse.sh FILE` runs the same detection the board runs over any audio file and prints when which
chakra was shown. Results on 24 October 2026, Flow, notes at A = 440 Hz:

| Recording | Result |
|---|---|
| 9 single singing bowls, tuning forks, a Japanese rin | each locks one chakra and stays (77 to 99 % of the time) |
| a 7-bowl chakra tune-up video, root to crown | 6 of 7 by note; its "root" bowl rings at G#3 (210 Hz), so learn your bowls |
| handpan, kalimba, koshi chimes (melodies) | Flow: 3 to 10 colour changes per piece; Strike follows every note (up to 116) |
| frame drum, drum cadence, egg shaker, rain sticks, wind chimes | mostly no colour (73 to 100 % silence) |
| gongs | a clear gong locks; a complex gong wash still wanders between chakras |

From the strike to the colour on the screen: 0.38 s in Flow, 0.13 s in Strike. A lamp adds its own path:
Hue on the local network is fast, a Wi-Fi bulb through a cloud integration can add a second or two.

The recordings came from Wikimedia Commons (free licences) and were used for testing only; none ships with
this repository.

## Try it without the board

You need macOS or Linux, Python 3 and ffmpeg. The app runs in Espressif's ESP-GSP simulator, fed by your
computer's microphone.

```sh
git clone https://github.com/esp-mosaico/esp-mosaico-vibe workspace   # Espressif's workspace (once)
git clone https://github.com/leopardcodeai/chakra-detection apps/chakra
cd apps/chakra
./sim.sh --mic                  # browser preview at http://127.0.0.1:3222, listening to your microphone
python3 tools/chakra_feed.py demo        # or: seven bowl tones, root to crown
python3 tools/chakra_feed.py tone 174.6  # one tone
tests/run.sh                    # the detection's host tests
tests/analyse.sh my_bowls.wav   # any recording
```

`sim.sh` expects this folder next to the workspace (`../../workspace`). With `CHAKRA_LIGHT=light.x`,
`HA_URL` and `HA_TOKEN` in the environment the simulator also colours that Home Assistant lamp.

## On the board

1. Install the app: `python3 mosaico.py iris system-update --project ../apps/chakra` from the workspace,
   then "Save Current Build" in Vibe Mode, the Mosaico's app launcher.
2. Lamps and extra Wi-Fi networks: copy `tools/provision.example.json`, fill in your lamps, put the secrets
   into the environment (`CHAKRA_HA_TOKEN`, `CHAKRA_HUE_KEY`, `CHAKRA_WIFI_PASS_1`), run
   `tools/provision.py --port <port> --dry-run`, then without `--dry-run`.
3. Philips Hue: press the bridge's link button, then `tools/hue_pair.py --host <bridge address>`. It stores
   the key and lists the lights with their numbers.

Pairing happens outside the app: Hue with the link button, Bluetooth lamps in Home Assistant (they then
appear as Home Assistant lights), WiZ bulbs by their address.

## How it is built

| Part | File |
|---|---|
| Detection: FFT, harmonics, steadiness, mappings, hysteresis, learned bowls | `components/app_chakra/chakra_detect.c` |
| Screen: colour fades, symbols, tuner ring, settings, learning, hold to close | `components/app_chakra/chakra_core.c`, scene `ui/make_scene.py` |
| Board: microphone, Wi-Fi, Home Assistant, Hue, WiZ, settings in NVS | `main/main.c` |
| Simulator side and feeder | `components/app_chakra/host/platform_pc.c`, `tools/chakra_feed.py` |
| Tests | `tests/test_detect.c`, `tests/analyse_wav.c` |

The detection is plain C with no dependencies, so the board, the simulator and the tests run the same code.
This app replaces four generations of an ESPHome firmware for the ESP32-S3-BOX-3 (2025 to 2026): their
512-point FFT gave 31 Hz bins, two semitones at 250 Hz, and nothing said how sure a reading was.

Status: runs in the simulator and builds for the ESP32-S31; the board is on order, so it is untested on
hardware.

## Licence

Apache-2.0. Made by [LeopardCode.AI](https://leopardcode.ai). Chakra colours, names and the C-to-B note
assignment follow common sound healing practice; this is a light and sound tool, not a medical device.
