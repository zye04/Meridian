# Deep research briefs (2026-11-22)

Three briefs for an external deep-research model. Run each as its own
session: paste the **Shared context** block, then one brief. Results go
in `docs/superpowers/research/2026-11-2x-<letter>-deep-*.md` and are
inputs to our own A/B measurements, never decisions on their own.

Written with the three lanes open on 2026-11-22 in mind:

- **image-stability (main):** G19 motion smear/jitter. DLSS DLAA is the
  default; an A/B of a separate mip bias for the alpha cut-out test is
  running (-1 measured worse in motion); next is cloud small-detail smear.
- **leaf-geometry (`leaf-geometry` branch):** tree leaf cards are traced
  into opaque triangles at cook time (decided, D1/D2: ~30M tree triangles
  over ~30 prototypes); crown coverage across the cluster LOD cut is being
  gated. Grass, flowers, ferns and the meadow stay alpha-tested.
- **G20 (`g20` branch):** cloud shadows stepping, fixed (shadow grid
  snapped in the cloud frame). Not a research topic any more.

So the briefs don't ask "cards or geometry for trees" (decided) or "how
to stop cloud shadows stepping" (fixed). They aim at what those lanes
leave open.

---

## Shared context (paste first in every session)

> You are researching for **Meridian**, a C++20 / Vulkan 1.3 game engine
> whose goal is photorealistic, first-person, open-world nature at grand
> scale (a 15 x 15 km natural scene: terrain, forests, meadows, clouds).
> The visual reference is the "Chlorophyll Engine" (an independent
> developer's engine shown on YouTube/X in 2025-26: cluster LOD meshes up
> close, voxels for far vegetation, screen-space + voxel GI without ray
> tracing, instance-less far proxies, 2.5D clouds). We aim to match four
> of its frames in look and beat it in motion stability.
>
> **Hardware and targets:** RTX 4080, native 2560x1440, DLSS 4 Super
> Resolution in DLAA mode (SDK 310.9.16, preset M) as the default
> anti-aliasing, our own TAA as the fallback; 180 fps on a benchmark
> route is the performance proof. HDR output is mandatory.
> The user's top complaint is **smear, jitter and shimmer in motion**;
> they prefer a stable image over an overly sharp one.
>
> **Architecture today (don't re-propose these, build on them):**
> - GPU-driven rendering with mesh shaders: Nanite-style cluster LOD DAG
>   (meshoptimizer), group BVH traversal, two-phase occlusion culling
>   (HZB), visibility buffer per instance; reverse-Z, camera-relative.
> - Forward shading of clusters into an HDR target plus motion vectors,
>   depth and G-buffer guides; masked (alpha-tested) geometry gets a
>   depth-only prepass, then a shaded pass with depth EQUAL.
> - Vegetation: ~4M placed instances (rocks, plants, Megascans trees from
>   Unreal: beech, hornbeam, black alder). Past a 2 px threshold a tree
>   draws as its prototype's voxels (rasterized camera-facing quads per
>   voxel, mips by distance); far forest cells draw as ray-marched voxel
>   bricks. No impostors (decided).
> - Lighting: raster cascaded sun shadow maps (5 cascades, cached),
>   terrain shadow from a height map, screen-space contact shadows,
>   cloud shadow map. Hardware ray tracing exists only for A/B and bakes,
>   not in the default frame. Ambient: sky irradiance + optional GI probes
>   (clipmap, traced against the heightfield and the far-field voxels;
>   off by default until the look passes). GTAO (XeGTAO-style, with a
>   visibility-bitmask fallback) is specced, not built.
> - Volumetric clouds: quarter-res march with temporal history,
>   bilateral upsample, composited into HDR before the upscaler, with
>   cloud motion vectors where opacity > 0.5.
> - Physically based units (lux, cd/m^2), Bruneton-style atmosphere,
>   AgX tonemapping via a baked 3D LUT.
>
> **How to report:**
> - Answer each numbered question separately. For each: the answer, the
>   evidence, and what it implies for an engine built as above.
> - Cite every claim: URL, author/studio, date. Prefer primary sources
>   (papers, GDC/SIGGRAPH/HPG talks and slides, vendor programming
>   guides, engine source and docs, shipped-title postmortems, developer
>   posts). Prefer 2023-2026; flag anything older that still stands as
>   current practice.
> - Label each claim **established** (documented, shipped), **reported**
>   (one credible source), or **speculative** (your inference). Never
>   mix them in one sentence.
> - Give costs as numbers with their hardware and resolution (GPU ms,
>   memory, triangle counts). If no number exists, say so.
> - Name failure modes and artifacts of every technique, not only its
>   benefits, and what shipping teams gave up to use it.
> - Where our choice above contradicts common practice, say so plainly
>   and explain why practice chose differently.
> - End with a ranked list: change, expected visible gain, cost, risk,
>   and the measurement that would confirm it.

