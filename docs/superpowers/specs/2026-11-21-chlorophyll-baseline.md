# Chlorophyll Engine baseline: facts, frame inferences, Meridian capabilities, cross-check

Status: draft for the gap brainstorm (2026-11-21). Inputs: everything known
about the Chlorophyll Engine (CE) so far, the five reference frames
(`assets/reference/chlorophyll/*.png`, `assets/reference/target-meadow.png`,
git-ignored), Meridian's capabilities on main (3f91f33), and the look-gap
audit (`2026-11-21-look-gap-audit.md`, G1-G11).

## 1. What we know about CE, graded

Grades: **S** = stated by the author (X post, video description; link or
quote; his replies are collected in
`../research/2026-11-21-chlorophyll-youtube-replies.md` and
`../research/2026-11-21-chlorophyll-x-replies.md`), **P** = second-hand (press paraphrase of his posts, not read at the
source), **I** = our inference from his frames (Section 2).

| # | Claim | Grade | Source |
|---|---|---|---|
| F1 | All raster, not path traced | S | X reply, 2026-11-17 ("no it's all raster") |
| F2 | GPU-driven, meshlets (cluster LOD / virtual geometry), small triangles rendered as voxels "with minimal to no detail loss", no impostors | S | X replies |
| F3 | Voxels generated during the LOD build (a modified meshoptimizer, custom DAG builder) | S | YouTube replies; shazoo.ru, 2026-11-10 |
| F4 | Very far: proxies without per-instance data; a per-tile voxel hierarchy traversed as one aggregate; proxy tiles unique, streamed, built on the player's hardware; low-end can swap a 2D representation | S | video description |
| F5 | Hybrid screen-space + voxel GI; no hardware ray tracing anywhere (min spec is for mesh shaders) | S | YouTube reply; user-shared architecture list |
| F6 | Clouds are 2.5D, not truly volumetric, art-directed, still improving | S | X post |
| F7 | Weather systems built in from the ground up (big part of his game) | S | video description |
| F8 | All terrain and scattering in Houdini, rule based; forest edges: size gradient on the skirt, full-canopy trees outside, "sparse bottom" | S | X reply |
| F9 | 1440p, RTX 3080: ~130 fps average uncapped, ~200 aerial, ~80 dense forest; RTX 3070 laptop ~60 fps; min. RTX 2060 / RX 6400. "No DLSS 5" means no generative filter, not no upscaler/AA (see F16) | S | YouTube pinned comment + replies |
| F10 | First showcase: ~3M trees, "1M instances x 4M tris" high-poly vegetation; later test 10M trees over 15 km² | S / P | X post (2113305054639784159), profile snippet |
| F11 | Hand-crafted engine, ~7-8 months of work | S / P | X post; shazoo |
| F12 | target-meadow frame: 60 fps (vsync), CPU 0.96 ms, GPU 7.67 ms; aerial views ~5 ms GPU; never below ~90 fps with GI on, ~120 average (RTX 3080) | S (his overlay) / S | target-meadow.png; X replies, Oct 22 and Nov 2 |
| F13 | Rivers, cliffs, hiking paths WIP (meshes / terrain splats) | S | video description |
| F14 | Earlier Godot 4.4 forest demo (2025-08), plausibly his: Houdini heightmap, colour map, grass-density map; Houdini points by slope + curvature | P (authorship unconfirmed) | 80.lv |
| F15 | D3D12 only (Vulkan maybe later); C++ engine, luau scripting; CPU mostly idle | S | YouTube replies |
| F16 | AA: DLSS/FSR guidance "only gets you so far"; his fixes: a lower mip bias on alpha-tested grass and a motion-based "smear mask" that sharpens selectively; reactive masks didn't help; alpha grass still smears a little, trees don't | S | YouTube reply |
| F17 | Visibility-buffer deferred path (his answer to foliage overdraw): most culling and all shading in compute; only the raster into the visibility buffer, output and a few effects on the graphics pipeline; mesh shaders (not compute emulation). Trees and bushes fully triangles or voxels, only grass and small ground cover alpha tested; no A2C. "Virtualize a lot" to keep a semi-constant workload | S | YouTube replies; X replies, Oct 22, Nov 2, Dec 15 2025 |
| F18 | Virtual textures on the terrain | S | YouTube reply |
| F19 | Shadows semi-static (VSM-like) plus a small dynamic map around the player for animated trees and characters; far animated trees get contact shadows only | S | YouTube reply |
| F20 | Tonemap AgX with contrast control; BRDF Lambert + GGX, EON for some materials (Burley made no measurable difference for foliage and bark); models fitted to measured data for grass (terrain material and meshes) and canopy | S | YouTube reply |
| F21 | The per-asset voxel opacity volume (built for GI) gives the light's optical depth through each crown plus light scattered between leaves (optional, for far forests) | S | YouTube reply |
| F22 | Triangles to voxels by screen error; tile proxies past ~1 km drop instance data and DAG traversal | S | YouTube replies |
| F23 | Distant grass goes through the same voxel path as trees; the terrain fakes grass shading (normals, SSS, BRDF) further out, transition visible on steep hills or flying | S | YouTube reply |
| F24 | Forest layout: ~2 m spacing between trees (walkability), size gradient skirt to interior; placement logic being ported from Houdini to an in-engine runtime library | S | YouTube replies |
| F25 | Water: planar reflections + SSR; cliffs unsolved (considering a virtual runtime renderer) | S | YouTube replies |
| F26 | Grass and small ground cover are left out of the shadow map pass; contact (screen-space) shadows alone shadow them ("not really worth the cost"; same in his Godot project) | S | X replies, Oct 29 and Mar 11 |
| F27 | The forest is procedural, the vegetation only partly; a procedural tree simulation is in progress (trunk geometry unfinished). Godot era: assets from an asset pack, Houdini splat maps, grass-type masks and a point cloud for larger objects (terrain simulations + Labs biome tools) | S | X replies, Oct 23 and Mar 11 |
| F28 | A comparable Unreal scene (Quixel vegetation, Nanite voxels) ran under 60 fps on his 3080, close views ~40, Lumen temporally unstable and noisy | S | X reply, Nov 3 |

