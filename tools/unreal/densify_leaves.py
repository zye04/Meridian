#!/usr/bin/env python3
"""Add copies of a tree's own leaf cards inside its crown (cook input).

    python3 -I tools/unreal/densify_leaves.py rotate COPIES SRC.gltf OUT.gltf
    python3 -I tools/unreal/densify_leaves.py fill HOLE_DEPTH SRC.gltf OUT.gltf

SRC is a cook_ready.py / retint_leaves.py output (an Unreal export: one mesh,
leaf cards in the primitive whose material has diffuse
transmission > 0). OUT gets the same glTF with that primitive's attributes
extended and a new OUT.bin; the caller links OUT's textures/ to SRC's.
meridian-cook --tree then traces and budgets it as any tree: give it a
triangle budget raised by the printed leaf factor, or the cook prunes the
copies away again and scales the survivors up (tree.cpp).

rotate: COPIES whole-crown copies, each turned about the trunk axis by a
further golden angle and drawn inward toward the crown centre, so the gaps
of one copy fall on leaves of the others (leaf area x (1 + COPIES)).

fill: copies of single sprays (position-connected card pieces, as the cook
groups them) where the crown's outer shell has holes. The shell is the
outermost leaf distance per direction from the crown centre; a direction is
a hole where that distance falls HOLE_DEPTH metres short of the envelope
(the shell's median over neighbouring directions). Each hole gets a spray
from the outer shell at a similar elevation, turned about the trunk axis to
the hole's azimuth (so it keeps a surface spray's outward tilt) and moved
along the hole's direction to just inside the envelope, until every hole
direction is covered.
"""
import json
import math
import os
import sys

import numpy as np

# Golden angle: no two copies ever share an orientation, and each new copy
# lands as far as possible from all earlier ones (phyllotaxis).
GOLDEN_ANGLE = math.pi * (3.0 - math.sqrt(5.0))
# Copies scale about the crown centre by this: a ~6 m crown radius puts the
# copy's outer shell ~0.6 m inside the original's, about one spray deep (95 %
# of card triangles are under 0.54 m), so copies fill under the surface
# instead of growing the outline.
INWARD_SCALE = 0.9
# Wood vertices this close to the lowest one are the trunk base (axis).
TRUNK_BASE_BAND = 0.3
# Positions welded to this grid (m) group cards into pieces, as the cook's
# position-connected pieces.
WELD = 1e-4
# Shell directions: azimuth bins of 5 deg; elevation bins uniform in
# sin(elevation) (equal solid angle) from below the crown's widest point to
# the zenith: views see crowns from level to steeply above.
AZIMUTH_BINS = 72
ELEVATION_BINS = 24
LOWEST_ELEVATION = math.radians(-30.0)
# The envelope is the shell's median over this many bins each way (15 deg
# azimuth): wide enough to bridge a hole a few sprays across, narrow enough
# to follow a dome's lobes.
ENVELOPE_RADIUS = 3
# Directions with fewer leaf vertices than this are outside the crown.
MIN_VERTICES = 8
# Sprays from the outer shell: centroid beyond this fraction of the
# envelope in its own direction.
SHELL_FRACTION = 0.85
# A filled spray's centroid sits this far inside the envelope (m): about
# half a spray, so it closes the surface without poking out.
FILL_INSET = 0.25
# Source sprays may come from this many elevation bins either side.
ELEVATION_REACH = 1
SEED = 1


def load(path):
    gltf = json.load(open(path))
    base = os.path.dirname(os.path.abspath(path))
    buffers = [open(os.path.join(base, b["uri"]), "rb").read() for b in gltf["buffers"]]
    return gltf, buffers


COMPONENTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}
DTYPES = {5126: np.float32, 5125: np.uint32, 5123: np.uint16, 5121: np.uint8}


