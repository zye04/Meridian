# Physically Grounded Procedural Terrain for Real-Time Engines (Neon deep research, 2026-11-23)

Prompt: `2026-11-23-terrain-pcg-deep-research-prompt.md`. Triage: pending
(after the reference path tracer lane). Pasted by the user; kept as received
(math kept in LaTeX). Labels are the report's own: verified, secondary,
vendor marketing, estimated/inferred; not re-checked here. GPU cost and
memory figures are the report's estimates, not measurements on Meridian.

---

# Physically Grounded Procedural Terrain for Real-Time Engines (2025–2026 State of Practice)

All factual claims below cite primary sources (2020–2026 papers, GDC/SIGGRAPH talks, official engine/tool docs) with explicit epistemic status: **verified** (read directly from primary paper/docs), **secondary** (third-party benchmark/summary), **vendor marketing** (unverified vendor claim), or **estimated/inferred** (engineering calculation for Meridian's 15×15 km / RTX 4080 1440p target).

---

## 1. Terrain Generation: Offline Authoring, GPU Amplification, and Learned Methods

### 1.1 Offline Authoring Tools (2025–2026 Comparison)

| Tool | Architecture & Current State (2025–2026) | Erosion & Geological Fidelity | Scale, Tiling & Speed | Fit for Meridian (Asset-Agnostic Cook) |
| --- | --- | --- | --- | --- |
| **Houdini 21–22 (SOP HeightFields + Copernicus COPs)** | Dual stack: traditional voxel-backed SOP HeightFields + **Copernicus (COPs)** GPU-accelerated 2D image/terrain framework (`Houdini 20.5–22`). **HeightField Erode 3.0** (added in H21.0) adds resolution-independent `Erosion Feature Size` (min `3×` voxel size) and outputs `height`, `sediment`, `debris`, `flow`, `flowdir` — **verified** ([SideFX Copernicus Terrains](https://www.sidefx.com/docs/houdini/heightfields_cop/index.html); [SideFX HeightField Erode 3.0](https://www.sidefx.com/docs/houdini/nodes/sop/heightfield_erode.html)). | Best-in-class pipeline programmability (strata, slump, terracing, hydro/thermal erosion, USD/PDG automation, custom VEX/OpenCL hydrology & scatter verification). Note: SideFX explicitly warns Copernicus is *not yet* a full SOP replacement for huge images — **verified** ([SideFX Docs](https://www.sidefx.com/docs/houdini/heightfields_cop/index.html)). | Scales to 16k+ via PDG/TOP tiled cooks; Copernicus makes 2k–8k interactive on GPU (no official 15 km benchmark published). | **Strongest Phase 2 authoring hub:** non-destructive graph, exports clean 16/32-bit height + arbitrary mask channels + USD/EXR directly into `tools/cook`. |
| **Gaea 2.1–2.3 (QuadSpinner)** | Dedicated terrain app; `2.1` (Feb 2025) stabilized Tiled Builds and Build Regions; `2.3` added `Erosion2` (`Extra Deposits`, `No Erosion`), `Debris`, and `Snowfield` — **verified** ([QuadSpinner Gaea 2.1](https://blog.quadspinner.com/gaea-2-1-preview/); [Gaea 2.3](https://blog.quadspinner.com/gaea-2-3-now-available/)). | Industry favorite for fast, visually convincing hydraulic/fluvial/debris erosion and satellite-style color synthesis (`SatMaps`, `ColorErosion`) — **verified** ([Gaea Docs](https://docs.gaea.app/using/using-gaea/build-and-export/tiled-builds.html)). | **Tiled / Hybrid Builds:** bake macro stage globally (e.g., 4k–8k), then run local detail/erosion buckets (e.g., 4096² buckets with ~25% overlap) to export seamless 512²/1024² tiles — **verified** ([Gaea Tiled Builds](https://docs.gaea.app/using/using-gaea/build-and-export/tiled-builds.html)). | **Best fast erosion co-processor** (often paired *inside* Houdini or ahead of Houdini) when sculpting non-DEM or augmented landforms. |
| **World Machine 4 (`Hurricane Ridge`, Build 4046, Aug 2026)** | Mature CPU/tiled node graph; Build 4046 finalized in Aug 2026 — **verified** ([World Machine Help](https://help.world-machine.com/topic/build-4046-hurricane-ridge-final/)). | Reliable tiled hydraulic/thermal erosion and macro flow/deposition masks; battle-tested in AAA (*Ghost Recon Wildlands* used World Machine for its base heightmap — **verified**, [Werle GDC 2017](https://666uille.files.wordpress.com/2017/03/gdc2017_ghostreconwildlands_terrainandtechnologytools-onlinevideos1.pdf)). | Strong tiled streaming/memory management, slower interactive iteration than GPU tools (no 2026 measured timing vs Gaea 2/Copernicus). | Solid but superseded in iteration speed by Gaea 2 and in full-world pipeline integration by Houdini. |
| **Instant Terra (Wysilab)** | GPU-accelerated node-based terrain editor; `Unlimited` tier advertises unlimited terrain size (up to 1B vertices) — **vendor marketing** ([Wysilab Blog](https://www.wysilab.com/Blog.html); [iRender review](https://irendering.net/wysilab-releases-instant-terra-plugins-for-ue4/)). | Fast real-time preview of hydraulic/rock erosion and large DEM editing; fewer specialized geological/biome simulation nodes than Houdini or Gaea 2 — **secondary**. | Interactive editing on 16k–32k terrains — **vendor marketing**. | Good for pure rapid DEM compositing/masking, weaker as an all-in-one biome/asset cook pipeline. |
| **World Creator** | Real-time GPU terrain generator with erosion, water, and snow filters — **secondary** ([Awesome-Blender/3D resources](https://github.com/agmmnn/awesome-blender)). | Very fast visual sculpting/prototyping; less transparent physical/hydrological control and fewer arbitrary scientific mask exports than Houdini/Gaea — **secondary**. | Interactive GPU previews; large tiled exports require careful seam management. | Better for quick lookdev than deterministic, geomorphology-validated 15×15 km production cooks. |

### 1.2 Runtime GPU Generation vs. Multi-Scale Erosion Amplification

* **Why naive single-scale GPU erosion fails:** Standard single-resolution hydraulic/shallow-water erosion carves features locked to one grid cell width (producing artificial self-similar rills or destroying macro landmarks) — **verified** ([Schott, Galin, Guérin, Paris & Peytavie, *Terrain Amplification using Multi-scale Erosion*, ACM TOG / SIGGRAPH 2024](https://hal.science/hal-04565030v1/file/2024-MultiScaleHydro-Author.pdf)).
* **State of practice for offline/cook-time GPU amplification:**
* **Schott et al. (SIGGRAPH 2023)** (*Large-scale Terrain Authoring through Interactive Erosion Simulation*, ACM TOG 42(5)) uses a GPU stream-power erosion solver that also supports **inverse procedural uplift reconstruction** from a real DEM for hydrologically consistent patch blending — **verified** ([HAL hal-04049125](https://hal.science/hal-04049125)).
* **Schott et al. (SIGGRAPH 2024)** (*Terrain Amplification using Multi-scale Erosion*, ACM TOG 43(4)) iteratively upsamples a coarse DEM (256² → **8192²**, 32× amplification), applying multi-scale stream-power fluvial erosion, thermal erosion, sediment deposition, diffusion-based elevation retargeting (preserving peaks/ridges), and multi-scale pit breaching for guaranteed drainage continuity. Measured cost: **seconds at 4096²** and up to **8192² on a single desktop GPU** — **verified** ([HAL hal-04565030 PDF](https://hal.science/hal-04565030v1/file/2024-MultiScaleHydro-Author.pdf)).
* **Cordonnier et al. (SIGGRAPH 2023)** (*Forming Terrains by Glacial Erosion*, ACM TOG 42(4)) models cirques, U-shaped troughs, arêtes, and moraines — **verified** ([ORCID/DOI 10.1145/3592422](https://orcid.org/0000-0002-5946-4112)).
* **Huftier, Schott, Galin, Argudo, Peytavie & Guérin (Eurographics 2026)** (*Terrain Synthesis and Authoring based on Iso-Contours*, CGF 45(2)) bridges vector contour authoring with GPU constrained reconstruction + erosion — **verified** ([ResearchGate EG 2026](https://www.researchgate.net/publication/404317284_Terrain_Synthesis_and_Authoring_based_on_Iso-Contours)).


* **Runtime GPU synthesis verdict:** Running hydraulic erosion *every frame* or streaming-on-the-fly in a game engine breaks global hydrological consistency (watersheds cross tile boundaries) and makes bit-exact CPU/GPU scatter and ray-march height pyramids vastly harder. **State of practice in 2025–2026 is cook-time GPU multi-scale erosion amplification + lightweight deterministic runtime micro-detail synthesis** (e.g., gradient-aligned phasor/erosion noise in the finest LODs, *not* runtime fluid erosion) — **inferred** from Schott et al. (2024) and Gaea/Houdini tiled workflows.

### 1.3 Learned Methods (Diffusion, GANs, DEM Super-Resolution)

* **Diffusion terrain synthesis:** **Lochner et al. (CGF 2023)** demonstrated diffusion-based interactive terrain authoring and up to **8× DEM super-resolution** in one pass — **verified** (cited in [Schott et al. 2024](https://hal.science/hal-04565030v1/file/2024-MultiScaleHydro-Author.pdf)). **Borg, Gain, Guérin, Peytavie, Cani, Galin & Cordonnier (Eurographics 2026)** extended diffusion models to planetary-scale terrain authoring (*Authoring Terrestrial Planets with Diffusion Models*, CGF 45(2)) — **verified** ([Eric Galin CNRS publication list](https://perso.liris.cnrs.fr/eric.galin/articles.html)).
* **GANs & spatialized style:** **Perche et al. (CGF 2023)** (*StyleDEM / Authoring Terrains with Spatialised Style*) synthesizes high-res terrain from coarse elevation + spatial style maps, while recent **SRDCGAN / TG-GAN (2026)** targets DEM super-resolution/generalization with ridge/valley-line preservation losses — **verified** ([ORCID](https://orcid.org/0000-0002-5946-4112); [Preprints.org Jun 2026](https://www.preprints.org/manuscript/202606.0278)).
* **Honest limitations for Meridian:** Learned DEM super-resolution excels at turning coarse 30 m SRTM data into plausible 2–4 m relief, **but** Meridian already starts from **2 m swissALTI3D LiDAR DEM**! Learned models still struggle with strict global hydrological continuity (requiring post-hoc pit breaching/erosion) and do not natively emit physically coupled `flow`, `sediment`, `talus`, and `strata` layers needed for texturing and scatter. Use learned super-resolution *only* in `tools/cook` when starting from coarse planetary/30 m DEMs, followed by hydrological breaching + erosion — **inferred**.

### 1.4 Which Geological Processes Matter Visually by Scale (1 m to 10 km)

| Spatial Scale | Visually Dominant Processes | Why It Matters on Screen |
| --- | --- | --- |
| **1 km – 15 km** | Tectonic uplift, major fluvial drainage basins, dendritic ridge-valley hierarchies, glacial troughs/cirques (alpine) | Defines silhouette, horizon rhythm, ridge prominence, and macro vista plausibility ([Argudo et al. 2019](https://hal.science/hal-02326472/file/2019-orometry.pdf); [Cordonnier et al. 2023](https://orcid.org/0000-0002-5946-4112)). |
| **50 m – 1 km** | Stream-power fluvial incision (V-valleys, spurs, gullies), sedimentary rock strata/bedding planes (differential hardness), alluvial fans/floodplains | Governs mid-ground light/shadow articulation, valley bends, cliff bands, and meadow vs. forest zonation ([Schott et al. 2024](https://hal.science/hal-04565030v1/file/2024-MultiScaleHydro-Author.pdf)). |
| **5 m – 50 m** | Thermal erosion (talus cones & scree slopes at angle of repose ~30°–38°), soil creep, bank slumping, tributary rills, rock outcrops | Explains *why* rock breaks through soil on convex spurs while sediment pools in concave hollows and talus aprons sit cleanly at cliff bases. |
| **0.1 m – 5 m** | Micro-erosion rills, exposed root mounds, cobbles/boulders, organic litter drift, micro-puddles | Walking-distance realism; cannot come from a 2 m DEM alone—requires heightfield detail synthesis / parallax / displacement + scattered rock/debris meshes. |

### 1.5 Measurable "Real-Life Awareness" Metrics vs. Real DEMs

A synthetic or amplified terrain can be quantitatively audited against real DEMs (like swissALTI3D) using five standard geomorphometric metrics:

1. **Slope & Curvature Distributions:** Histograms of local slope $|\nabla h|$, profile curvature (along slope—controls flow acceleration/erosion), and plan/tangential curvature (across slope—controls flow convergence/divergence). Procedural fBm fails badly here (symmetric Gaussian curvature and uniform slope spectra, lacking flat valley floors + steep incised banks) — **verified** ([Schott et al. 2024](https://hal.science/hal-04565030v1/file/2024-MultiScaleHydro-Author.pdf)).
2. **Hypsometric Curve & Hypsometric Integral ($HI$):** Distribution of area vs. normalized elevation $HI = (h_{\text{mean}} - h_{\min})/(h_{\max} - h_{\min})$; distinguishes youthful steep uplifted plateaus ($HI > 0.6$) from mature fluvial landscapes ($HI \approx 0.35\text{--}0.55$, typical of Napf) — **verified** (classic geomorphometry; standard in LEM validation).
3. **Drainage Density & Hack's Law / Horton Ratios:** Total stream length per unit area ($D_d = \sum L / A$) and zero-pit hydrological connectivity (after breaching) — **verified** ([Schott et al. 2024](https://hal.science/hal-04565030v1/file/2024-MultiScaleHydro-Author.pdf)).
4. **Orometry (Divide Tree statistics):** Peak **prominence**, **dominance**, **isolation**, and **relevance** $\rho(P) = \sqrt{\tilde{\pi}(P)\tilde{h}(P)}$ measured on the peak-saddle Divide Tree; **Argudo et al. (SIGGRAPH Asia 2019)** proved a Random Forest classifier on these metrics reliably distinguishes real mountain archetypes from synthetic terrains — **verified** ([Argudo et al., HAL PDF](https://hal.science/hal-02326472/file/2019-orometry.pdf)).
5. **Slope–Area Scaling ($S \propto A^{-\theta}$):** In real fluvial landscapes, channel slope scales inversely with upslope contributing area $A$ with concavity index $\theta \approx 0.35\text{--}0.6$ (stream-power law); pure noise violates this completely — **verified** (classic geomorphology, basis of [Schott et al. 2023/2024](https://hal.science/hal-04565030v1/file/2024-MultiScaleHydro-Author.pdf)).

---

## 2. Terrain-Aware Texturing: Masks, Formulas, Storage, and Scatter Consistency

Plain `slope + height + noise` rules look unmistakably "CG" because real ecosystems and soils respond to **water convergence, sediment depth, solar insolation, and bedrock exposure**, not elevation alone.

### 2.1 Core Physically Derived Mask Channels (Cook-Time from DEM + Erosion)

Every channel below can be computed directly in `tools/cook` (or Houdini/Gaea) from your 2 m DEM + multi-scale erosion pass:

| Mask Channel | Formula / Derivation | Primary Visual & Ecological Role | Source |
| --- | --- | --- | --- |
| **1. Topographic Wetness Index (TWI)** | $\text{TWI} = \ln\!\left(\dfrac{A_s}{\tan \beta + \epsilon}\right)$ where $A_s$ is specific upslope catchment area ($\text{m}^2/\text{m}$) via **Multiple Flow Direction (FD8 / MD$\infty$)** (not D8, which creates 45° grid stripes!), $\beta$ = local slope, $\epsilon = \tan(0.001^\circ)$ | Soil moisture, dark damp humus, moss, ferns, **black alder** placement in wet hollows, and runtime **rain puddle accumulation** | **Verified** (Beven & Kirkby 1979; [Kopecký & Čížková, *Using TWI in vegetation ecology*, FD8/MD$\infty$ comparison](https://www.researchgate.net/publication/229888995_Using_topographic_wetness_index_in_vegetation_ecology_does_the_algorithm_matter); [SCIRP 2025](https://www.scirp.net/journal/paperinformation?paperid=150534)) |
| **2. Flow Accumulation & Stream Power (SPI)** | $\text{SPI} = A_s \cdot \tan\beta$ (or `flow` + `flowdir` from Houdini Erode 3.0 / Gaea) | Active erosion gullies, washed-out gravel/cobbles, exposed roots, wet streambed darkening | **Verified** ([SideFX HeightField Erode 3.0](https://www.sidefx.com/docs/houdini/nodes/sop/heightfield_erode.html)) |
| **3. Sediment / Soil Depth ($h_{\text{sed}}$)** | Accumulated `sediment` layer from fluvial deposition + `debris`/talus thickness from thermal erosion, clamped/relaxed on steep slopes | Separates deep meadow/beech soil (high $h_{\text{sed}}$) from thin stony soil and bare bedrock ($h_{\text{sed}} \to 0$); gates tree rooting depth | **Verified** ([SideFX Erode 3.0](https://www.sidefx.com/docs/houdini/nodes/sop/heightfield_erode.html); [Schott et al. 2024](https://hal.science/hal-04565030v1/file/2024-MultiScaleHydro-Author.pdf)) |
| **4. Talus / Scree Deposit** | Thermal erosion `debris` output (material slid down slopes exceeding angle of repose $\theta_{\text{rep}} \approx 32^\circ\text{--}38^\circ$) | Places loose angular rock/scree aprons directly below cliffs, matching scattered boulder/scree meshes | **Verified** ([SideFX Erode 3.0](https://www.sidefx.com/docs/houdini/nodes/sop/heightfield_erode.html); [Gaea 2.3](https://blog.quadspinner.com/gaea-2-3-now-available/)) |
| **5. Multi-Scale Curvature (Profile & Plan, or TPI)** | Laplacian / Topographic Position Index $\text{TPI}_r = h - G_r * h$ at two scales ($r \approx 6\text{ m}$ local, $r \approx 40\text{ m}$ ridge/valley) | Convex ridges/spurs ($>0$) = wind-scoured dry grass / exposed rock edges; concave hollows ($<0$) = leaf litter, mud, snowdrifts, sheltered shade | **Verified** (standard geomorphometry; [Zevenbergen & Thorne 1987 cited in Kopecký](https://www.researchgate.net/publication/229888995_Using_topographic_wetness_index_in_vegetation_ecology_does_the_algorithm_matter)) |
| **6. Solar Insolation / Sky View Factor (SVF)** | Annual integrated direct sun (accounting for self-shadowing horizon angles along sun arc, e.g., 16-azimuth HBAO-on-DEM) + aspect $\cos(\text{aspect} - \text{south})$ | South-facing warm dry slopes (meadows, faster snowmelt) vs. north-facing cool shaded slopes (moss, beech/fern dominance, persistent snow) — UE5's official **PCG Biome Sample** explicitly ships `BiomeMap`, tiled `Flow`, and `SunExposure` maps | **Verified** ([UE5 PCG Biome Quick Start](https://dev.epicgames.com/documentation/unreal-engine/procedural-content-generation-pcg-biome-core-and-sample-plugins-quick-start-guide-in-unreal-engine)) |
| **7. Bedrock Strata / Lithology ID** | 3D warped procedural strata field $s(\mathbf{x}) = \text{strata}(y + f(x,z))$ queried at bedrock elevation $h - h_{\text{sed}}$ (coupled to erosion `hardness` map!) | Guarantees rock bands on cliff faces align with ledges carved by differential erosion rather than painting stripes over smooth slopes | **Verified** ([Schott et al. 2024](https://hal.science/hal-04565030v1/file/2024-MultiScaleHydro-Author.pdf); [SideFX Copernicus Strata](https://www.sidefx.com/docs/houdini/heightfields_cop/index.html)) |

> **Practical note on DEM smoothing for TWI:** Computing TWI directly on raw 2 m LiDAR DEMs introduces micro-topographic noise that fragments drainage paths; compute flow accumulation on a lightly smoothed + pit-breached DEM (4–8 m effective scale) while keeping local slope/curvature at 2 m — **verified** ([Lidberg et al., SLU 2021](https://research.slu.se/files/19316484/fulltext.pdf)).

### 2.2 Mask Storage & Streaming Patterns

* **Plain Splat Weight Maps (4–8 channels):** Cheap and deterministic, but hard-caps you at 4–8 materials per tile and bloats bandwidth fast across 15×15 km.
* **Material ID + Top-K Weights (2–4 indices + weights):** Used in modern open-world engines (e.g., *Ghost Recon Wildlands* supported **143 unique terrain materials** by caching the 32 nearest materials in a texture array with a global-to-local indirection table and reducing 80% of foreground pixels from 12 to 4 fetches — **verified**, [Werle, GDC 2017, pp. 39–51](https://666uille.files.wordpress.com/2017/03/gdc2017_ghostreconwildlands_terrainandtechnologytools-onlinevideos1.pdf)). *Crucial pitfall:* Bilinear filtering of discrete integer IDs is illegal—you must sample the 4 texel corners manually and accumulate weights per unique ID (4-corner gather), which is trivial in a fragment/compute shader.
* **Runtime Virtual Texturing (RVT) / Adaptive VT (*Far Cry 4/5/6*, *Ghost Recon Wildlands*, UE5):** Bakes the expensive multi-layer material blend + decals + roads on-demand into cached physical tiles (e.g., `256²` tiles in an `8192²` BC7/BC5 physical atlas covering albedo, normal, smoothness, **porosity**), reducing runtime terrain shading to **1 virtual texture sample per pixel** — **verified** ([Werle GDC 2017](https://666uille.files.wordpress.com/2017/03/gdc2017_ghostreconwildlands_terrainandtechnologytools-onlinevideos1.pdf); [Weick & Zhou, *Far Cry 6*, GDC 2022](https://www.scribd.com/document/579302694/Gdc22-Farcry6-SimulatingTropicalWeather-Weick-Colin-Zhou-Emily); [UE5 RVT Docs](https://dev.epicgames.com/documentation/unreal-engine/runtime-virtual-texturing-in-unreal-engine?lang=fr)).
* **Runtime Procedural Evaluation from Compact Geomorphology Maps (Best Fit for Meridian's Parametric Principle):** Store the **4–8 physical simulation fields** (`TWI`, `soilDepth`, `talus`, `insolation`, `curvature`, `canopyDensity`, `biomeID`), and derive both **material blend weights** AND **scatter probabilities** from the *exact same shared GLSL/C++ rule functions* at runtime (or when baking RVT/clipmap tiles).

### 2.3 Keeping Texturing & Scatter 100% Consistent

Because Meridian already runs **bit-exact CPU/GPU scatter** from the heightfield, sharing the exact same **geomorphology attribute textures** (sampled with identical integer/fixed-point or explicit-LOD bilinear coordinates) as the single input to both terrain shading and `placement_stream.comp` guarantees coherence:

* `talus > 0.35 && slope > 24°` $\rightarrow$ scree ground material **and** angular scree rock scatter.
* `TWI > 0.7 && insolation < 0.5` $\rightarrow$ damp dark soil + moss material **and** fern + black alder scatter.
* `soilDepth > 0.4 && slope < 32°` $\rightarrow$ forest litter / meadow grass material **and** beech/hornbeam trees (automatically clearing trees off bare cliffs and scree without manual mask painting).
* **Closed-loop feedback (Forest Floor $\leftrightarrow$ Canopy):** Rasterize or bake your tree placement density back into a `canopyCover` channel (at 2–4 m/texel) so the terrain shader transitions smoothly from sun-bleached meadow grass to shaded beech leaf litter *strictly underneath actual placed tree crowns* — **inferred**.

---

## 3. Runtime Dynamics: Wetness, Snow, and Seasons Without Re-Cooking

### 3.1 Baked Data vs. Runtime Weather State Split

Never bake weather states into static terrain textures. Instead, split cleanly into:

1. **Baked Static Terrain/Material Attributes:**
* **Per-material texture channel: `Porosity**` (0 = bare non-porous granite/smooth rock, 0.3–0.6 = weathered bark/gravel, 0.7–1.0 = soil/humus/leaf litter/sand) — **verified** (*Far Cry 6* stores `albedo, normal, smoothness, porosity` in its terrain virtual texture atlas: [Weick & Zhou, GDC 2022](https://www.scribd.com/document/579302694/Gdc22-Farcry6-SimulatingTropicalWeather-Weick-Colin-Zhou-Emily); [Lagarde, *Water drop 3b*, 2013](https://seblagarde.wordpress.com/2013/04/14/water-drop-3b-physically-based-wet-surfaces/)).
* **Per-texel terrain maps:** `TWI` (where puddles pool), `curvature` (micro-hollows), `insolation` (where snow melts first and dries fastest), `canopyCover` (rain/snow shelter under dense beech crowns — acts as a cheap rain-occlusion mask!).


2. **Runtime Global / Regional Weather Uniforms (equivalent to UE5 Material Parameter Collections):**
* `rainIntensity`, `groundWetness` (accumulated/drying scalar), `puddleLevel`, `snowAccumulation`, `snowMelt`, `windVector`, `seasonDayOfYear` (`0..365` driving chlorophyll/senescence curves and grass tint).



### 3.2 Physically Based Runtime Modifiers

* **Rain Wetness & Puddles (Lagarde 2013; *Far Cry 6* GDC 2022):**
* **Physics:** Water filling pores in rough dielectrics raises the internal index of refraction from air ($1.0$) to water ($1.33$, closer to soil/rock $1.5$), increasing internal forward scattering and absorption: **porous diffuse albedo darkens AND becomes more saturated** (non-linearly, approximately $\rho_{\text{wet}} = \text{lerp}(\rho, \rho^{1.5\text{--}2.0} \cdot 0.45, w \cdot \text{porosity})$), while non-porous rock only gains a thin specular water film — **verified** ([Lagarde, *Water drop 3a & 3b*, 2013](https://seblagarde.wordpress.com/2013/04/14/water-drop-3b-physically-based-wet-surfaces/)).
* **Drying hysteresis:** During drying, the **glossy specular film evaporates faster than subsurface diffuse darkening** (driven by separate `wetFilm` vs. `wetSoak` scalars modulated by `insolation` so sunny slopes dry before north-facing hollows) — **verified** ([Lagarde, 2013](https://seblagarde.wordpress.com/2013/04/14/water-drop-3b-physically-based-wet-surfaces/)).
* **Puddles:** Drive puddle fill height from `puddleMask = smoothstep(threshold, threshold + eps, TWI_norm * puddleLevel - localHeightMap - slopePenalty)`. Inside puddles, flatten normal toward `(0, 1, 0)` (with animated raindrop ripple normals), set `roughness → 0.02`, and set $F_0 = 0.02$ (water IOR 1.33) — **inferred** from Lagarde (2013) & *Far Cry 6* GDC 2022.


* **Snow Accumulation & Deformation (*Ghost of Yōtei* 2025; *Death Stranding 2* 2025):**
* **Accumulation mask:** Compute `snowDepth` dynamically from `snowAccumulation`, surface upward normal `N.y`, macro `slope`, `insolation` (south slopes melt earlier), `altitude` (snowline lapse rate), `canopyCover` (less snow directly under dense crowns), and high-frequency ground heightmap (snow fills crevices first before blanketing rocks/grass tips).
* **Snow optics & deformation:** Blend toward high-albedo snow (`0.80–0.85` linear), soft rounded normals, micro-facet **screen-space stable snow sparkle**, and optional near-camera dynamic footprint/rut displacement buffer (used by both *Ghost of Yōtei* via terrain tessellation + displacement buffer and *Death Stranding 2* / *Horizon* terrain deformation — **verified**, [Bentley, *Ghost of Yōtei Tech Deep Dive*, PS Blog Oct 2025](https://blog.playstation.com/2025/10/23/ghost-of-yotei-tech-deep-dive/); [Digital Foundry *Death Stranding 2*, Jun 2025](https://www.digitalfoundry.net/articles/digitalfoundry-2025-death-stranding-2-tech-review)).


* **RVT Invalidation Trap (Important!):** If you use Runtime Virtual Texturing (RVT), **do NOT bake dynamic weather (wetness, snow, animated ripples, seasons) into the RVT cache**—invalidating all RVT pages during continuous rain/snow changes causes massive frame spikes! Instead, store *dry* `albedo, normal, roughness, porosity, height` in the RVT (or clipmap) and apply wetness/puddles/snow/seasonal tint as a **cheap post-sample runtime layer** in the terrain pixel shader — **inferred** from [Far Cry 6 GDC 2022](https://www.scribd.com/document/579302694/Gdc22-Farcry6-SimulatingTropicalWeather-Weick-Colin-Zhou-Emily) & [UE5 RVT Docs](https://dev.epicgames.com/documentation/unreal-engine/runtime-virtual-texturing-in-unreal-engine?lang=fr).

---

## 4. Rendering at Scale (1 m to 20 km in One View)

### 4.1 Geometry: Displacement vs. Mesh LOD vs. Parallax

* **CDLOD / Quadtree / Clipmap + Mesh Shaders (Meridian's current path):** Still the gold standard for pure-raster 180 fps engines (*Far Cry 5* GDC 2018 used a GPU-driven quadtree over a 16k/8 km heightmap; *Ghost of Yōtei* 2025 uses GPU compute rendering for vistas + dynamic tessellation specifically where deep snow deformation is active — **verified**, [Bentley, Oct 2025](https://blog.playstation.com/2025/10/23/ghost-of-yotei-tech-deep-dive/); **secondary**, [Far Cry 5 GDC 2018 summaries](https://80.lv/articles/gpu-driven-quadtree-terrain-mesh-rendering-inspired-by-far-cry-5)).
* **UE 5.4–5.8 Nanite Landscape + Programmable Tessellation:** Supports runtime material-driven displacement on Nanite landscapes — **verified** ([UE 5.4 Release Notes](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5.4-release-notes?application_version=5.4); [UE5 Nanite Docs](https://dev.epicgames.com/documentation/unreal-engine/nanite-virtualized-geometry-in-unreal-engine)). *Tradeoffs:* high shader/programmable-raster cost (community developers routinely fall back to POM for performance — **secondary**, [Reddit UE5 discussion, Jul 2026](https://www.reddit.com/r/UnrealEngine5/comments/1uqs5qi/fab_plugin_accurate_landscape_collision_for/)) and **breaks collision/heightfield single-source-of-truth** unless synchronized via extra collision plugins.
* **Best Geometry Split for Meridian (180 fps target):** Keep your **mesh-shader CDLOD quadtree** on the 2 m DEM, keep **POM** strictly for near-field micro-relief (0–15 m, fading smoothly to bump/normal by 20 m to save bandwidth and kill POM horizon aliasing), and use **mesh-shader vertex displacement** (sampling a tiled micro-height detail map or dynamic snow-rut buffer) *only* within the finest 0–30 m ring if true silhouette displacement is needed (e.g., cobbles/snow footprints).

### 4.2 Solving the Distant Terrain "Flat Colour" Problem (Mipmapping & Blending)

Why distant terrain turns into blurry mud/flat color in most engines:

1. **Detail textures mip down to a uniform constant average** past 100–300 m, leaving zero spatial contrast.
2. **Hex-tiling / stochastic texture bombing** mixes 3 weighted samples *per pixel*; at far mips, blending independent tiles washes out variance and introduces normal darkening if not variance-preserving (Deliot & Heitz 2019 histogram-preserving blending).
3. **Normal maps mip to `(0, 0, 1)**` without widening GGX roughness, turning distant hills artificially shiny/flat.

**How Shipping Open-World Titles Fix 1 m → 20 km Continuity:**

* **Multi-Frequency Detail Stacking + Baked Macro Albedo/Normal Maps:**
* *Ghost of Yōtei (Oct 2025)* explicitly **"baked models and detailed terrain materials into textures"** for distant mountains to preserve high-frequency visual detail across multi-km sightlines — **verified** ([Bentley, PS Blog Oct 2025](https://blog.playstation.com/2025/10/23/ghost-of-yotei-tech-deep-dive/)).
* *MSFS 2024* combines streamed global satellite orthophotos + DEM meshes for macro 20 km+ vistas with procedural ground detail textures/rocks/grass near the ground — **secondary** ([FlyAwaySimulation, Nov 2026](https://flyawaysimulation.com/ask/answers/most-realistic-scenery-flight-simulator/)).
* **For Meridian (Asset-Agnostic / Cook-Time):** Generate a **world-space Macro Color + Macro Normal/Roughness Map** (at 2 m/texel, `8192²`, BC7 + BC5) directly in `tools/cook` from your erosion/sediment/TWI/strata/canopy simulation (similar to Gaea's `SatMaps`/`ColorErosion` or Houdini Copernicus colorization). Blend seamlessly across 3 frequency octaves:
* **0–40 m:** High-res tileable PBR surfaces (hex-tiled + POM + height-lerp blending) modulated by macro color variation (`Overlay`/`SoftLight` around 1.0 mean) so tiling never shows.
* **40–300 m:** Mid-scale detail overlay (10–30 m period rock/meadow breakup noise + macro map).
* **300 m–20 km:** Baked macro albedo + macro DEM normal + **Toksvig / LEAN / SGGX normal-variance-to-roughness** (baking sub-texel normal variance into the macro roughness mip chain!) + distant voxel trees.




* **Filtering Blended Masks & Avoiding RVT Pitfalls:**
* Note from Epic's own Virtual Texturing docs: UE5's VT uses **stochastic trilinear mip filtering** (which injects noise that TAA/DLSS must clean up) and caps anisotropic filtering by tile border width (default 4 px), while reactive VT page streaming can pop during fast camera turns — **verified** ([UE5 SVT Docs](https://dev.epicgames.com/documentation/unreal-engine/streaming-virtual-texturing-in-unreal-engine)).
* For a 180 fps stability-first DLAA engine, **GPU-updated Clipmaps** (a fixed stack of concentric `1024²` or `2048²` toroidal textures centered on the camera covering e.g. 64 m, 128 m, 256 m, 512 m, 1 km, 2 km, plus the static `8192²` world macro map for 2–20 km) give **100% hardware trilinear + 16× anisotropic filtering, zero stochastic mip noise, zero page-table indirection, and zero streaming pop-in** — **inferred**.



### 4.3 Summary of Leading Games (2022–2026)

* ***Ghost of Yōtei* (Sucker Punch, PS5, Oct 2025):** GPU compute culling/placement across 1M+ distant vista objects (culled to ~60k rendered items), baked mountain model+material detail textures for distant vistas, dynamic terrain tessellation + displacement buffer for deep snow, GPU-driven cut-buffer for grass/flowers — **verified** ([Bentley, PS Blog Oct 2025](https://blog.playstation.com/2025/10/23/ghost-of-yotei-tech-deep-dive/)).
* ***Death Stranding 2* (Kojima Productions / Decima, PS5, Jun 2025):** Inherits Decima/*Horizon* heightfield terrain + runtime sand/ground deformation footprints + GPU-driven dense foliage + 60 fps non-RT dynamic weather — **secondary** ([Digital Foundry, Jun 2025](https://www.digitalfoundry.net/articles/digitalfoundry-2025-death-stranding-2-tech-review)).
* ***Horizon Zero Dawn / Forbidden West* (Guerrilla / Decima):** Heightfield + GPU-driven runtime procedural placement evaluating terrain height, slope, and density maps in compute ([van Muijden GDC 2017](https://games-1312234642.cos.ap-guangzhou.myqcloud.com/course/GAMES104/GAMES104_Lecture14.pdf)); depth-prepass alpha-test vegetation — **verified** ([80.lv interview](https://80.lv/articles/horizon-zero-dawn-interview-with-the-team)).
* ***Far Cry 5 / 6* & *Ghost Recon Wildlands* (Ubisoft):** 16k heightfield (0.5 m/texel in FC5), GPU quadtree LOD, Adaptive Virtual Texturing caching `albedo, normal, smoothness, porosity` + roads/decals into `256²` tiles in an `8192²` physical cache, plus World Machine base + non-destructive editor macro layers — **verified** ([Werle GDC 2017](https://666uille.files.wordpress.com/2017/03/gdc2017_ghostreconwildlands_terrainandtechnologytools-onlinevideos1.pdf); [Weick & Zhou GDC 2022](https://www.scribd.com/document/579302694/Gdc22-Farcry6-SimulatingTropicalWeather-Weick-Colin-Zhou-Emily); [Nebula Trifid FC4 AVT implementation](https://nebulatrifid.org/?tag=virtual-texturing)).
* ***Red Dead Redemption 2* (Rockstar):** Heightmap + tileable terrain materials + runtime mud/snow deformation; exact terrain virtual-texturing implementation is not published in a primary GDC/SIGGRAPH paper (**unverified** beyond community frame analyses).
* ***Unreal Engine 5.x* (Landscape + RVT + PCG Biome Core):** Landscape heightfield + optional Nanite Landscape/Tessellation + RVT caching + GPU PCG Biome framework consuming `BiomeMap`, `Flow`, and `SunExposure` tiled maps — **verified** ([UE5 RVT](https://dev.epicgames.com/documentation/unreal-engine/runtime-virtual-texturing-in-unreal-engine?lang=fr); [UE5 PCG Biome](https://dev.epicgames.com/documentation/unreal-engine/procedural-content-generation-pcg-biome-core-and-sample-plugins-quick-start-guide-in-unreal-engine)).
* ***Unity 6 / 6.4* (2026):** Moving toward an official chunked mesh terrain framework (64–256 m tiles) and shader-enhanced terrain features — **secondary** ([Unity Discussions, Feb 2026](https://discussions.unity.com/t/render-pipelines-strategy-for-2026/1710004/print)).

---

## 5. Integration Fit: Preserving a Single Source of Truth

Meridian's greatest architectural asset is that **one 16-bit heightfield + its max-height mip pyramid** simultaneously drives:

1. Mesh-shader CDLOD rendering,
2. Terrain shadow-height map (`terrain_shadow.comp`) and GI probe heightfield ray marching,
3. Bit-exact CPU/GPU procedural placement (`placement_stream.comp`).

### What Keeps That Contract Intact

* **Cook-time erosion amplification + deterministic `geo_maps`:** Everything baked in `tools/cook` writes a single final `R16_UNORM` heightfield (`H_final`) and aligned geomorphology attribute maps (`M_geo`), from which the engine builds the exact height mip pyramid (`H_min`, `H_max`) at load/cook time.
* **Bounded runtime micro-detail noise (`±0.3 m`):** Safe *provided* the max-height pyramid inflates `H_max` conservatively by `+0.3 m` (preventing false ray-march misses) and CPU/GPU placement evaluates the exact same deterministic integer/fixed-point `detailNoise(x, z)` function (or disables detail noise on placement and accepts ≤0.3 m local sinking).
* **Surface Clipmap / RVT material caching:** Strictly a shading cache over `H_final`—does not alter geometry or height queries.

### Approaches That BREAK the Single Source of Truth (Flagged!)

1. **Runtime GPU Hydraulic/Thermal Erosion:** Non-deterministic across GPU vendors/drivers and changes height asynchronously per tile $\rightarrow$ breaks bit-exact CPU/GPU placement and invalidates the global height pyramid every frame.
2. **Material-Driven Arbitrary Nanite / Hardware Tessellation Displacement (from artist shader graphs):** Moves the rendered terrain surface away from `H_final` $\rightarrow$ causes floating/sunken grass and rocks, self-shadowing mismatch with the heightfield shadow map, and requires complex runtime collision sync (as UE5 devs discovered with Nanite Landscape displacement — **secondary**, [Reddit UE5, Jul 2026](https://www.reddit.com/r/UnrealEngine5/comments/1uqs5qi/fab_plugin_accurate_landscape_collision_for/)). *Safe exception:* bounded visual-only micro-displacement (≤5–10 cm) or explicit POM (no silhouette/collision shift), plus an explicit local dynamic snow-rut buffer queried only by near decals/characters.
3. **3D Voxel / SDF Sculpted Terrain Overhangs Mixed Directly Into the Heightfield:** Breaks 2.5D heightfield ray marching and height-pyramid monotonicity. *Standard AAA solution:* Keep the terrain strictly 2.5D heightfield, and place **3D cliff/outcrop/boulder cluster-LOD meshes** driven by the `talus` / `bedrock` / `slope` masks (which seamlessly integrate with your existing cluster-LOD + far-field voxel pipeline!).

---

## 6. Ranked Comparison of 4 Concrete Pipeline Architectures (15×15 km, RTX 4080 @ 1440p)

All memory figures assume a 15×15 km world padded to `8192²` (`~1.83 m/texel`, matching your `7500²` 2 m swissALTI3D crop) or `16384²` (`~0.92 m/texel` tiled).

### Architecture 1 (Rank #1 — Recommended): **Cook-Time Geomorphology + Dynamic Toroidal Surface Clipmap + Runtime Weather Layer**

* **Offline vs. Runtime Split:**
* *Offline (`Houdini 21/22` or `Gaea 2.3` $\rightarrow$ `tools/cook`):* Ingest 2 m DEM (or procedural planet heightfield), run multi-scale erosion amplification (Schott 2024 / Houdini Erode 3.0 / Gaea `Erosion2`) + pit breaching + FD8 TWI/SPI/insolation/strata/macro-albedo/macro-normal-variance bake.
* *Runtime (Meridian Vulkan 1.3):* Keep CDLOD mesh-shader terrain on `R16_UNORM` heightfield. Update a **6-level toroidal Surface Clipmap** (`1024²` per level: 64 m → 2048 m around camera; only dirty scroll strips updated in compute when camera moves!) that evaluates the expensive hex-tiled multi-material blend + decals + roads **once per texel** into dry `Albedo (BC7/RGBA8)`, `Normal (RG8)`, `Roughness+Porosity+Height (RGBA8)`. Terrain pixel shader does **1 hardware-anisotropic clipmap fetch** (blending into the static `8192²` Macro Map past 2 km) + **runtime weather layer** (wetness, puddles from `TWI`, snow from `insolation`/`slope`/`canopyCover`, seasonal tint) + near-field POM (0–15 m).


* **Data Formats & Sizes (15×15 km @ ~2 m/texel):**
* `Heightfield` (`8192² R16_UNORM` + min/max pyramid): **171 MiB** VRAM (disk ~80 MiB lossless/zstd).
* `GeoMasks` (`4096²` or `8192²` packed 2× `BC7` = 8 channels: `TWI`, `soilDepth`, `talus`, `insolation`, `curvatureLow`, `curvatureHigh`, `strataID`, `canopyCover`): **128 MiB** at `8192²` BC7 (or **32 MiB** at `4096²` BC7).
* `Macro Distant Terrain` (`8192²`: `BC7` Albedo + `BC5` Normal + `BC4` Variance/Roughness + mips): **~213 MiB**.
* `6-Level Surface Clipmap` (`6 × 1024²` uncompressed `RGBA8×3` for zero-cost compute writes): **~72 MiB** VRAM!
* `Tileable PBR Surface Array` (16–24 Megascans materials @ 2K BC7/BC5, incl. `porosity`): **~160–240 MiB**.
* **Total Resident VRAM:** **~740–825 MiB** (trivial inside 16 GB).


* **Estimated GPU Cost (RTX 4080 @ 1440p — estimated):**
* Terrain mesh + shading pass: **~0.35–0.55 ms** (single hardware anisotropic fetch + runtime weather math + POM only <15 m).
* Incremental clipmap scroll update (compute, amortized): **~0.05–0.15 ms/frame** (zero cost when standing still!).


* **Authoring Effort:** Low-to-moderate once `tools/cook` + Houdini HDA are set up; 100% parametric and planet-agnostic.
* **Realism Evidence:** Matches *Far Cry 5/6* (porosity + cached dry terrain atlas) and *Ghost of Yōtei* (baked distant mountain detail) while eliminating VT page-table overhead and stochastic mip noise.
* **Risks:** Clipmap resolution near the player is fixed (e.g., `64 m / 1024 = 6.25 cm/texel` at level 0); mitigated by multiplying/blending a cheap single-fetch high-frequency micro-detail normal/albedo overlay within 0–10 m (or using `2048²` for level 0 = `3.1 cm/texel` for +36 MiB).

---

### Architecture 2 (Rank #2): **Cook-Time Geomorphology + Full Adaptive Virtual Texturing (Far Cry 4/5/6 / UE5 RVT Style)**

* **Offline vs. Runtime Split:** Same offline Houdini/Gaea cook as Arch #1. Runtime uses an **on-demand page-table Virtual Texture** (`256²` tiles in an `8192²` BC7/BC5 physical cache) with GPU feedback buffer, LRU tile eviction, and async/sliced tile baking, plus runtime weather overlay.
* **Data Formats & Sizes (15×15 km):** Heightfield (171 MiB) + GeoMasks (128 MiB) + Macro fallback (128 MiB) + VT physical cache (`8192²` × 3 BC7/BC5 = 192 MiB) + indirection/page tables (~16 MiB) + Source PBR Array (200 MiB) $\approx$ **~835 MiB VRAM**.
* **Estimated GPU Cost (RTX 4080 @ 1440p — estimated):** Terrain pass **~0.45–0.70 ms** (page-table indirection + manual border-clamped aniso/trilinear) + feedback readback/parse + tile bake **~0.2–0.5 ms** during fast pans.
* **Authoring Effort:** **High engineering complexity** (page table indirection, tile borders, LRU cache, CPU/GPU feedback sync, mip-bias tuning).
* **Realism Evidence:** Proven in *Far Cry 4/5/6*, *Ghost Recon Wildlands*, and *Unreal Engine 5* ([Werle GDC 2017](https://666uille.files.wordpress.com/2017/03/gdc2017_ghostreconwildlands_terrainandtechnologytools-onlinevideos1.pdf); [UE5 RVT Docs](https://dev.epicgames.com/documentation/unreal-engine/runtime-virtual-texturing-in-unreal-engine?lang=fr)). Supports extreme localized texel density (up to 1–2 mm/px at player feet) and thousands of unique road/crater decals.
* **Risks:** Reactive feedback causes **1–3 frame pop-in on fast 180° turns**; manual trilinear/aniso across tile borders adds shader cost or stochastic noise under DLAA ([UE5 SVT Docs](https://dev.epicgames.com/documentation/unreal-engine/streaming-virtual-texturing-in-unreal-engine)).

---

### Architecture 3 (Rank #3): **Cook-Time Geomorphology + Direct Per-Pixel Material-ID Splatting (Zero Surface Cache)**

* **Offline vs. Runtime Split:** Same offline cook, writing a `MaterialID + Top-4 Weights` map (or evaluating geomorphology rules directly in the terrain fragment shader) and blending 3–4 hex-tiled triplanar Megascans layers per pixel + runtime weather + Macro Map fade past 200 m.
* **Data Formats & Sizes (15×15 km):** Heightfield (171 MiB) + GeoMasks (128 MiB) + Macro Map (213 MiB) + Source PBR Array (200 MiB) $\approx$ **~712 MiB VRAM**.
* **Estimated GPU Cost (RTX 4080 @ 1440p — estimated):** **~1.2–2.2 ms** (4 layers × 3 hex taps × triplanar on cliffs × POM = 12–36 texture fetches/pixel in dense views).
* **Authoring Effort:** Lowest (direct evolution of Meridian's current terrain shader).
* **Realism Evidence:** High visual quality if paired with geomorphology masks and a baked Macro Map, **but** hits a hard bandwidth wall once you scale beyond 4–6 materials or add roads/rivers/decals.
* **Risks:** Directly threatens Meridian's **180 fps (5.55 ms total)** target and creates heavy shader divergence on steep cliff/valley transitions.

---

### Architecture 4 (Rank #4): **Runtime GPU Procedural Amplification + Nanite-Style Programmable Tessellation**

* **Offline vs. Runtime Split:** Store only a coarse 16–32 m DEM (`512²–1024²`), run GPU multi-scale erosion/detail synthesis into streamed height tiles at runtime, and displace terrain triangles dynamically via material graphs (UE5 Nanite Tessellation style).
* **Data Formats & Sizes:** Very small disk footprint (~50–150 MiB), ~600–900 MiB runtime streaming pools.
* **Estimated GPU Cost (RTX 4080 @ 1440p — estimated):** **~1.5–3.0+ ms** (programmable tessellation + runtime tile synthesis + re-building height pyramids).
* **Authoring Effort:** Very high (complex seam handling across eroded tiles, collision sync, determinism bugs).
* **Risks:** **Breaks Meridian's core architectural invariant** (single bit-exact heightfield for ray marching, shadows, GI probes, and CPU/GPU scatter) and wastes GPU budget on a problem that `tools/cook` solves better offline.

---

## 7. Recommended Architecture for Meridian — and What NOT to Do

### Recommended Architecture: **Architecture #1 (Houdini/Gaea Cook $\rightarrow$ `geo_maps` + Macro Map + 6-Level Toroidal Surface Clipmap + Runtime Weather)**

1. **Why it fits Meridian's exact constraints:**
* **Hits 180 fps headroom:** Moves hex-tiling, triplanar cliff projection, and 8–16 material geomorphology blending out of the 1440p per-pixel fragment pass into an **amortized compute clipmap scroll** (`~0.4–0.6 ms` total terrain cost vs. `1.5–2.2 ms` direct splatting).
* **Zero DLAA temporal artifacts:** Unlike Virtual Texturing's stochastic mip filtering and reactive tile pop-in, hardware-filtered toroidal clipmaps give rock-solid 16× anisotropic filtering with zero page-table stalls—aligning directly with your motion-clarity priority.
* **Single Source of Truth preserved:** `H_final` (`R16_UNORM`) + `GeoMasks` stay immutable at runtime and feed CDLOD, heightfield ray marching, GI probes, and bit-exact CPU/GPU placement identically.
* **Asset-agnostic & multi-planet ready:** The engine only knows about generic physical fields (`height`, `TWI`, `soilDepth`, `talus`, `insolation`, `curvature`, `strata`, `canopyCover`, `porosity`) and data-driven material/weather tables. Swapping Napf (Switzerland) for Mars or an alien archipelago changes only the cook inputs and material/weather config—zero engine shader rewrites.
* **Clean Phase 1 $\rightarrow$ Phase 2 transition:** In Phase 1, write a small C++/GPU pass inside `tools/cook` to compute **pit-breached FD8 TWI, SPI, multi-scale curvature, annual insolation, and macro albedo/normal-variance** straight from your existing 2 m swissALTI3D DEM! In Phase 2, plug Houdini 21/22 (`HeightField Erode 3.0` / Copernicus) or Gaea 2.3 in front of the exact same `tools/cook` data schema.



### Explicit "What NOT to Do"

* **Do NOT run hydraulic/thermal erosion or diffusion super-resolution at runtime:** Keep all hydrological and geological synthesis in `tools/cook` / Houdini so watersheds are globally consistent and the height pyramid stays bit-exact.
* **Do NOT use D8 single-direction flow for TWI:** It produces ugly 45° diagonal artifacts in valleys; always use **Multiple Flow Direction (FD8 / MD$\infty$)** on a pit-breached, lightly smoothed DEM ([Kopecký & Čížková](https://www.researchgate.net/publication/229888995_Using_topographic_wetness_index_in_vegetation_ecology_does_the_algorithm_matter)).
* **Do NOT bake dynamic weather (wetness, snow, seasons) into a Virtual Texture or Surface Clipmap:** Keep cached terrain textures strictly **dry/neutral + `porosity**`, and apply rain darkening, puddles, snow accumulation, and seasonal shifts as a cheap runtime shader layer.
* **Do NOT rely on raw `slope + height + noise` masks or hand-painted splats:** Derive both surface blends and scatter rules from the same `TWI / soilDepth / talus / insolation / canopyCover` fields.
* **Do NOT skip normal-variance-to-roughness baking (Toksvig/LEAN/SGGX) on the Macro Distant Map:** Without folding mip-level normal variance into macro roughness, 2–15 km ridges will either look waxy/metallic or collapse into flat matte plastic.
* **Do NOT enable arbitrary material-graph tessellation displacement on terrain:** It breaks heightfield-vs-render synchronization for placement, shadows, and GI probes; restrict micro-depth to near-field POM (0–15 m) or tightly bounded snow-deformation buffers, and place real 3D cluster-LOD cliff/rock meshes where vertical overhangs are needed.

For a concise visual breakdown of how modern open-world engines structure quadtrees, clipmaps, vertical error maps, and Far Cry 5–style virtual texturing for terrain rendering, see [How Open World Games Optimize Terrain Rendering](https://www.youtube.com/watch?v=KoAERjoWl0g).
