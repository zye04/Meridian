#!/usr/bin/env python3
"""Megascans Trees (export_trees.py output) -> glTFs ready for meridian-cook --tree.

    python3 -I tools/unreal/cook_ready.py ~/Downloads/unreal-export/blackalder OUT [ASSET ...]

The Unreal glTF export carries geometry only (material baking does nothing
headless). Its materials are the mesh's slots in order, without the impostor
slot (only on the impostor LOD). This assigns each slot its MA_Foliage
textures from manifest.json:
  Albedo  RGB base colour (its alpha is solid)
  Mask    R opacity (leaf and decoration cards), G roughness, B translucency
          (leaves bright, twigs dark); A unused
  Normal  already OpenGL (green up), as glTF wants: measured, not assumed
          (dNx/drow correlates negatively with dNy/dcol, like the known-GL
          Bark012 map; a DirectX map correlates positively)
Leaf atlases are downscaled to --leaf-size, bark and decoration to --bark-size
(the sources are 8K). Needs ImageMagick (magick).
"""
import argparse
import json
import os
import subprocess
import sys

# Leaf transmittance over reflectance of the leaves the albedo textures show
# (mature summer leaves): beech over the visible range, reflectance 5.7%,
# transmittance 6.4% (EGU2019-6557; young spring leaves ~2). The source
# factor meridian-cook --tree reads ("the back transmits t times the base
# colour") and translates to KHR_materials_diffuse_transmission. One value
# per material: the Mask's B (translucency) is an artist weight, not a
# measure.
LEAF_TRANSMITTED_OVER_REFLECTED = 1.1


def magick(*args):
    subprocess.run(["magick", *args], check=True)


def adjustments(inventory, texture):
    """magick arguments for the albedo's Unreal texture adjustments, which the
    PNG exporter leaves out (the beech's leaves: brightness 0.9, hue 355)."""
    t = inventory.get("textures", {}).get(texture, {})
    get = lambda k, d: float(t.get(k, d)) if t.get(k) not in (None, "<n/a>") else d
    brightness, saturation, hue = get("adjust_brightness", 1.0), get("adjust_saturation", 1.0), get("adjust_hue", 0.0)
    for k, identity in (("adjust_brightness_curve", 1.0), ("adjust_vibrance", 0.0), ("adjust_rgb_curve", 1.0)):
        if get(k, identity) != identity:
            print("warning: %s %s = %s not applied" % (texture, k, t[k]))
    if (brightness, saturation, hue) == (1.0, 1.0, 0.0):
        return []
    # Unreal adjusts linear colour in HSV; -modulate's hue is 100 + degrees / 1.8.
    hue = (hue + 180.0) % 360.0 - 180.0
    return ["-colorspace", "RGB", "-define", "modulate:colorspace=HSV", "-modulate",
            "%g,%g,%g" % (100.0 * brightness, 100.0 * saturation, 100.0 + hue / 1.8), "-colorspace", "sRGB"]


def kind(slot):
    s = slot.lower()
    if "twosided" in s:
        return "leaf"
    if "decoration" in s:
        return "decoration"
    return "bark"


