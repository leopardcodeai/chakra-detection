#!/bin/sh
# What: builds and runs the host tests of the chakra detection (tests/test_detect.c) and of the
#       light's kick, gong swell and lamp pace (tests/test_light.c) with the Mac's compiler. Why: the same C file runs on the board and in the simulator.
# Usage: tests/run.sh   (no side effects beyond a binary in build-test/)
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$HERE/build-test"
cc -std=c11 -O2 -Wall -Wextra -Werror -I"$HERE/components/app_chakra/include" \
  "$HERE/components/app_chakra/chakra_detect.c" "$HERE/tests/test_detect.c" -lm -o "$HERE/build-test/test_detect"
"$HERE/build-test/test_detect"
cc -std=c11 -O2 -Wall -Wextra -Werror -I"$HERE/components/app_chakra/include" \
  "$HERE/components/app_chakra/chakra_detect.c" "$HERE/components/app_chakra/chakra_light.c" \
  "$HERE/tests/test_light.c" -lm -o "$HERE/build-test/test_light"
"$HERE/build-test/test_light"
