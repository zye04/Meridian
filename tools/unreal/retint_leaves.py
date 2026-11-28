#!/usr/bin/env python3
"""Retint the Megascans Trees' leaf atlas toward a palette colour (cook input).

    python3 -I tools/unreal/retint_leaves.py PALETTE.json SRC_DIR OUT_DIR

SRC_DIR is a cook_ready.py output (<species>/: glTFs + textures/). OUT_DIR
gets the same glTFs, the leaf atlas (*_Summer_color.png) retinted, and
every other texture as a symlink, so meridian-cook --tree can cook it
unchanged. The leaf's transmission colour is its base colour times a factor
(cook_ready.py, tree.cpp cookTexturedMaterial), so it follows the retint.

The retint is a transform of the whole atlas in OKLab, not a flat fill: the
leaf texels' alpha-weighted mean (linear) moves to the palette's target
(hue rotation, chroma scale, uniform linear scale for lightness) and every
texel moves with it, keeping the texture's own variation around the mean.
`strength` in [0, 1] takes that fraction of the move (as a fraction of the
hue angle and of the log chroma and lightness scales).

The target is the atlas as exported, i.e. the leaf's reflectance: the cook
scales it by 1 + t (t = transmitted over reflected) into the base colour and
gives back t / (1 + t) as transmission, so the glTF base colour is ~2.1x
brighter (linear) than the target.
"""
import json
import os
import shutil
import subprocess
import sys

import numpy as np

# Texels below this alpha are outside the leaf cards (cook_ready.py writes
# the Mask's opacity as the atlas alpha; the glTF cutoff is 0.5).
LEAF_ALPHA = 0.5


def to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def oklab(rgb):
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    l = np.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b)
    m = np.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b)
    s = np.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b)
    return np.stack([0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
                     1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
                     0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s], axis=-1)


def linear_rgb(lab):
    L, a, b = lab[..., 0], lab[..., 1], lab[..., 2]
    l = (L + 0.3963377774 * a + 0.2158037573 * b) ** 3
    m = (L - 0.1055613458 * a - 0.0638541728 * b) ** 3
    s = (L - 0.0894841775 * a - 1.2914855480 * b) ** 3
    return np.stack([4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
                     -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                     -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s], axis=-1)


def lch(lab):
    return lab[0], float(np.hypot(lab[1], lab[2])), float(np.degrees(np.arctan2(lab[2], lab[1])))


def read_rgba(path):
    w, h = map(int, subprocess.run(["magick", "identify", "-format", "%w %h", path],
                                   capture_output=True, check=True).stdout.split())
    raw = subprocess.run(["magick", path, "-depth", "8", "rgba:-"], capture_output=True,
                         check=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(h, w, 4)


def retint(path, out, target, strength):
    rgba = read_rgba(path)
    alpha = rgba[..., 3] / 255.0
    rgb = to_linear(rgba[..., :3] / 255.0)
    leaf = alpha >= LEAF_ALPHA
    mean = (rgb[leaf] * alpha[leaf, None]).sum(0) / alpha[leaf].sum()
    L0, C0, h0 = lch(oklab(mean))
    Lt, Ct, ht = target["L"], target["C"], target["h"]
    turn = np.radians(((ht - h0 + 180.0) % 360.0 - 180.0) * strength)
    chroma = (Ct / C0) ** strength
    lightness = (Lt / L0) ** strength
    lab = oklab(rgb)
    a, b = lab[..., 1], lab[..., 2]
    lab = np.stack([lab[..., 0] * lightness,
                    chroma * (a * np.cos(turn) - b * np.sin(turn)),
                    chroma * (a * np.sin(turn) + b * np.cos(turn))], axis=-1)
    srgb = np.rint(to_srgb(linear_rgb(lab)) * 255.0).astype(np.uint8)
    result = np.concatenate([srgb, rgba[..., 3:]], axis=-1)
    h, w = result.shape[:2]
    subprocess.run(["magick", "-size", f"{w}x{h}", "-depth", "8", "rgba:-", out],
                   input=result.tobytes(), check=True)
    new = (to_linear(srgb[leaf] / 255.0) * alpha[leaf, None]).sum(0) / alpha[leaf].sum()
    L1, C1, h1 = lch(oklab(new))
    print(f"{os.path.basename(path)}: mean L {L0:.3f} C {C0:.3f} h {h0 % 360:.1f} -> "
          f"L {L1:.3f} C {C1:.3f} h {h1 % 360:.1f}")


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    palette_path, src, out = sys.argv[1:]
    palette = json.load(open(palette_path))
    os.makedirs(os.path.join(out, "textures"), exist_ok=True)
    for name in os.listdir(src):
        if name.endswith(".gltf"):
            shutil.copyfile(os.path.join(src, name), os.path.join(out, name))
    for name in os.listdir(os.path.join(src, "textures")):
        source = os.path.abspath(os.path.join(src, "textures", name))
        target = os.path.join(out, "textures", name)
        if os.path.lexists(target):
            os.remove(target)
        if name.lower().endswith("_summer_color.png"):
            retint(source, target, palette["leaf"], palette["strength"])
        else:
            os.symlink(source, target)


if __name__ == "__main__":
    main()