def build_textures(src, tex_dir, name, files, k, size, adjust):
    """Writes <name>_color/_normal/_orm.png once; returns their paths."""
    color, normal, orm = (os.path.join(tex_dir, name + s + ".png") for s in ("_color", "_normal", "_orm"))
    albedo, mask, nrm = (os.path.join(src, files[r]) for r in ("Albedo", "Mask", "Normal"))
    resize = ["-resize", "%dx%d" % (size, size)]
    if not os.path.exists(color):
        # Adjusted on its own: settings such as -colorspace leak into the
        # composite below.
        magick(albedo, "-alpha", "off", *resize, *adjust, color)
        if k != "bark":
            magick(color, "(", mask, "-alpha", "off", *resize, "-channel", "R", "-separate", ")",
                   "-compose", "CopyOpacity", "-composite", color)
    if not os.path.exists(normal):
        magick(nrm, "-alpha", "off", *resize, normal)
    if not os.path.exists(orm):
        # glTF metallicRoughness: G roughness, B metallic (0); R unused.
        magick(mask, "-alpha", "off", *resize, "-channel", "R", "-evaluate", "set", "100%",
               "-channel", "B", "-evaluate", "set", "0", "+channel", orm)
    return color, normal, orm


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("export_dir")
    ap.add_argument("out_dir")
    ap.add_argument("assets", nargs="*", help="asset names (default: all in the manifest)")
    ap.add_argument("--leaf-size", type=int, default=4096)
    ap.add_argument("--bark-size", type=int, default=2048)
    a = ap.parse_args()
    src = os.path.join(a.export_dir, "textures")
    tex_dir = os.path.join(a.out_dir, "textures")
    os.makedirs(tex_dir, exist_ok=True)
    manifest = json.load(open(os.path.join(a.export_dir, "manifest.json")))
    inventory = json.load(open(os.path.join(a.export_dir, "inventory.json")))
    for asset in manifest["assets"]:
        if a.assets and asset["name"] not in a.assets or not asset["gltf"]:
            continue
        gltf_path = os.path.join(a.export_dir, asset["gltf"])
        gltf = json.load(open(gltf_path))
        slots = [s for s in asset["slots"] if "impostor" not in (s["slot"] or "").lower()]
        if len(slots) != len(gltf.get("materials", [])):
            sys.exit("%s: %d slots without the impostor, %d glTF materials" % (
                asset["name"], len(slots), len(gltf.get("materials", []))))
        images, textures, materials = [], [], []

        def texture(path):
            images.append({"uri": os.path.relpath(path, a.out_dir)})
            textures.append({"source": len(images) - 1})
            return {"index": len(textures) - 1}

        for slot in slots:
            k = kind(slot["slot"])
            files, paths = {}, {}
            for lst in slot["textures_by_role"].values():
                for e in lst:
                    if e["param"] in ("Albedo", "Mask", "Normal") and e["file"]:
                        files[e["param"]] = e["file"]
                        paths[e["param"]] = e["texture"]
            if len(files) != 3:
                sys.exit("%s slot %s: missing textures %s" % (asset["name"], slot["slot"], files))
            tex_name = os.path.splitext(files["Albedo"])[0].split("_T_", 1)[-1].replace("_Albedo", "")
            color, normal, orm = build_textures(
                src, tex_dir, tex_name, files, k, a.leaf_size if k == "leaf" else a.bark_size,
                adjustments(inventory, paths["Albedo"]))
            m = {"name": slot["slot"],
                 "pbrMetallicRoughness": {"baseColorTexture": texture(color),
                                          "metallicRoughnessTexture": texture(orm),
                                          "metallicFactor": 0.0},
                 "normalTexture": texture(normal)}
            if k != "bark":
                m.update(alphaMode="MASK", alphaCutoff=0.5, doubleSided=True)
                # Decoration cards (lichen, knots on the bark) don't transmit.
                m["extensions"] = {"KHR_materials_diffuse_transmission": {
                    "diffuseTransmissionFactor": LEAF_TRANSMITTED_OVER_REFLECTED if k == "leaf" else 0.0}}
            materials.append(m)
        gltf.update(materials=materials, images=images, textures=textures)
        gltf["extensionsUsed"] = sorted(set(gltf.get("extensionsUsed", [])) | {"KHR_materials_diffuse_transmission"})
        for b in gltf.get("buffers", []):
            b["uri"] = os.path.relpath(os.path.join(a.export_dir, b["uri"]), a.out_dir)
        with open(os.path.join(a.out_dir, asset["gltf"]), "w") as f:
            json.dump(gltf, f, indent=1)
        print("%s: %s" % (asset["name"], ", ".join("%s (%s)" % (s["slot"], kind(s["slot"])) for s in slots)))


if __name__ == "__main__":
    main()
