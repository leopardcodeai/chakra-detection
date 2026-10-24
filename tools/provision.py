#!/usr/bin/env python3
"""Writes the chakra app's lamps and extra Wi-Fi networks into the Mosaico's NVS over USB.

What: reads a JSON config (lamp names, addresses, network names; nothing secret) and the secrets
from the environment, builds an NVS image with namespace "chakra" in memory, writes it to a
private temporary file only while esptool needs it (mode 0600, deleted at once) and flashes it
to the "nvs" partition (0x200000, 64 KB, partitions.csv). main/main.c reads it at boot.

Config (default ~/Library/Application Support/LeopardCode/chakra/provision.json, or --config;
see tools/provision.example.json):
  ha_url      ws://homeassistant.local:8123/api/websocket
  ha_lights   {"light.living_room_strip": "Living room LED strip", ...}   up to 4
  hue_host    the bridge's address on the LAN;  hue_lights  [5, 6, 8]
  wiz         ["192.168.1.30", ...]
  wifi        ["Second network", ...]   up to 4, besides the one set in Vibe Mode
Secrets, from the environment only (on a Mac: the keychain, exported by the login shell):
  CHAKRA_HA_TOKEN       a long-lived Home Assistant token for this device
  CHAKRA_HUE_KEY        from tools/hue_pair.py
  CHAKRA_WIFI_PASS_1..4 the passwords of the extra networks, in their order

Why: tokens and passwords never sit in a file of the project or on a command line.
Pitfalls:
  * Overwrites the whole app NVS partition, including the app's saved settings and learned
    bowls; run it once after `iris system-update`, again only to change lamps.
  * Needs the board on USB (ROM download mode or the BSP's console with auto-download).
  * Untested on hardware until the board arrives.

Usage:
  /bin/zsh -lc 'python3 tools/provision.py --port /dev/cu.usbmodem1101 --dry-run'
  /bin/zsh -lc 'python3 tools/provision.py --port /dev/cu.usbmodem1101'
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

IDF = Path.home() / "Coding/esp/toolchain/esp-idf-mosaico"
DEFAULT_CONFIG = Path.home() / "Library/Application Support/LeopardCode/chakra/provision.json"
NVS_OFFSET, NVS_SIZE = "0x200000", "0x10000"


def csv_value(v: str) -> str:
    if any(c in v for c in ',"\n'):
        return '"' + v.replace('"', '""') + '"'
    return v


def build(cfg: dict) -> tuple[list[tuple[str, str]], list[str]]:
    """The NVS entries and a summary without secrets."""
    out, said = [], []
    token = os.environ.get("CHAKRA_HA_TOKEN", "")
    lights = cfg.get("ha_lights", {})
    if cfg.get("ha_url") and lights:
        out += [("ha_url", cfg["ha_url"]), ("ha_token", token),
                ("ha_lights", ";".join(f"{e}={n}" for e, n in list(lights.items())[:4]))]
        said.append(f"Home Assistant {cfg['ha_url']}: {len(lights)} light(s), token {'set' if token else 'MISSING'}")
    key = os.environ.get("CHAKRA_HUE_KEY", "")
    if cfg.get("hue_host") and cfg.get("hue_lights"):
        out += [("hue_host", cfg["hue_host"]), ("hue_key", key),
                ("hue_lights", ",".join(str(i) for i in cfg["hue_lights"]))]
        said.append(f"Hue bridge {cfg['hue_host']}: lights {cfg['hue_lights']}, key {'set' if key else 'MISSING'}")
    if cfg.get("wiz"):
        out.append(("wiz", ",".join(cfg["wiz"])))
        said.append(f"WiZ: {len(cfg['wiz'])} bulb(s)")
    for i, ssid in enumerate(cfg.get("wifi", [])[:4]):
        pw = os.environ.get(f"CHAKRA_WIFI_PASS_{i + 1}", "")
        out.append((f"net{i}", f"{ssid}\t{pw}"))
        said.append(f"Wi-Fi \"{ssid}\": password {'set' if pw else 'MISSING'}")
    return out, said


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", required=True)
    ap.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    if not a.config.is_file():
        sys.exit(f"no config at {a.config} (copy tools/provision.example.json there)")
    entries, said = build(json.loads(a.config.read_text()))
    for line in said:
        print("  " + line)
    if a.dry_run:
        print(f"(dry run) would write NVS 'chakra' with {len(entries)} keys to {NVS_OFFSET} on {a.port}")
        return
    if any(not v or v.endswith("\t") for k, v in entries if k in ("ha_token", "hue_key") or k.startswith("net")):
        sys.exit("a secret is missing (see above); add it to the keychain and use a login shell")
    csv = "key,type,encoding,value\nchakra,namespace,,\n" + "".join(
        f"{k},data,string,{csv_value(v)}\n" for k, v in entries)
    gen = IDF / "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"
    with tempfile.TemporaryDirectory() as tmp:
        os.chmod(tmp, 0o700)
        src, img = Path(tmp) / "chakra.csv", Path(tmp) / "chakra_nvs.bin"
        src.write_text(csv)
        try:
            subprocess.run([sys.executable, str(gen), "generate", str(src), str(img), NVS_SIZE], check=True)
            src.unlink()
            subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s31", "--port", a.port,
                            "write-flash", NVS_OFFSET, str(img)], check=True)
        finally:
            for p in (src, img):
                if p.exists():
                    p.unlink()
    print("  written; restart the Mosaico")


if __name__ == "__main__":
    main()
