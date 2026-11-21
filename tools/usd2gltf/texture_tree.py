"""Textures an untextured Megaplants glTF (usd2gltf output) from Megascans scans.

Megaplants leaves are modelled leaf outlines whose UVs point into an atlas
that ships only in the Unreal asset. Each leaf piece (connected component)
is mapped onto one leaf of a Megascans leaf atlas instead: its UV outline's
long axis (PCA) becomes the atlas leaf's vertical axis, the petiole (a run
of thin width at one end, else the end the area centroid leans to: leaves
are ovate) goes to the atlas leaf's bottom, and the blade's extent fills
the atlas leaf's bounding box (a long petiole reaches past it). The wood
material gets a Megascans bark surface (B, N, ORM); its tiling UVs stay.

usage: texture_tree.py in.gltf out.gltf --leaf-atlas DIR --bark DIR [--atlas-leaves I,J]
  DIR (leaf atlas): a Megascans atlas with *_BaseColor.jpg, *_Normal.jpg,
  *_Opacity.jpg (white leaves on black, petiole down), *_Roughness.jpg.
  DIR (bark): a Megascans surface's Textures/ with T_*_B.png, _N.png, _ORM.png.
Writes out.gltf and out.bin beside it; textures are referenced where they are.
Needs ImageMagick (`magick`) to read the atlas opacity.
"""

import argparse
import array
import json
import math
import os
import subprocess
import sys

kAnalysisSize = 512
kMinLeafArea = 0.002  # of the atlas; drops specks and fragments


def find(directory, suffix):
    hits = sorted(f for f in os.listdir(directory) if f.lower().endswith(suffix.lower()))
    if not hits:
        sys.exit(f"{directory}: no *{suffix}")
    return os.path.abspath(os.path.join(directory, hits[0]))


def pixels(path, fmt):
    return subprocess.run(
        ["magick", path, "-resize", f"{kAnalysisSize}x{kAnalysisSize}!", "-depth", "8",
         f"{fmt}:-"], check=True, capture_output=True).stdout


def atlas_leaves(on):
    """Bounding boxes (u0, v0, u1, v1) of the atlas's leaves, largest first."""
    n = kAnalysisSize
    label = [0] * (n * n)
    boxes = []
    for start in range(n * n):
        if not on[start] or label[start]:
            continue
        label[start] = len(boxes) + 1
        stack = [start]
        x0 = y0 = n
        x1 = y1 = area = 0
        while stack:
            p = stack.pop()
            x, y = p % n, p // n
            x0, x1, y0, y1 = min(x0, x), max(x1, x), min(y0, y), max(y1, y)
            area += 1
            for q in (p - 1 if x else -1, p + 1 if x + 1 < n else -1,
                      p - n if y else -1, p + n if y + 1 < n else -1):
                if q >= 0 and on[q] and not label[q]:
                    label[q] = label[start]
                    stack.append(q)
        boxes.append((area, (x0 / n, y0 / n, (x1 + 1) / n, (y1 + 1) / n)))
    boxes = [b for a, b in sorted(boxes, reverse=True) if a >= kMinLeafArea * n * n]
    if not boxes:
        sys.exit("no leaves found in the atlas opacity")
    return boxes


class Gltf:
    def __init__(self, path):
        with open(path) as f:
            self.json = json.load(f)
        buffers = self.json["buffers"]
        if len(buffers) != 1:
            sys.exit("expected one buffer")
        with open(os.path.join(os.path.dirname(path), buffers[0]["uri"]), "rb") as f:
            self.bin = bytearray(f.read())

    def view(self, accessor_index, typecode, width):
        a = self.json["accessors"][accessor_index]
        bv = self.json["bufferViews"][a["bufferView"]]
        if bv.get("byteStride", 4 * width) != 4 * width:
            sys.exit("interleaved accessors are not supported")
        offset = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
        return offset, a["count"] * width

    def read(self, accessor_index):
        a = self.json["accessors"][accessor_index]
        width = {"SCALAR": 1, "VEC2": 2, "VEC3": 3}[a["type"]]
        typecode = {5126: "f", 5125: "I", 5123: "H", 5121: "B"}[a["componentType"]]
        offset, count = self.view(accessor_index, typecode, width)
        values = array.array(typecode)
        values.frombytes(bytes(self.bin[offset:offset + count * values.itemsize]))
        return values, width

    def write_floats(self, accessor_index, values):
        offset, count = self.view(accessor_index, "f", 2)
        self.bin[offset:offset + count * 4] = values.tobytes()


