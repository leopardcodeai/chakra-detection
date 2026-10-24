#!/bin/sh
# What: starts the ESP-GSP simulator of the chakra app for the ESP-Mosaico on the Mac, in the browser
#       (http://127.0.0.1:3222/, Safari works) or headless with one dumped frame.
# Why:  the board is on order (24.10.2026); the UI, chakra_core.c and the detection run here
#       first, fed with real audio by tools/chakra_feed.py. No ESP-IDF needed for the preview.
# Pitfalls:
#   * --interactive needs the esp-gsp 1.5.1 component; it lives in the LeopardCode state folder
#     (downloaded from components.espressif.com), set through ESP_GSP_COMPONENT_DIR.
#   * The microphone comes from the feeder (port 8698); Wi-Fi, BLE and the lamps need the board.
#   * Firmware builds need ESP-IDF master at the commit pinned in the workspace README.
# Usage:  ./sim.sh            browser preview for 10 min (CHAKRA_SIM_SECONDS=28800 for a day)
#         ./sim.sh --mic      the same, listening to the Mac's microphone (tools/chakra_feed.py mic);
#                             CHAKRA_LIGHT=light.x also colours that Home Assistant lamp
#                             (HA_URL and HA_TOKEN from the environment), CHAKRA_LAMP_NAME names it
#         ./sim.sh --frame    headless, writes frame.png next to this script
#         ./sim.sh --dry-run  prints what it would run
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
WS=$(cd "$HERE/../../workspace" && pwd)
GSP="$HOME/Library/Application Support/LeopardCode/mosaico/esp-gsp-1.5.1"
URL=https://components-file.espressif.com/components/espressif/esp-gsp/1.5.1/espressif__esp-gsp-v1.5.1.zip
if [ "${1:-}" = "--dry-run" ]; then
  [ -d "$GSP" ] || echo "would download $URL to $GSP"
  echo "would run: python3 $WS/mosaico.py project sim --project $HERE --interactive"
  exit 0
fi
if [ ! -d "$GSP" ]; then
  mkdir -p "$GSP" && curl -s -m 120 -o "$GSP.zip" "$URL" && unzip -q "$GSP.zip" -d "$GSP" && rm "$GSP.zip"
fi
cd "$WS"
if [ "${1:-}" = "--frame" ]; then
  python3 mosaico.py project sim --project "$HERE" --headless --frames 30 --dump-ppm "$HERE/frame.ppm"
  sips -s format png "$HERE/frame.ppm" --out "$HERE/frame.png" >/dev/null && rm "$HERE/frame.ppm"
  echo "$HERE/frame.png"
else
  echo "Browser: http://127.0.0.1:3222/"
  if [ "${1:-}" = "--mic" ]; then
    # The feeder connects once the simulator listens (it retries), and ends with it.
    python3 "$HERE/tools/chakra_feed.py" mic --wait ${CHAKRA_LIGHT:+--light "$CHAKRA_LIGHT"} &
  fi
  ESP_GSP_COMPONENT_DIR="$GSP" exec python3 mosaico.py project sim --project "$HERE" --interactive --duration "${CHAKRA_SIM_SECONDS:-600}"
fi
