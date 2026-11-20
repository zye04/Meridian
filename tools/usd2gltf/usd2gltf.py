"""Converts a Megascans Megaplants USD tree into an unflattened glTF.

Usage: usd2gltf.py in.usd out.gltf (writes out.bin beside it)

Each mesh prim becomes one glTF mesh (one primitive per bound material) and
each place it is drawn, directly or through nested PointInstancers, one node
with the composed transform. Geometry is only converted (triangulated,
face-varying data split into vertices); meridian-cook --tree simplifies and
flattens it. Skinning primvars are ignored (no wind). UsdPreviewSurface
texture inputs (UsdUVTexture files) become glTF textures that reference the
image files where they are.
"""

import array
import json
import os
import sys
import urllib.parse
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
    def __init__(self, stage, out_dir):
        self.stage = stage
        self.out_dir = out_dir
        self.xforms = UsdGeom.XformCache(TIME)
        self.bin = bytearray()
        self.gltf = {
            "asset": {"version": "2.0", "generator": "meridian usd2gltf"},
            "scene": 0, "scenes": [{"nodes": []}], "nodes": [], "meshes": [],
            "materials": [], "accessors": [], "bufferViews": [], "buffers": [],
        }
        self.meshes = {}
        self.materials = {}
        self.images = {}
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
                consumed = self.textures(shader, entry)
                for name, key in (("diffuseColor", None), ("opacity", None),
                                  ("metallic", "metallicFactor"),
                                  ("roughness", "roughnessFactor")):
                    surface_input = shader.GetInput(name)
                    # A texture's input carries its fallback value only.
                    if not surface_input or name in consumed:
                        continue
                    value = surface_input.Get()
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

    def texture_source(self, shader, name):
        """The UsdUVTexture file and output channel connected to a
        UsdPreviewSurface input, or None."""
        surface_input = shader.GetInput(name)
        if not surface_input or not surface_input.HasConnectedSource():
            return None
        connected = surface_input.GetConnectedSource()
        if not connected:
            print("warning: %s.%s has an invalid connection; ignored"
                  % (shader.GetPath(), name), file=sys.stderr)
            return None
        source, output, _ = connected
        texture = UsdShade.Shader(source.GetPrim())
        if not texture or texture.GetIdAttr().Get() != "UsdUVTexture":
            print("warning: %s.%s is connected to %s, not a UsdUVTexture; ignored"
                  % (shader.GetPath(), name, texture.GetIdAttr().Get()), file=sys.stderr)
            return None
        file_input = texture.GetInput("file")
        asset = file_input.Get(TIME) if file_input else None
        path = asset.resolvedPath if asset else ""
        if not path or "[" in path:
            print("warning: %s.%s: texture file %s does not resolve to a plain file; ignored"
                  % (shader.GetPath(), name, asset), file=sys.stderr)
            return None
        for setting, default in (("scale", Gf.Vec4f(1.0)), ("bias", Gf.Vec4f(0.0))):
            value = texture.GetInput(setting).Get() if texture.GetInput(setting) else None
            expected = (Gf.Vec4f(2, 2, 2, 1), Gf.Vec4f(-1, -1, -1, 0)) if name == "normal" else ()
            if value is not None and value != default and value not in expected:
                print("warning: %s.%s: texture %s %s is not applied"
                      % (shader.GetPath(), name, setting, value), file=sys.stderr)
        return Path(path).resolve(), output

    def image_texture(self, path):
        """A glTF texture index for an image file, referenced by its path
        relative to the output glTF's directory."""
        if path not in self.images:
            relative = os.path.relpath(path, self.out_dir).replace(os.sep, "/")
            self.gltf.setdefault("images", []).append({"uri": urllib.parse.quote(relative)})
            self.gltf.setdefault("textures", []).append(
                {"source": len(self.gltf["images"]) - 1})
            self.images[path] = len(self.gltf["textures"]) - 1
        return self.images[path]

    def textures(self, shader, entry):
        """Carries UsdPreviewSurface texture inputs into glTF slots: diffuse
        color (with a cutout opacity from the same file's alpha), normal,
        roughness and metallic from one ORM-packed file (G and B), occlusion
        (R). Returns the inputs a texture now provides."""
        pbr = entry["pbrMetallicRoughness"]
        consumed = set()

        def ignored(name, source, why):
            print("warning: %s.%s from %s:%s %s; ignored"
                  % (shader.GetPath(), name, source[0], source[1], why), file=sys.stderr)

        color = self.texture_source(shader, "diffuseColor")
        if color and color[1] == "rgb":
            pbr["baseColorTexture"] = {"index": self.image_texture(color[0])}
            pbr["baseColorFactor"] = [1.0, 1.0, 1.0, 1.0]
            consumed.add("diffuseColor")
        elif color:
            ignored("diffuseColor", color, "is not RGB")
        opacity = self.texture_source(shader, "opacity")
        threshold_input = shader.GetInput("opacityThreshold")
        threshold = threshold_input.Get() if threshold_input else None
        if opacity and "diffuseColor" in consumed and opacity == (color[0], "a") and \
                threshold and threshold > 0.0:
            entry["alphaMode"] = "MASK"
            entry["alphaCutoff"] = float(threshold)
            consumed.add("opacity")
        elif opacity:
            ignored("opacity", opacity,
                    "is not a cutout (opacityThreshold > 0) of the diffuse texture's alpha")
        normal = self.texture_source(shader, "normal")
        if normal and normal[1] == "rgb":
            entry["normalTexture"] = {"index": self.image_texture(normal[0])}
            consumed.add("normal")
        elif normal:
            ignored("normal", normal, "is not RGB")
        roughness = self.texture_source(shader, "roughness")
        metallic = self.texture_source(shader, "metallic")
        metallic_input = shader.GetInput("metallic")
        metallic_value = metallic_input.Get() if metallic_input and not metallic else None
        if roughness and roughness[1] == "g" and (
                metallic == (roughness[0], "b") or (not metallic and not metallic_value)):
            pbr["metallicRoughnessTexture"] = {"index": self.image_texture(roughness[0])}
            pbr["roughnessFactor"] = 1.0
            pbr["metallicFactor"] = 1.0 if metallic else 0.0
            consumed.update(("roughness", "metallic"))
        else:
            for name, source in (("roughness", roughness), ("metallic", metallic)):
                if source:
                    ignored(name, source, "is not one glTF-packed file (roughness G, "
                            "metallic B or 0)")
        occlusion = self.texture_source(shader, "occlusion")
        if occlusion and occlusion[1] == "r":
            entry["occlusionTexture"] = {"index": self.image_texture(occlusion[0])}
        elif occlusion:
            ignored("occlusion", occlusion, "is not R")
        return consumed

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
    converter = Converter(stage, out.parent.resolve())
    converter.write(out)
    print("Wrote %s: %d meshes, %d nodes" % (out, len(converter.gltf["meshes"]),
                                              len(converter.gltf["nodes"])))


if __name__ == "__main__":
    main()
