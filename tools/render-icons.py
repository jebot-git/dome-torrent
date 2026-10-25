#!/usr/bin/env python3
"""Render the scalable application icon. Requires CairoSVG only when regenerating."""
from pathlib import Path
import re
import cairosvg

root = Path(__file__).resolve().parents[1]
source = root / 'packaging/org.domed.Torrent.svg'
path = re.search(r'd="([^"]+)"/>', source.read_text()).group(1)
native = (root / 'native/suite-icons.h').read_text()
assert f'if (role == "torrent") return "{path}";' in native, 'Update the native window/tray glyph to match the SVG'
for size in (16, 24, 32, 48, 64, 128, 256):
    target = root / f'packaging/icons/{size}x{size}/org.domed.Torrent.png'
    target.parent.mkdir(parents=True, exist_ok=True)
    cairosvg.svg2png(url=str(source), write_to=str(target), output_width=size, output_height=size)