def read(gltf, buffers, index):
    a = gltf["accessors"][index]
    view = gltf["bufferViews"][a["bufferView"]]
    n = COMPONENTS[a["type"]]
    dtype = np.dtype(DTYPES[a["componentType"]])
    stride = view.get("byteStride", 0)
    if stride and stride != n * dtype.itemsize:
        raise SystemExit("interleaved buffer views are not supported")
    offset = view.get("byteOffset", 0) + a.get("byteOffset", 0)
    data = np.frombuffer(buffers[view.get("buffer", 0)], dtype, a["count"] * n, offset)
    return data.reshape(a["count"], n).copy()


def leaf_primitive(gltf):
    found = []
    for mesh in gltf["meshes"]:
        for prim in mesh["primitives"]:
            ext = gltf["materials"][prim["material"]].get("extensions", {})
            t = ext.get("KHR_materials_diffuse_transmission", {}).get("diffuseTransmissionFactor", 0)
            if t > 0:
                found.append(prim)
    if len(found) != 1:
        raise SystemExit(f"expected one leaf primitive, found {len(found)}")
    if len(gltf["meshes"]) != 1 or len(gltf["nodes"]) != 1 or "matrix" in gltf["nodes"][0] \
            or any(k in gltf["nodes"][0] for k in ("rotation", "scale", "translation")):
        raise SystemExit("expected one untransformed mesh node (an Unreal export)")
    return found[0]


def trunk_axis(gltf, buffers, leaf):
    wood = [read(gltf, buffers, p["attributes"]["POSITION"])
            for p in gltf["meshes"][0]["primitives"] if p is not leaf]
    wood = np.concatenate(wood)
    low = wood[wood[:, 1] < wood[:, 1].min() + TRUNK_BASE_BAND]
    return low[:, 0].mean(), low[:, 2].mean()


def turn(points, angle, ax, az):
    c, s = math.cos(angle), math.sin(angle)
    x, z = points[:, 0] - ax, points[:, 2] - az
    out = points.copy()
    out[:, 0] = ax + c * x + s * z
    out[:, 2] = az - s * x + c * z
    return out


def pieces(positions):
    """Piece id per triangle: triangles sharing a welded position."""
    keys = np.round(positions / WELD).astype(np.int64)
    _, vertex = np.unique(keys, axis=0, return_inverse=True)
    vertex = vertex.reshape(-1, 3)
    label = np.arange(vertex.max() + 1)
    while True:
        tri = label[vertex].min(axis=1)
        before = label.copy()
        for k in range(3):
            np.minimum.at(label, vertex[:, k], tri)
        label = label[label]
        if np.array_equal(label, before):
            break
    _, piece = np.unique(label[vertex[:, 0]], return_inverse=True)
    return piece


def direction_bins(points, centre):
    d = points - centre
    r = np.linalg.norm(d, axis=1)
    azimuth = np.arctan2(d[:, 2], d[:, 0])
    elevation = np.arcsin(np.clip(d[:, 1] / np.maximum(r, 1e-9), -1, 1))
    ia = ((azimuth + math.pi) / (2 * math.pi) * AZIMUTH_BINS).astype(int) % AZIMUTH_BINS
    low = math.sin(LOWEST_ELEVATION)
    ie = np.floor((np.sin(elevation) - low) / (1 - low) * ELEVATION_BINS).astype(int)
    return ia, np.clip(ie, -1, ELEVATION_BINS - 1), r


def bin_direction(ia, ie):
    azimuth = (ia + 0.5) / AZIMUTH_BINS * 2 * math.pi - math.pi
    low = math.sin(LOWEST_ELEVATION)
    elevation = math.asin(low + (ie + 0.5) / ELEVATION_BINS * (1 - low))
    return np.array([math.cos(elevation) * math.cos(azimuth), math.sin(elevation),
                     math.cos(elevation) * math.sin(azimuth)])