def components(indices, positions):
    """Connected pieces as (vertices, triangles); vertices sharing a position connect."""
    parent = list(range(len(positions) // 3))

    def root(v):
        while parent[v] != v:
            parent[v] = parent[parent[v]]
            v = parent[v]
        return v

    first = {}
    for v in range(len(parent)):
        p = first.setdefault(tuple(positions[3 * v:3 * v + 3]), v)
        if p != v:
            parent[root(v)] = root(p)
    for t in range(0, len(indices), 3):
        a = root(indices[t])
        for v in indices[t + 1:t + 3]:
            b = root(v)
            if a != b:
                parent[b] = a
    groups = {}
    for v in sorted(set(indices)):
        groups.setdefault(root(v), ([], []))[0].append(v)
    for t in range(0, len(indices), 3):
        groups[root(indices[t])][1].append(tuple(indices[t:t + 3]))
    return list(groups.values())


def remap_piece(uv, vertices, triangles, box):
    cu = sum(uv[2 * v] for v in vertices) / len(vertices)
    cv = sum(uv[2 * v + 1] for v in vertices) / len(vertices)
    suu = sum((uv[2 * v] - cu) ** 2 for v in vertices)
    svv = sum((uv[2 * v + 1] - cv) ** 2 for v in vertices)
    suv = sum((uv[2 * v] - cu) * (uv[2 * v + 1] - cv) for v in vertices)
    angle = 0.5 * math.atan2(2 * suv, suu - svv)
    ax, ay = math.cos(angle), math.sin(angle)  # long axis
    along = {v: (uv[2 * v] - cu) * ax + (uv[2 * v + 1] - cv) * ay for v in vertices}
    across = {v: -(uv[2 * v] - cu) * ay + (uv[2 * v + 1] - cv) * ax for v in vertices}
    a0, a1 = min(along.values()), max(along.values())
    # Width profile along the axis: a petiole is a run of thin bins at one end.
    bins = 32
    low, high = [math.inf] * bins, [-math.inf] * bins
    for v in vertices:
        i = min(int((along[v] - a0) / max(a1 - a0, 1e-9) * bins), bins - 1)
        low[i], high[i] = min(low[i], across[v]), max(high[i], across[v])
    width = [h - l if h >= l else 0.0 for l, h in zip(low, high)]
    thin = [w < 0.15 * max(width) for w in width]
    start = next(i for i in range(bins) if not thin[i])
    end = bins - next(i for i in range(bins) if not thin[bins - 1 - i])
    if start > bins - end:
        petiole_at_low = True
    elif bins - end > start:
        petiole_at_low = False
    else:
        # No petiole to see: leaves are ovate, the area centroid lies toward it.
        area = moment = 0.0
        for a, b, c in triangles:
            w = abs((along[b] - along[a]) * (across[c] - across[a]) -
                    (along[c] - along[a]) * (across[b] - across[a]))
            area += w
            moment += w * (along[a] + along[b] + along[c]) / 3
        petiole_at_low = area > 0 and moment / area < 0.5 * (a0 + a1)
    # The blade fills the atlas leaf; a petiole reaches past its base.
    b0 = a0 + start / bins * (a1 - a0)
    b1 = a0 + end / bins * (a1 - a0)
    if petiole_at_low:
        along = {v: -s for v, s in along.items()}
        b0, b1 = -b1, -b0
    blade = [v for v in vertices if b0 <= along[v] <= b1] or vertices
    c0, c1 = min(across[v] for v in blade), max(across[v] for v in blade)
    u0, v0, u1, v1 = box
    for v in vertices:
        s, t = along[v], across[v]
        # Petiole side (s = b1) at the bottom of the atlas leaf (v1).
        uv[2 * v] = u0 + (t - c0) / max(c1 - c0, 1e-9) * (u1 - u0)
        uv[2 * v + 1] = v0 + (s - b0) / max(b1 - b0, 1e-9) * (v1 - v0)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("--leaf-atlas", required=True)
    parser.add_argument("--bark", required=True)
    parser.add_argument("--atlas-leaves", help="comma-separated atlas leaves to use, "
                        "0 = the largest (default: all)")
    args = parser.parse_args()

    gltf = Gltf(args.input)
    g = gltf.json
    leaf = [m.get("name") == "TwoSided_Material" or m.get("doubleSided", False)
            for m in g["materials"]]
    if not any(leaf) or all(leaf):
        sys.exit("expected leaf and wood materials")

    on = [b >= 128 for b in pixels(find(args.leaf_atlas, "_Opacity.jpg"), "gray")]
    boxes = atlas_leaves(on)
    if args.atlas_leaves:
        boxes = [boxes[int(i)] for i in args.atlas_leaves.split(",")]
    roughness = pixels(find(args.leaf_atlas, "_Roughness.jpg"), "gray")
    leaf_roughness = sum(r for r, inside in zip(roughness, on) if inside) / 255 / max(sum(on), 1)
    done = set()
    pieces = 0
    for mesh in g["meshes"]:
        for primitive in mesh["primitives"]:
            if not leaf[primitive.get("material", 0)]:
                continue
            uv_index = primitive["attributes"]["TEXCOORD_0"]
            key = (uv_index, primitive["indices"])
            if key in done:
                continue
            if any(k[0] == uv_index for k in done):
                sys.exit("a UV accessor shared by different index lists")
            done.add(key)
            uv, _ = gltf.read(uv_index)
            indices, _ = gltf.read(primitive["indices"])
            positions, _ = gltf.read(primitive["attributes"]["POSITION"])
            for vertices, triangles in components(indices, positions):
                remap_piece(uv, vertices, triangles, boxes[pieces % len(boxes)])
                pieces += 1
            # One UV per position: a UV seam would lock those vertices in the
            # cluster LOD, so whole leaves could no longer simplify away.
            shared = {}
            for v in range(len(uv) // 2):
                shared.setdefault(tuple(positions[3 * v:3 * v + 3]), v)
            for v in range(len(uv) // 2):
                p = shared[tuple(positions[3 * v:3 * v + 3])]
                uv[2 * v], uv[2 * v + 1] = uv[2 * p], uv[2 * p + 1]
            gltf.write_floats(uv_index, uv)

    out_dir = os.path.dirname(os.path.abspath(args.output))
    os.makedirs(out_dir, exist_ok=True)
    images = g.setdefault("images", [])
    textures = g.setdefault("textures", [])

    def texture(path):
        images.append({"uri": os.path.relpath(path, out_dir)})
        textures.append({"source": len(images) - 1})
        return {"index": len(textures) - 1}

    leaf_color = texture(find(args.leaf_atlas, "_BaseColor.jpg"))
    leaf_normal = texture(find(args.leaf_atlas, "_Normal.jpg"))
    bark_color = texture(find(args.bark, "_B.png"))
    bark_normal = texture(find(args.bark, "_N.png"))
    bark_orm = texture(find(args.bark, "_ORM.png"))
    for material, is_leaf in zip(g["materials"], leaf):
        pbr = material.setdefault("pbrMetallicRoughness", {})
        pbr["baseColorFactor"] = [1.0, 1.0, 1.0, 1.0]
        if is_leaf:
            pbr["baseColorTexture"] = dict(leaf_color)
            # Its mean only: a roughness map would need an ORM packed for it.
            pbr["roughnessFactor"] = round(leaf_roughness, 3)
            material["normalTexture"] = dict(leaf_normal)
        else:
            pbr["baseColorTexture"] = dict(bark_color)
            pbr["metallicRoughnessTexture"] = dict(bark_orm)
            pbr["roughnessFactor"] = 1.0
            pbr["metallicFactor"] = 1.0
            material["normalTexture"] = dict(bark_normal)
            material["occlusionTexture"] = dict(bark_orm)

    stem = os.path.splitext(os.path.basename(args.output))[0]
    g["buffers"][0]["uri"] = stem + ".bin"
    with open(os.path.join(out_dir, stem + ".bin"), "wb") as f:
        f.write(gltf.bin)
    with open(args.output, "w") as f:
        json.dump(g, f)
    print(f"{stem}: {pieces} leaf pieces onto {len(boxes)} atlas leaves")


if __name__ == "__main__":
    main()