---

## Brief A: dense foliage at distance, and the alpha-tested grass that remains

> **Why:** foliage is the largest remaining source of instability in
> motion (moving frames differ ~6x more than still ones even under
> DLAA). Trees are moving from alpha-tested cards to opaque leaf
> triangles traced from the atlas (~150-400 triangles per former card,
> ~1.2M per tree variant, ~30M over all tree prototypes), simplified by
> a cluster LOD DAG, then switched to per-prototype voxels under 2 px.
> Grass, flowers, ferns and the streamed meadow (millions of instances)
> stay alpha-tested.
>
> 1. **UE 5.7 Nanite Foliage** (and anything Epic published on it since):
>    its exact representation (geometric leaves, voxels, "assemblies"),
>    how it switches between them without popping or a change in
>    coverage/colour, how wind deforms both, its costs and its known
>    artifacts. Compare it point by point with our pipeline above.
> 2. **Keeping crown coverage through simplification:** how do shipping
>    engines stop dense opaque foliage thinning out as LOD simplifies it
>    (area/coverage-preserving simplification, Nanite's "preserve area",
>    leaf merging, aggregation into fewer larger shapes, opacity/colour
>    matching at the voxel switch)? Quantify what coverage loss is
>    visible.
> 3. **Sub-pixel opaque leaf triangles under temporal upscaling:** with no
>    alpha test left, how much shimmer remains from geometric aliasing of
>    leaves that are smaller than a pixel, and from specular aliasing of
>    their normals? Which methods work under DLSS/DLAA (geometric
>    specular AA, Toksvig/vMF normal-variance prefiltering, roughness
>    from LOD level, stochastic or dithered LOD transitions)? How does the
>    DLSS 4 transformer model behave on dense sub-pixel foliage?
> 4. **Alpha-tested grass under DLSS/DLAA:** compare cut-out mip bias
>    (we measured -1 as worse in motion), coverage-preserving alpha mips
>    (Castaño), distance-scaled alpha / "anti-aliased alpha test"
>    (Golus), hashed alpha (Wyman & McGuire), alpha-to-coverage, and
>    replacing near grass blades with geometry. Which stays stable
>    without smear when the camera strafes at 5 m/s or turns at 1 rad/s?
>    What do shipped open-world titles (e.g. Ghost of Tsushima, Horizon,
>    RDR2, Flight Simulator 2024, The Witcher 4 demo) do?
> 5. **Wind on dense geometry and voxels:** how do engines animate
>    millions of leaf triangles and their voxel stand-ins consistently,
>    with correct motion vectors (vertex wind vs hierarchical/pivot
>    wind, Nanite's world-position-offset costs), and what does that do
>    to stability and occlusion culling?
> 6. **Memory for dense foliage:** bytes per triangle achieved by shipping
>    cluster-LOD systems (Nanite compression, meshlet compression,
>    quantization), and streaming strategies, so ~30M+ tree triangles fit
>    comfortably in 16 GB of VRAM next to everything else.
> 7. **Chlorophyll Engine specifically:** everything public about how it
>    renders trees and grass (it reports trees fully as triangles or
>    voxels, only grass and small ground cover alpha-tested, and "alpha
>    grass still smears a little, trees don't"). Find any further detail.

---

## Brief B: leaf and canopy appearance, and light under the canopy without ray tracing

> **Why:** our forests read grey and washed out (G7), distant forests as
> grey speckle rather than tree masses (G1), backlit foliage doesn't glow
> (G3), leaf back faces render black (G13), leaves look posterized and
> glossy up close (G15), fireflies/sparkle on leaves (G18), and there is
> no mid-scale ambient occlusion (G2, GTAO specced). Leaves become opaque
> double-sided triangles (Brief A), so the leaf material now carries the
> whole look.
>
> 1. **Measured leaf optics:** reflectance and transmittance (spectral and
>    in RGB) of the upper and lower surface for European beech, hornbeam
>    and black alder or close temperate broadleaf species (LOPEX, ANGERS,
>    PROSPECT model outputs); specular roughness and Fresnel of the
>    cuticle; how these vary with leaf age and season. Provide numbers we
>    can calibrate against.
> 2. **Thin-leaf shading models in real time:** how shipping engines
>    shade double-sided leaves (UE "Two Sided Foliage" and Substrate,
>    Frostbite, Decima, Snowdrop, id Tech), especially back faces: which
>    normal, which albedo, how transmission is lit (sun through the leaf,
>    sky through the canopy), and how they avoid black back faces and
>    over-bright rims. Include the cost.
> 3. **Shading a canopy at distance:** aggregate appearance models that
>    keep a forest's colour, brightness and specular response consistent
>    from single leaves to cluster LOD to voxels (Bruneton & Neyret's
>    forest rendering, volumetric canopy/foliage BSDFs, SGGX-style
>    microflake phase functions for voxels). How do you match a voxel's
>    shading to the mean of the leaves it replaces?
> 4. **Light under the canopy:** physically grounded values for gap
>    fraction, leaf area index, clumping, sky visibility and irradiance
>    under beech/hornbeam forest in summer; dappled light statistics. We
>    need targets to check our crown-visibility term and GI against.
> 5. **Global illumination for open-world vegetation without ray tracing,
>    2024-2026:** radiance cascades (incl. 3D/world-space variants),
>    voxel GI successors, surfel GI, screen-space GI + probe hybrids, and
>    what Chlorophyll's "screen-space + voxel GI" most likely is. For each:
>    cost at 1440p on an RTX 4080-class GPU, stability in motion (no
>    boiling or lag under a moving camera and wind-animated leaves), and
>    behaviour under dense canopy.
> 6. **Ambient occlusion on foliage:** how GTAO/XeGTAO and visibility
>    bitmask AO behave on dense thin geometry (over-darkening, halos,
>    temporal noise under DLAA); thickness heuristics; how engines
>    combine screen-space AO with a coarser GI/AO term without double
>    darkening.
> 7. **Why forests look grey in renders:** perceptual and physical causes
>    (missing interreflection, exposure metering, haze over dark canopy,
>    tonemapper desaturation of greens) and how photographs of sunlit
>    temperate forests actually distribute luminance and saturation.

---

## Brief C: temporal reconstruction in practice (DLSS 4 / DLAA) and motion clarity

> **Why:** the image smears and jitters in motion; this is the user's top
> priority. We run DLSS 4 Super Resolution as DLAA at native 1440p
> (preset M, 8 jitter phases, Halton(2,3), texture mip bias 0, Ray
> Reconstruction off), feeding colour, depth, motion vectors (UV,
> previous minus current), a transparency mask (blended surfaces and
> cloud coverage) and pre-exposed HDR. Volumetric clouds are rendered at
> quarter resolution with their own temporal history and composited
> before DLSS. We measure clarity as the difference between frames 120
> and 121 at the same pose while moving (jitter), and between a moving
> and a still frame (smear).
>
> 1. **Inputs and their failure signatures:** from NVIDIA's DLSS
>    programming guide, SDK release notes (DLSS 4 / 4.5, presets J, K, L,
>    M) and developer reports: for each input (jitter sequence and phase
>    count, mip bias, motion vectors, depth, exposure / pre-exposure,
>    transparency and bias-current-colour masks, reset), what goes wrong
>    when it's slightly off, and how it looks on screen. Is our setup
>    above correct for DLAA?
> 2. **Presets on foliage:** measured or credibly reported differences
>    between the transformer presets on dense vegetation, thin alpha-tested
>    geometry and disocclusions (ghosting, smear, shimmer). Which preset
>    do shipping nature-heavy games use, and why?
> 3. **Animated content:** best practice for motion vectors on
>    wind-animated foliage, scrolling/procedural textures, and particles,
>    and what DLSS does where motion vectors can't describe the change
>    (the masks, or rendering such content after the upscaler).
> 4. **Volumetrics and the upscaler:** should temporally accumulated
>    volumetric clouds be composited before or after DLSS? What motion
>    vectors and mask values should cloud pixels carry? How do shipped
>    titles (Horizon Forbidden West / Nubis 3, Flight Simulator 2024,
>    Frostbite) handle cloud detail smear under camera motion and their
>    own temporal reprojection?
> 5. **Measuring motion clarity:** how do NVIDIA, Digital Foundry and
>    engine teams quantify smear and temporal instability (metrics, test
>    scenes, camera speeds)? Is a frame-to-frame difference at a fixed
>    pose a sound jitter metric, and what would be better (e.g. FLIP,
>    temporal FLIP, optical-flow-compensated error)?
> 6. **Beyond the upscaler:** engine-side causes of motion smear that no
>    upscaler can fix (shading that changes per frame with jitter, noisy
>    stochastic effects, LOD transitions, history-based effects stacked
>    before the upscaler), and how teams audit a frame for them.