def fill(attributes, ax, az, hole_depth):
    positions = attributes["POSITION"]
    piece = pieces(positions)
    y = positions[:, 1]
    centre = np.array([ax, 0.5 * (np.percentile(y, 2) + y.max()), az])
    ia, ie, r = direction_bins(positions, centre)
    inside = ie >= 0
    shell = np.full((AZIMUTH_BINS, ELEVATION_BINS), -np.inf)
    count = np.zeros((AZIMUTH_BINS, ELEVATION_BINS), int)
    np.add.at(count, (ia[inside], ie[inside]), 1)
    np.maximum.at(shell, (ia[inside], ie[inside]), r[inside])
    shell[count < MIN_VERTICES] = np.nan
    envelope = np.full_like(shell, np.nan)
    for a in range(AZIMUTH_BINS):
        for e in range(ELEVATION_BINS):
            rows = [(a + k) % AZIMUTH_BINS for k in range(-ENVELOPE_RADIUS, ENVELOPE_RADIUS + 1)]
            cols = slice(max(0, e - ENVELOPE_RADIUS), e + ENVELOPE_RADIUS + 1)
            window = shell[rows][:, cols]
            if np.isfinite(window).sum() > window.size // 2:
                envelope[a, e] = np.nanmedian(window)
    hole = np.isfinite(envelope) & ~(shell >= envelope - hole_depth)

    # Outer-shell sprays, by elevation bin of their centroid.
    tri_centroid = positions.reshape(-1, 3, 3).mean(axis=1)
    n_pieces = piece.max() + 1
    centroid = np.zeros((n_pieces, 3))
    np.add.at(centroid, piece, tri_centroid)
    centroid /= np.bincount(piece, minlength=n_pieces)[:, None]
    pa, pe, pr = direction_bins(centroid, centre)
    ok = pe >= 0
    outer = np.zeros(n_pieces, bool)
    outer[ok] = pr[ok] > SHELL_FRACTION * np.nan_to_num(envelope[pa[ok], pe[ok]], nan=np.inf)
    by_elevation = [np.flatnonzero(outer & (pe == e)) for e in range(ELEVATION_BINS)]
    triangles_of = np.split(np.argsort(piece, kind="stable"),
                            np.cumsum(np.bincount(piece, minlength=n_pieces))[:-1])

    rng = np.random.default_rng(SEED)
    covered = ~hole
    targets = list(zip(*np.nonzero(hole)))
    rng.shuffle(targets)
    added = []
    for a, e in targets:
        if covered[a, e]:
            continue
        lo, hi = max(0, e - ELEVATION_REACH), min(ELEVATION_BINS, e + ELEVATION_REACH + 1)
        source = np.concatenate(by_elevation[lo:hi])
        if source.size == 0:
            continue
        p = int(rng.choice(source))
        target = bin_direction(a, e)
        angle = math.atan2(centroid[p, 2] - az, centroid[p, 0] - ax) - math.atan2(target[2], target[0])
        tris = triangles_of[p]
        verts = (tris[:, None] * 3 + np.arange(3)).ravel()
        moved = turn(positions[verts], angle, ax, az)
        c = turn(centroid[p:p + 1], angle, ax, az)[0]
        goal = centre + target * (envelope[a, e] - FILL_INSET)
        moved += goal - c
        normals = turn(attributes["NORMAL"][verts], angle, 0.0, 0.0)
        added.append((verts, moved, normals))
        ma, me, _ = direction_bins(moved, centre)
        keep = me >= 0
        covered[ma[keep], me[keep]] = True
    print(f"shell: {np.isfinite(envelope).sum()} directions, {hole.sum()} holes "
          f"(depth {hole_depth} m), {(hole & ~covered).sum()} left uncovered; "
          f"{len(added)} sprays added from {outer.sum()} outer of {n_pieces}")
    return added


