#!/bin/sh
# What: runs the chakra detection over any audio file (WAV, MP3, OGG, MP4 ...) and prints when
#       which chakra was locked. Why: tests against real recordings without the simulator.
# Usage: tests/analyse.sh FILE [notes|solfeggio|bowls F0,...,F6] [--a4 440]
#        tests/analyse.sh --dry-run FILE   only says what it would run
# Needs ffmpeg; writes a 16 kHz mono copy next to the binary in build-test/.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
if [ "${1:-}" = "--dry-run" ]; then
  echo "would convert $2 to 16 kHz mono and run build-test/analyse_wav on it"; exit 0
fi
IN="$1"; shift
mkdir -p "$HERE/build-test"
cc -std=c11 -O2 -Wall -Wextra -Werror -I"$HERE/components/app_chakra/include" \
  "$HERE/components/app_chakra/chakra_detect.c" "$HERE/tests/analyse_wav.c" -lm -o "$HERE/build-test/analyse_wav"
ffmpeg -loglevel error -y -i "$IN" -vn -ac 1 -ar 16000 -c:a pcm_s16le "$HERE/build-test/input.wav"
"$HERE/build-test/analyse_wav" "$HERE/build-test/input.wav" "$@"
