"""Unreal Editor Python: inventory and export Megaplants trees to glTF + PNG textures.

DRAFT, written without Unreal installed. Every step is wrapped in try/except;
failures land in manifest.json "errors" and in the log. The first run is a
DIAGNOSTIC run on one beech: it tells us what a PVE/Megaplants asset actually
is in the project (static mesh? skeletal mesh? Nanite assembly? placed ISMs?).
The script never modifies or deletes project assets (exports only).

HOW TO RUN (inside the Linux editor, project with the Megaplants added)
  1. Python console / Output Log (Cmd mode Python):
       py /abs/path/export_trees.py --folder /Game/Fab/Megaplants/EuropeanBeech --species beech
     or select assets (or one folder) in the Content Browser and run with no args:
       py /abs/path/export_trees.py
     Diagnostics only (no export):   ... --inventory-only
  2. Headless / command line:
       UnrealEditor <project>.uproject -run=pythonscript \
         -script="/abs/path/export_trees.py --folder /Game/Fab/Megaplants/EuropeanBeech"
     (A level with the tree placed/generated is only seen when a level is open;
      the commandlet loads the default map. Prefer the interactive editor first.)

Options
  --folder CONTENT_PATH   recursive; else Content Browser selection (assets, then folders)
  --out DIR               default ~/Downloads/unreal-export
  --species NAME          default: folder name / first asset name, lowercased
  --inventory-only        write inventory + log, export nothing
  --bake {disabled,simple,mesh}   GLTFMaterialBakeMode (default simple)
  --source-model {auto,on,off}    export SourceModel vs render data (default auto,
                          see NANITE below)

Output: <out>/<species>/
  inventory.json  export_log.txt  manifest.json  placements.json (if placed ISMs found)
  <asset>.gltf/.bin (+ textures the glTF exporter wrote)  <species>_assembly.gltf
  textures/<name>.png   every texture referenced by the materials (raw PNG export;
                        normal maps are UE convention, green down; the ones inside the
                        glTFs are converted by adjust_normalmaps)

SEND BACK AFTER THE FIRST RUN: inventory.json, manifest.json, export_log.txt, and a
listing of the output folder (`ls -lR <out>/<species>`). If something crashed the
editor, the last 50 lines of the editor log (Saved/Logs/<project>.log) too.

PLACEMENT CONVERSION (placements.json)
  UE is cm, Z up, X forward, left-handed. The glTF exporter (UNVERIFIED, from memory
  of GLTFConverterUtilities) maps position (x,y,z)_ue -> (y,z,x) * export_uniform_scale
  (0.01) and flips triangle winding. We use the same map P for placements:
  M_gltf = P * T_linear * P^-1 and translation = 0.01 * P(t), computed by pushing the
  basis vectors through Transform.transform_location (so no Euler/quat convention is
  assumed). Matrices are 16 floats, column-major (glTF order). Compare against the
  per-mesh glTF export to confirm the axis map on the first run.

NANITE: GLTFExportOptions has no Nanite-fallback option (checked, 36 properties).
  For a Nanite mesh the render-data LOD 0 is the Nanite FALLBACK mesh (often a few
  percent of the triangles). `export_source_model` exports the source model instead
  (full detail) but PVE-baked meshes may have none. Mode auto: try source model,
  and if the glTF has 0 triangles / fails, retry with render data. The manifest
  records "gltf_triangles" (counted from the exported glTF) next to
  "triangles_lod0_unreal" so we can see which one we got.

WIND: skeletal meshes are exported with export_vertex_skin_weights=False (the wind
  data lives in bones/weights; the inventory lists bone counts only).

VERIFIED against docs (dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/<Name>)
  GLTFExporter.export_to_gltf(object, file_path, options, selected_actors) classmethod
  GLTFExportOptions: texture_image_format, bake_material_inputs, default_level_of_detail,
    export_vertex_colors, export_uniform_scale (0.01), export_source_model,
    export_vertex_skin_weights, adjust_normalmaps, default_material_bake_size,
    export_morph_targets, export_animation_sequences, export_lights, export_cameras,
    use_mesh_quantization, export_hidden_in_game, make_skinned_meshes_root
  GLTFTextureImageFormat {NONE, PNG, JPEG}; GLTFMaterialBakeMode {DISABLED, SIMPLE,
    USE_MESH_DATA}  (the pages say 5.8; the 5.7 names are assumed identical)
  AssetExportTask props: automated, exporter, filename, object, options, prompt,
    replace_identical, selected, use_file_archive, write_empty_files, errors
  MaterialEditingLibrary: get_{scalar,vector,texture,static_switch}_parameter_names,
    get_material_instance_{scalar,vector,texture,static_switch}_parameter_value
    (instance, name, association)
  StaticMeshEditorSubsystem: get_lod_count, get_number_verts, get_number_materials,
    get_nanite_settings  (NO triangle count function exists)
  SkeletalMeshEditorSubsystem: get_lod_count, get_num_verts, get_num_sections
  InstancedStaticMeshComponent: get_instance_count, get_instance_transform(i, world_space),
    static_mesh
  EditorUtilityLibrary: get_selected_assets, get_selected_asset_data,
    get_selected_folder_paths
UNVERIFIED (guessed; each guarded)
  unreal.Exporter.run_asset_export_task, unreal.TextureExporterPNG,
  AssetToolsHelpers.get_asset_tools().export_assets, AssetRegistry get_assets_by_path
  / AssetData.asset_class_path, EditorAssetLibrary.load_asset, StaticMesh.static_materials,
  SkeletalMesh.materials / .skeleton, MaterialInstanceConstant.parent /
  .base_property_overrides / .*_parameter_values, MaterialParameterAssociation enum,
  Transform.transform_location, unreal.Set(unreal.Actor) for selected_actors,
  EditorActorSubsystem.get_all_level_actors, UnrealEditorSubsystem.get_editor_world,
  SkeletalMeshComponent.skeletal_mesh_asset, GLTFExportMessages fields, bone count.
PVE background: docs describe the Export node as baking Nanite Foliage assets
  (skeletal deformation, assemblies, voxelization) or a plain static mesh; Nanite
  assemblies are skeletal/static meshes with micro-instanced parts
  (dev.epicgames.com/documentation/en-us/unreal-engine/procedural-vegetation-editor-in-unreal-engine,
   .../nanite-assemblies). No Python API for assembly parts was found, so the
  inventory dumps every property and method name of each mesh to look for one.
"""

