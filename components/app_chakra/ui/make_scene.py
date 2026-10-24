#!/usr/bin/env python3
"""Generates the chakra app's ESP-GSP scene ui/main.json (480 x 480) and its charset.

What: the whole screen in one scene. The background takes the chakra's colour, so the display
      itself is the lamp; a dark glass disc holds the chakra's symbol, its name and the note
      below; 16 spectrum bars and the seven chakra dots sit at the bottom. The gear opens the
      settings (light targets, brightness, sensitivity, after-glow, note or Solfeggio mapping).
      Holding the top bar 3 s closes the app back to Vibe Mode, as in Muse.
Why:  owner 24.10.2026: "stylisches Design und Chakra-Visualisierung ... Hintergrund soll die
      Farbe wechseln, dann kann man das auch ohne Lampen verwenden".
Pitfalls:
  * Bound text needs font_charset_file; every character the C code may set must be in
    charset.txt (built here from STRINGS). Bound text cannot take text_vertical_align.
  * Colour setters are RGB565 natively; chakra_core.c uses the _rgb888 variants.
  * Images play at their native size: the symbols are drawn at 168 px (tools/make_symbols.py).
Usage:  make_scene.py   (writes main.json, charset.txt and icons.txt next to it)
"""
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MED, BOLD, XBOLD, ICONS = "fonts/Inter-Medium.ttf", "fonts/Inter-Bold.ttf", "fonts/Inter-ExtraBold.ttf", "fonts/MaterialDesignIcons.ttf"
CHARSET, ICON_CHARSET = "charset.txt", "icons.txt"
WHITE, IDLE_BG, INK, ORANGE, SHADOW = "#FFFFFF", "#17141F", "#1C1E1E", "#FF9500", "#E2E2E7"
CHAKRA_RGB = ["#E0262B", "#F47A1F", "#F2B705", "#2FA84F", "#1E88E5", "#4B3FB5", "#9B3FC8"]
SYMBOL = 168
BARS = 16
# Material Design Icons 7.4.47: cog, home-assistant, lightbulb, wifi, bluetooth, check-circle
COG, HA, BULB, WIFI, BT, CHECK = 0xF0493, 0xF07D0, 0xF0335, 0xF05A9, 0xF00AF, 0xF05E0
LAMPS = 4  # rows in the lamp list; chakra_core.h CHAKRA_LAMPS
SLIDERS = [("bright", "Brightness", 10, 100, 60), ("sens", "Sensitivity", 1, 10, 8), ("hold", "Afterglow", 5, 50, 15)]
OPTIONS = [("map_notes", "Notes"), ("map_solf", "Solfeggio"), ("map_bowls", "Bowls"), ("lang_en", "EN"), ("lang_de", "DE")]

# Every string the C side may put into a bound label (chakra_core.c, both languages), for the
# charset; English is the default, German is chosen in the settings.
STRINGS = [
    "Root", "Sacral", "Solar plexus", "Heart", "Throat", "Third eye", "Crown",
    "Wurzel", "Sakral", "Solarplexus", "Herz", "Hals", "Stirn", "Krone",
    "Muladhara", "Svadhisthana", "Manipura", "Anahata", "Vishuddha", "Ajna", "Sahasrara",
    "Ready", "Strike a bowl or hum", " ct", "+", ".", ",", "Bereit", "Klangschale anschlagen oder summen", "Hz", "dB", "%", " s", " · ",
    "LIGHTS", "LICHT", "Brightness", "Helligkeit", "Sensitivity", "Empfindlichkeit", "Afterglow", "Nachklang",
    "Mapping", "Zuordnung", "Notes", "Noten", "Solfeggio", "Bowls", "Schalen", "Strike your", "bowl",
    "-Schale anschlagen", "Hold the tone", "Ton halten", "learned", "angelernt", "/", "Flow", "Strike", "Fließen", "Anschlag", "Language", "Sprache", "English", "Deutsch",
    "Home Assistant", "Hue", "Wi-Fi", "WLAN", "Bluetooth",
    "not paired", "nicht gekoppelt", "Living room LED strip", "EN", "DE",
    "Hold: close Chakra", "Halten: Chakra schließen", "Closing Chakra", "Chakra wird geschlossen", "--:--",
]