def rotate(attributes, ax, az, copies):
    positions = attributes["POSITION"]
    y = positions[:, 1]
    centre_y = 0.5 * (np.percentile(y, 2) + y.max())
    verts = np.arange(len(positions))
    added = []
    for k in range(1, copies + 1):
        moved = turn(positions, k * GOLDEN_ANGLE, ax, az)
        moved[:, 0] = ax + INWARD_SCALE * (moved[:, 0] - ax)
        moved[:, 1] = centre_y + INWARD_SCALE * (moved[:, 1] - centre_y)
        moved[:, 2] = az + INWARD_SCALE * (moved[:, 2] - az)
        added.append((verts, moved, turn(attributes["NORMAL"], k * GOLDEN_ANGLE, 0.0, 0.0)))
    print(f"{copies} copies, turned by k x {math.degrees(GOLDEN_ANGLE):.1f} deg, scaled "
          f"{INWARD_SCALE} about ({ax:.2f}, {centre_y:.2f}, {az:.2f})")
    return added


def main():
    if len(sys.argv) != 5 or sys.argv[1] not in ("rotate", "fill"):
        sys.exit(__doc__)
    mode, amount, src, out = sys.argv[1:]
    gltf, buffers = load(src)
    leaf = leaf_primitive(gltf)
    ax, az = trunk_axis(gltf, buffers, leaf)
    attributes = {k: read(gltf, buffers, i) for k, i in leaf["attributes"].items()}
    if "indices" in leaf:
        # Indexed cards (beech) are expanded to one vertex per corner; the cook welds by
        # position, so its pieces are unchanged.
        indices = read(gltf, buffers, leaf["indices"]).ravel()
        attributes = {k: v[indices] for k, v in attributes.items()}
    added = rotate(attributes, ax, az, int(amount)) if mode == "rotate" \
        else fill(attributes, ax, az, float(amount))

    extended = {}
    for name, data in attributes.items():
        parts = [data]
        for verts, moved, normals in added:
            parts.append(moved.astype(data.dtype) if name == "POSITION"
                         else normals.astype(data.dtype) if name == "NORMAL" else data[verts])
        extended[name] = np.concatenate(parts)
    count = len(extended["POSITION"])
    print(f"leaf card triangles {len(attributes['POSITION']) // 3} -> {count // 3} "
          f"(leaf factor {count / len(attributes['POSITION']):.3f})")

    blob = bytearray(b"".join(buffers))
    if len(buffers) != 1:
        raise SystemExit("expected one buffer")

    def append(array, target, accessor_type, component, normalized=False):
        while len(blob) % 4:
            blob.append(0)
        gltf["bufferViews"].append({"buffer": 0, "byteOffset": len(blob),
                                    "byteLength": array.nbytes, "target": target})
        blob.extend(array.tobytes())
        accessor = {"bufferView": len(gltf["bufferViews"]) - 1, "count": len(array),
                    "type": accessor_type, "componentType": component}
        if normalized:
            accessor["normalized"] = True
        if accessor_type == "VEC3" and component == 5126:
            accessor["min"] = array.min(axis=0).tolist()
            accessor["max"] = array.max(axis=0).tolist()
        gltf["accessors"].append(accessor)
        return len(gltf["accessors"]) - 1

    for name, data in extended.items():
        old = gltf["accessors"][leaf["attributes"][name]]
        leaf["attributes"][name] = append(np.ascontiguousarray(data), 34962, old["type"],
                                          old["componentType"], old.get("normalized", False))
    if "indices" in leaf:
        leaf["indices"] = append(np.arange(count, dtype=np.uint32), 34963, "SCALAR", 5125)
    bin_name = os.path.splitext(os.path.basename(out))[0] + ".bin"
    gltf["buffers"] = [{"uri": bin_name, "byteLength": len(blob)}]
    with open(os.path.join(os.path.dirname(out), bin_name), "wb") as f:
        f.write(blob)
    with open(out, "w") as f:
        json.dump(gltf, f)


if __name__ == "__main__":
    main()