import argparse
import contextlib
import datetime
import io
import json
import os
import re
import sys
import traceback

import unreal

ERRORS = []
LOG_LINES = []
LOG_PATH = []
EXPORTED_TEX = {}
TOOL_VERSION = 1


def append_log(lines):
    try:
        with open(LOG_PATH[0], "a", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
            f.flush()
    except Exception:
        pass


def set_log_dir(out_dir):
    LOG_PATH[:] = [os.path.join(out_dir, "export_log.txt")]
    try:
        with open(LOG_PATH[0], "w", encoding="utf-8") as f:
            f.write("\n".join(LOG_LINES) + "\n")
            f.flush()
    except Exception:
        pass


def log(msg):
    line = str(msg)
    LOG_LINES.append(line)
    if LOG_PATH:
        append_log([line])
    try:
        unreal.log(line)
    except Exception:
        print(line)


def err(asset, reason):
    ERRORS.append({"asset": str(asset), "reason": str(reason)})
    log("ERROR [%s] %s" % (asset, reason))


def safe(fn, what, default=None, asset=""):
    try:
        return fn()
    except Exception as e:  # noqa: BLE001
        err(asset or what, "%s: %s: %s" % (what, type(e).__name__, e))
        return default


def jsonable(v, depth=0):
    if v is None or isinstance(v, (bool, int, float, str)):
        return v
    if isinstance(v, (list, tuple, set)):
        return [jsonable(x, depth + 1) for x in list(v)[:64]]
    if isinstance(v, dict):
        return {str(k): jsonable(x, depth + 1) for k, x in v.items()}
    return str(v)[:300]


def short(v):
    if v is None or isinstance(v, (bool, int, float, str)):
        return v
    try:
        if isinstance(v, (list, tuple)) or type(v).__name__ in ("Array", "FixedArray"):
            items = list(v)
            return {"len": len(items), "first": [short(x) for x in items[:8]]}
    except Exception:
        pass
    return str(v)[:300]


_BASE = set()


def base_names():
    if not _BASE:
        try:
            _BASE.update(dir(unreal.Object))
        except Exception:
            pass
    return _BASE


def dump_props(obj):
    """Every non-Object attribute: properties with values, methods as names."""
    props, methods = {}, []
    base = base_names()
    for n in dir(obj):
        if n.startswith("_") or n in base:
            continue
        try:
            v = getattr(obj, n)
        except Exception as e:  # noqa: BLE001
            props[n] = "<unreadable: %s>" % type(e).__name__
            continue
        if callable(v):
            methods.append(n)
        else:
            props[n] = short(v)
    return {"properties": props, "methods": methods}


def enum_name(v):
    try:
        return v.name
    except Exception:
        return str(v)


def asset_path(obj):
    if obj is None:
        return None
    try:
        return obj.get_path_name()
    except Exception:
        return str(obj)


def class_name(obj):
    try:
        return obj.get_class().get_name()
    except Exception:
        return type(obj).__name__


def get_prop(obj, name, default=None):
    try:
        return obj.get_editor_property(name)
    except Exception:
        return default


def get_sub(cls_name):
    cls = getattr(unreal, cls_name, None)
    if cls is None:
        return None
    try:
        return unreal.get_editor_subsystem(cls)
    except Exception:
        return None


def first_call(label, candidates):
    """candidates: list of zero-arg lambdas; return first that works."""
    for c in candidates:
        try:
            r = c()
            if r is not None:
                return r
        except Exception:
            continue
    return None


# ------------------------------------------------------------------ gathering

def parse_args():
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("--folder")
    ap.add_argument("--out", default=os.path.expanduser("~/Downloads/unreal-export"))
    ap.add_argument("--species")
    ap.add_argument("--inventory-only", action="store_true")
    ap.add_argument("--bake", choices=["disabled", "simple", "mesh"], default="disabled")
    ap.add_argument("--source-model", choices=["auto", "on", "off"], default="auto")
    buf = io.StringIO()
    try:
        with contextlib.redirect_stderr(buf):
            args, unknown = ap.parse_known_args(sys.argv[1:])
    except SystemExit:
        log("argument error: %s" % (buf.getvalue().strip() or "bad value"))
        try:
            os.makedirs(os.path.expanduser("~/Downloads/unreal-export"), exist_ok=True)
            set_log_dir(os.path.expanduser("~/Downloads/unreal-export"))
        except Exception:
            pass
        return None
    if unknown:
        log("ignoring unknown args: %s" % unknown)
    return args


def list_folder(folder):
    """Return [(object_path, class_name_from_registry)] without loading."""
    out = []
    ar = safe(lambda: unreal.AssetRegistryHelpers.get_asset_registry(), "asset registry")
    if ar is not None:
        datas = safe(lambda: ar.get_assets_by_path(folder, recursive=True), "get_assets_by_path", [])
        for d in datas or []:
            cn = None
            try:
                cn = str(d.asset_class_path.asset_name)
            except Exception:
                cn = str(getattr(d, "asset_class", "?"))
            try:
                p = "%s.%s" % (d.package_name, d.asset_name)
            except Exception:
                p = str(d.object_path)
            out.append((p, cn))
    if not out:
        paths = safe(lambda: unreal.EditorAssetLibrary.list_assets(folder, True, False), "list_assets", [])
        for p in paths or []:
            out.append((str(p), "?"))
    return out


def load(path):
    return safe(lambda: unreal.EditorAssetLibrary.load_asset(path), "load_asset", None, path)


def gather_scope(args):
    """-> (assets list of (path, registry_class, obj), folder_or_None)"""
    items = []
    folder = args.folder
    if folder:
        for p, cn in list_folder(folder):
            items.append((p, cn, load(p)))
        return items, folder
    sel = safe(lambda: list(unreal.EditorUtilityLibrary.get_selected_assets()), "get_selected_assets", [])
    for o in sel or []:
        items.append((asset_path(o), class_name(o), o))
    if items:
        return items, None
    folders = safe(lambda: list(unreal.EditorUtilityLibrary.get_selected_folder_paths()),
                   "get_selected_folder_paths", [])
    for f in folders or []:
        # selected folder paths may come as '/All/Game/...' in the Content Browser
        f = str(f)
        if f.startswith("/All/"):
            f = f[4:]
        folder = folder or f
        for p, cn in list_folder(f):
            items.append((p, cn, load(p)))
    return items, folder


# ------------------------------------------------------------------ inventory

TEX = getattr(unreal, "Texture", None)
STATIC_MESH = unreal.StaticMesh
SKEL_MESH = unreal.SkeletalMesh


def is_a(obj, name):
    cls = getattr(unreal, name, None)
    try:
        return cls is not None and isinstance(obj, cls)
    except Exception:
        return False


def nanite_info(sm, sms):
    ns = None
    if sms is not None:
        ns = safe(lambda: sms.get_nanite_settings(sm), "get_nanite_settings", None, asset_path(sm))
    if ns is None:
        ns = get_prop(sm, "nanite_settings")
    if ns is None:
        return None
    d = {}
    for n in ("enabled", "fallback_target", "fallback_percent_triangles", "fallback_relative_error",
              "position_precision", "preserve_area", "keep_percent_triangles", "trim_relative_error"):
        v = get_prop(ns, n, "<n/a>")
        d[n] = short(v) if v != "<n/a>" else v
    d["_all"] = dump_props(ns)["properties"]
    return d


def slot_list(mesh, skeletal):
    slots = []
    arr = None
    for n in (("materials",) if skeletal else ("static_materials",)):
        arr = get_prop(mesh, n)
    if arr is None:
        return slots
    for i, s in enumerate(arr):
        mi = get_prop(s, "material_interface")
        slots.append({"index": i, "slot_name": str(get_prop(s, "material_slot_name", "")),
                      "material": asset_path(mi), "material_class": class_name(mi) if mi else None})
    return slots


def mesh_info(mesh, sms, skms):
    path = asset_path(mesh)
    d = {"path": path, "class": class_name(mesh)}
    if is_a(mesh, "StaticMesh"):
        n = safe(lambda: sms.get_lod_count(mesh), "get_lod_count", None, path) if sms else None
        d["lod_count"] = n
        lods = []
        for i in range(n or 0):
            v = safe(lambda i=i: sms.get_number_verts(mesh, i), "get_number_verts", None, path)
            lods.append({"lod": i, "verts": v})
        d["lods"] = lods
        d["nanite"] = nanite_info(mesh, sms)
        d["slots"] = slot_list(mesh, False)
        d["has_source_model"] = None
        sm = get_prop(mesh, "source_models")
        if sm is not None:
            try:
                d["has_source_model"] = len(sm)
            except Exception:
                pass
    else:
        n = safe(lambda: skms.get_lod_count(mesh), "get_lod_count", None, path) if skms else None
        d["lod_count"] = n
        lods = []
        for i in range(n or 0):
            v = safe(lambda i=i: skms.get_num_verts(mesh, i), "get_num_verts", None, path) if skms else None
            s = safe(lambda i=i: skms.get_num_sections(mesh, i), "get_num_sections", None, path) if skms else None
            lods.append({"lod": i, "verts": v, "sections": s})
        d["lods"] = lods
        skel = get_prop(mesh, "skeleton")
        d["skeleton"] = asset_path(skel)
        bones = first_call("bones", [
            lambda: len(mesh.get_editor_property("ref_skeleton").get_bone_names()),
            lambda: len(skel.get_editor_property("bone_tree")),
            lambda: len(mesh.get_bone_names()),
            lambda: len(unreal.AnimationLibrary.get_bone_names_for_skeleton(skel)),
        ])
        d["bone_count"] = bones
        d["slots"] = slot_list(mesh, True)
        d["nanite_assembly_props"] = [n for n in dir(mesh) if "assembl" in n.lower() or "nanite" in n.lower()]
    d["dump"] = dump_props(mesh)
    return d


def material_info(mat, cache):
    path = asset_path(mat)
    if path in cache:
        return cache[path]
    d = {"path": path, "class": class_name(mat), "parent_chain": [], "scalars": {}, "vectors": {},
         "textures": {}, "switches": {}, "overrides": {}}
    cache[path] = d
    mel = unreal.MaterialEditingLibrary

    chain, cur, base = [], mat, mat
    for _ in range(12):
        parent = get_prop(cur, "parent") if is_a(cur, "MaterialInstance") else None
        if parent is None:
            break
        chain.append(asset_path(parent))
        cur = base = parent
    d["parent_chain"] = chain
    d["base_material"] = asset_path(base)
    d["blend_mode"] = enum_name(get_prop(base, "blend_mode", "?"))
    two = get_prop(base, "two_sided")
    d["base_two_sided"] = two
    shading = get_prop(base, "shading_model")
    d["shading_model"] = enum_name(shading) if shading is not None else None
    # instance overrides of base properties (two sided / blend mode can be overridden per instance)
    eff_two, eff_blend = two, d["blend_mode"]
    two_set = blend_set = False
    cur = mat
    for _ in range(12):
        if not is_a(cur, "MaterialInstance"):
            break
        o = get_prop(cur, "base_property_overrides")
        if o is not None:
            if not two_set and get_prop(o, "override_two_sided") is True:
                eff_two = get_prop(o, "two_sided")
                two_set = True
                d["overrides"]["two_sided@" + str(asset_path(cur))] = eff_two
            if not blend_set and get_prop(o, "override_blend_mode") is True:
                blend_set = True
                eff_blend = enum_name(get_prop(o, "blend_mode", "?"))
                d["overrides"]["blend_mode@" + str(asset_path(cur))] = eff_blend
        cur = get_prop(cur, "parent")
        if cur is None:
            break
    d["two_sided"] = eff_two
    d["effective_blend_mode"] = eff_blend

    assoc = getattr(unreal, "MaterialParameterAssociation", None)
    glob = getattr(assoc, "GLOBAL_PARAMETER", None) if assoc else None

    def param_value(getter, name):
        for args in ((mat, name, glob), (mat, name)):
            try:
                return getter(*args)
            except Exception:
                continue
        return None

    def names(fn):
        return [str(n) for n in (safe(lambda: fn(mat), getattr(fn, "__name__", "?"), [], path) or [])]

    for n in names(mel.get_scalar_parameter_names):
        d["scalars"][n] = short(param_value(mel.get_material_instance_scalar_parameter_value, n))
    for n in names(mel.get_vector_parameter_names):
        d["vectors"][n] = short(param_value(mel.get_material_instance_vector_parameter_value, n))
    for n in names(mel.get_static_switch_parameter_names):
        d["switches"][n] = short(param_value(mel.get_material_instance_static_switch_parameter_value, n))
    for n in names(mel.get_texture_parameter_names):
        tex = param_value(mel.get_material_instance_texture_parameter_value, n)
        d["textures"][n] = asset_path(tex)
    # direct override arrays as a second source (works even if the library calls fail)
    if is_a(mat, "MaterialInstance"):
        for n in ("scalar_parameter_values", "vector_parameter_values", "texture_parameter_values"):
            arr = get_prop(mat, n)
            if arr is not None:
                d["overrides"][n] = [short(x) for x in list(arr)[:64]]
        for tv in list(get_prop(mat, "texture_parameter_values") or []):
            info = get_prop(tv, "parameter_info")
            nm = str(get_prop(info, "name", "")) if info is not None else ""
            val = get_prop(tv, "parameter_value")
            if nm and not d["textures"].get(nm):
                d["textures"][nm] = asset_path(val)
    return d


def texture_info(t):
    d = {"path": asset_path(t), "class": class_name(t)}
    for n in ("compression_settings", "srgb", "lod_group", "never_stream", "mip_gen_settings",
              "virtual_texture_streaming", "compression_no_alpha", "flip_green_channel",
              # Applied at texture build time, not in the source the PNG exporter writes.
              "adjust_brightness", "adjust_brightness_curve", "adjust_saturation", "adjust_vibrance",
              "adjust_rgb_curve", "adjust_hue"):
        v = get_prop(t, n, "<n/a>")
        d[n] = enum_name(v) if hasattr(v, "name") else short(v)
    sx = safe(lambda: t.blueprint_get_size_x(), "size_x", None, d["path"])
    sy = safe(lambda: t.blueprint_get_size_y(), "size_y", None, d["path"])
    d["size"] = [sx, sy]
    return d


# ------------------------------------------------------------------ level scan

def scan_level(mesh_paths):
    """Actors whose components reference scope meshes. Returns list of dicts."""
    found = []
    eas = get_sub("EditorActorSubsystem")
    if eas is None:
        log("level scan: EditorActorSubsystem unavailable")
        return found, []
    actors = safe(lambda: list(eas.get_all_level_actors()), "get_all_level_actors", [])
    log("level scan: %d actors in the open level" % len(actors or []))
    hit_actors = []
    for a in actors or []:
        hit = False
        for cname, prop_names in (("StaticMeshComponent", ("static_mesh",)),
                                  ("SkeletalMeshComponent", ("skeletal_mesh_asset", "skeletal_mesh"))):
            cls = getattr(unreal, cname, None)
            if cls is None:
                continue
            comps = safe(lambda a=a, cls=cls: list(a.get_components_by_class(cls)), "get_components_by_class",
                         [], asset_path(a))
            for c in comps or []:
                mesh = None
                for pn in prop_names:
                    mesh = get_prop(c, pn)
                    if mesh is not None:
                        break
                mp = asset_path(mesh)
                if mp not in mesh_paths:
                    continue
                hit = True
                rec = {"actor": str(safe(lambda: a.get_actor_label(), "label", "?")),
                       "actor_class": class_name(a), "component": str(c.get_name()),
                       "component_class": class_name(c), "mesh": mp, "transforms": []}
                if is_a(c, "InstancedStaticMeshComponent"):
                    n = safe(lambda c=c: c.get_instance_count(), "get_instance_count", 0, rec["actor"]) or 0
                    rec["instance_count"] = n
                    for i in range(n):
                        t = safe(lambda c=c, i=i: c.get_instance_transform(i, True), "get_instance_transform",
                                 None, rec["actor"])
                        if isinstance(t, tuple):
                            t = t[-1]
                        if t is not None:
                            rec["transforms"].append(t)
                else:
                    t = safe(lambda c=c: c.get_world_transform(), "get_world_transform", None, rec["actor"])
                    if t is not None:
                        rec["transforms"].append(t)
                found.append(rec)
        if hit:
            hit_actors.append(a)
    return found, hit_actors


def to_gltf_matrix(t):
    """UE Transform (cm, Z up) -> 16 floats column-major, glTF space (m, Y up). See docstring."""
    def loc(x, y, z):
        v = t.transform_location(unreal.Vector(x, y, z))
        return (v.x, v.y, v.z)

    def P(v):
        return (v[1], v[2], v[0])

    o = loc(0, 0, 0)
    cols = []
    # glTF basis e_gx,e_gy,e_gz = P(ue basis (0,1,0), (0,0,1), (1,0,0)); P^-1 e_g* = those ue axes
    for b in ((0, 1, 0), (0, 0, 1), (1, 0, 0)):
        p = loc(*b)
        cols.append(P((p[0] - o[0], p[1] - o[1], p[2] - o[2])))
    tr = P(o)
    m = []
    for c in cols:
        m += [c[0], c[1], c[2], 0.0]
    m += [tr[0] * 0.01, tr[1] * 0.01, tr[2] * 0.01, 1.0]
    return [round(x, 6) for x in m]


# ------------------------------------------------------------------ export

def make_options(args, source_model, bake_override=None):
    o = unreal.GLTFExportOptions()
    bake_enum = getattr(unreal, "GLTFMaterialBakeMode", None)
    bake = bake_override or args.bake
    settings = [
        ("texture_image_format", getattr(getattr(unreal, "GLTFTextureImageFormat", None), "PNG", None)),
        ("bake_material_inputs",
         getattr(bake_enum, {"disabled": "DISABLED", "simple": "SIMPLE", "mesh": "USE_MESH_DATA"}[bake], None)
         if bake_enum else None),
        ("default_level_of_detail", 0),
        ("export_vertex_colors", True),
        ("export_source_model", bool(source_model)),
        ("export_uniform_scale", 0.01),
        ("adjust_normalmaps", True),
        ("export_vertex_skin_weights", False),
        ("export_morph_targets", False),
        ("export_animation_sequences", False),
        ("export_lights", False),
        ("export_cameras", False),
        ("use_mesh_quantization", False),
        ("skip_near_default_values", False),
        ("texture_image_quality", 100),
    ]
    for k, v in settings:
        if v is None:
            log("option %s: skipped (value unavailable)" % k)
            continue
        try:
            o.set_editor_property(k, v)
        except Exception as e:  # noqa: BLE001
            log("option %s not set: %s" % (k, e))
    return o


def empty_actor_set():
    for f in (lambda: unreal.Set(unreal.Actor), lambda: set()):
        try:
            return f()
        except Exception:
            continue
    return None


def run_gltf_export(obj, path, opts, actors=None):
    """Return (ok, messages list)."""
    msgs = []
    sel = empty_actor_set()
    if actors:
        for a in actors:
            try:
                sel.add(a)
            except Exception:
                pass
    fn = getattr(unreal.GLTFExporter, "export_to_gltf", None)
    if fn is not None:
        r = fn(obj, path, opts, sel)
        if r is not None:
            for k in ("errors", "warnings", "suggestions"):
                v = get_prop(r, k)
                if v:
                    msgs.append("%s: %s" % (k, [str(x) for x in list(v)[:10]]))
        return os.path.exists(path), msgs
    task = unreal.AssetExportTask()
    task.object, task.filename, task.automated = obj, path, True
    task.replace_identical, task.prompt = True, False
    task.options = opts
    task.exporter = unreal.GLTFExporter()
    ok = unreal.Exporter.run_asset_export_task(task)
    msgs += [str(e) for e in list(task.errors or [])]
    return bool(ok) and os.path.exists(path), msgs


def gltf_stats(path):
    d = {"file": os.path.basename(path), "triangles": 0, "meshes": 0, "nodes": 0, "materials": 0,
         "double_sided": 0, "images": 0}
    try:
        with open(path, "r", encoding="utf-8") as f:
            g = json.load(f)
        acc = g.get("accessors", [])
        for m in g.get("meshes", []):
            d["meshes"] += 1
            for p in m.get("primitives", []):
                if "indices" in p and p.get("mode", 4) == 4:
                    d["triangles"] += acc[p["indices"]]["count"] // 3
        d["nodes"] = len(g.get("nodes", []))
        d["materials"] = len(g.get("materials", []))
        d["double_sided"] = sum(1 for m in g.get("materials", []) if m.get("doubleSided"))
        d["images"] = len(g.get("images", []))
        d["material_names"] = [m.get("name") for m in g.get("materials", [])]
    except Exception as e:  # noqa: BLE001
        d["error"] = str(e)
    return d


def export_mesh(mesh, out_dir, args, used_names):
    name = mesh.get_name()
    base, k = name, 1
    while base in used_names:
        k += 1
        base = "%s_%d" % (name, k)
    used_names.add(base)
    path = os.path.join(out_dir, base + ".gltf")
    is_nanite = False
    attempts = {"auto": [True, False], "on": [True], "off": [False]}[args.source_model]
    if is_a(mesh, "SkeletalMesh"):
        attempts = [False]  # source model option is a static mesh notion
    result = {"file": None, "stats": None, "messages": [], "source_model_used": None}
    for src in attempts:
        for bake in ([args.bake] if args.bake == "disabled" else [args.bake, "disabled"]):
            try:
                if os.path.exists(path):
                    os.remove(path)  # our own previous output only
                ok, msgs = run_gltf_export(mesh, path, make_options(args, src, bake))
                result["messages"] += msgs
                if not ok:
                    log("%s: export source_model=%s bake=%s produced no file %s" % (name, src, bake, msgs))
                    continue
                st = gltf_stats(path)
                log("%s: exported source_model=%s bake=%s -> %s" % (name, src, bake, st))
                if st.get("triangles", 0) == 0:
                    continue
                result.update(file=os.path.basename(path), stats=st, source_model_used=src, bake=bake)
                return result
            except Exception as e:  # noqa: BLE001
                err(name, "gltf export (source_model=%s bake=%s): %s: %s" % (src, bake, type(e).__name__, e))
    if os.path.exists(path) and result["file"] is None:
        result.update(file=os.path.basename(path), stats=gltf_stats(path))
        err(name, "glTF has 0 triangles in every attempt")
    return result


def export_texture(tex, tex_dir):
    name = tex.get_name()
    tp = asset_path(tex)
    if tp in EXPORTED_TEX:
        return EXPORTED_TEX[tp]
    key = str(tp).split(".")[0].replace("/", "_")
    if key.startswith("_Game"):
        key = key[len("_Game"):]
    key = key.lstrip("_") or name
    path = os.path.join(tex_dir, key + ".png")
    if os.path.exists(path):
        os.remove(path)  # stale output of an earlier run
    r = export_texture_to(tex, tex_dir, name, key, path)
    if r:
        EXPORTED_TEX[tp] = r
    return r


def export_texture_to(tex, tex_dir, name, key, path):
    try:
        task = unreal.AssetExportTask()
        task.object, task.filename, task.automated = tex, path, True
        task.replace_identical, task.prompt = True, False
        exp = getattr(unreal, "TextureExporterPNG", None)
        if exp is not None:
            task.exporter = exp()
        ok = unreal.Exporter.run_asset_export_task(task)
        if os.path.exists(path):
            return os.path.basename(path)
        err(name, "texture export produced no file (ok=%s errors=%s)" % (ok, list(task.errors or [])))
    except Exception as e:  # noqa: BLE001
        err(name, "texture export via task: %s: %s" % (type(e).__name__, e))
    try:
        for fn in os.listdir(tex_dir):
            if fn.startswith(name + "."):
                os.remove(os.path.join(tex_dir, fn))  # stale, or another texture's
        unreal.AssetToolsHelpers.get_asset_tools().export_assets([asset_path(tex).split(".")[0]], tex_dir)
        for fn in os.listdir(tex_dir):
            if fn.startswith(name + "."):
                new = key + os.path.splitext(fn)[1]
                os.replace(os.path.join(tex_dir, fn), os.path.join(tex_dir, new))
                return new
    except Exception as e:  # noqa: BLE001
        err(name, "texture export via asset tools: %s: %s" % (type(e).__name__, e))
    return None


def role_of(param):
    n = param.lower().replace("_", "").replace(" ", "")
    tokens = set(t.lower() for t in re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", param).replace("_", " ").split())
    if "normal" in n:
        return "normal"
    if any(k in n for k in ("opacity", "translucen", "subsurf", "transmiss", "thickness")) or "sss" in tokens:
        return "translucency" if "opacity" not in n else "opacity"
    if tokens & {"orm", "arm", "packed"} or (
            "mask" in tokens and not any(k in n for k in ("color", "albedo", "diffuse"))):
        return "orm"
    if "rough" in n:
        return "roughness"
    if any(k in n for k in ("basecolor", "albedo", "diffuse", "color")):
        return "base_color"
    return "other"


# ------------------------------------------------------------------ main

def main():
    args = parse_args()
    if args is None:
        return 2

    def init_out(species):
        out_dir = os.path.join(os.path.expanduser(args.out), species)
        tex_dir = os.path.join(out_dir, "textures")
        os.makedirs(tex_dir, exist_ok=True)
        set_log_dir(out_dir)
        return out_dir, tex_dir

    species = args.species or (args.folder or "").rstrip("/").split("/")[-1].lower() or None
    if species:
        out_dir, tex_dir = init_out(species)
    items, folder = gather_scope(args)
    if not items:
        log("No assets: pass --folder /Game/... or select assets/folder in the Content Browser.")
        return 2

    if not species:
        src = (folder or "").rstrip("/").split("/")[-1] or next((i[0].split("/")[-1].split(".")[0] for i in items), "tree")
        species = src.lower()
        out_dir, tex_dir = init_out(species)
    log("export_trees v%d  species=%s  out=%s  folder=%s  %s" % (
        TOOL_VERSION, species, out_dir, folder, datetime.datetime.now().isoformat()))
    try:
        log("engine version: %s" % unreal.SystemLibrary.get_engine_version())
    except Exception:
        pass

    sms, skms = get_sub("StaticMeshEditorSubsystem"), get_sub("SkeletalMeshEditorSubsystem")
    inv = {"tool_version": TOOL_VERSION, "folder": folder, "assets": [], "materials": {}, "textures": {},
           "referenced_outside_scope": [], "level_actors": []}
    meshes, mat_cache, tex_objs = [], {}, {}
    scope_paths = set()

    for path, rc, obj in items:
        rec = {"path": path, "registry_class": rc, "class": class_name(obj) if obj else None}
        scope_paths.add(asset_path(obj) if obj else path)
        if obj is None:
            rec["note"] = "failed to load"
        else:
            if is_a(obj, "StaticMesh") or is_a(obj, "SkeletalMesh"):
                rec.update(safe(lambda: mesh_info(obj, sms, skms), "mesh_info", {}, path))
                meshes.append(obj)
            elif is_a(obj, "MaterialInterface"):
                m = safe(lambda: material_info(obj, mat_cache), "material_info", None, path)
                rec["detail"] = m
            elif is_a(obj, "Texture2D") or (TEX is not None and isinstance(obj, TEX)):
                rec.update(safe(lambda: texture_info(obj), "texture_info", {}, path))
                tex_objs[asset_path(obj)] = obj
            else:
                rec["dump"] = safe(lambda: dump_props(obj), "dump_props", {}, path)
        inv["assets"].append(rec)
        log("%-28s %s" % (rec.get("class") or rc, path))

    # materials used by meshes but outside the scope, plus their textures
    for rec in inv["assets"]:
        for s in rec.get("slots", []) or []:
            mp = s.get("material")
            if mp and mp not in mat_cache:
                mo = load(mp)
                if mo is not None:
                    safe(lambda: material_info(mo, mat_cache), "material_info", None, mp)
                    if mp not in scope_paths:
                        inv["referenced_outside_scope"].append(mp)
    for m in list(mat_cache.values()):
        for tp in m["textures"].values():
            if tp and tp not in tex_objs:
                to = load(tp)
                if to is not None:
                    tex_objs[tp] = to
                    inv["textures"][tp] = safe(lambda to=to: texture_info(to), "texture_info", {}, tp)
                    if tp not in scope_paths:
                        inv["referenced_outside_scope"].append(tp)
    for rec in inv["assets"]:
        if rec.get("class") and "Texture" in rec["class"] and rec["path"] not in inv["textures"]:
            inv["textures"][rec["path"]] = {k: v for k, v in rec.items() if k != "registry_class"}
    inv["materials"] = mat_cache

    placements, hit_actors = [], []
    scan = safe(lambda: scan_level({asset_path(m) for m in meshes}), "level scan", ([], []))
    placements, hit_actors = scan
    for p in placements:
        inv["level_actors"].append({k: (v if k != "transforms" else
                                        [str(t) for t in v[:5]]) for k, v in p.items()})
    log("level actors referencing scope meshes: %d components" % len(placements))

    with open(os.path.join(out_dir, "inventory.json"), "w", encoding="utf-8") as f:
        json.dump(jsonable(inv), f, indent=1)

    manifest = {"tool_version": TOOL_VERSION, "species": species, "folder": folder, "assets": [],
                "textures_exported": {}, "errors": ERRORS, "placements_file": None, "assembly_file": None}

    def write_manifest():
        try:
            manifest["output_files"] = sorted(
                os.path.relpath(os.path.join(r, fn), out_dir) for r, _, fs in os.walk(out_dir) for fn in fs)
            with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8") as f:
                json.dump(jsonable(manifest), f, indent=1)
        except Exception as e:  # noqa: BLE001
            log("manifest write failed: %s: %s" % (type(e).__name__, e))

    try:
        if not args.inventory_only:
            write_manifest()
            used = set()
            for mesh in meshes:
                try:
                    mp = asset_path(mesh)
                    info = next((r for r in inv["assets"] if r["path"].split(".")[0] == mp.split(".")[0]), {})
                    res = export_mesh(mesh, out_dir, args, used)
                    slots = []
                    for s in info.get("slots", []) or []:
                        m = mat_cache.get(s.get("material"))
                        roles = {}
                        if m:
                            for pn, tp in m["textures"].items():
                                if not tp:
                                    continue
                                roles.setdefault(role_of(pn), []).append(
                                    {"param": pn, "texture": tp, "file": None})
                        slots.append({"slot": s.get("slot_name"), "material": s.get("material"),
                                      "base_material": m and m.get("base_material"),
                                      "two_sided": m and m.get("two_sided"),
                                      "blend_mode": m and m.get("effective_blend_mode"),
                                      "textures_by_role": roles})
                    lod0 = (info.get("lods") or [{}])[0]
                    manifest["assets"].append({
                        "name": mesh.get_name(), "class": class_name(mesh), "path": mp,
                        "triangles_lod0_unreal": None, "verts_lod0_unreal": lod0.get("verts"),
                        "gltf": res["file"], "gltf_triangles": (res["stats"] or {}).get("triangles"),
                        "gltf_stats": res["stats"], "source_model_used": res.get("source_model_used"),
                        "bake_mode": res.get("bake"), "export_messages": res["messages"], "slots": slots})
                except Exception:  # noqa: BLE001
                    err(asset_path(mesh), "mesh export: " + traceback.format_exc())
                write_manifest()

            # textures referenced by materials of exported meshes
            # Only the summer set of the pack's own textures: the PNG exporter
            # asserts (crashes the editor) on engine float textures such as
            # PivotPainter's, and impostor slots are replaced by our voxel tier.
            needed = {}
            for a in manifest["assets"]:
                for s in a["slots"]:
                    if "impostor" in (s["slot"] or "").lower():
                        continue
                    for lst in s["textures_by_role"].values():
                        for e in lst:
                            if (e["texture"] or "").startswith("/Game/") and "winter" not in e["param"].lower():
                                needed[e["texture"]] = None
            for tp in needed:
                try:
                    to = tex_objs.get(tp) or load(tp)
                    if to is not None:
                        needed[tp] = export_texture(to, tex_dir)
                except Exception:  # noqa: BLE001
                    err(tp, "texture export: " + traceback.format_exc())
            manifest["textures_exported"] = needed
            for a in manifest["assets"]:
                for s in a["slots"]:
                    for lst in s["textures_by_role"].values():
                        for e in lst:
                            e["file"] = needed.get(e["texture"])
            write_manifest()

            # level assembly + placements
            if placements and hit_actors:
                try:
                    gl_path = os.path.join(out_dir, species + "_assembly.gltf")
                    try:
                        world = first_call("world", [
                            lambda: get_sub("UnrealEditorSubsystem").get_editor_world(),
                            lambda: unreal.EditorLevelLibrary.get_editor_world()])
                        ok, msgs = run_gltf_export(world, gl_path, make_options(args, False), hit_actors)
                        if ok:
                            manifest["assembly_file"] = os.path.basename(gl_path)
                            manifest["assembly_stats"] = gltf_stats(gl_path)
                        log("assembly export ok=%s %s" % (ok, msgs))
                    except Exception as e:  # noqa: BLE001
                        err("assembly", "world export: %s: %s" % (type(e).__name__, e))
                    by_mesh = {}
                    for p in placements:
                        for t in p["transforms"]:
                            m = safe(lambda t=t: to_gltf_matrix(t), "to_gltf_matrix", None, p["mesh"])
                            if m:
                                by_mesh.setdefault(p["mesh"], []).append(m)
                    with open(os.path.join(out_dir, "placements.json"), "w", encoding="utf-8") as f:
                        json.dump({"conversion": "UE cm Z-up -> glTF m Y-up; P(x,y,z)=(y,z,x), translation*0.01; "
                                   "M=P*T_lin*P^-1; 16 floats column-major; UNVERIFIED axis map (see docstring)",
                                   "meshes": by_mesh}, f)
                    manifest["placements_file"] = "placements.json"
                except Exception:  # noqa: BLE001
                    err("placements", "placements block: " + traceback.format_exc())
    finally:
        write_manifest()

    n_meshes = sum(1 for a in manifest["assets"] if a["gltf"])
    n_tex = sum(1 for v in manifest["textures_exported"].values() if v)
    log("SUMMARY: exported %d meshes, %d textures, %d failures; inventory at %s" % (
        n_meshes, n_tex, len(ERRORS), os.path.join(out_dir, "inventory.json")))
    return 0


try:
    main()
except Exception:  # noqa: BLE001
    log("FATAL: " + traceback.format_exc())