Unknown (not found anywhere): AA method (DLSS, FSR or his own TAA; F16
names their guidance only), AO, wind, current asset sources, exposure, GI
details, per-pass costs, the smear mask's exact construction.
Best source for these: his X replies tab (needs the user's login) or asking
him directly.

## 2. What his frames show (grade I)

From the five frames (midday sun, high elevation, all compressed YouTube
phone captures: judge fine detail only qualitatively).

| # | Observation | Frames | What it implies technically |
|---|---|---|---|
| R1 | Trees at every distance read as individual rounded crowns with a lit side and a dark side, dark gaps between crowns, a dark crown interior | all | Crown-scale self-shadowing and occlusion survive into the far tier: far trees are lit as volumes with sun visibility (shadowed voxels / shadow maps on the far tier), not flat-shaded points |
| R2 | Trees cast clear shadows onto the meadow, also far away (aerials) | aerials, target-meadow | Sun shadows reach the far field (a far shadow technique over the whole visible area) |
| R3 | Strong mid-scale occlusion: ground darkens under and between trees, forest edges dark at the base, grass darkens toward its roots | target-meadow, meadow-cliff | Screen-space AO and/or voxel/canopy occlusion (fits F5's hybrid GI) |
| R4 | Meadow ground is one continuous saturated lime green; far grass blends into a terrain whose colour matches the blades; no soil shows through | meadows, aerials | Terrain base colour authored to match the grass (Houdini colour map, F14), grass density map; content as much as engine |
| R5 | Grass is luminous and yellow-green toward the sun, darker blue-green away from it | meadows | Foliage transmission / translucency, wrap lighting; possibly a grass-specific shading model |
| R6 | Little haze: distant hills crisp, slightly blue; sky deep blue at the zenith, pale near the horizon | all | Clean atmosphere, aerial perspective kept light |
| R7 | Bright, high-key exposure, saturated greens, moderate contrast, no crushed blacks except deep forest shadow | all | Grade/tonemap tuned for saturated foliage; exposure biased up |
| R8 | Clouds: soft, flat-bottomed cumulus, smooth, no visible noise | all | 2.5D (F6), art-directed |
| R9 | Near grass looks soft/blurred in the meadow frames | meadow-cliff, flower-meadow | Wind motion + motion blur, or video compression; not a target to copy blindly |
| R10 | Dark cliff, brown dirt at clearing edges and paths, river with gravel banks | meadow-cliff, aerials | Content (F13) |

## 3. Meridian capabilities on main (3f91f33)

| Area | What Meridian has |
|---|---|
| Platform | C++20, Vulkan 1.3, mesh shaders, bindless, buffer device addresses, GPU-driven; RTX 4080 dev box |
| Geometry | Cluster LOD DAG (meshoptimizer) with per-level group BVH, GPU traversal, two-phase occlusion culling, Nanite-style LOD cut; masked depth prepass; shared pools |
| Far vegetation | Prototype voxel trees (rasterized voxels past a 2 px switch, coverage-keeping mips, baked at load by ray queries on the BLAS); far-field bricks per world cell (marched), far sun shadow map; no instance-less tile aggregates yet |
| Placement | Integer-exact GPU placement rules (CPU reference), dither patterns, 64 m cells, streamed meadow layers (ring around the camera), ~2.2M live grass instances |
| Terrain | 15 km swissALTI3D heightfield, CDLOD quadtree to 524 km, detail noise, Megascans layer material with triplanar rock, POM, shadow-height map |
| Sun shadows | Raster cascaded shadow maps (5 cascades, cached, texel-snapped, 4-tap PCF) default; terrain via the shadow-height map; far trees via the far shadow map / coarsest cascade; contact shadows (screen-space march); RT shadows as an A/B |
| Ambient | Sky irradiance from the sky-view LUT + a cloud sky capture; crown sky visibility (per vertex); micro occlusion on plants; GI probe clipmap (heightfield + voxel traced, radiance cache, off by default); no screen-space AO |
| Materials | Metal/rough PBR, DFG LUT, alpha-tested foliage, a diffuse transmission factor; no thickness maps, no foliage-specific shading model, no specular occlusion |
| Atmosphere | Bruneton-style LUTs (transmittance, multi-scatter, sky-view, aerial perspective), parametric planet, valley-anchored aerosol layer |
| Clouds | Volumetric layer + cirrus, weather map, Beer shadow map, quarter/half-res march with temporal resolve, outline effect |
| Weather | Presets, fronts, wind profile + integral, weather history; precipitation channel unused |
| Post | Native TAA (motion-adaptive sharpen, Keys cubic history), DLSS as an A/B, energy-conserving bloom, histogram exposure with a highlight cap, grade + AgX baked to a 3D LUT, dither |
| Tools | Motion/shimmer/SSIM gates, flicker metric, cloud metrics, perf overlay + CSV, G panel, cook (KTX2, trees), usd2gltf, Unreal headless export (Megascans Trees) |

## 4. Cross-check: CE facts and frame inferences vs our gaps

| Gap | CE evidence | Meridian today | Gap type | What CE most likely does differently (hypothesis, to verify by research) |
|---|---|---|---|---|
| G1 far forests speckle | R1, R2; F2, F3, F4, F21, F22 | Binary-coverage flat-shaded voxels; voxel albedo from wrong materials | Engine | Voxels as part of the LOD hierarchy carry filtered opacity, normals and occlusion; far tier lit with sun visibility per voxel/brick; tile aggregates prefiltered (density + colour + normal per voxel, à la sparse voxel octree / "voxel LOD" literature) |
| G2 no mid-scale AO | R3; F5, F21 | Micro + crown terms only; GI off | Engine | Screen-space AO (GTAO-class) + voxel occlusion from his GI voxels (F5 states the SS + voxel GI hybrid; F21 the crown optical depth from the same voxels; an AO pass itself is not stated) |
| G3 foliage not luminous | R5 | Weak untinted transmission | Engine (+ maps) | Thin-surface translucency with thickness/translucency maps, wrap or forward-scatter lobe |
| G4 grass bland | R4, R5, R3; F20, F23 | Shading + placeholder terrain colour | Both | Grass colour authored to match the terrain (Houdini maps), translucency, AO in the mat, density map |
| G5 weak contact/micro shadows | R2, R3; F19, F26 | Binary contact march; soft cascade 1; static ground cover (poppies, goldenrod, ferns) still drawn into the cascades | Engine | Semi-static VSM-like maps plus a small dynamic map around the player (F19); grass and small ground cover left out of the shadow pass, contact shadows alone (F26) |
| G6 tree materials wrong | — | Import bug | Pipeline | n/a (our bug) |
| G7 grey washed image | R6, R7 | Undiagnosed | Engine/look | Clean atmosphere + high-key saturated grade |
| G8 terrain colour | R4, R10 | Placeholder material | Content | Houdini colour map (F8, F14) |
| G9 motion shimmer | F9 (1440p), F16 | Native TAA above DLSS shimmer | Engine | AA method unknown (DLSS, FSR or own TAA); he follows their guidance plus a lower mip bias on alpha-tested grass and a motion-based smear mask that sharpens selectively (F16); compressed video hides shimmer: verify with his raw footage only |
| G10 forest edge cost | F9 (~80 fps dense forest on a 3080), F12, F17 | 13.2 ms on a 4080 | Engine | Small triangles to voxels earlier (F2, F22), cheaper far tier (F4), visibility-buffer shading in compute, which he states is his overdraw answer (F17) |
| G11 no far tree shadows | R2 | WIP branch | Engine | Far shadows cover the whole view |

Gaps with no CE evidence either way: G9's AA method and the smear mask's construction.
The next step is research per gap (reputable sources: GDC/SIGGRAPH talks,
papers, vendor docs, engine docs), then the brainstorm.