objects: list = []


def add(kind, name=None, parent=None, **fields):
    o = {"type": kind}
    if parent:
        o["parent_name"] = parent
    else:
        o["parent"] = -1
    if name:
        o["name"] = name
    o.update(fields)
    objects.append(o)
    return name


def group(name, parent, x, y, w, h, hidden=True):
    """Container whose only job is visibility."""
    return add("container", name, parent, x=x, y=y, w=w, h=h, hidden=hidden, bind=name, bind_target="visible")


def text(name, parent, x, y, w, h, size, font, color, align="left", sample="", charset=CHARSET, **extra):
    fields = dict(x=x, y=y, w=w, h=h, text=sample, font_size=size, font=font, fg_color=color,
                  text_align=align, bind=name, font_charset_file=charset)
    fields.update(extra)
    return add("label", name, parent, **fields)


def icon(parent, x, y, size, code, color=WHITE, name=None):
    fields = dict(x=x, y=y, w=size, h=size, text=chr(code), font_size=size, font=ICONS, fg_color=color,
                  text_align="center", font_charset_file=ICON_CHARSET)
    return add("label", name, parent, **fields)


def scene():
    # The lamp: the whole screen in the chakra's colour, faded by chakra_core.c.
    add("container", "bg", None, x=0, y=0, w=480, h=480, bg_color=IDLE_BG, bind="bg", bind_target="color")

    # Centre: a pulse while a tone sounds, the glass disc, the symbol.
    group("grp_pulse", None, 70, 26, 340, 340)
    add("effect", None, "grp_pulse", x=0, y=0, w=340, h=340, effect="pulse", fg_color=WHITE, period_ms=2400)
    add("container", None, None, x=118, y=74, w=244, h=244, radius=122, bg_color="#000000", bg_opacity=70,
        border_color=WHITE, border_width=2, border_opacity=90)
    for i in range(7):
        group(f"grp_sym_{i}", None, 156, 112, SYMBOL, SYMBOL)
        add("image", None, f"grp_sym_{i}", x=0, y=0, w=SYMBOL, h=SYMBOL, image=f"art/chakra_{i}.png", fit="contain")
    # Tuning ring around the disc: fills the closer the tone sits to its note (chakra_core.c).
    group("grp_tune", None, 108, 64, 264, 264)
    add("arc", "tune_arc", "grp_tune", x=0, y=0, w=264, h=264, min=0, max=100, value=0, start_angle=270, sweep=360,
        thickness=6, bg_color="#FFFFFF2E", fg_color=WHITE)
    text("sanskrit_text", None, 128, 282, 224, 22, 15, MED, "#FFFFFFC8", align="center", sample="Anahata",
         overflow="ellipsis")
    group("grp_sym_idle", None, 156, 112, SYMBOL, SYMBOL, hidden=False)
    add("image", None, "grp_sym_idle", x=0, y=0, w=SYMBOL, h=SYMBOL, image="art/idle.png", fit="contain")

    text("name_text", None, 24, 330, 432, 52, 42, XBOLD, WHITE, align="center", sample="Ready", overflow="ellipsis")
    text("sub_text", None, 24, 384, 432, 28, 21, BOLD, WHITE, align="center",
         sample="Strike a bowl or hum", overflow="ellipsis")

    # Spectrum: 16 bands, 60 Hz .. 4 kHz (chakra_detect.c).
    bw, gap = 12, 9
    x0 = (480 - (BARS * bw + (BARS - 1) * gap)) // 2
    for b in range(BARS):
        add("progress", f"bar_{b}", None, x=x0 + b * (bw + gap), y=420, w=bw, h=26, vertical=True,
            min=0, max=100, value=0, bg_color="#FFFFFF2E", fg_color=WHITE, radius=3)

    # The seven chakras as dots, the current one ringed.
    d, gap = 12, 20
    x0 = (480 - (7 * d + 6 * gap)) // 2
    for i in range(7):
        x = x0 + i * (d + gap)
        group(f"grp_ring_{i}", None, x - 5, 451, d + 10, d + 10)
        add("container", None, f"grp_ring_{i}", x=0, y=0, w=d + 10, h=d + 10, radius=(d + 10) // 2,
            bg_opacity=0, border_color=WHITE, border_width=2)
        add("container", None, None, x=x, y=456, w=d, h=d, radius=d // 2, bg_color=CHAKRA_RGB[i],
            border_color=WHITE, border_width=1)

    # Top bar on a dark veil, as in Muse: name left, clock centred, dot and gear right.
    add("container", None, None, x=0, y=0, w=480, h=62, bg_color="#000000", bg_opacity=60)
    add("button", "topbar", None, x=0, y=0, w=416, h=62, bg_opacity=0,
        events=[{"event": "press", "action": "call", "target_name": "hold_start"},
                {"event": "release", "action": "call", "target_name": "hold_end"}])
    add("label", None, None, x=24, y=10, w=150, h=44, text="Chakra", font_size=30, font=XBOLD, fg_color=WHITE)
    text("clock", None, 165, 12, 150, 40, 28, BOLD, WHITE, align="center", sample="12:00", overflow="clip")
    add("container", "conn_dot", None, x=396, y=26, w=12, h=12, radius=6, bg_color=ORANGE,
        bind="conn_dot", bind_target="color")
    add("button", "gear", None, x=420, y=5, w=52, h=52, radius=26, bg_opacity=0, callback="gear")
    icon("gear", 9, 9, 34, COG)

    # Settings, over everything below the top bar. Every label is bound: the language switches.
    group("grp_settings", None, 0, 62, 480, 418)
    add("container", None, "grp_settings", x=0, y=0, w=480, h=418, bg_color="#141219")
    text("lbl_lights", "grp_settings", 24, 8, 300, 20, 14, BOLD, "#FFFFFFA0", sample="LIGHTS")
    # The lamp list: a row per lamp the platform knows (paired outside the app), ticked when it
    # follows the chakra; a lamp that is not paired is greyed and cannot be ticked.
    for i in range(LAMPS):
        y = 32 + i * 48
        group(f"grp_lamp_{i}", "grp_settings", 24, y, 432, 42)
        row = f"lamp_{i}"
        add("button", row, f"grp_lamp_{i}", x=0, y=0, w=432, h=42, radius=14, bg_color="#2C2936",
            bind=row, bind_target="color", callback=row)
        text(f"lamp_icon_{i}", row, 12, 9, 24, 24, 24, ICONS, WHITE, align="center", sample=chr(HA),
             charset=ICON_CHARSET)
        text(f"lamp_name_{i}", row, 48, 10, 206, 22, 17, BOLD, WHITE, sample="Living room LED strip",
             overflow="ellipsis")
        text(f"lamp_info_{i}", row, 258, 12, 130, 18, 13, MED, "#FFFFFFB0", align="right", sample="not paired",
             overflow="ellipsis")
        group(f"grp_check_{i}", row, 398, 9, 24, 24)
        icon(f"grp_check_{i}", 0, 0, 24, CHECK)
        group(f"grp_veil_{i}", f"grp_lamp_{i}", 0, 0, 432, 42)
        add("container", None, f"grp_veil_{i}", x=0, y=0, w=432, h=42, radius=14, bg_color="#141219",
            bg_opacity=150)
    for i, (key, label, lo, hi, val) in enumerate(SLIDERS):
        y = 230 + i * 50
        text(f"lbl_{key}", "grp_settings", 24, y, 300, 22, 16, BOLD, WHITE, sample=label if label != "Notes" else "Notes 440")
        if key == "hold":  # the after-glow row also chooses how fast colours follow
            text(f"val_{key}", "grp_settings", 136, y, 90, 22, 16, BOLD, "#FFFFFFC0", sample="1.5 s")
            for j, (b, lab) in enumerate([("react_flow", "Flow"), ("react_strike", "Strike")]):
                add("button", b, "grp_settings", x=300 + j * 80, y=y - 3, w=76, h=26, radius=13,
                    bg_color="#6E5ACF" if j == 0 else "#3A3744", bind=b, bind_target="color", callback=b)
                text(f"lbl_{b}", b, 0, 3, 76, 20, 14, BOLD, WHITE, align="center", sample=lab)
        else:
            text(f"val_{key}", "grp_settings", 336, y, 120, 22, 16, BOLD, "#FFFFFFC0", align="right", sample="60 %")
        add("slider", f"sl_{key}", "grp_settings", x=24, y=y + 23, w=432, h=24, min=lo, max=hi, value=val,
            track_size=8, radius=12, bg_color="#3A3744", fg_color=WHITE, knob_color=WHITE, bind=f"sl_{key}",
            events=[{"event": "value", "action": "call", "target_name": f"sl_{key}"}])
    # One row: mapping left (notes, Solfeggio, the user's own bowls), language right. A tap on
    # Bowls before any are learned, or holding it, starts learning them.
    xs, ws = [24, 118, 212, 318, 390], [90, 90, 90, 66, 66]
    for i, (b, label) in enumerate(OPTIONS):
        if b == "map_bowls":
            ev = {"events": [{"event": "click", "action": "call", "target_name": b},
                             {"event": "long", "action": "call", "target_name": "bowls_learn"}]}
        else:
            ev = {"callback": b}
        add("button", b, "grp_settings", x=xs[i], y=378, w=ws[i], h=34, radius=17,
            bg_color="#6E5ACF" if i in (0, 3) else "#3A3744", bind=b, bind_target="color", **ev)
        text(f"lbl_{b}", b, 0, 7, ws[i], 22, 15, BOLD, WHITE, align="center", sample=label if label != "Notes" else "Notes 440")

    # Hold to close, as in Muse: the bar fills orange from the left over 3 s.
    group("grp_hold", None, 0, 0, 480, 62)
    add("progress", "hold_bar", "grp_hold", x=0, y=0, w=480, h=62, min=0, max=1000, value=0,
        bg_color=SHADOW, fg_color=ORANGE, radius=0)
    text("hold_text", "grp_hold", 0, 14, 480, 36, 24, XBOLD, INK, align="center",
         sample="Hold: close Chakra", overflow="ellipsis")

    return {"screen": "chakra", "w": 480, "h": 480, "screen_bg": IDLE_BG, "font": MED,
            "default_font_size": 22, "swipe": False, "objects": objects}


def main():
    for i in range(7):
        if not (HERE / f"art/chakra_{i}.png").is_file():
            sys.exit("art/ missing: run tools/make_symbols.py first")
    doc = scene()
    chars = set(" 0123456789:.,-–…·°%#")
    chars.update(chr(c) for c in range(0x20, 0x7F))
    chars.update("ÄÖÜäöüß")
    for s in STRINGS:
        chars.update(s)
    (HERE / CHARSET).write_text("".join(sorted(chars)), encoding="utf-8")
    (HERE / ICON_CHARSET).write_text("".join(chr(c) for c in (COG, HA, BULB, WIFI, BT, CHECK)), encoding="utf-8")
    (HERE / "main.json").write_text(json.dumps(doc, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    print(f"main.json: {len(doc['objects'])} objects; charset.txt: {len(chars)} characters")


if __name__ == "__main__":
    main()
