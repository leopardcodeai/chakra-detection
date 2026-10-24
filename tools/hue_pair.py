#!/usr/bin/env python3
"""Pairs the chakra app with a Philips Hue bridge and lists its lights.

What: asks the bridge on the LAN for an app key (press the bridge's round link button first,
then run this within 30 s), hands the key to the keychain through
~/.config/private/keychain-add.sh CHAKRA_HUE_KEY (over stdin, never on a command line; without
that script it prints the key once for you to store), and lists the bridge's colour lights with
their numbers for "hue_lights" in the provisioning config.
Why: Hue is the fastest lamp path (local REST, no cloud); pairing needs a hand on the bridge, so
it happens on site, outside the app.
Pitfalls:
  * The bridge answers "link button not pressed" for 30 s after a press; run it again then.
  * --list uses CHAKRA_HUE_KEY from the environment (a login shell exports the keychain).
Usage:
  tools/hue_pair.py --host 192.168.1.2 --dry-run   says what it would do
  tools/hue_pair.py --host 192.168.1.2             pairs (button pressed), stores, lists
  tools/hue_pair.py --host 192.168.1.2 --list      lists the lights with the stored key
"""
import argparse
import json
import os
import subprocess
import sys
import urllib.request
from pathlib import Path

ADD = Path.home() / ".config/private/keychain-add.sh"


def call(url, body=None):
    req = urllib.request.Request(url, data=json.dumps(body).encode() if body else None,
                                 headers={"Content-Type": "application/json"}, method="POST" if body else "GET")
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read())


def list_lights(host, key):
    lights = call(f"http://{host}/api/{key}/lights")
    for lid, l in sorted(lights.items(), key=lambda kv: int(kv[0])):
        colour = "colour" if "xy" in l.get("state", {}) else "white only"
        print(f"  {lid:>3}  {l.get('name', '?'):32} {colour}{'' if l.get('state', {}).get('reachable') else ', unreachable'}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", required=True, help="the bridge's address on the LAN")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    if a.dry_run:
        print(f"would ask http://{a.host}/api for an app key, store it as CHAKRA_HUE_KEY, list the lights")
        return
    if a.list:
        key = os.environ.get("CHAKRA_HUE_KEY")
        if not key:
            sys.exit("CHAKRA_HUE_KEY not in the environment; pair first or use a login shell")
        list_lights(a.host, key)
        return
    answer = call(f"http://{a.host}/api", {"devicetype": "chakra#mosaico"})[0]
    if "error" in answer:
        sys.exit(f"bridge: {answer['error'].get('description')} (press the link button, then run again within 30 s)")
    key = answer["success"]["username"]
    if ADD.is_file():
        subprocess.run([str(ADD), "CHAKRA_HUE_KEY"], input=key + "\n", text=True, check=True,
                       stdout=subprocess.DEVNULL)
        print("  key stored in the keychain as CHAKRA_HUE_KEY")
    else:
        print(f"  key (store it now, it is shown once): {key}")
    list_lights(a.host, key)


if __name__ == "__main__":
    main()
