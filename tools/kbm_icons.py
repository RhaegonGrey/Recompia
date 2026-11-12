#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates the keyboard and mouse prompt icons the port draws on the game's button glyphs.

runtime/games/pkp/pkp_kbm.cpp loads assets/kbm_icons/<name>.png next to the exe (Kenney's Input Prompts names) and the
build copies them from the folder this writes. The pack is CC0 and not in the repository, so this makes an equivalent
set: a white key cap with the letter cut out (transparent), which the port plates with the game's rim colour and fills
the cut-out letter with the game's letter colour. Same names, same 64 px grid, wider caps for the long keys.

    tools/kbm_icons.py [--out "work/assets/kenney_input_prompts/Keyboard & Mouse/Default"]
"""
import argparse
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("Pillow is needed: python3 -m pip install pillow")

FONT = "/System/Library/Fonts/Helvetica.ttc"
# The letter cut-outs need a bold sans face; the first one that exists wins, so the tool runs on the platform that builds
# the release (macOS ships Helvetica, the Linux runners install fonts-dejavu-core, Windows has Arial).
FONT_CANDIDATES = [
    FONT,
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "C:/Windows/Fonts/arialbd.ttf",
    "C:/Windows/Fonts/arial.ttf",
]
WHITE = (255, 255, 255, 255)


def face(px: int):
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            return ImageFont.truetype(path, px)
    try:
        return ImageFont.load_default(size=px)   # Pillow >= 10.1
    except TypeError:
        return ImageFont.load_default()


def cap(w: int, h: int, label: str, out_dir: str, name: str, font_px: int = 34) -> None:
    mask = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(mask)
    d.rounded_rectangle([1, 1, w - 2, h - 2], radius=max(6, min(w, h) // 7), fill=255)
    if label:
        font = face(font_px)
        x0, y0, x1, y1 = d.textbbox((0, 0), label, font=font)
        tw, th = x1 - x0, y1 - y0
        # cut the label out of the cap (alpha 0), centred, a hair high so it sits optically centred
        d.text(((w - tw) / 2 - x0, (h - th) / 2 - y0 - h * 0.02), label, font=font, fill=0)
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    img.paste(WHITE, (0, 0, w, h), mask)
    img.save(os.path.join(out_dir, name + ".png"))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="work/assets/kenney_input_prompts/Keyboard & Mouse/Default")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    o = args.out

    for ch in "abcdefghijklmnopqrstuvwxyz":
        cap(64, 64, ch.upper(), o, "keyboard_" + ch)
    for n in range(1, 13):
        cap(64, 64, f"F{n}", o, f"keyboard_f{n}", 26)
    for ch in "0123456789":
        cap(64, 64, ch, o, "keyboard_" + ch)

    cap(176, 64, "", o, "keyboard_space")                       # a bare wide cap: the space bar
    cap(128, 64, "ENTER", o, "keyboard_return", 24)
    cap(128, 64, "ENTER", o, "keyboard_numpad_enter", 24)
    cap(128, 64, "SHIFT", o, "keyboard_shift", 24)
    cap(112, 64, "CTRL", o, "keyboard_ctrl", 24)
    cap(96, 64, "ALT", o, "keyboard_alt", 24)
    cap(96, 64, "ESC", o, "keyboard_escape", 24)
    cap(112, 64, "BKSP", o, "keyboard_backspace", 22)
    cap(96, 64, "TAB", o, "keyboard_tab", 24)
    cap(64, 64, "\u2191", o, "keyboard_arrow_up")
    cap(64, 64, "\u2193", o, "keyboard_arrow_down")
    cap(64, 64, "\u2190", o, "keyboard_arrow_left")
    cap(64, 64, "\u2192", o, "keyboard_arrow_right")
    cap(112, 64, "CAPS", o, "keyboard_capslock", 22)
    cap(96, 64, "DEL", o, "keyboard_delete", 24)
    cap(80, 64, "END", o, "keyboard_end", 22)
    cap(88, 64, "HOME", o, "keyboard_home", 22)
    cap(96, 64, "INS", o, "keyboard_insert", 24)
    cap(112, 64, "PGUP", o, "keyboard_page_up", 22)
    cap(112, 64, "PGDN", o, "keyboard_page_down", 22)
    cap(64, 64, "-", o, "keyboard_minus")
    cap(64, 64, "=", o, "keyboard_equals")
    cap(64, 64, ",", o, "keyboard_comma")
    cap(64, 64, ".", o, "keyboard_period")
    cap(64, 64, "/", o, "keyboard_slash_forward")
    cap(64, 64, "\\", o, "keyboard_slash_back")
    cap(64, 64, ";", o, "keyboard_semicolon")
    cap(64, 64, "'", o, "keyboard_apostrophe")
    cap(64, 64, "`", o, "keyboard_tilde")
    cap(64, 64, "[", o, "keyboard_bracket_open")
    cap(64, 64, "]", o, "keyboard_bracket_close")
    cap(64, 64, "+", o, "keyboard_numpad_plus")

    cap(96, 64, "LMB", o, "mouse_left", 22)
    cap(96, 64, "RMB", o, "mouse_right", 22)
    cap(96, 64, "SCRL", o, "mouse_scroll", 20)
    cap(64, 64, "\u25b2", o, "mouse_scroll_up", 28)
    cap(64, 64, "\u25bc", o, "mouse_scroll_down", 28)

    n = len([f for f in os.listdir(o) if f.endswith(".png")])
    print(f"{n} icons in {o}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
