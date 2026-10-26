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
- **Calm light.** Brightness starts at 30 % (60 % before 26 October 2026; a stored setting stays as it
  was). The lamps fade by themselves: one command per colour change, two per kick or gong swell (one up,
  one down), and a Hue bridge or Home Assistant gets at most 8 commands a second (Hue takes about 10).
- **A screen as the lamp.** The screen lamp (ticked by default) is a page that fills a whole browser window
  with the colour, the chakra's symbol and names faint on top. Its address stands large under Connections:
  type it into a TV or laptop browser and go fullscreen. On the board the device serves it itself, at
  `http://<its address>/lamp`.
- **Your colour, if you like.** Under **Settings**, Colour: Auto (default) follows the chakra; a tap on one
  of the seven colours, or the Colour slider for any hue, gives a steady light that the tones do not change.
- **Local AI sound gate (optional, off by default).** Under Settings, "Local AI sound gate" On: only bowl-like sounds change the colour,
  fades get softer, and a strike gives the light a small brightness kick instead of a wrong colour: 8
  points and 8 % towards white, at most one every 2 s, so a drum at 4 beats a second never flashes per
  beat (the change stays under the WCAG flash line for every colour). A gong never kicks: the lamps swell
  12 points over 1.5 s and fall back over 7 s, with the colour kept. On the Mac it runs EfficientAT (mn05)
  with a small head trained on freely licensed recordings (`tools/ai_gate.py`, `chakra_feed.py mic --ai`),
  and a second head that tells gongs from other sounds (`tools/bench/gong_head.py`; it finds 45 % of the
  Commons gongs and 33 % of our own, a gong it misses gets the small kick); on the board it is meant for the ESP32-S31's AI
  instructions through ESP-DL (mn05 at 8-bit weights and 16-bit activations, about 1.9 MB, estimated 30 to
  60 ms per second of audio, to be measured when the board is here). On 79 Commons recordings (26 October 2026,
  table below): gongs coloured the room 65 % of the time they sounded without the gate and 17 % with it,
  voices 46 % and 3 %, while singing bowls kept their colour 84 % of the time (91 % without).
  Nothing leaves the device: no camera, no cloud, no account. Without a gate running the setting changes
  nothing, so the room never stays dark because of it.
- **Flow or Strike.** Flow (default) waits 0.4 s for a steady tone and fades softly, made for a journey.
  Strike reacts in 0.13 s to every strike. In both, a bowl struck while a louder one still rings takes the
  colour: each strike is found on its own (spectral flux), and its new tone is read with the ringing ones
  taken off.
- **Three ways to map a tone to a chakra.** Notes (C root to B crown, A = 440 Hz or 432 Hz), Solfeggio
  (396 to 963 Hz), or **your own bowls**: strike each of your seven bowls once and the app learns them.
- **English and German**, switched in the settings. Hold the top bar for 3 s to close the app.

![From a singing bowl to the colour of the room](docs/how-it-works.png)

The symbols are the project's own artwork, drawn from geometry (petal count and inner figure of each
chakra): ![The seven chakra symbols](docs/symbols.png)

## Tested against real recordings

`tests/analyse.sh FILE` runs the same detection the board runs over any audio file and prints when which
chakra was shown. Results on 26 October 2026, notes at A = 440 Hz, Flow unless Strike is named:

