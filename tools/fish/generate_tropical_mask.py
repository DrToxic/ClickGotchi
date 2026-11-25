#!/usr/bin/env python3
"""Make the plain Tropical fish (species 0) mask for clickgotchi_art.h.

Source: LoRagotchi-DX examples/cyd_pet/CydSplashArt.h, the CYD's generated fish bitmap
(112x80, four 1-bit planes; source grid 28x20 at scale 4). Planes map to the mask roles the
way CydTankArt::drawBitmapFish colours them: plane 0 body, planes 1-2 fins, plane 3 eyes.
Output: 32x24 mask, 2 bits per pixel, pixel (x,y) at index y*32+x (byte idx/4, shift (idx%4)*2),
the fish centred at (2,2). Both frames are the same (the CYD fish does not animate).
"""
import re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT.parent / "LoRagotchi-DX/examples/cyd_pet/CydSplashArt.h"
text = SRC.read_text()
fish = text.split("fish[4480] = {", 1)[1].split("};", 1)[0]
data = bytes(int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]{2}", fish))
W, H = 112, 80
assert len(data) == 4 * ((W + 7) // 8) * H
stride = (W + 7) // 8
plane_bytes = stride * H
SRC_W, SRC_H, SCALE = 28, 20, 4
MW, MH = 32, 24
OX, OY = (MW - SRC_W) // 2, (MH - SRC_H) // 2
ROLE = [1, 2, 2, 3]

def plane_bit(p, x, y):
    return (data[p * plane_bytes + y * stride + x // 8] >> (7 - (x % 8))) & 1

mask = bytearray(MW * MH // 4)
for sy in range(SRC_H):
    for sx in range(SRC_W):
        px, py = sx * SCALE, sy * SCALE
        role = 0
        for p in range(4):
            if plane_bit(p, px, py):
                role = ROLE[p]
                break
        idx = (sy + OY) * MW + (sx + OX)
        mask[idx // 4] |= role << ((idx % 4) * 2)

def c_bytes(b):
    rows = []
    for i in range(0, len(b), 32):
        rows.append("    " + ",".join(f"0x{v:02x}" for v in b[i:i + 32]) + ",")
    return "\n".join(rows)

out = ["  { // Tropical fish", "    {", c_bytes(mask), "    },", "    {", c_bytes(mask), "    },", "  },"]
print("\n".join(out))
