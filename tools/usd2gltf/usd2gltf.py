"""Converts a Megascans Megaplants USD tree into an unflattened glTF.

Usage: usd2gltf.py in.usd out.gltf (writes out.bin beside it)

Each mesh prim becomes one glTF mesh (one primitive per bound material) and
each place it is drawn, directly or through nested PointInstancers, one node
with the composed transform. Geometry is only converted (triangulated,
face-varying data split into vertices); meridian-cook --tree simplifies and
flattens it. Skinning primvars are ignored (no wind).
"""

import array
import json
import sys
from pathlib import Path

from pxr import Gf, Usd, UsdGeom, UsdShade

TIME = Usd.TimeCode.Default()
# Megaplants instancer prototypes live under `class` prims, which the default
# predicate skips as abstract; outside prototypes, classes are not drawn.
PROTOTYPE_PREDICATE = Usd.PrimIsActive & Usd.PrimIsDefined & Usd.PrimIsLoaded


def floats(vt_array):
    """A flat float32 view of a Vt array of float vectors."""
    if not len(vt_array):
        return []
    view = memoryview(vt_array)
    if view.format != "f":
        sys.exit("unsupported array element type %s" % view.format)
    return view.cast("B").cast("f")


class Converter:
    def __init__(self, stage):
        self.stage = stage
        self.xforms = UsdGeom.XformCache(TIME)
        self.bin = bytearray()
        self.gltf = {
            "asset": {"version": "2.0", "generator": "meridian usd2gltf"},
            "scene": 0, "scenes": [{"nodes": []}], "nodes": [], "meshes": [],
            "materials": [], "accessors": [], "bufferViews": [], "buffers": [],
        }
        self.meshes = {}
        self.materials = {}
        self.double_sided = set()

    def add_view(self, data, target):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.gltf["bufferViews"].append({"buffer": 0, "byteOffset": len(self.bin),
                                         "byteLength": len(data), "target": target})
        self.bin += data
        return len(self.gltf["bufferViews"]) - 1

    def add_accessor(self, values, kind, components, target, bounds=False):
        accessor = {"bufferView": self.add_view(values.tobytes(), target),
                    "componentType": 5125 if values.typecode == "I" else 5126,
                    "count": len(values) // components, "type": kind}
        if bounds:
            accessor["min"] = [min(values[c::components]) for c in range(components)]
            accessor["max"] = [max(values[c::components]) for c in range(components)]
        self.gltf["accessors"].append(accessor)
        return len(self.gltf["accessors"]) - 1

    def material(self, material):
        path = material.GetPath() if material else None
        if path not in self.materials:
            entry = {"name": path.name if path else "default",
                     "pbrMetallicRoughness": {"baseColorFactor": [0.18, 0.18, 0.18, 1.0],
                                              "metallicFactor": 0.0, "roughnessFactor": 0.5}}
            shader = material.ComputeSurfaceSource()[0] if material else None
            if shader:
                pbr = entry["pbrMetallicRoughness"]
                for name, key in (("diffuseColor", None), ("opacity", None),
                                  ("metallic", "metallicFactor"),
                                  ("roughness", "roughnessFactor")):
                    value = shader.GetInput(name).Get() if shader.GetInput(name) else None
                    if value is None:
                        continue
                    if name == "diffuseColor":
                        pbr["baseColorFactor"][:3] = [float(c) for c in value]
                    elif name == "opacity":
                        pbr["baseColorFactor"][3] = float(value)
                    else:
                        pbr[key] = float(value)
            # Megaplants mark leaf materials by name; their meshes leave
            # doubleSided unset.
            self.materials[path] = len(self.gltf["materials"])
            if path and "twosided" in path.name.lower():
                self.double_sided.add(self.materials[path])
            self.gltf["materials"].append(entry)
        return self.materials[path]

    def primitive(self, corners, points, normals, normal_corner, st, st_corner, faces):
        """Splits face-varying data into glTF vertices for `faces` (corner lists)."""
        keys = {}
        positions, out_normals, uvs, indices = array.array("f"), array.array("f"), \
            array.array("f"), array.array("I")
        for face in faces:
            face_indices = []
            for corner in face:
                point = corners[corner]
                n = tuple(normals[3 * normal_corner(point, corner):][:3]) if normals else ()
                t = tuple(st[2 * st_corner(point, corner):][:2]) if st else ()
                # Face-varying values repeat per corner; equal values share a vertex.
                key = (point, n, t)
                index = keys.get(key)
                if index is None:
                    index = keys[key] = len(keys)
                    positions.extend(points[3 * point:3 * point + 3])
                    out_normals.extend(n)
                    if st:
                        uvs.extend((t[0], 1.0 - t[1]))
                face_indices.append(index)
            for k in range(1, len(face_indices) - 1):
                indices.extend((face_indices[0], face_indices[k], face_indices[k + 1]))
        attributes = {"POSITION": self.add_accessor(positions, "VEC3", 3, 34962, True)}
        if normals:
            attributes["NORMAL"] = self.add_accessor(out_normals, "VEC3", 3, 34962)
        if st:
            attributes["TEXCOORD_0"] = self.add_accessor(uvs, "VEC2", 2, 34962)
        return {"attributes": attributes,
                "indices": self.add_accessor(indices, "SCALAR", 1, 34963)}

    def mesh(self, prim):
        path = prim.GetPath()
        if path in self.meshes:
            return self.meshes[path]
        mesh = UsdGeom.Mesh(prim)
        counts = mesh.GetFaceVertexCountsAttr().Get(TIME)
        corners = mesh.GetFaceVertexIndicesAttr().Get(TIME)
        points = floats(mesh.GetPointsAttr().Get(TIME))
        left_handed = mesh.GetOrientationAttr().Get(TIME) == UsdGeom.Tokens.leftHanded

        primvars = UsdGeom.PrimvarsAPI(prim)

        def lookup(interpolation, indices):
            """Maps (point, corner) to a value index for an interpolation."""
            if interpolation == UsdGeom.Tokens.faceVarying:
                pick = lambda point, corner: corner
            elif interpolation in (UsdGeom.Tokens.vertex, UsdGeom.Tokens.varying):
                pick = lambda point, corner: point
            elif interpolation == UsdGeom.Tokens.uniform:
                pick = lambda point, corner: face_of[corner]
            else:
                pick = lambda point, corner: 0
            if indices:
                return lambda point, corner: indices[pick(point, corner)]
            return pick

        face_of = []
        for face, count in enumerate(counts):
            face_of.extend([face] * count)

        normals, normal_corner = None, None
        normal_primvar = primvars.GetPrimvar("normals")
        if normal_primvar and normal_primvar.HasAuthoredValue():
            normals = floats(normal_primvar.Get(TIME))
            normal_corner = lookup(normal_primvar.GetInterpolation(),
                                   normal_primvar.GetIndices(TIME))
        elif mesh.GetNormalsAttr().HasAuthoredValue():
            normals = floats(mesh.GetNormalsAttr().Get(TIME))
            normal_corner = lookup(mesh.GetNormalsInterpolation(), None)

        st, st_corner = None, None
        st_primvar = primvars.GetPrimvar("st")
        if st_primvar and st_primvar.HasAuthoredValue():
            st = floats(st_primvar.Get(TIME))
            st_corner = lookup(st_primvar.GetInterpolation(), st_primvar.GetIndices(TIME))

        face_corners = []
        offset = 0
        for count in counts:
            face = range(offset, offset + count)
            face_corners.append(list(reversed(face)) if left_handed else list(face))
            offset += count

        # Faces grouped by bound material: GeomSubsets first, the rest to the
        # prim's own binding.
        groups = {}
        assigned = set()
        for subset in UsdShade.MaterialBindingAPI(prim).GetMaterialBindSubsets():
            indices = subset.GetIndicesAttr().Get(TIME) or []
            material = UsdShade.MaterialBindingAPI(subset.GetPrim()).ComputeBoundMaterial()[0]
            groups.setdefault(self.material(material), []).extend(indices)
            assigned.update(indices)
        rest = [face for face in range(len(counts)) if face not in assigned]
        if rest:
            material = UsdShade.MaterialBindingAPI(prim).ComputeBoundMaterial()[0]
            groups.setdefault(self.material(material), []).extend(rest)
        if mesh.GetDoubleSidedAttr().Get(TIME):
            self.double_sided.update(groups)

        primitives = []
        for material, faces in groups.items():
            primitive = self.primitive(corners, points, normals, normal_corner, st, st_corner,
                                       [face_corners[face] for face in sorted(faces)])
            primitive["material"] = material
            primitives.append(primitive)
        self.gltf["meshes"].append({"name": prim.GetName(), "primitives": primitives})
        self.meshes[path] = len(self.gltf["meshes"]) - 1
        return self.meshes[path]

    def add_node(self, prim, world):
        node = {"mesh": self.mesh(prim)}
        if world != Gf.Matrix4d(1.0):
            # USD matrices act on row vectors, so their rows are glTF's columns.
            node["matrix"] = [float("%.7g" % world[r][c]) for r in range(4) for c in range(4)]
        self.gltf["nodes"].append(node)
        self.gltf["scenes"][0]["nodes"].append(len(self.gltf["nodes"]) - 1)

    def visit(self, root, root_world, relative_to):
        """Adds every mesh drawn under `root`; `relative_to` is the prim whose
        transform `root_world` already holds (None for the stage root)."""
        predicate = Usd.PrimDefaultPredicate if relative_to is None else PROTOTYPE_PREDICATE
        prims = iter(Usd.PrimRange(root, predicate))
        for prim in prims:
            imageable = UsdGeom.Imageable(prim)
            # A prototype root's own visibility hides only its source
            # definition, not its instances.
            if prim != root and imageable and (
                    imageable.GetVisibilityAttr().Get(TIME) == UsdGeom.Tokens.invisible or
                    imageable.GetPurposeAttr().Get(TIME) in (UsdGeom.Tokens.guide,
                                                             UsdGeom.Tokens.proxy)):
                prims.PruneChildren()
                continue
            if prim.IsInstance():
                print("warning: %s uses native instancing, which is not traversed; its meshes "
                      "are missing" % prim.GetPath(), file=sys.stderr)
            if relative_to is None:
                local = self.xforms.GetLocalToWorldTransform(prim)
            else:
                local = self.xforms.ComputeRelativeTransform(prim, relative_to)[0]
            if prim.IsA(UsdGeom.PointInstancer):
                self.instancer(UsdGeom.PointInstancer(prim), local * root_world)
                prims.PruneChildren()
            elif prim.IsA(UsdGeom.Mesh):
                self.add_node(prim, local * root_world)

    def instancer(self, instancer, world):
        prototypes = instancer.GetPrototypesRel().GetTargets()
        indices = instancer.GetProtoIndicesAttr().Get(TIME) or []
        mask = instancer.ComputeMaskAtTime(TIME)
        # Includes each prototype root's own transform.
        transforms = instancer.ComputeInstanceTransformsAtTime(TIME, TIME)
        for i, (proto, transform) in enumerate(zip(indices, transforms)):
            if mask and not mask[i]:
                continue
            root = self.stage.GetPrimAtPath(prototypes[proto])
            self.visit(root, transform * world, root)

    def write(self, out):
        self.visit(self.stage.GetPseudoRoot(), Gf.Matrix4d(1.0), None)
        for index in self.double_sided:
            self.gltf["materials"][index]["doubleSided"] = True
        bin_path = out.with_suffix(".bin")
        self.gltf["buffers"].append({"uri": bin_path.name, "byteLength": len(self.bin)})
        bin_path.write_bytes(self.bin)
        out.write_text(json.dumps(self.gltf, separators=(",", ":")))


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: usd2gltf.py in.usd out.gltf")
    stage = Usd.Stage.Open(sys.argv[1])
    if UsdGeom.GetStageUpAxis(stage) != UsdGeom.Tokens.y:
        sys.exit("only Y-up stages are supported")
    out = Path(sys.argv[2])
    out.parent.mkdir(parents=True, exist_ok=True)
    if UsdGeom.GetStageMetersPerUnit(stage) != 1.0:
        sys.exit("only stages in meters are supported")
    converter = Converter(stage)
    converter.write(out)
    print("Wrote %s: %d meshes, %d nodes" % (out, len(converter.gltf["meshes"]),
                                              len(converter.gltf["nodes"])))


if __name__ == "__main__":
    main()