| Recording | Result |
|---|---|
| 7 bowls, C3 to B3, struck one after another with felt about every 1.2 s, the root bowl about three times louder (own recordings: 2 with marked strikes, 4 more rounds) | the marked rounds 6 of 7 and 7 of 7, in Flow and in Strike (the first one's crown never shows); the 4 rounds 6, 6, 6 and 7 of 7 in Flow, 7, 7, 6 and 7 in Strike: 38 and 40 of 42 strikes. Before the strike path the ringing root bowl masked them (Root alone, for up to 50 s) |
| 10 single singing bowls with a Japanese rin, 2 tuning forks | 10 of 12 lock one chakra and stay; the main chakra shows 67 to 99 % of the time (as before) |
| a 7-bowl chakra tune-up video, root to crown, each bowl struck 3 times (-ar9vsmFhJU, its chapters give the bowls) | 17 of 21 strikes in Flow and in Strike; the 3 root strikes fail: its "root" bowl rings at G#3 (208 Hz), so learn your bowls |
| a bowl seller naming each bowl, crown to root (u1KjQTAHSlo, from its captions) | 3 of 7 in Flow, 4 of 7 in Strike: some bowls' upper partials read as another chakra |
| handpan, kalimba, koshi chimes, melodies (24 October) | Flow: 3 to 10 colour changes per piece; Strike follows every note (up to 116) |
| 44 drums, shakers and rain sticks | 40 of 44 mostly without colour (at most 27 % of the time), as before |
| 12 gongs | a clear gong locks; a complex gong wash still wanders between chakras. Flow colours 51 % of the time (49 % before), 5.0 colour changes a recording (4.8); Strike is calmer, 18 changes (22) |

From the strike to the colour on the screen, median over the strikes whose colour changed (8 recordings,
own rounds and tune-ups): 0.29 s in Flow (n = 45, 95 % interval 0.28 to 0.32 s), 0.03 s in Strike (n = 47,
0.02 to 0.09 s), good to about 0.1 s (the detector prints tenths of a second; the reference is the flux peak
of the strike). A lamp adds its own path: Hue on the local network is fast, a Wi-Fi bulb through a cloud
integration can add a second or two.

**With and without the local AI sound gate** (`tools/bench/gate_bench.py` in the evaluation pipeline, 79
Commons recordings, 26 October 2026): the share of each recording's sounding time that has a colour, mean
with its 95 % bootstrap interval over recordings. mn05 on 2 s windows, the model meant for the board;
"trained" is a small head on its embedding, tested on recordings it never saw (5-fold by recording, 85 % of
windows right). 12 of the 91 clips of 25 October were no instruments but someone saying the word (Lingua
Libre and Wiktionary pronunciations of "shaker" and "gong"); they are left out now.

| Sound | Recordings | Should colour | No AI | AI, pretrained | AI, trained |
|---|---|---|---|---|---|
| singing bowl | 10 | yes | 91 % [83, 97] | 84 % [77, 90] | 84 % [78, 90] |
| bell | 18 | yes | 42 % [27, 57] | 28 % [15, 42] | 24 % [11, 39] |
| tuning fork | 2 | yes | 93 % [89, 96] | 72 % [54, 89] | 61 % [54, 68] |
| gong | 11 | no | 65 % [44, 83] | 24 % [7, 44] | 17 % [1, 35] |
| voice | 5 | no | 46 % [24, 71] | 9 % [0, 23] | 3 % [0, 9] |
| drum | 20 | no | 14 % [3, 28] | 1 % [0, 2] | 1 % [0, 2] |
| shaker, rain stick | 13 | no | 0 % | 0 % | 0 % |

The gate does what a sound healing room needs from it, gongs, drums and voices mostly stop painting the
room, at a price: bells lose colour too, and bowls lose a little. Across mn04, mn05 and dymn04 with 0.5, 1 and 2 s
windows the picture is the same (25 October, 91 clips), the numbers spread: bowls keep 72 to 87 %, gongs keep 12 to 34 %, voices
1 to 20 %; mn04 on 2 s with the trained head drops gongs furthest (12 %) at the same bowl share (84 %).
22 to 26 ms a window on the Mac's CPU. Two tuning forks are too few to read much into.

The recordings came from Wikimedia Commons (free licences) and were used for testing only; none ships with
this repository.

## Evaluation pipeline

`tools/bench/pipeline.py` makes every number above from one command, as separate stages: fetch the Commons
clips, export and label the own videos, run the detector over every recording in Flow and Strike, score it
against known strikes, run the AI gate bench, measure the light, and write the report. Each run gets its own
folder with a copy of the config, the commits of both chakra repositories and hashes of the code and the data
lists, so a result can be traced and repeated. The public report of the last run, with sample sizes, 95 %
intervals, the definitions and the limitations: [docs/evaluation/report.md](docs/evaluation/report.md).

| Measure (26 October 2026) | Flow | Strike |
|---|---|---|
| Strikes that show the right chakra (8 recordings, 70 strikes) | 83 % [71, 93] | 87 % [79, 96] |
| Of these, public tune-ups only (2 videos, 28 strikes) | 71 % [43, 81] | 75 % [57, 81] |
| From the onset to the right colour, median | 0.29 s (n = 45) | 0.03 s (n = 47) |
| Colour changes a minute in sessions on the body (10 videos) | 7.5 [4.9, 8.2] | 15.0 [11.4, 19.1] |
| Colours that stand under 3 s, same sessions | 32 % [20, 38] | 56 % [48, 64] |
| Sessions with more than three changes in a second | 0 of 10 | 2 of 10 |

Where the strike times come from: the owner's hand marks, the known order root to crown aligned to the
spectral flux (checked against the marks: within 0.04 s where the marks were checked), and for public
videos their own chapter lists and spoken captions (`tools/bench/strike_truth.json`). Small n, one set of
own bowls, one person's labels: read the limitations in the report. Run it:
`tools/bench/pipeline.py --dry-run`, then without; details in [tools/bench/README.md](tools/bench/README.md).

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
tests/run.sh                    # the host tests: detection, kick, gong swell, lamp pace
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
| Detection: FFT, harmonics, steadiness, strikes (spectral flux), mappings, hysteresis, learned bowls | `components/app_chakra/chakra_detect.c` |
| Screen: colour fades, symbols, tuner ring, settings, learning, hold to close | `components/app_chakra/chakra_core.c`, scene `ui/make_scene.py` |
| Light: strike kick (at most 1 per 2 s), gong swell, the AI gate's colour rule, lamp pace | `components/app_chakra/chakra_light.c` |
| Board: microphone, Wi-Fi, Home Assistant, Hue, WiZ, the screen lamp's page (`/lamp`), settings in NVS | `main/main.c` |
| Simulator side and feeder | `components/app_chakra/host/platform_pc.c`, `tools/chakra_feed.py` |
| Tests | `tests/test_detect.c`, `tests/test_light.c`, `tests/analyse_wav.c` |
| How the light would change over a recording: changes a minute, holds, flicker, dark gaps | `tools/light_timeline.py` |
| Evaluation: data, detector, strikes, AI gate, light, report, one folder per run | `tools/bench/pipeline.py`, `tools/bench/README.md` |

The detection is plain C with no dependencies, so the board, the simulator and the tests run the same code.
This app replaces four generations of an ESPHome firmware for the ESP32-S3-BOX-3 (2025 to 2026): their
512-point FFT gave 31 Hz bins, two semitones at 250 Hz, and nothing said how sure a reading was.

Status: runs in the simulator and builds for the ESP32-S31; the board is on order, so it is untested on
hardware.

## Licence

Apache-2.0. Made by [LeopardCode.AI](https://leopardcode.ai). Chakra colours, names and the C-to-B note
assignment follow common sound healing practice; this is a light and sound tool, not a medical device.
