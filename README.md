# ChakraA: Chakra Detection

<img src="docs/icon.svg" width="72" align="right" alt="ChakraA icon: a singing bowl with sound waves">

**Strike a singing bowl. The room takes its colour.**

An open-source app for Espressif's [ESP-Mosaico](https://mosaico.espressif.com) (ESP32-S31, 2.16" 480 x 480
touch screen, microphone). It hears a singing bowl, a tuning fork or a hummed tone, shows the chakra with its
symbol, note and frequency, and colours the room: the screen itself turns the chakra's colour, and your lamps
follow (Philips Hue, Home Assistant, WiZ, or any TV or laptop browser as a screen lamp). Made for sound healing journeys: put it in the middle of the room
and play.

**Which one?** There are two apps, from the same code. In yoga and sound healing many people want no AI
at all, or only AI that stays in the room: ChakraA is built for that. It works without any AI, and
its one AI feature runs locally, on the device, with no camera and no cloud. ChakraV is the experimental
sibling that tries what a camera and bigger models can add. (ChakraA: the audio one. ChakraV: with video.)

| | [ChakraA](https://github.com/leopardcodeai/chakraa) (this one) | [ChakraV](https://github.com/leopardcodeai/chakrav) |
|---|---|---|
| Listens | microphone | microphone and camera |
| AI | none needed; an optional local AI sound gate, on the device only | as much on the device as the ESP32-S31 runs, plus optional cloud models with your own API key |
| For | rooms where a camera is not wanted: the classic, private choice | experiments: what can it see and mark? |

<p align="center"><img src="docs/demo.gif" width="360" alt="The app hearing seven bowls, root to crown: the screen turns red, orange, yellow, green, blue, indigo and violet"></p>

| Ready | A bowl sounds | Crown | Settings | Connections |
|---|---|---|---|---|
| ![Ready](docs/screen_ready.png) | ![Heart, F3 at 174.4 Hz](docs/screen_heart.png) | ![Crown, B3](docs/screen_crown.png) | ![Colour Auto or fixed, brightness, sensitivity, Flow or Strike, mapping, language](docs/screen_settings.png) | ![Lamps: the screen lamp ticked with its address, Home Assistant, Hue, Wi-Fi](docs/screen_connections.png) |

## What it does

- **Hears the tone, not the noise.** A colour changes only for a tone that is loud enough, carries most of its
  energy on its harmonics and holds its pitch. Drums, rattles, rain sticks and wind chimes in the wind mostly
  leave the colour alone.
- **Shows it like a tuner.** Chakra name, Sanskrit name, note, frequency to a tenth of a hertz and cents off
  the note; a ring around the symbol fills the closer the tone sits to its note. Below, a spectrum with one
  band a semitone over the octave of the tone that locked and three semitones either side (A2 to D4 for
  octave 3, the 4096-point FFT's 3.9 Hz bins resolve a semitone there): that tone's band lights in its
  chakra's colour, and the seven chakra dots sit under the bands of their notes (C root to B crown). The
  orange dot in the top bar says the microphone listens, as on iOS.
- **Colours the room.** The whole screen fades to the chakra's colour, so it works without any lamp. Tick
  the lamps that should follow under **Connections**; lamps that are not paired yet are shown greyed.
- **A screen as the lamp.** The screen lamp (ticked by default) is a page that fills a whole browser window
  with the colour, the chakra's symbol and names faint on top. Its address stands large under Connections:
  type it into a TV or laptop browser and go fullscreen. On the board the device serves it itself, at
  `http://<its address>/lamp`.
- **Your colour, if you like.** Under **Settings**, Colour: Auto (default) follows the chakra; a tap on one
  of the seven colours, or the Colour slider for any hue, gives a steady light that the tones do not change.
- **Local AI sound gate (optional, off by default).** Under Settings, "Local AI sound gate" On: only bowl-like sounds change the colour,
  fades get softer, and a gong or a drum strike gives the light a short brightness kick instead of a wrong
  colour. On the Mac it runs EfficientAT (mn05) with a small head trained on freely licensed recordings
  (`tools/ai_gate.py`, `chakra_feed.py mic --ai`); on the board it is meant for the ESP32-S31's AI
  instructions through ESP-DL (mn05 at 8-bit weights and 16-bit activations, about 1.9 MB, estimated 30 to
  60 ms per second of audio, to be measured when the board is here). On 91 recordings (25 October 2026,
  table below): gongs coloured the room 57 % of the time they sounded without the gate and 21 % with it,
  voices 46 % and 4 %, while singing bowls kept their colour 83 % of the time (91 % without).
  Nothing leaves the device: no camera, no cloud, no account. Without a gate running the setting changes
  nothing, so the room never stays dark because of it.
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

**With and without the local AI sound gate** (`tools/bench/gate_bench.py`, 91 Commons recordings, 25 October
2026): the share of each recording's sounding time that has a colour. mn05 on 2 s windows, the model meant
for the board; "trained" is a small head on its embedding, tested on recordings it never saw (5-fold by
recording, 84 % of windows right).

| Sound | Recordings | Should colour | No AI | AI, pretrained | AI, trained |
|---|---|---|---|---|---|
| singing bowl | 10 | yes | 91 % | 84 % | 83 % |
| bell | 18 | yes | 42 % | 28 % | 24 % |
| tuning fork | 2 | yes | 93 % | 72 % | 56 % |
| gong | 12 | no | 57 % | 22 % | 21 % |
| voice | 5 | no | 46 % | 9 % | 4 % |
| drum | 20 | no | 14 % | 1 % | 1 % |
| shaker, rain stick | 24 | no | 0 % | 0 % | 0 % |

The gate does what a sound healing room needs from it, gongs, drums and voices mostly stop painting the
room, at a price: bells lose colour too, and bowls lose a little. Across mn04, mn05 and dymn04 with 0.5, 1 and 2 s
windows the picture is the same, the numbers spread: bowls keep 72 to 87 %, gongs keep 12 to 34 %, voices
1 to 20 %; mn04 on 2 s with the trained head drops gongs furthest (12 %) at the same bowl share (84 %).
22 to 26 ms a window on the Mac's CPU. Two tuning forks are too few to read much into.

The recordings came from Wikimedia Commons (free licences) and were used for testing only; none ships with
this repository.

## Try it without the board

You need macOS or Linux, Python 3 and ffmpeg. The app runs in Espressif's ESP-GSP simulator, fed by your
computer's microphone.

```sh
git clone https://github.com/esp-mosaico/esp-mosaico-vibe workspace   # Espressif's workspace (once)
git clone https://github.com/leopardcodeai/chakraa apps/chakra
cd apps/chakra
./sim.sh --mic                  # browser preview at http://127.0.0.1:3222, listening to your microphone
python3 tools/chakra_feed.py demo        # or: seven bowl tones, root to crown
python3 tools/chakra_feed.py tone 174.6  # one tone
tests/run.sh                    # the detection's host tests
tests/analyse.sh my_bowls.wav   # any recording
```

`sim.sh` expects this folder next to the workspace (`../../workspace`). With `CHAKRA_LIGHT=light.x`,
`HA_URL` and `HA_TOKEN` in the environment the simulator also colours that Home Assistant lamp.
`python3 tools/chakra_feed.py mic --lamp http://127.0.0.1:3310` sends every light command to a screen
lamp page instead (the protocol line `light\t<chakra>\t<RRGGBB>\t<bright>\t<transition>`, POSTed to
`/light`, with the `reading` lines: names, note, Hz, cents), so a screen in fullscreen shows the colours
without any real lamp. `CHAKRA_LAMP_URL` sets the address the Connections page shows for it (default
`127.0.0.1:3310`); with `CHAKRA_LIGHT` set, the Home Assistant row is paired too.

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

**Security.** Use a `wss://` Home Assistant address where you can (checked against the certificate
bundle) and a Home Assistant user for this device that may only switch lights; over `ws://` the token
travels unencrypted on your network. The Hue path uses the bridge's local v1 API over plain HTTP with
the key in the request path, so keep the device on a network you trust; the HTTPS v2 API is planned.
Tokens, keys and Wi-Fi passwords never sit in a file of this repository: `provision.py` reads them from
the environment and writes them straight to the board.

## How it is built

| Part | File |
|---|---|
| Detection: FFT, harmonics, steadiness, mappings, hysteresis, learned bowls | `components/app_chakra/chakra_detect.c` |
| Screen: colour fades, symbols, tuner ring, settings, learning, hold to close | `components/app_chakra/chakra_core.c`, scene `ui/make_scene.py` |
| Board: microphone, Wi-Fi, Home Assistant, Hue, WiZ, the screen lamp's page (`/lamp`), settings in NVS | `main/main.c` |
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
