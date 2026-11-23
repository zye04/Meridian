# Pure-Raster Photorealistic Daylight Forest Rendering: Technical Research Report

**Date:** 2026-11-23

**Evidence Labels:** Every claim below is labeled **VERIFIED** (read directly from primary documentation, paper, or slide deck), **SECONDARY** (third-party summary, forum, or press), or **INFERRED** (engineering synthesis). Costs are stated with GPU and resolution only when reported by a source; missing or disputed numbers are called out explicitly.

## 1. Local Exposure / Local Tonemapping for Games (2023–2026)

### Techniques, Costs & Artifacts

* **Unreal Engine 5 Local Exposure — Bilateral Grid (`BILATERAL`) & Exposure Fusion (`FUSION`):**

  * UE5 implements two local exposure modes in post-processing: `BILATERAL` (decomposes log-luminance into a base layer and detail layer via a 3D bilateral grid, reducing base-layer contrast via `Highlight Contrast` and `Shadow Contrast`, typically `0.6–1.0`, while keeping `Detail Strength = 1.0`) and `FUSION` (fuses multiple synthetic exposures according to quality weights, exposed in UE 5.5+) — **VERIFIED** ([UE5.5 `LocalExposureMethod` API](http://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/LocalExposureMethod?application_version=5.5); [UE5.1 `PostProcessSettings` API](http://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/PostProcessSettings?application_version=5.1); [UEFN Lumen Exposure Manager docs](https://dev.epicgames.com/documentation/fortnite/using-the-lumen-exposure-manager-in-unreal-editor-for-fortnite)).

  * *Cost:* Epic's official docs do not publish a 1440p RTX-class millisecond figure; a UE5-Main community profiler measured $\approx 0.13\text{ ms}$ on a GTX 1070 (resolution unspecified) — **SECONDARY** ([Unreal Forums UE5 Local Exposure thread](https://forums.unrealengine.com/t/local-exposure-is-in-ue5-main/274274)).

  * *Artifacts:* Pure bilateral decomposition exhibits **gradient reversals and edge ringing** on sharp high-contrast edges, and dark halos around high-intensity emissives or sun-lit sky gaps — **VERIFIED** ([Bart Wronski, Feb 2022](https://bartwronski.com/2022/02/28/exposure-fusion-local-tonemapping-for-real-time-rendering/)); **SECONDARY** ([Unreal Forums](https://forums.unrealengine.com/t/local-exposure-is-in-ue5-main/274274)).

* **Bilateral Grid + Gaussian Blend (*Ghost of Tsushima*, Patry 2021):**

  * Sucker Punch blends a bilateral grid with a Gaussian blur to trade off Gaussian halos against bilateral gradient reversals/edge ringing, paired with a `Blurred Luminance Blend` parameter (the same mechanism later adopted in UE5 Local Exposure) — **VERIFIED** ([Bart Wronski citing Jasmin Patry SIGGRAPH 2021](https://bartwronski.com/2022/02/28/exposure-fusion-local-tonemapping-for-real-time-rendering/); [Polyphony Digital SIGGRAPH 2025 slides](https://blog.selfshadow.com/publications/s2025-shading-course/pdi/s2025_pbs_pdi_slides_v1.1.pdf)).

  * *Cost:* No isolated 1440p PC ms number is publicly reported in the available abstracts/summaries.

* **Exposure Fusion via Laplacian Pyramids (Mertens et al. 2007; Wronski 2022; UE5 `FUSION`):**

  * Generates 3 synthetic exposures (shadows, midtones, highlights) using the global tonemapper, computes exposure-only weights (omitting Mertens' contrast/saturation weights to avoid the "toxic HDR" look), builds a 5–6 level Laplacian pyramid of lightness and a Gaussian pyramid of weights, blends per frequency band, and transfers lightness back to the HDR/tonemapped color — **VERIFIED** ([Bart Wronski, *Exposure Fusion – local tonemapping for real-time rendering*, Feb 2022](https://bartwronski.com/2022/02/28/exposure-fusion-local-tonemapping-for-real-time-rendering/)).

  * *Artifacts & motion:* Blending frequency-by-frequency hides transitions at sharp edges (sunlit canopy holes against dark trunks) and spreads them over flat regions, substantially reducing halos vs. Gaussian or pure bilateral grids — **VERIFIED** ([Wronski, Feb 2022](https://bartwronski.com/2022/02/28/exposure-fusion-local-tonemapping-for-real-time-rendering/)). However, strong local tonemapping under a moving camera still suffers from **temporal swimming/breathing** as coarse pyramid texels shift across screen edges unless the coarse level is spatially stable or gently clamped — **INFERRED**.

  * *Cost:* Wronski's 2022 article provides a WebGL demo without a published 1440p RTX GPU ms figure (pyramid build + collapse on 3 scalar lightness channels is bandwidth-bound and typically sub-$0.3\text{ ms}$ on modern GPUs) — **INFERRED**.

* **GI-Based Local Tone Mapping (*Far Cry 5*, McAuley 2018) & Lottes (GDC 2016):**

  * *Far Cry 5* drove local exposure compression from world-space GI / sky-visibility luminance rather than screen-space luminance alone, avoiding screen-edge halos when panning — **SECONDARY** (cited as `GI-Based local tone mapping [McAuley 18]` in [Polyphony Digital SIGGRAPH 2025 slides](https://blog.selfshadow.com/publications/s2025-shading-course/pdi/s2025_pbs_pdi_slides_v1.1.pdf)).

  * Timothy Lottes (GDC 2016) documented global filmic tone curves with crosstalk and HDR color-pipeline optimization; local dodging/burning in *God of War* (2018) used wide Gaussian blur plus artist-placed spherical exposure volumes, which Wronski explicitly notes produced distracting halos in motion when pushed — **VERIFIED** ([Wronski, Aug 2016](https://bartwronski.com/2016/08/29/localized-tonemapping/); [Wronski, Feb 2022](https://bartwronski.com/2022/02/28/exposure-fusion-local-tonemapping-for-real-time-rendering/)).

### Keeping Forest Shade Readable Without Lifting Global Exposure

* Keep your **3 EV global cap** (so forest clearings and sky stay anchored and dark shade stays perceptually darker than sunlit meadows), and apply **gentle Exposure Fusion or Bilateral+Gaussian Local Exposure** with `Shadow Contrast` $\approx 0.70\text{--}0.85$ and `Highlight Contrast` $\approx 0.85\text{--}0.95$ — **INFERRED**. Because local exposure compresses only the *low-frequency base layer* while preserving the *high-frequency detail layer*, tree trunks remain readable and bark micro-contrast stays crisp without washing out the forest floor into mid-grey — **VERIFIED** ([UE5.5 `LocalExposureMethod`](http://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/LocalExposureMethod?application_version=5.5); [Wronski, Feb 2022](https://bartwronski.com/2022/02/28/exposure-fusion-local-tonemapping-for-real-time-rendering/)).

## 2. Auto-Exposure Metering Rules in Shipped Open-World Games

* **Histogram Percentile Trimming (Unreal Engine 4.25–5.x):**

  * Epic unified Histogram and Basic metering around 18% middle grey ($\text{EV}_{100} = \log_2(L)$ with lens attenuation $q = 0.78$ canceling the ISO 12232 $1.2$ factor), trimming extreme dark and bright tails before averaging log-luminance — **VERIFIED** ([John Hable, *How Epic Games is handling auto exposure in 4.25*, Epic Tech Blog, Mar 2020](https://www.unrealengine.com/en-US/tech-blog/how-epic-games-is-handling-auto-exposure-in-4-25)).

  * Recommended percentile windows in Epic's documentation: **Low Percent = 70–80%**, **High Percent = 80–95%** (biasing toward upper midtones/highlights to protect skies from blowing out while preventing dark foliage shadows from dragging exposure up), paired with tight `Histogram Min/Max EV100` bounds to stabilize jitter — **VERIFIED** ([UE5.1 `PostProcessSettings` API](http://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/PostProcessSettings?application_version=5.1); [UEFN Lumen Exposure Manager](https://dev.epicgames.com/documentation/fortnite/using-the-lumen-exposure-manager-in-unreal-editor-for-fortnite); [UE Physical Lighting Units docs](https://dev.epicgames.com/documentation/unreal-engine/using-physical-lighting-units-in-unreal-engine)).

* **Spatial Metering Masks / Center Weighting:**

  * Epic replaced the legacy focus parameter with a **full-screen Exposure Metering Mask texture** (weighted strongly toward screen center, attenuated near edges and top sky band) that applies to Histogram mode — **VERIFIED** ([Hable, Epic Tech Blog, Mar 2020](https://www.unrealengine.com/en-US/tech-blog/how-epic-games-is-handling-auto-exposure-in-4-25)). Center-weighted metering is standard in shipped titles (*Far Cry 4*, *Frostbite*, etc.) to stop sky/horizon framing changes from pumping exposure — **VERIFIED** ([Krzysztof Narkowicz, *Automatic Exposure*, Jan 2016](https://knarkowicz.wordpress.com/2016/01/09/automatic-exposure/)).

* **Albedo-Independent / Incident-Light (Illuminance) Metering:**

  * A fundamental flaw of standard reflected-luminance (`mean-to-18%-grey`) metering is that it cannot distinguish a sunlit dark pine forest (3–6% albedo) from a dim white room, so it always lifts dark forests toward 18% grey — **VERIFIED** ([Narkowicz, 2016](https://knarkowicz.wordpress.com/2016/01/09/automatic-exposure/); [Lagarde & de Rousiers, *Moving Frostbite to PBR 2.0*](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/course-notes-moving-frostbite-to-pbr-v2.pdf)).

  * Incident-light metering (metering pre-albedo irradiance, or weighting by a world-space exposure/sun+sky illuminance curve with a small $\pm 1\text{--}2\text{ EV}$ adaptive trim) prevents dark albedos from lifting forest interiors to mid-grey — **SECONDARY** ([UE Forums light-buffer metering discussion](https://forums.unrealengine.com/t/automatic-exposure-should-be-based-on-the-light-buffer-instead-of-the-image-buffer/310609)); **INFERRED**.

* **Published EV Clamps for Forest Interiors:**

  * **Plain statement:** No primary AAA studio talk found in this search publishes a canonical numeric `Min/Max EV100` clamp specifically for temperate forest interiors. In real physical units, open midday sun+sky sits around $\text{EV}_{100} \approx 14.5\text{--}15.5$ ($\approx 100{,}000\text{ lux}$), while closed beech forest shade (1.5–5% irradiance) sits $4.3\text{--}6.0\text{ EV}$ **lower** ($\text{EV}_{100} \approx 9\text{--}11$) — **INFERRED** from the measured PAR transmittance ratios in Section 8. Allowing only a **2–3 EV** global lift inside forests (preserving $\approx 2\text{--}3$ stops of perceived darkness relative to open sun) and letting Local Exposure lift shadow detail matches photographic practice — **INFERRED**.

## 3. Canopy Occlusion of Sky Light WITHOUT Ray Tracing

### General Non-RT Mechanisms

1. **Voxel / Opacity-Volume Cone Tracing & Multi-Axis Optical Depth:** Stores directional or mipmapped voxel opacity per asset/tile and marches/cones upward over the hemisphere to compute sky visibility and canopy transmittance (as Chlorophyll Engine explicitly states it does via its per-asset voxel opacity volume) — **VERIFIED** (target engine statement; [CryEngine SVOGI overview](https://www.tweaktown.com/news/49267/kingdom-deliverance-teased-svogi-effects-looks-incredible/index.html)).

2. **Precomputed / Streamed SH Probes from Sky Visibility:** Stores directional sky visibility + bent normal / bent cone and evaluates dynamic sky radiance + sun bounce at runtime — **VERIFIED** ([Jasmin Patry, *Ghost of Tsushima* SIGGRAPH 2021 abstract](https://www.glowybits.com/blog/2022/12/18/ghost_talks/)).

3. **Top-Down World Lightmap / Canopy Density Map:** Renders or bakes a top-down 2D world-space transmittance/AO map (e.g., *Assassin's Creed III* / *RDR2* style) to occlude sky light on terrain and lower trunks at $\approx 0.05\text{ ms}$ lookup cost — **SECONDARY** ([RDR2 Graphics Study, 2020](https://imgeself.github.io/posts/2020-06-19-graphics-study-rdr2/)); **INFERRED**.

4. **Signed Distance Field Cone Tracing (DFAO / Lumen Software RT):** Marches a few wide cones over the hemisphere against per-mesh SDFs and a global clipmap SDF — **VERIFIED** ([UE5 Mesh Distance Fields docs](https://dev.epicgames.com/documentation/unreal-engine/mesh-distance-fields-in-unreal-engine)).

### What Shipped Titles Actually Did for Forests

* ***Ghost of Tsushima*** **(Sucker Punch, PS4, pure raster):** Computed **SH irradiance probes from baked sky visibility data**, synthesizing dynamic sky and sun/moon bounce lighting at runtime without ray tracing — **VERIFIED** ([Jasmin Patry, *Real-Time Samurai Cinema*, SIGGRAPH 2021](https://www.glowybits.com/blog/2022/12/18/ghost_talks/)).

* ***Red Dead Redemption 2*** **(Rockstar, PS4/PC, pure raster):** Combined **SSAO** (near-field), a **top-down world lightmap** (macro outdoor/canopy occlusion), **baked large-scale AO cubemaps** (`R8_UNORM`), and a dynamic camera-centered **diffuse/specular environment cubemap** — **SECONDARY** ([Graphics Study: Red Dead Redemption 2, Jun 2020](https://imgeself.github.io/posts/2020-06-19-graphics-study-rdr2/)).

* ***Horizon Forbidden West*** **(Guerrilla, PS4/PS5, pure raster):** Uses **pre-calculated static global illumination/sky visibility probes** (no hardware RT anywhere) layered with screen-space ambient occlusion and high-res shadows — **SECONDARY** ([Digital Foundry / GPU Zen 4 references](https://dokumen.pub/gpu-zen-4-advanced-rendering-techniques.html); [HFW tech breakdown summary](https://www.reddit.com/r/Games/comments/ssbay3/horizon_forbidden_west_digital_foundry_tech/)). Exact Decima canopy sky-visibility data structure is not publicly documented in available GDC/SIGGRAPH slides (**unknown**).

* ***Unreal Engine 5*** **(Lumen Software RT & DFAO):** Uses **Mesh Distance Fields (SDFs)** (`Detail Tracing` per-mesh SDF or `Global Tracing` global clipmap SDF) with a **`Two-Sided Distance Field Generation`** option specifically for foliage cards/crowns so thin leaves produce soft translucent sky occlusion rather than solid blobs; grass is excluded to avoid tile-buffer overflow. Lumen also exposes an explicit **`Skylight Leaking`** knob to keep dark interiors/canopies from going pitch black — **VERIFIED** ([UE5 Mesh Distance Fields docs](https://dev.epicgames.com/documentation/unreal-engine/mesh-distance-fields-in-unreal-engine); [UE5 Lumen GI docs](https://dev.epicgames.com/documentation/unreal-engine/lumen-global-illumination-and-reflections-in-unreal-engine)).

* ***Avatar: Frontiers of Pandora*** **(Ubisoft Massive / Snowdrop, 2023–2024):** *Important correction:* Snowdrop does **not** run a pure-raster-without-RT GI path; it ships **mandatory ray tracing** (hardware RT on capable GPUs, custom BVH software ray tracing fallback on non-RT hardware) for diffuse GI, reflections, and distant shadows, and explicitly **skips alpha-test evaluation during ray traversal** on vegetation for speed — **VERIFIED** ([Oleksandr Koshlo, *Ray Tracing in Snowdrop: Scene Representation and Custom BVH*, GDC 2024 slides](https://media.gdcvault.com/gdc2024/Slides/GDC+slide+presentations/Koshlo_Oleksandr_Snowdrop_Custom_BVH.pdf)).

* ***Alan Wake 2*** **(Remedy / Northlight, 2023–2024):** Uses **software ray-traced SDF/voxel GI** as its baseline non-HWRT GI on consoles/PC, layered with screen-space reflections/traces and environment cubemaps — **VERIFIED** for architecture outline ([Remedy Northlight PS5 Pro blog, Oct 2024](https://www.remedygames.com/article/alan-wake-2-on-playstation-5-pro---behind-the-scenes)); **SECONDARY** for SDF/software RT details ([Reddit tech discussion](https://www.reddit.com/r/Games/comments/1fi3k14/remedy_entertainment_addresses_alan_wake_2/)). Public GDC 2024 slides with exact ms costs for Northlight's non-RT forest sky-visibility pass were not accessible (**unknown**).

### Direct Implication for Your Measured 10–14% Ground Sky Ambient Problem

If your ground sky ambient only drops to realistic levels when your probe GI clipmap is active, your default raster path lacks a **macro canopy sky-visibility term on terrain and trunks**. Two cheap pure-raster fixes:

1. Bake/update a **2D top-down canopy optical-depth / sky-visibility map** from your existing far-field voxel grid (integrating a 5–9 cone hemisphere gather over the voxel canopy volume) and multiply terrain/trunk sky irradiance by it ($\approx 0.1\text{--}0.2\text{ ms}$ amortized or baked per tile) — **INFERRED**.

2. Or evaluate your existing **far-field voxel opacity volume directly** along a 4–6 cone upward gather (or store pre-convolved sky visibility in the voxel clipmap alpha/SH) so terrain and trunks get continuous 1.5–5% interior sky visibility without waiting on probe clipmap resolution — **INFERRED**.

## 4. Foliage Light Transport

### Measured Leaf Data (PROSPECT / LOPEX / Goniophotometry)

* **Spectral reflectance & transmittance (PROSPECT-4/5, LOPEX, ANGERS):** Broadleaf chlorophyll strongly absorbs blue ($400\text{--}500\text{ nm}$) and red ($600\text{--}700\text{ nm}$), creating a visible reflectance/transmittance peak at $\approx 550\text{ nm}$ (green) and a 40–50% plateau in NIR — **VERIFIED** ([Féret et al., *PROSPECT-4 and 5*, RSE 2008](https://www.ipgp.fr/~jacquemoud/publications/feret2008.pdf)).

* **European Beech (*Fagus sylvatica*) BRDF/BTDF measurements:** Direct spectro-photo-goniometer measurements yield a cuticle index of refraction $n \approx 1.5$ ($F_0 = 0.04$) and microfacet roughness $\sigma \approx 0.35$ (perceptual GGX roughness $\approx 0.45\text{--}0.60$), with transmittance dropping at grazing incidence due to Fresnel reflection at the cuticle — **VERIFIED** ([Bousquet et al., *Leaf BRDF and BTDF Measurements and Model*, 2005](https://www.ipgp.fr/~jacquemoud/publications/bousquet2005b.pdf)).

* **Rough Diffuse BRDF (EON — used by Chlorophyll Engine):** Portsmouth et al. (JCGT 2025) introduced **EON (Energy-Preserving Oren-Nayar)**, a closed-form reciprocal extension of Fujii's Oren-Nayar that passes the white-furnace test and reproduces the characteristic "flattening" and grazing **backscattering (opposition/hot-spot) peak** of rough organic surfaces like bark, soil, moss, and foliage canopies without QON's energy loss — **VERIFIED** ([Portsmouth, Kulla, Hill, *EON: A Practical Energy-Preserving Rough Diffuse BRDF*, JCGT Vol. 14 No. 1, Mar 2025](https://jcgt.org/published/0014/01/06/paper-lowres.pdf)).

### Two-Sided BRDF, Avoiding Black Backlit Leaves, and Back-Face Ambient

* **Why backlit leaves go near-black with GI on (Root Cause Diagnosis):**

  1. **Directional GI / SH probes are evaluated with** $+\mathbf{N}$ **only:** When a leaf is viewed from its shadowed underside ($\mathbf{N}_{\text{view}} \cdot \mathbf{L}_{\text{sun}} < 0$), evaluating irradiance probes strictly along the viewer-facing flipped normal $\mathbf{N}_{\text{view}}$ samples the *dark ground/interior hemisphere*, completely missing the bright sky and sunlit upper-canopy irradiance hitting the **opposite face (**$-\mathbf{N}_{\text{view}}$**)** and transmitting through the leaf — **INFERRED**.

  2. **Geometric-normal terminator gating:** If direct sun transmission is gated by $\mathbf{N}_{\text{geom}} \cdot \mathbf{L} < 0$ alongside $\mathbf{N}_{\text{shaded}} \cdot \mathbf{L} < 0$, normal-mapped leaves fall into a sign-disagreement dead zone where neither reflection nor transmission fires — **INFERRED**.

  3. **Missing multiple scattering inside the crown:** Single-leaf direct transmission drops to zero once the leaf is inside the sun shadow map. In reality, light scatters multiple times through a $3\text{--}8\text{ m}$ crown volume.

* **Fix (Standard Production Thin-Leaf + Crown Transport Model):**

  1. **Direct Sun Two-Sided Transport:** On the front face ($\mathbf{N}$ flipped toward camera), evaluate diffuse + GGX specular with soft wrap $(\mathbf{N}\cdot\mathbf{L} + w) / (1 + w)$ ($w \approx 0.15\text{--}0.25$). On the back face, evaluate **diffuse transmission** $T_{\text{diff}} \cdot \max(0, -\mathbf{N}\cdot\mathbf{L}) / \pi$ PLUS a **view-dependent forward-scattering lobe** $T_{\text{fwd}} \cdot \max(0, \mathbf{V}\cdot -\text{normalize}(\mathbf{L} + \mathbf{N}\delta))^p$ ($p \approx 4\text{--}6$, $\delta \approx 0.3$), tinted warmer yellow-green than diffuse albedo (since blue is strongly absorbed by chlorophyll during transmission) and clamped so $R + T \le 0.9$ — **VERIFIED** for UE/Frostbite structure ([UE Material Properties](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-material-properties)); **INFERRED** for exact tuning constants.

  2. **Two-Sided Ambient / Probe GI Evaluation (CRITICAL):** For thin foliage, evaluate your GI probe / sky SH **on both** $+\mathbf{N}$ **and** $-\mathbf{N}$:
     

     $$
     L_{\text{amb}} = \rho_{\text{diff}} \cdot E_{\text{GI}}(+\mathbf{N}) + \tau_{\text{trans}} \cdot E_{\text{GI}}(-\mathbf{N})
     $$

     
     Evaluating $E_{\text{GI}}(-\mathbf{N})$ lets bright sky light and upper-canopy sun bounce transmit downward through backlit leaves, completely eliminating black undersides when probe GI is enabled — **INFERRED**.

  3. **Crown Multiple Scattering (Chlorophyll's Voxel Opacity Volume):** Query the per-asset voxel opacity volume along the sun vector to obtain crown optical depth $\tau_{\text{sun}}$ and add a multi-scattered translucent sun term $\propto L_{\text{sun}} \cdot \tau_{\text{trans}} \cdot e^{-\sigma \tau_{\text{sun}}} \cdot \Phi_{\text{HG}}(\mathbf{V}\cdot\mathbf{L})$ even when the discrete shadow map reports "in shadow" — **INFERRED** (matching Chlorophyll's stated use of its per-asset voxel opacity volume).

## 5. Screen-Space + Voxel GI Hybrids at 2–3 ms (1440p)

| Technique | Mechanism | Reported Cost (GPU / Res) | Stability Under Foliage Motion | Source & Status | 
| ----- | ----- | ----- | ----- | ----- | 
| **Visibility Bitmask AO + SSGI** (Therrien et al. 2023) | Replaces GTAO's 2 horizon angles per slice with a 32-bit angular visibility bitmask using finite sample thickness $t$; integrates multiple unoccluded sky cones + 1-bounce screen-space GI in $O(1)$ per sample | Exact paper ms table could not be fetched directly (`arxiv.org/html` fetch error); paper reports "low performance overhead" comparable to GTAO ($\approx 0.5\text{--}1.5\text{ ms}$ at 1440p depending on slice/step count — **INFERRED**) | **Best screen-space option for foliage:** finite thickness lets light pass *behind* thin leaves/grass without GTAO halos; temporal accumulation still needs careful clamping on fast foliage sway | **VERIFIED** ([Therrien, Levesque, Gilet, arXiv:2301.11376v2, Jan 2023](https://arxiv.org/html/2301.11376v2)) | 
| **XeGTAO / GTAO** (Jimenez 2016; Intel) | Analytic horizon-angle integration over screen-space depth slices | No 1440p RTX 4080 number verified in this session (Intel XeGTAO targets sub-$1\text{ ms}$ on midrange GPUs — **SECONDARY**) | **Poor on dense thin foliage** without heavy thickness fallback: infinite-thickness horizon assumption creates dark halos around grass blades and leaf silhouettes | **VERIFIED** ([Therrien et al. 2023](https://arxiv.org/html/2301.11376v2)) | 
| **Screen-Space / Split Radiance Cascades** (Sannikov 2024; 2025–2026) | Angular-spatial cascade probes merging near-field high-spatial / low-angular rays with far-field low-spatial / high-angular rays | No verified 1440p open-world 3D foliage ms figure published (**unknown**); *Path of Exile 2* ships 2D/height-constrained RC, while Bevy's developer abandoned screen-space RC for general 3D due to parallax/occlusion issues | **Zero Monte Carlo noise** (deterministic), so no boiling, **but** pure screen-space RC fails on thin overlapping 3D foliage depth layers and suffers off-screen energy loss unless paired with a 3D world cascade | **SECONDARY** ([Bevy dev blog, Aug 2024](https://jms55.github.io/posts/2024-08-30-bevy-fourth-birthday/); [Graphics Programming Weekly / Split RC summary](https://www.jendrikillner.com/article_database/); [Reddit discussion](https://www.reddit.com/r/GraphicsProgramming/comments/1modl30/whats_good_gi_method_to_implement_in_a_hobby/)) | 
| **Voxel Cone Tracing / SVOGI** (CryEngine / *KCD 1 & 2*) | Traces coarse cones through a hierarchical voxel opacity + radiance clipmap for multi-bounce diffuse GI and macro AO | No verified 1440p RTX 4080 ms figure in primary docs (**unknown**) | **Temporally rock-solid in motion** (world-space, noise-free); **limitation:** CryEngine explicitly excludes procedural/merged grass from casting occlusion, and dynamic bounce lags 1–2 frames | **SECONDARY** ([CryEngine SVOGI technical overview via TweakTown](https://www.tweaktown.com/news/49267/kingdom-deliverance-teased-svogi-effects-looks-incredible/index.html)) | 

### Best 2–3 ms Pure-Raster Hybrid for Foliage Motion Stability

Pair **world-space voxel/probe GI (macro occlusion + multi-bounce color,** $> 1.5\text{ m}$**)** with **Visibility Bitmask AO + 1-bounce SSGI (near-field** $0.1\text{--}1.5\text{ m}$**, finite thickness)** — **INFERRED**. This avoids Monte Carlo RT denoising lag, kills GTAO thin-leaf halos, stays inside $2\text{--}3\text{ ms}$ at 1440p, and aligns closely with Chlorophyll's reported screen-space + voxel GI hybrid — **INFERRED**.

## 6. Contact Shadows and Micro-Occlusion for Vegetation

* **Bend Studio Wavefront Screen-Space Shadows (*Days Gone*, Aldridge SIGGRAPH 2023):**

  * Dispatches compute wavefronts **aligned along the projected 2D sun direction** so each thread walks a contiguous line in the depth buffer with coherent L1/L2 cache reads and **no stochastic per-frame jitter** — **VERIFIED** for origin/purpose ([Bend Studio blog, *Inside Bend: Screen Space Shadows*](https://www.bendstudio.com/blog/inside-bend-screen-space-shadows/)); **SECONDARY** for implementation/tuning parameters (e.g., typical length $\approx 0.35\text{ m}$, thickness $\approx 0.1\text{ m}$, fade distance $\approx 80\text{ m}$ in community ports: [ValheimVisuals Bend SSS port](https://old.thunderstore.io/c/valheim/p/Ivvty/ValheimVisuals/); [AK Plugins docs](https://unreal-engine-ace7bf.gitlab.io/docs/xfsdk-light/screen-space-shadows)).

  * *Why your contact shadows look weak & unstable:* Stochastic per-pixel marches with short lengths or soft fade read faint on grass and flicker under temporal AA, whereas Bend's wavefront traversal lets you march **32–60 screen-space texels deterministically** at low cost, cleanly grounding grass blades, twigs, and distant unshadowed foliage (the exact strategy Chlorophyll uses for grass and far animated trees) — **INFERRED**.

  * *Thickness heuristic & artifacts:* Fixed world-space thickness causes false shadows on distant grass (too thick) or missed shadows up close (too thin); scaling thickness proportional to view depth (`thickness = base + k * linearDepth`) and fading at grazing sun/normal angles prevents self-shadowing bands — **INFERRED**.

* **Specular Occlusion from AO:**

  * Applying diffuse AO directly to specular reflections fails at grazing angles or leaves rough foliage looking plastic/silvery under a bright sky. Lagarde & de Rousiers (*Moving Frostbite to PBR 2.0*, 2014) derived an empirical specular occlusion term from diffuse AO, roughness, and $\mathbf{N}\cdot\mathbf{V}$:
    

    $$
    \text{SO} = \text{clamp}\!\left((\mathbf{N}\cdot\mathbf{V} + \text{AO})^{\exp(-16 \cdot r - 1)} - 1 + \text{AO},\; 0,\; 1\right)
    $$

    
    which strongly darkens specular reflections in occluded canopy interiors and grass bases while letting grazing highlights survive on exposed tips — **SECONDARY** (well-known formula from [Lagarde & de Rousiers, *Moving Frostbite to PBR 2.0*](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/course-notes-moving-frostbite-to-pbr-v2.pdf); exact equation page not directly excerpted in search snippet).

* **Grass Base Darkening (Micro-Occlusion):**

  * Because screen-space AO cannot resolve millimeter-scale occlusion between overlapping grass stems, shipping open-world grass shaders apply an analytical vertical gradient `ao_base = lerp(0.15..0.30, 1.0, pow(bladeHeight01, 1.5..2.0))` to both diffuse and specular ambient — **INFERRED**. Combined with `min(screenAO, ao_base)` (rather than blind multiplication where AO overlaps), this immediately grounds meadows without muddy black patches — **INFERRED**.

## 7. Shadow Maps for Forests

* **Virtual Shadow Maps (VSM) vs. Cached Cascaded Shadow Maps (CSM):**

  * A VSM is a conceptual $16\text{k} \times 16\text{k}$ clipmap subdivided into $128 \times 128$ physical pages, rendering only visible invalidated pages — **VERIFIED** ([UE5 VSM docs](https://dev.epicgames.com/documentation/unreal-engine/virtual-shadow-maps-in-unreal-engine); [StraySpark Studio UE 5.7 VSM guide, Apr 2026](https://www.strayspark.studio/blog/virtual-shadow-map-optimization-open-worlds-ue5-7)).

  * *The Foliage Invalidation Trap (Critical Cost Data):* Animated foliage (WPO or skeletal wind) invalidates cached VSM pages every frame. Disabling shadow WPO on small foliage dropped forest-floor VSM cost from $8.2\text{ ms}$ **to** $3.1\text{ ms}$ **on PS5**; high-end PC open-world targets budget $1.5\text{--}2.5\text{ ms}$ for VSMs by capping shadow-WPO distance ($\approx 30\text{ m}$ mid-plants, $\approx 80\text{ m}$ trees) — **SECONDARY** ([StraySpark Studio, Apr 2026](https://www.strayspark.studio/blog/virtual-shadow-map-optimization-open-worlds-ue5-7)).

  * *Cached CSM + Small Dynamic Near Map (Chlorophyll's Hybrid):* If you already have cached CSMs, separating a **static/semi-static shadow cache** (trunks + static canopies) from a **small dynamic near map** (animated trees within $\approx 30\text{--}60\text{ m}$) while excluding grass (relying on Bend SSS) captures 90% of VSM's performance advantage without a full virtual page-table rewrite — **INFERRED**.

  * *UE 5.8 Prefiltered Distant Shadows (2026):* Epic added an experimental `PrefilteredDistant` clipmap mode that renders distant casters at much lower resolution and prefilters + temporally reprojects them, matching ground truth better than SMRT at lower cost — **SECONDARY** ([Tom Looman, *UE 5.8 Performance Highlights*, Aug 2026](https://tomlooman.com/unreal-engine-5-8-performance-highlights/)).

* **Filtering Leaf Shadows (PCSS / SMRT vs. Moment Shadow Maps) & Sun Angular Size:**

  * **Real sun angular diameter is** $\approx 0.53^\circ$ ($\tan(0.265^\circ) \approx 0.00463$). A leaf $15\text{ m}$ above the forest floor casts a geometric penumbra of $15 \times 0.00926 \approx 14\text{ cm}$ (turning individual small leaf holes into soft circular solar pinhole images—dappled sunflecks), while a twig $0.2\text{ m}$ above ground has a razor-sharp $\approx 2\text{ mm}$ penumbra — **INFERRED** (basic solar geometry). Uniform PCF destroys either the near crispness or the high-canopy softness — **VERIFIED** ([UE5 VSM docs](https://dev.epicgames.com/documentation/unreal-engine/virtual-shadow-maps-in-unreal-engine)).

  * **Shadow Map Ray Tracing (SMRT / PCSS):** Traces 4–8 shadow-map rays (4–8 samples/ray) within the sun's `Source Angle` cone for true contact hardening — **VERIFIED** ([UE5 VSM docs](https://dev.epicgames.com/documentation/unreal-engine/virtual-shadow-maps-in-unreal-engine)). *Artifacts:* Wide penumbras show stochastic noise unless temporally denoised, and adaptive ray counts can create moving pixel artifacts (`r.Shadow.Virtual.SMRT.AdaptiveRayCount 0` workaround) — **SECONDARY** ([UE Forums, Jul 2024](https://forums.unrealengine.com/t/virtual-shadow-maps-for-high-quality-rendering-cinematics/1922741); [Hellblade 2 VSM review, Sep 2024](https://medium.com/@shinsoj/technical-review-lumen-and-vsm-in-hellblade-2-925d17f65e04)).

  * **Moment Shadow Maps (MSM, Peters & Klein 2015; Improved MSM, JCGT 2017):** Store 4 depth moments (`64 bpp` or `128 bpp`) so shadow maps can be pre-blurred/mipmapped cheaply, giving clean antialiased shadows at high resolutions — **VERIFIED** ([Peters & Klein, *Improved Moment Shadow Maps*, JCGT 2017](https://www.jcgt.org/published/0006/01/03/paper-lowres.pdf)). *Failure mode on forests:* Multi-layer overlapping leaf canopies at vastly different depths (high crown + understory + trunks) cause **light leaking** in 4-moment reconstructions unless bumped to 6 moments / `128 bpp`, which triples bandwidth — **VERIFIED** ([Peters & Klein, 2017](https://www.jcgt.org/published/0006/01/03/paper-lowres.pdf)); **INFERRED** for multi-layer forest penalty.

## 8. Physical & Perceptual Calibration Data (Numeric Targets)

### Measured Irradiance & Luminance Ratios (Verified Primary Data)

| Quantity | Measured Value / Ratio | Notes & Source | Status | 
| ----- | ----- | ----- | ----- | 
| **Closed deciduous forest floor diffuse shade vs. open sky** | $\approx 1.5\text{--}5\%$ **of open irradiance** ($< 30\text{ W/m}^2$ diffuse shade; $\approx 3\%$ in oak-hornbeam at Báb; $\approx 1.5\%$ in closed summer European beech) | Upper $4\text{--}5\text{ m}$ of canopy absorbs $\approx 50\%$ of PAR; full crowns filter 75–95% ([Eliáš et al. / *Sunflecks in forest communities*, 2017](https://www.researchgate.net/publication/313530322_Sunflecks_in_forest_communities_and_their_importance_for_plant_life_in_a_forest_understorey)) | **VERIFIED** | 
| **Sunfleck vs. forest-floor shade** | $10\times$ **to** $100\times$ (1–2 orders of magnitude, i.e. $+3.3$ to $+6.6\text{ EV}$) | Clear-day sunflecks jump 1–2 orders of magnitude above the $< 30\text{ W/m}^2$ diffuse shade floor ([*Sunflecks in forest communities*, 2017](https://www.researchgate.net/publication/313530322_Sunflecks_in_forest_communities_and_their_importance_for_plant_life_in_a_forest_understorey)) | **VERIFIED** | 
| **Clear sky luminance** | $2{,}800\text{--}5{,}000\text{ cd/m}^2$ **(nits)** | Measured on-site for *Gran Turismo 7* Scapes ([Polyphony Digital SIGGRAPH 2025 slides, p. 32](https://blog.selfshadow.com/publications/s2025-shading-course/pdi/s2025_pbs_pdi_slides.pdf)) | **VERIFIED** | 
| **Sunlit cloud top luminance** | **up to** $\approx 34{,}000\text{ cd/m}^2$ **(nits)** | Same on-site *GT7* Scapes measurements; clouds & sunlit snow are among the few non-sun natural surfaces exceeding $10{,}000\text{ nits}$ ([Polyphony Digital SIGGRAPH 2025, p. 32](https://blog.selfshadow.com/publications/s2025-shading-course/pdi/s2025_pbs_pdi_slides.pdf)) | **VERIFIED** | 
| **Sunlit foliage vs. clear sky luminance** | **Sunlit broadleaf foliage** $\approx 1{,}500\text{--}4{,}500\text{ cd/m}^2$ vs. clear sky $2{,}800\text{--}5{,}000\text{ cd/m}^2$ (ratio $\approx 0.4\times\text{--}1.2\times$) | Derived from $\approx 80{,}000\text{--}100{,}000\text{ lux}$ direct sun $\times \approx 0.06\text{--}0.14$ linear green reflectance $/ \pi$ — **INFERRED** | **INFERRED** | 
| **Real forest photograph OKLab / CIE** $L^*$ **distributions** | **Unknown / no canonical published dataset found** | While OKLab (Ottosson 2020) and photographic film saturation curves (peak saturation in $\approx 25\text{--}65\%$ display luminance) are documented ([EmulsionPresets Styleguide](https://github.com/peva3/EmulsionPresets/blob/main/STYLEGUIDE.md)), no peer-reviewed paper in this search tabulates a standard OKLab / CIE $L^*$ histogram benchmark for sunlit temperate forest photography — **plainly unknown**. Build your own 20–30 photo RAW-calibrated reference set (shot with a ColorChecker in sunlit beech/hornbeam woods) rather than relying on arbitrary numbers. | **VERIFIED** (gap) / **INFERRED** (remedy) | 

## Ranked Implementation Roadmap

| Rank | Technique | Question(s) | Expected Visual Gain | Cost (1440p RTX 4080 est. / reported) | Risk | Primary Sources | 
| ----- | ----- | ----- | ----- | ----- | ----- | ----- | 
| **1** | **Two-sided ambient/probe evaluation** $E_{\text{GI}}(-\mathbf{N})$ **+ tinted forward-scatter transmission + crown voxel optical-depth multi-scatter** | Q4 | Immediately fixes near-black backlit leaves with GI on; restores luminous green canopy depth and glowing backlit crowns | $\approx 0.05\text{--}0.15\text{ ms}$ shader ALU (0 extra passes) — **INFERRED** | Over-bright crowns if $R + T \le 0.9$ energy conservation isn't enforced | [Bousquet et al. 2005](https://www.ipgp.fr/~jacquemoud/publications/bousquet2005b.pdf); [Féret et al. 2008](https://www.ipgp.fr/~jacquemoud/publications/feret2008.pdf); [UE Material Properties](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-material-properties) | 
| **2** | **Macro canopy sky-visibility map / voxel-volume upward cone occlusion on terrain & trunks** | Q3, Q8 | Drops forest floor sky ambient from 10–14% down to physical **1.5–5%** even without probe GI; creates authentic sunlit-clearing vs. deep-forest contrast | $\approx 0.1\text{--}0.2\text{ ms}$ lookup (precomputed/amortized from existing voxel far-field) — **INFERRED** | Over-darkening forest floor if paired without Rank #3 (Local Exposure) or multi-bounce green GI | [Patry SIGGRAPH 2021](https://www.glowybits.com/blog/2022/12/18/ghost_talks/); [UE5 MDF Docs](https://dev.epicgames.com/documentation/unreal-engine/mesh-distance-fields-in-unreal-engine); [Sunflecks study 2017](https://www.researchgate.net/publication/313530322_Sunflecks_in_forest_communities_and_their_importance_for_plant_life_in_a_forest_understorey) | 
| **3** | **Local Exposure (Exposure Fusion Laplacian pyramid or Bilateral+Gaussian grid) + Upper-Percentile / Center-Weighted Auto-Exposure (70–95%)** | Q1, Q2 | Keeps trunks and deep shade readable under a strict 3 EV global exposure cap *without* lifting forests to washed-out mid-grey or blowing out clearings | $\approx 0.15\text{--}0.3\text{ ms}$ ($\approx 0.13\text{ ms}$ reported on GTX 1070 for UE5 Bilateral) — **SECONDARY/INFERRED** | Halos around bright canopy sky holes or swimming in motion if pushed too hard (keep `Shadow Contrast` $0.75\text{--}0.85$) | [Wronski Feb 2022](https://bartwronski.com/2022/02/28/exposure-fusion-local-tonemapping-for-real-time-rendering/); [UE5 `LocalExposureMethod`](http://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/LocalExposureMethod?application_version=5.5); [Hable Epic Blog 2020](https://www.unrealengine.com/en-US/tech-blog/how-epic-games-is-handling-auto-exposure-in-4-25) | 
| **4** | **Bend-style deterministic wavefront Screen-Space Shadows + depth-scaled thickness + analytical grass base darkening** | Q6 | Grounds grass, ferns, bark crevices, and distant unshadowed foliage cleanly; removes stochastic contact-shadow flicker under DLAA | $\approx 0.2\text{--}0.4\text{ ms}$ compute — **INFERRED** | False shadow bands on grazing terrain if depth-scaled thickness and normal-fade aren't tuned | [Bend Studio SIGGRAPH 2023](https://www.bendstudio.com/blog/inside-bend-screen-space-shadows/) | 
| **5** | **Visibility Bitmask AO + 1-bounce SSGI (**$0.1\text{--}1.5\text{ m}$**) + Lagarde Specular Occlusion** | Q5, Q6 | Adds crisp near-field crevice/trunk/canopy occlusion and green bounce without GTAO thin-foliage halos, while killing silvery specular sheen in shade | $\approx 0.6\text{--}1.2\text{ ms}$ at 1440p (fits inside $2\text{--}3\text{ ms}$ hybrid budget alongside voxel/probe GI) — **INFERRED** | Global thickness heuristic $t$ needs tuning to balance grass transparency vs. rock solidity | [Therrien et al. 2023](https://arxiv.org/html/2301.11376v2); [Lagarde & de Rousiers Frostbite PBR](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/course-notes-moving-frostbite-to-pbr-v2.pdf) | 
| **6** | **Contact-hardening sun shadows (PCSS / SMRT-style blocker search in cascade 0–1 + static/dynamic cache split)** | Q7 | Reproduces real $0.53^\circ$ solar penumbras: razor-sharp ground twig shadows and soft dappled sunflecks from $15\text{ m}$ tree crowns | $\approx 0.5\text{--}1.0\text{ ms}$ extra filter cost on cascades 0–1 (VSM full budget: $1.5\text{--}2.5\text{ ms}$ PC) — **SECONDARY/INFERRED** | Penumbra noise under DLAA if ray count is too low or adaptive ray count shifts per frame | [UE5 VSM Docs](https://dev.epicgames.com/documentation/unreal-engine/virtual-shadow-maps-in-unreal-engine); [StraySpark Studio Apr 2026](https://www.strayspark.studio/blog/virtual-shadow-map-optimization-open-worlds-ue5-7) | 
| **7** | **EON (Energy-Preserving Oren-Nayar) rough diffuse BRDF on bark, soil, and distant canopy/voxels** | Q4 | Reproduces natural backscattering / hot-spot brightening and organic flatness of rough bark, forest floor litter, and canopies without QON darkening | $\approx 0.03\text{--}0.05\text{ ms}$ ALU — **INFERRED** | Minimal; strictly energy-preserving and drop-in replacement for Lambert | [Portsmouth et al., JCGT 2025](https://jcgt.org/published/0014/01/06/paper-lowres.pdf) | 

## Recommended Companion Talk

* **Jasmin Patry (Sucker Punch), *Lighting, Atmosphere, and Tonemapping in Ghost of Tsushima*:** Covers Sucker Punch's pure-raster sky-visibility SH probes, dynamic bounce lighting, and bilateral-grid + Gaussian local tonemapping in detail — [YouTube video](https://www.youtube.com/watch?v=GOee6lcEbWg).