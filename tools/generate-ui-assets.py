#!/usr/bin/env python3
"""Generate the embedded-resolution glyph atlas and launcher icon with Pillow.

The Inter variable font is from google/fonts commit e1d6480102fed30739fead0faee463101f892c8f.
"""
import argparse
from hashlib import sha256
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

parser = argparse.ArgumentParser()
parser.add_argument("inter", type=Path)
args = parser.parse_args()
output = Path(__file__).resolve().parent.parent / "packaging"
if sha256(args.inter.read_bytes()).hexdigest() != "29160a80ff49ddcab2c97711247e08b1fab27a484a329ce8b813d820dc559031":
    parser.error("Inter source hash does not match the pinned font")
font = ImageFont.truetype(str(args.inter), 58)
font.set_variation_by_axes([24, 500])
cell = 76
atlas = Image.new("L", (16 * cell, 6 * cell))
draw = ImageDraw.Draw(atlas)
advances = bytearray()
for index, code in enumerate(range(32, 127)):
    char = chr(code)
    x, y = (index % 16) * cell, (index // 16) * cell
    draw.text((x + 3, y + 58), char, fill=255, font=font, anchor="ls")
    advances.append(round(font.getlength(char)))
(output / "ui-glyphs.bin").write_bytes(advances + atlas.tobytes())

icon = Image.new("RGBA", (320, 320))
shape = Image.new("L", icon.size)
ImageDraw.Draw(shape).rounded_rectangle((2, 2, 317, 317), radius=76, fill=255)
gradient = Image.new("RGBA", icon.size)
gradient.putdata([(int(29 + 22 * (1 - y / 320)), int(54 + 42 * (1 - y / 320)),
                   int(105 + 75 * (1 - y / 320)), 255)
                  for y in range(320) for _ in range(320)])
icon.paste(gradient, (0, 0), shape)
draw = ImageDraw.Draw(icon)
draw.rounded_rectangle((75, 81, 244, 201), radius=24, outline="white", width=10)
draw.rounded_rectangle((163, 174, 224, 246), radius=14, fill=(127, 177, 255, 255))
draw.rounded_rectangle((103, 218, 142, 228), radius=5, fill="white")
icon.resize((80, 80), Image.Resampling.LANCZOS).save(output / "icon.png")
