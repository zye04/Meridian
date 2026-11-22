# Deep Research Report: Meridian (Briefs A, B, and C)

**Date:** 2026-11-22  
**Scope:** Combined research pass for Brief A (Dense Foliage & Alpha-Tested Grass), Brief B (Leaf/Canopy Appearance & Non-RT Canopy Lighting), and Brief C (DLSS 4 / DLAA Temporal Reconstruction & Motion Clarity).  
**Evidence Labels:** All claims below are tagged **established** (documented, shipped), **reported** (one credible source), or **speculative** (inference), kept in separate sentences. Meridian codebase references cite the repository snapshot (`meridian.xml` parts 00–08 and `2026-11-22-deep-research-briefs.md`).

---

# Brief A: Dense Foliage at Distance, and the Alpha-Tested Grass That Remains

### A1. UE 5.7 Nanite Foliage vs. Meridian

**Answer & Evidence**
* **Representation:** UE 5.7+ Nanite Foliage (Experimental) combines three systems: **Nanite Assemblies** (micro-instancing up to 65k branch/frond part instances inside one mesh hierarchy, evaluated on-demand during cluster culling), **Nanite Voxels** (near-pixel-sized $4 \times 4 \times 4$ aggregate voxel bricks generated in the Nanite builder when clusters shrink below a screen-error threshold), and **Nanite Skinning** (skeletal bone hierarchies driven by the Dynamic Wind plugin) — **established** (Epic Games, *Nanite Foliage*, UE 5.7/5.9 docs, 2025–2026, https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage).
* **Why Epic abandoned cards and WPO:** Epic explicitly rejects alpha-masked cards (programmable raster cost + overdraw + flat look), 100% triangle representations (poor distant cluster culling/simplification + huge disk footprint), and World Position Offset (per-vertex cost + conservative cluster bounds that break culling) — **established** (Epic Games, *Nanite Foliage*, 2025–2026, https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage).
* **Switching, coverage, and normals:** Nanite switches clusters to $4 \times 4 \times 4$ voxel bricks when voxel error beats triangle error, rasterizing voxel clusters front-to-back into the visibility buffer — **established** (Epic Games, *Nanite Foliage*, 2025–2026, https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage). Each voxel stores a normal distribution and selects a normal stochastically per pixel at G-buffer write, with Epic noting plans to fold normal variance into roughness instead — **reported** (Meridian Research A citing Epic 5.7 docs, Nov 2026).
* **Costs & numbers:** In *The Witcher 4* tech demo tree example (41M triangles, 12 unique assembly parts, 2,160 part instances, 850 skeletal bones), Assemblies shrank `.uasset` disk size from **3.5 GB to ~29 MB** and single-view streaming memory from **~36 MB to ~2.7 MB**, enabling 500k tree instances (1–10M triangles each) — **established** (Epic Games, *Nanite Foliage*, 2025–2026, https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage). No per-pass GPU ms breakdown for Nanite Voxels is published by Epic; third-party profiling on UE 5.7 reports that Virtual Shadow Map (VSM) shadow cost becomes the dominant bottleneck before instance count does — **reported** (StraySpark Studio, Mar 2026, https://www.strayspark.studio/blog/ue5-nanite-foliage-procedural-placement-performance).
* **Known artifacts & what Epic gave up:** Nanite Foliage remains Experimental in 5.7 with incomplete physics/collision support and high skeletal instancing overhead in early builds — **established** (Digital Production, Nov 2025, https://digitalproduction.com/2025/11/12/unreal-engine-5-7-foliage-pcg-and-in-editor-ai/; UE Forums 5.7 Preview, Sep 2025, https://forums.unrealengine.com/t/unreal-engine-5-7-preview/2658958?page=3). Hardware ray tracing in UE 5.7 still traces coarse fallback meshes rather than Nanite Voxels, causing reported RT shadow mismatches — **reported** (UE Forums, 2025, https://forums.unrealengine.com/t/bad-raytraced-shadows-with-voxelised-nanite-foliage-in-5-7/2696233).

**Point-by-Point Comparison & Implications for Meridian**
* **What Meridian already matches:** Meridian's 3-tier tree pipeline (cluster LOD DAG -> rasterized prototype voxels -> far-field cell bricks) already mirrors Nanite Voxels + Chlorophyll's far tile aggregates without billboards.
* **Where Meridian diverges (and pays for it):**
  1. *No Assemblies:* Meridian flattens Megascans trees into ~1–4M unique triangles per prototype, forcing a hard variant cut (~30 prototypes / ~30M triangles $\approx 3.5$ GB). Adding micro-instanced parts (branch prototypes inside a tree prototype) is the exact architectural mechanism that lets Epic hold 41M-triangle trees in 29 MB — **speculative**.
  2. *Forward shading vs. Visibility Buffer:* Nanite Voxels write a compact vis-buffer and shade once in compute, whereas Meridian's `voxel_instance.frag` shades every rasterized 2 px quad forward (including cascade PCF and aerial perspective).
  3. *Binary hashed voxel coverage vs. filtered opacity:* `voxel_bricks.comp` drops voxels via a deterministic binary hash (`noise < opacity`) on 2 px quads. Because each voxel is only 2 px wide, that binary decision creates G1's salt-and-pepper speckle.

---

### A2. Keeping Crown Coverage Through Simplification

**Answer & Evidence**
* **Nanite `Preserve Area`:** When simplifying disjoint foliage triangles/quads, Nanite redistributes the area of collapsed/removed leaves by scaling up surviving leaves (and thickening grass ribbons) — **established** (Epic Games, *Nanite Technical Details* & *Working with Nanite-Enabled Content*, 2025–2026, https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-technical-details).
* **Stochastic Simplification (Cook et al. 2007):** Randomly pruning disjoint sub-elements per LOD level and scaling surviving elements by $1/\sqrt{p_{\text{keep}}}$ preserves total projected area and silhouette density — **established** (Cook, Halstead, Planck, Ryu, SIGGRAPH 2007, classic still standing, https://graphics.pixar.com/library/StochasticSimplification/). Meridian already uses this once at cook time in `tools/cook/tree.cpp`, but **not** inside the runtime cluster LOD DAG (`engine/src/cluster.cpp`).
* **Hybrid Mesh-Volume LODs (Loubet & Neyret 2017):** Standard quadric simplification collapses disjoint leaves at coarse levels because QEM measures geometric distance, not silhouette coverage; switching sub-resolution clusters to prefiltered microflake/extinction volumes preserves optical depth and projected area — **established** (Loubet & Neyret, CGF 2017, classic still standing, https://hal.archives-ouvertes.fr/hal-01468817v1).
* **Quantifying visible coverage loss:** No universal perceptual threshold is standardized in literature; Epic's `Preserve Area` docs give no numeric percentage, while Meridian's own M2a spec targets **$\le 5\%$ silhouette coverage drift** across LOD levels (a practical threshold, since $> 10\%$ reads as visible crown thinning or brightening against sky) — **speculative**.

**Failure Modes & Implications for Meridian**
* **Contradiction with current Meridian pipeline:** Tracing leaf cards into opaque triangles (`leaf-geometry` branch) turns each card into 3–23 disjoint leaf pieces. Standard `meshopt_simplify` inside `clodBuild` (`engine/src/cluster.cpp`) will aggressively drop small disjoint leaf pieces at coarse cluster levels—repeating the exact "bare poles past 200 m" failure seen on Megaplants.
* **Fix path:** Either (a) hook area-preserving pruning + scaling (Cook et al. / Nanite `Preserve Area`) into each DAG level of `cluster.cpp` for leaf primitives, or (b) switch to prototype voxels earlier (e.g., 3–4 px) once G1's voxel shading and filtered coverage are fixed. Without one of those two, Gate 2 of the leaf-geometry spec will fail — **speculative**.

---

### A3. Sub-Pixel Opaque Leaf Triangles Under Temporal Upscaling (DLAA / DLSS 4)

**Answer & Evidence**
* **Geometric vs. Specular Aliasing:** Removing alpha-test kills *mip-cutoff flicker*, but sub-pixel opaque triangles ($< 1$ px) still flip coverage per pixel under sub-pixel Halton jitter, causing high-frequency geometric shimmer; simultaneously, curved leaf normals and normal maps varying across sub-pixel footprints produce intense specular fireflies (G18) — **established** (Kaplanyan et al., HPG 2016, https://diglib.eg.org/handle/10.2312/hpg20161201; Tokuyoshi & Kaplanyan, *Improved Geometric Specular Antialiasing*, I3D 2019, https://pbr-book.org/4ed/References).
* **Geometric Specular AA (Tokuyoshi & Kaplanyan 2019):** Estimates screen-space normal variance from `dFdx(N)` and `dFdy(N)` in the fragment shader and widens GGX roughness $\alpha^2 \leftarrow \text{clamp}(\alpha^2 + \sigma_N^2, 0, 1)$, costing ~10–15 ALU instructions and zero extra memory — **established** (Unity HDRP *Geometric Specular Anti-Aliasing* docs, https://docs.unity3d.com/Packages/hdrp/manual/Geometric-Specular-Anti-Aliasing.html).
* **Toksvig / vMF normal-map prefiltering:** Baking normal-map mip variance into roughness mipmaps at cook time removes texture-driven specular aliasing at zero runtime cost — **established** (Kaplanyan et al., HPG 2016, https://diglib.eg.org/handle/10.2312/hpg20161201).
* **DLSS 4 / 4.5 Transformer behavior on sub-pixel foliage:** Preset M (2nd-gen transformer, Jan 2026) improves fine-edge stability on thin grass and trees compared to Preset K in *Black Myth: Wukong* and reduces disocclusion pixelation in *Horizon Forbidden West* — **reported** (Digital Foundry, Jan 2026, https://www.digitalfoundry.net/features/dlss-4-5-review-where-its-better-and-where-it-needs-work; PCOptimizedSettings, Jan 2026, https://pcoptimizedsettings.com/nvidia-dlss-4-preset-k-vs-dlss-4-5-preset-l-vs-m-dlss-4-looks-better-than-4-5-in-this-game/). However, Preset M and L amplify shimmering on fine, high-contrast ground/foliage details during camera panning if specular highlights or upstream stochastic passes are unfiltered — **reported** (MSFS 2024 user reports, Feb 2026, https://forums.flightsimulator.com/t/dlss-4-5-graphics-shimmering/756473; Digital Foundry, Feb 2026, https://www.digitalfoundry.net/features/dlss-4-5-preset-l-tested-how-good-can-a-4k-upscale-from-720p-look).
* **Failure modes:** Over-aggressive geometric specular AA (`fwidth(N)`) on dense $2 \times 2$ pixel quads can look overly matte on curved surfaces at grazing angles; dithered LOD cross-fades (like Meridian's 10% voxel switch band using IGN in `mesh.frag` and `voxel_band.frag`) inject high-frequency per-frame switching that Preset M can resolve as crawl if the band is too wide — **speculative**.

**Implications for Meridian**
* Meridian's `mesh.frag` currently has **no geometric specular AA** and **no Toksvig roughness widening**, while leaves use a fixed $F_0 = 0.04$. Adding Tokuyoshi & Kaplanyan's `dFdx/dFdy(N)` roughness widening to `mesh.frag` (for leaves, bark, and terrain) plus cook-time Toksvig on normal+roughness mips will directly attack G15/G18 fireflies and motion sparkle at near-zero cost.

---

### A4. Alpha-Tested Grass Under DLSS / DLAA

**Answer & Evidence**
* **Cut-out mip bias (-1):** Meridian already measured `-1` as *worse* in motion. This matches standard signal theory: negative mip bias increases spatial frequencies above Nyquist on alpha-test edges, causing sub-pixel binary flips under jitter when no upscaling headroom exists (DLAA is 1:1 native, where NVIDIA recommends mip bias `0`) — **established** (Meridian `decisions.md`, `upscaler.h` citing DLSS Programming Guide).
* **Coverage-preserving alpha mips (Castaño 2010):** Prevents distant grass from vanishing by rescaling mip alpha to keep mip-0 coverage — **established** (Castaño, 2010, classic still standing, https://www.ludicon.com/castano/blog/articles/computing-alpha-mipmaps/). Meridian already ships this (`ktx2.cpp` `coverageMips`), including the plateau-midpoint fix. *Failure mode:* preserves average density, but does not stop per-frame edge aliasing on sub-pixel blades.
* **Anti-aliased alpha test (`fwidth` rescaling) & Alpha-to-Coverage (Golus 2017):** Rescaling alpha around the cutoff by `max(fwidth(alpha), 0.0001)` keeps cutout edges crisp at ~1 px width without blurry mip shrinkage, and pairs with MSAA Alpha-to-Coverage — **established** (Ben Golus, Aug 2017, classic still standing, https://bgolus.medium.com/anti-aliased-alpha-test-the-esoteric-alpha-to-coverage-8b177335ae4f). *Failure mode:* A2C requires MSAA (incompatible with Meridian's single-sample forward/G-buffer attachments without a major bandwidth hit) and only resolves one layer of partial coverage per sample mask — **established** (Golus, 2017).
* **Hashed alpha testing (Wyman & McGuire 2017):** Replaces fixed cutoff with an object-space hash so distant alpha cards thin stochastically without disappearing — **established** (Wyman & McGuire, I3D 2017, https://research.nvidia.com/sites/default/files/pubs/2017-02_Hashed-Alpha-Testing/Wyman2017Hashed.pdf). *Failure mode:* turns aliasing into high-frequency noise that relies heavily on temporal accumulation, which shimmers during fast 5 m/s strafes and 1 rad/s turns — **established**.
* **Geometric blades (shipped practice):** *Ghost of Tsushima* generates individual curved geometry blades on the GPU (no alpha test on grass), clamping blade width to pixel footprint and blending normals toward the terrain normal with distance — **reported** (Wohllaib, GDC 2021 summaries; session page https://gdcvault.com/play/1027214). *Horizon Forbidden West* kept alpha-tested foliage cards for dense plants, solving the overdraw/quad-overshading cost with a 32-bit Visibility Buffer + async-compute deferred texturing — **established** (James McLaren, Guerrilla, GDC 2022, https://media.gdcvault.com/GDC+2022/Speaker+Slides/AdventuresWithDeferred_McLaren_James.pdf). *The Witcher 4* demo uses geometric Nanite foliage + TSR thin-geometry detection — **established** (Tom Looman, Nov 2025, https://tomlooman.com/unreal-engine-5-7-performance-highlights/).

**Implications for Meridian**
* **Plain contradiction check:** Trying to fix alpha-tested grass card shimmer solely via mip bias or temporal tuning fights physics—sub-pixel binary cutouts cannot be temporally stabilized at 5 m/s strafe or 1 rad/s turn without smearing.
* If grass stays as Megascans alpha cards, the highest-stability recipe under DLAA is: (1) keep `mipBias = 0` (or test **positive** `+0.5` on the cutout only to soften sub-pixel blades), (2) blend blade normals rapidly toward `terrainNormal()` by 15–30 m (Meridian currently only blends normals during `inStreamFade < 1.0`, which doesn't start until ~30–60 m), and (3) add geometric specular AA (`fwidth(N)`) so card edges don't sparkle. Long-term, geometric grass clumps (or screen-space blade-width clamping) beat alpha cards for motion clarity — **speculative**.

---

### A5. Wind on Dense Geometry and Voxels

**Answer & Evidence**
* **Hierarchical / Skeletal Skinning vs. WPO:** UE 5.7's **Nanite Skinning + Dynamic Wind** animates trees via skeletal bone hierarchies (e.g., 850 bones) instead of per-vertex WPO, because bone transforms are evaluated per-bone and give tight, exact cluster bounds for HZB occlusion culling and exact previous-frame bone matrices for motion vectors — **established** (Epic Games, *Nanite Foliage*, 2025–2026, https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage; Epic dev reply, Nov 2025, https://forums.unrealengine.com/t/foliage-in-current-version-and-forward-looking/2685424). Crucially, bone transforms deform both triangle clusters and Nanite Voxel bricks consistently — **established** (Epic Games, *Nanite Foliage*, 2025–2026).
* **Pivot Painter / Hierarchical Vertex Wind:** Older UE/AAA pipelines encode branch hierarchy pivots in textures/UVs and evaluate vertex displacement twice (current and previous wind state) to emit exact motion vectors — **established** (classic practice; Meridian already carries `scene.wind` and `scene.previousWind` in `common.glsl` for this exact contract).
* **Failure modes & costs:** WPO forces bloated conservative cluster bounds (hurting HZB culling) and disables Nanite fast-path rasterization — **established** (Epic Games, 2025). Shadow maps (CSM/VSM) are the biggest trap: animating foliage invalidates cached shadow pages/cascades every frame — **established** (Epic Games VSM docs; Chlorophyll Engine F19 solves this by keeping shadows semi-static + a small dynamic shadow map around the player, leaving distant animated trees to contact shadows only).

**Implications for Meridian**
* Because Meridian caches all 5 sun shadow cascades until the camera moves (`renderer_shadows.cpp`), adding wind to trees will **break cascade caching** unless you follow Chlorophyll's split: animate wind only inside cascade 0–1 (or accept static shadows on cascades 2–4 where sway is sub-texel) — **speculative**.
* For prototype voxels, applying a single low-frequency trunk/canopy bend matrix (using `windOffset` / `gust` at the tree's root in `voxel_instance.mesh` for both current and previous frames) animates both clusters and voxels cohesively with exact motion vectors and zero per-voxel bone cost — **speculative**.

---

### A6. Memory for Dense Foliage (Compression & Streaming)

**Answer & Evidence**
* **Nanite compression (Karis et al. 2021):** Quantized cluster-local vertex positions (variable bit-width relative to cluster anchor), quantized normals/UVs, and generalized triangle-strip index encoding achieve roughly **~14 bytes per triangle** in memory (~14.4 B/tri) and similar on disk (classic still standing) — **reported** (Karis et al., *Nanite: A Deep Dive*, SIGGRAPH 2021, https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf).
* **AMD Dense Geometry Format (DGF, 2024):** Packs up to 64 triangles and 64 vertices into fixed **128-byte blocks** using a 24-bit signed integer grid anchor + variable-bit per-axis LSB offsets and generalized triangle strips, achieving compression rates comparable to Nanite while supporting direct hardware ray-tracing access — **established** (Barczak, Benthin, McAllister, AMD, July 2024, https://www.researchgate.net/publication/382442920_DGF_A_Dense_Hardware-Friendly_Geometry_Format_for_Lossily_Compressing_Meshlets_with_Arbitrary_Topologies).
* **Nanite Assemblies (2025):** Re-instancing repeatable branches cuts tree memory by ~10–$100\times$ (3.5 GB -> 29 MB disk; 36 MB -> 2.7 MB streamed) — **established** (Epic Games, *Nanite Foliage*, 2025, https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage).

**Implications for Meridian**
* **Huge low-hanging win:** Meridian currently stores an uncompressed **48-byte `Vertex`** (`position` f32$\times 3$, `normal` f32$\times 3$, `uv` f32$\times 2$, `tangent` f32$\times 4$) plus 4 B cluster vertex indices and 4 B per triangle, totaling **~110–125 B/triangle** ($8\times$ Nanite's footprint).
* Without even building a streaming pager, simply quantizing vertices to **16 bytes** (16-bit unorm/snorm cluster-local position = 6 B, octahedral snorm16 normal = 4 B, fp16 UV = 4 B, octahedral tangent/sign = 2 B, or deriving tangents in the fragment shader) cuts 30M tree triangles from **~3.5 GB down to ~600–800 MB** — **speculative**. Dropping the resident hardware-RT TLAS/BLAS (~1.27 GB in `decisions.md`) once `farm-cpuvox` replaces `farfield_bake.comp` frees another **~1.27 GB** immediately.

---

### A7. Chlorophyll Engine Specifics

**Answer & Evidence**
* Everything verified from Toni Macaroni's own X and YouTube replies is already accurately collected in Meridian's `2026-11-21-chlorophyll-x-replies.md` and `2026-11-21-chlorophyll-youtube-replies.md` (and no new public technical blog post or GDC talk was found as of Nov 2026):
  * Trees and bushes are **100% opaque triangles or voxels** (no leaf cards, no impostors, no MSAA A2C); only grass and small ground cover are alpha-tested — **established**.
  * Uses a **modified `meshoptimizer` + custom DAG builder** that emits voxels during the CLOD build step, swapping triangles -> voxels by screen error, and dropping instance data + DAG traversal past ~1 km into per-tile voxel proxies — **established**.
  * **Distant grass uses the same voxel path as trees**, with the terrain faking grass normals/SSS/BRDF beyond it — **established**.
  * Uses a **visibility-buffer deferred path** with almost all culling and shading in compute specifically to beat alpha-tested grass/foliage overdraw — **established**.
  * For AA on alpha grass, he states DLSS/FSR guidelines only go so far, so he tweaked alpha-grass mip bias and added a **motion-based "smear mask"** to selectively sharpen moving pixels (reactive masks did not help him) — **established**.

---

# Brief B: Leaf and Canopy Appearance, and Light Under the Canopy Without Ray Tracing

### B1. Measured Leaf Optics (European Beech, Hornbeam, Alder)

**Answer & Evidence**
* **Visible reflectance & transmittance (PROSPECT / LOPEX / ANGERS datasets):** In fresh temperate broadleaf leaves (400–700 nm), chlorophyll $a+b$ absorbs strongly in blue (400–500 nm) and red (600–700 nm), with a transmission/reflectance peak around **550 nm (green)**, while NIR (800–1000 nm) hits a 40–50% reflectance and 40–50% transmittance plateau — **established** (Féret et al., *PROSPECT-4 and 5*, 2008, https://www.ipgp.fr/~jacquemoud/publications/feret2008.pdf; Non-destructive leaf transmittance study, 2017, https://www.researchgate.net/publication/284887215_Non-destructive_measurement_of_chlorophyll_in_tomato_leaves_using_spectral_transmittance).
* **Cuticle specular roughness & Fresnel (*Fagus sylvatica*, European beech):** Direct spectro-photo-goniometer measurements of European beech leaves (*Fagus sylvatica* L.) fit to a Cook-Torrance microfacet + diffuse model yield a wavelength-independent specular roughness of **$\sigma \approx 0.35$** (roughly perceptual GGX roughness **0.45–0.60**) and a visible cuticle refractive index of **$n \approx 1.5$** (corresponding to $F_0 = ((1.5-1)/(1.5+1))^2 = \mathbf{0.04}$), dropping to $n \approx 1.3$ in NIR — **established** (Bousquet, Lachérade, Jacquemoud, Moya, *Leaf BRDF and BTDF Measurements and Model*, ISPMSRS / RSE 2005, https://www.ipgp.fr/~jacquemoud/publications/bousquet2005b.pdf).
* **Transmittance angular dependence & adaxial vs. abaxial:** Leaf directional-hemispherical transmittance decreases with incidence angle (Fresnel reflection at grazing angles reduces light entering the mesophyll) — **established** (Bousquet et al., 2005, https://www.ipgp.fr/~jacquemoud/publications/bousquet2005b.pdf). The lower (abaxial) spongy mesophyll scatters more diffusely and reflects slightly higher (paler, less glossy) than the upper (adaxial) palisade surface, while typical temperate broadleaf visible reflectance is ~0.04–0.06 (R/B) and ~0.10–0.16 (G), with transmittance roughly **$0.5\times$ to $1.0\times$ of diffuse reflectance** in green and much lower in blue (giving transmitted light a distinctly warmer **yellow-green** tint than reflected light) — **reported** (PROSPECT / Jacquemoud & Ustin literature; exact per-species RGB triplets for hornbeam/alder were not directly tabulated in fetched PDFs).

**Implications for Meridian (Important Correction)**
* **Your $F_0 = 0.04$ hypothesis is physically *right* for beech ($n \approx 1.5$), not wrong.** If Meridian's Megascans Trees look wet/silvery/plastic (G15), the culprit is much more likely:
  1. **Too-low roughness** (Unreal's `MA_Foliage` masks or `Roughness` values down at 0.2–0.4 without geometric specular AA or specular occlusion), and/or
  2. **Unoccluded full-sky specular reflection** (`lighting.glsl` reflects the full unoccluded sky-view LUT when GI is off, with only vertex `crownVisibility` through Lagarde's empirical fit).
* **Transmitted light MUST be warmer/yellower than reflected albedo** (chlorophyll absorbs blue strongly through the leaf thickness). Meridian currently uses untinted `sh.diffuseColor` for transmission.

---

### B2. Thin-Leaf Shading Models in Real Time (and Why Meridian's Back Faces Are Black)

**Answer & Evidence**
* **Unreal Engine `Two Sided Foliage`:** Automatically flips the shading normal for back faces, adds a wrap-diffuse / back-scattered transmission term driven by `SubsurfaceColor` (typically a brighter, yellower-green tint than `BaseColor`), and combines a diffuse back-face transmission term `saturate(-N.L)` with a view-dependent forward-scattering lobe toward `-L` — **established** (Epic Games, *Material Properties*, https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-material-properties; Bugnet UE guide, Jun 2026, https://bugnet.io/blog/how-to-fix-unreal-two-sided-foliage-material-lit-flat-on-backfaces).
* **Frostbite Translucency (Barré-Brisebois & Bouchard 2011, classic still standing):** Adds a view-dependent forward-scatter lobe $T_{\text{fwd}} = \text{saturate}(\mathbf{V} \cdot -\text{normalize}(\mathbf{L} + \mathbf{N}\delta))^p \cdot s \cdot t$ plus an ambient translucency term, at ~15–25 ALU instructions and zero extra render passes — **established** (confirmed in Meridian Research C).

**Direct Codebase Catch — Why Meridian Has Black Back Faces (G13)**
Look at `engine/shaders/mesh.frag` lines ~960–1000:
1. **Audit hypothesis G13 ("no normal flip") is actually incorrect:** `mesh.frag` *does* flip `normal = -normal;` when `!gl_FrontFacing`.
2. **Real Bug #1 — The Sign-Disagreement Dead Zone:**
   In `mesh.frag`, front-lit sun requires `sh.NoL > 0.0 && dot(geometricNormal, sh.L) > 0.0`, while back-lit transmission requires `dot(sh.N, sh.L) < 0.0 && dot(geometricNormal, sh.L) < 0.0`. Because `sh.N` is normal-mapped and `geometricNormal` is flat, whenever the normal map tilts across the terminator (`dot(sh.N, sh.L)` and `dot(geometricNormal, sh.L)` have opposite signs), **both `if` blocks are skipped—producing zero direct light AND zero transmission**.
3. **Real Bug #2 — Crushed Transmission Magnitude & Missing Forward Lobe:**
   `cook_ready.py` sets `diffuseTransmissionFactor = 0.35 * translucency`, which equals **0.147** for alder and **0.217** for beech. In `mesh.frag`, that multiplies `sh.diffuseColor / kPi` with **no view-dependent forward lobe**, **no warm yellow-green tint**, and **no wrap lighting**, while `s.occlusion *= crownOcclusion()` crushes interior ambient to 5–20%. Result: a sunlit leaf seen from behind receives ~10% of the energy of a front-lit leaf and reads black next to it.
4. **Fix:** Remove the `geometricNormal` sign-gating from the transmission branch (use wrap `(N.L + w)/(1+w)` for reflection, `saturate(-N.L)` + Frostbite forward lobe `pow(saturate(dot(V, -normalize(L + N * 0.35))), 5.0)` tinted by `vec3(1.2, 1.05, 0.55)` for transmission, and clamp energy so reflectance + transmittance $\le 0.9$) — **speculative**.

---

### B3. Shading a Canopy at Distance (Matching Voxels to Leaves)

**Answer & Evidence**
* **Why linear averaging fails (Bruneton & Neyret 2012):** Averaging leaf albedo and normal linearly into a voxel and evaluating the leaf BRDF on the average produces wrong brightness and specular response because self-shadowing, view-light masking correlation (the *hot-spot / opposition effect* where a canopy looks brighter looking away from the sun because shadows hide behind leaves), and GGX specular are non-linear — **established** (Bruneton & Neyret, *Real-time Realistic Rendering and Lighting of Forests*, CGF 2012, classic still standing, https://radar.inria.fr/report/2012/maverick/uid83.html).
* **Microflake / SGGX Voxel Shading (Heitz et al. 2015; Loubet & Neyret 2017):** Representing each voxel's sub-resolution leaf orientation as an SGGX microflake distribution (symmetric $3 \times 3$ or mean normal + normal spread / Toksvig length) allows analytic, prefilterable directional projected area and rough diffuse+specular phase functions without stochastic noise — **established** (Heitz et al., SIGGRAPH 2015, https://research.nvidia.com/labs/rtr/publication/heitz2015sggx; Loubet & Neyret, CGF 2017).
* **Chlorophyll's crown optical depth (F21):** Uses the per-asset voxel opacity volume to compute sun optical depth through each crown and add inter-leaf scattered light, bringing distant forests close to a path-traced reference — **established** (Toni Macaroni YouTube reply).

**Implications for Meridian**
* Look at `voxel_instance.frag`: Meridian *already* bakes 6-axis optical depths (`protoVoxelDepth`) and sky occlusion per voxel, which is huge. What's missing is why G1 looks like flat grey speckle:
  1. **No crown-scale normal or normal spread:** `voxelNormal(voxel.x)` averages leaf normals (which point every which way inside a 0.2–4 m voxel, collapsing to random noise or `(0,1,0)`). Storing a **crown-ellipsoid / opacity-gradient normal** blended with the baked normal, plus **normal length** to drive roughness/wrap, restores rounded 3D crown form immediately — **speculative**.
  2. **No leaf transmission in `voxel_instance.frag` or `voxel.frag`:** Both voxel shaders call `addSunLight` only when `sh.NoL > 0.0` and have **zero** backlit transmission or multiple-scatter approximation (unlike `mesh.frag`), creating a jarring brightness/color drop across the 2 px cluster->voxel switch — **established** (code inspection of `voxel_instance.frag` and `voxel.frag`).
  3. **Binary hash coverage:** Replacing binary `noise < opacity` in `voxel_bricks.comp` with larger voxels (3–4 px) + alpha/coverage compositing (or screen-space blue-noise dithered into a visibility/accumulation pass) removes the salt-and-pepper dot noise.

---

### B4. Light Under the Canopy (Physical Calibration Targets)

**Answer & Evidence**
* **Leaf Area Index (LAI):** Mature summer European beech (*Fagus sylvatica*) stands reach maximum LAI of **4.5–5.1** (sycamore maple ~4.0) — **established** (Urban et al., ResearchGate PDF, https://www.researchgate.net/publication/327947605_Leaf_area_index_estimated_by_direct_semi-direct_and_indirect_methods_in_European_beech_and_sycamore_maple_stands).
* **Understory PAR transmittance & sky visibility:** Under a fully foliated summer European beech canopy, total PAR transmittance drops from **33%** (bare branches in spring) to **1.5% of open-sky ambient illumination** by late May — **established** (iForest 2014, https://iforest.sisef.org/contents/?id=ifor1307-007). Across a forest edge transect, transmittance spans **45–55%** at the edge, **~12%** 32 m inside a mixed/thinned understory, and **$< 5\%$** in closed interior stands — **established** (Czech J. For. Sci. 2005, https://jfs.agriculturejournals.cz/pdfs/jfs/2005/01/01.pdf).
* **Illuminance in lux:** Against a ~100,000 lux open midday sun+sky, closed beech understory diffuse shade sits around **1,500–2,000 lux** (~1.5–2% of open), while sunflecks (dappled direct sun through canopy gaps) spike 20–$50\times$ higher — **established** (Beech forest microclimate study, https://www.researchgate.net/publication/226511524_Spatial_variability_of_summer_microclimates_and_plant_species_response_along_transects_within_clearcuts_in_a_beech_forest).

**Implications for Meridian**
* A closed beech forest floor should receive **~2–5% of open-sky irradiance** (not 15–30%), with dramatic contrast against direct sunflecks. Meridian's current `exposure.comp` meters the dark forest floor up until the sky blows out (partially mitigated by the `kHighlightMax = 2.0` p95 cap), and without sky occlusion on the terrain (`--gi off`), the forest floor still receives **100% of open-sky irradiance** whenever it's in sun shadow—which is why forest interiors read washed out and flat (G2/G7).

---

### B5. Non-RT Global Illumination for Open-World Vegetation (2024–2026)

**Answer & Evidence**

| Approach | Mechanism | Published Cost / HW | Motion & Foliage Behavior | Source & Grade |
|---|---|---|---|---|
| **AMD FidelityFX Brixelizer GI** (2024–2025) | Sparse SDF brick cascades in compute (Vulkan & DX12) + two-level radiance cache + screen-space trace; multi-bounce from previous frame | No fixed 4080 1440p ms published on GPUOpen overview page | SDF bricks struggle with thin, porous leaf geometry (blobs or high brick churn); screen-space denoiser can boil under DLSS 4.5 Preset M | **established** (AMD GPUOpen, Jul 2024–May 2025, https://gpuopen.com/fidelityfx-brixelizer/) |
| **3D Radiance Cascades** (Sannikov 2024; RCPG 2026) | Angular-spatial cascade probes storing spherical radiance images with power-of-two ray intervals, merged top-down | No shipped 3D open-world 1440p ms yet; 3D spherical probe memory scales steeply | Completely noise-free and rock-solid in motion (zero temporal boiling), but 3D memory/bandwidth and light leaking across thin terrain/canopy remain open engineering hurdles | **reported** (Osborne & Sannikov 2024; RCPG, Jul 2026, https://researchportal.tuni.fi/files/177869673/rcpg_compressed-1.pdf) |
| **Godot HDDAGI** (2024–2025) | Hierarchical DDA voxel clipmap + software-traced irradiance probes, no RT | 0.93 ms on GTX 1650 (PR #86267, in repo) | World-space probes are temporally stable; voxel resolution limits sub-meter detail | **reported** (Godot PR #86267, cited in `decisions.md`) |
| **Screen-Space + Voxel/Probe Hybrid** (Chlorophyll / Meridian `--gi probes`) | World-space voxel/probe clipmap for multi-bounce & canopy occlusion + screen-space bitmask/AO for near contacts | Meridian measured `--gi probes`: **0.82 ms** meadow, **1.5–2.3 ms** forest on RTX 4080 @ 1440p (`decisions.md`) | World probes stay stable in motion; screen-space layer provides sub-probe crevices without full-frame GI boiling | **established** (Meridian measurements; Chlorophyll F5) |

* **What Chlorophyll most likely runs:** A camera-centered sparse voxel clipmap (reusing the exact voxel opacity bricks built for its far vegetation tier, F21) supplying low-frequency indirect bounce + canopy occlusion, paired with a screen-space visibility-bitmask / SSGI pass (F5) in its compute visibility-buffer shading pass — **speculative**.

**Implications for Meridian**
* **Do NOT switch to Brixelizer GI or 3D Radiance Cascades right now.** Meridian's `--gi probes` + ground radiance cache is *already* architecturally aligned with HDDAGI/Chlorophyll, runs at **0.8–2.3 ms** on your 4080, and is inherently motion-stable.
* The two real bottlenecks in Meridian's GI (`decisions.md` 2026-11-21) are solvable inside your existing system: (1) debug the 3D-texture probe volume upload (hardware trilinear instead of 8 manual `giLoadSh` buffer loads cuts the +1.0 ms scene-early overhead), and (2) replace `farFieldTransmittance` inside `gi_probes.comp` with a coarser/cheaper L1/L2-only brick march (cutting `gi probes` from 1.0 ms toward ~0.4 ms) — **speculative**.

---

### B6. Ambient Occlusion on Foliage (GTAO vs. Visibility Bitmask)

**Answer & Evidence**
* **GTAO / XeGTAO on thin geometry:** Horizon-based AO assumes every depth sample is an infinitely thick solid wall, causing severe **over-darkening and black halos** behind thin grass blades and leaf silhouettes — **established** (Therrien, Levesque, Gilet, *Screen Space Indirect Lighting with Visibility Bitmask*, arXiv 2301.11376, 2023, https://arxiv.org/abs/2301.11376; Jimenez et al., SIGGRAPH 2016). XeGTAO mitigates this via a falloff/thickness heuristic, costing ~0.56 ms at 1080p on an RTX 2060 (~0.3–0.4 ms est. at 1440p on a 4080) — **established** for 2060, **speculative** for 4080 (Intel XeGTAO README, https://github.com/GameTechDev/XeGTAO).
* **Visibility Bitmask AO (Therrien et al. 2023):** Replaces the two horizon angles per slice with a 32-bit sector bitmask where each depth sample only blocks the angular span of a finite thickness `T`, allowing light to pass behind thin leaves and grass blades without dark halos — **established** (Therrien et al., 2023, https://arxiv.org/abs/2301.11376). *Failure mode:* thickness `T` is a global heuristic; too small leaks light through real walls/rocks, too large reverts to GTAO halos.
* **Avoiding double darkening with GI/probes:** Standard practice splits occlusion strictly by radius: screen-space AO covers **0.1–1.5 m** (radius falloff to 0 by 1.5–2 m), while world probes/voxels cover **$> 2$ m**; baked/procedural per-plant AO is either attenuated or combined via `min(screenAO, plantAO)` rather than multiplied blindly — **established** (Lumen / Frostbite / XeGTAO practice).

**Implications for Meridian**
* **Plain contradiction check on `2026-11-21-gtao-design.md`:** Meridian's GTAO spec proposes writing **always-on attachments 3, 5, and a new attachment 7 (ambient radiance)** in forward shading just so a post-pass can subtract `(1 - AO) * ambient`. With 8 color attachments + a masked depth prepass, you are paying deferred-renderer memory bandwidth without getting a visibility buffer's single-shade savings. If you build GTAO/bitmask AO now, **use the visibility-bitmask kernel directly** (since 80% of your scene is thin grass/leaves), and strongly consider whether moving to a visibility buffer first (or testing `aoFoliageWeight` ~0.25 on grass/leaves) avoids throwing away 8-attachment forward plumbing later — **speculative**.

---

### B7. Why Forests Look Grey in Renders (G7 Diagnosis)

**Answer & Evidence**
Four physical and perceptual mechanisms stack together to turn rendered forests grey:
1. **Missing multi-bounce green interreflection:** A single leaf absorbs blue and red (~90–95%) and reflects/transmits green (~10–15%). Each bounce inside a canopy squares the spectral reflectance ($G^2 / B^2 \gg G / B$), dramatically **increasing green chroma** in real forest shadows — **established** (Féret et al., 2008; Jacquemoud & Ustin, 2019). Replacing that multi-bounce green light with neutral/blue open-sky irradiance (as Meridian does with `--gi off`) desaturates shadows into cool grey — **speculative**.
2. **Exposure metering on dark canopies:** Real sunlit forests have low mean reflectance (3–6% linear albedo). Auto-exposure targeting 18% middle grey (`kMeanToGreyEv = 0.8` in Meridian's `exposure.comp`) lifts dark foliage by +1.5 to +2.0 EV into flat mid-grey and washes out the sky — **established** (measured in Meridian `decisions.md` 2026-11-21: forest overview metered 1.4 EV brighter than meadow).
3. **Aerial perspective single-scattering over low-albedo surfaces:** Even modest aerosol optical depth adds bright, desaturated blue-white in-scatter on top of dark forest shadows (0.01–0.03 linear), crushing shadow contrast and raising shadow $L^*$ from 0.13–0.23 up to 0.30+ — **established** (measured in Meridian `decisions.md` "Haze as a boundary layer").
4. **AgX inset desaturation + high specular reflectance:** AgX intentionally desaturates high-luminance colors via its inset matrix; combined with bright sky specular reflections off glossy/unoccluded leaf normals ($F_0 = 0.04$, low roughness), sunlit canopies pick up a pale silvery-grey veil — **established** (Meridian `decisions.md` "Starting grade" and "Megascans Trees").

**Implications for Meridian**
* Before touching tonemappers, run Phase 0 Task 4's A/B on G7 specifically checking: (1) **canopy ambient darkening** (dropping forest floor/interior sky irradiance toward the physical 2–10%), (2) **green-tinted bounce / GI**, (3) **specular occlusion + roughness widening** on leaves, and (4) **capping auto-exposure EV lift** so forests aren't lifted to the same display brightness as open sunlit meadows.

---

# Brief C: Temporal Reconstruction in Practice (DLSS 4 / DLAA) and Motion Clarity

### C1. DLSS 4 / 4.5 Inputs, Failure Signatures, and Audit of Meridian's Setup

**Answer & Evidence**
* **Jitter sequence & phase count:** NVIDIA recommends Halton(2,3) over $\lceil 8 \cdot (W_{\text{out}}/W_{\text{in}})^2 \rceil$ phases (8 phases for DLAA 1:1, $\ge 32$ for RR), passed as the negative of the projection sample offset — **established** (NVIDIA DLSS Programming Guide; verified in `engine/include/meridian/upscaler.h` and `engine/dlss/src/dlss.cpp`). *Failure signature of wrong sign/scale:* blurred stills, crawling fine edges, double-vision jitter.
* **Texture mip bias:** $\log_2(W_{\text{in}}/W_{\text{out}}) - 1$ when upscaling, and **`0.0` for DLAA** — **established** (DLSS Programming Guide). *Failure signature of negative bias at DLAA:* severe shimmering on fine textures and alpha cutouts (which Meridian confirmed when testing `-1`).
* **Motion vectors (`MVLowRes`, `InMVScale`):** Must be unjittered, pixel-space (after `InMVScale`), pointing current -> previous (or scaled appropriately; Meridian uses `previous - current` in UV $\times$ `renderSize`) — **established**. *Failure signature:* wrong sign causes violent trailing/smear; jittered MVs cause full-screen swimming.
* **Depth (`DepthInverted`):** Reverse-Z infinite far (sky = 0) with `DepthInverted` flag set — **established** (`dlss.cpp` sets this correctly).
* **Exposure (`InPreExposure` vs. `AutoExposure`):** When `AutoExposure` is set (required/always-on for Presets L and M), DLSS ignores external exposure textures — **established** (`decisions.md` citing DLSS 310.9.16 guide §3.9). *Failure signature:* if HDR input is pre-exposed by a *rapidly adapting* GPU exposure without passing `InPreExposure`, history color lags slightly during fast brightness transitions (documented in `backlog.md`).
* **Transparency / Bias-Current-Color Mask (`pInTransparencyMask`):** Tells DLSS to **reduce temporal history weight and trust the current frame** where motion vectors don't match the visible surface (e.g., transparent overlays, animated textures without MVs) — **established** (NVIDIA DLSS Programming Guide). *Failure signature of over-marking:* **shimmer, jitter, and pixelation** because DLSS throws away temporal stability on masked pixels.

**Critical Audit of Meridian's DLAA Setup (Two Direct Contradictions Found in Code)**
1. **BUG / CONTRADICTION #1 (`clouds_composite.comp` + `dlss.cpp`):**
   `clouds_composite.comp` writes `transparencyMask = max(mask, 1.0 - cloud.a)` on **every cloud pixel**, including solid cloud pixels (`opacity > 0.5`) where it *also* writes accurate cloud motion vectors. Then `dlss.cpp` passes `transparencyMask` directly to `eval.pInTransparencyMask`. You are simultaneously handing DLSS cloud motion vectors AND telling DLSS `mask = 1.0` ("ignore history, trust only this single jittered frame"). That directly causes cloud small-detail jitter/shimmer under DLAA. **Fix:** Only write `transparencyMask` on *semi-transparent* cloud edges (e.g., `1.0 - abs(2.0 * opacity - 1.0)` or `0` where `opacity > 0.5`), or composite clouds *after* DLAA (see C4).
2. **CONTRADICTION #2 (Forcing Preset M at DLAA):**
   In `engine/dlss/src/dlss.cpp`, Meridian overrides NGX's default Preset K to **Preset M** for DLAA, Quality, and Balanced. See C2 below—this contradicts NVIDIA's own 2026 recommendation.

---

### C2. DLSS 4 / 4.5 Transformer Presets (J, K, L, M) on Foliage

**Answer & Evidence**
* **NVIDIA's official preset mapping (Jan 2026):** NVIDIA's DLSS 4.5 "Recommended" mapping assigns **Preset K** (1st-gen transformer) to **DLAA, Quality, and Balanced**, **Preset M** (2nd-gen transformer) to **Performance**, and **Preset L** (2nd-gen transformer) to **Ultra Performance** — **established** (NVIDIA GeForce News, Jan 6 2026, https://www.nvidia.com/en-eu/geforce/news/dlss-4-5-dynamic-multi-frame-gen-6x-2nd-gen-transformer-super-res/).
* **Cost & behavior differences:**
  * Preset M is ~18% heavier than K and Preset L ~27% heavier than K on RTX 3070 (on Meridian's RTX 4080 at 1440p DLAA, you measured **K = 0.75 ms** vs. **M = 2.36 ms**—a massive **$3.1\times$ cost jump**) — **established** (Digital Foundry, Feb 4 2026, https://www.digitalfoundry.net/features/dlss-4-5-preset-l-tested-how-good-can-a-4k-upscale-from-720p-look; Meridian `decisions.md`).
  * **Where M beats K:** Sharper motion preservation, less ghosting on moving characters/swords, less disocclusion pixelation (*Horizon Forbidden West*, *Assassin's Creed Shadows*), and cleaner thin grass in *Black Myth: Wukong* — **reported** (Digital Foundry, Jan 23 2026, https://www.digitalfoundry.net/features/dlss-4-5-review-where-its-better-and-where-it-needs-work; PCOptimizedSettings, Jan 22 2026, https://pcoptimizedsettings.com/nvidia-dlss-4-preset-k-vs-dlss-4-5-preset-l-vs-m-dlss-4-looks-better-than-4-5-in-this-game/).
  * **Where K beats M/L (CRITICAL FOR MERIDIAN):** Preset M and L exhibit **noticeable shimmering/boiling on fine ground detail, foliage, and any upstream temporally-denoised/stochastic effects** (*Oblivion Remastered* foliage, *The Outer Worlds 2*, *MSFS 2024* ground panning)—so much so that Digital Foundry found disabling an upstream denoiser fixed Preset M/L boiling, and *MSFS* users rolled back to Preset K to stop panning shimmer — **reported** (Digital Foundry, Feb 4 2026; MSFS Forums, Feb 2026, https://forums.flightsimulator.com/t/dlss-4-5-graphics-shimmering/756473).

**Implications for Meridian**
* Look back at why Meridian rejected Preset K in `decisions.md`: Preset K had **shimmer 0.27/0.30** ($3\times$ MORE stable than Preset M's 0.80/0.40) and cost **0.75 ms** (saving **1.61 ms** toward your 180 fps bar), and was ONLY rejected because it scored 0.44 on the old forward-Laplacian *ratio* gate—which you later realized was biased toward oversharpening, and the user explicitly stated they **prefer stability over sharpness**.
* **Immediate test:** Re-run `tools/motion-clarity.sh` comparing **DLAA Preset K vs. Preset M vs. Preset L** on your current raster-shadow + dense-meadow build. You may get dramatically better stability *and* +1.6 ms back for free — **speculative**.

---

### C3. Animated Content & Motion Vectors

**Answer & Evidence**
* **Best practice:** Every vertex-animated mesh (skeletal/pivot wind on foliage) must evaluate both current and previous-frame transforms to write exact analytical motion vectors — **established** (Epic Games, *Nanite Skinning*; Horizon Forbidden West, McLaren GDC 2022).
* **When MVs cannot describe the change (particles, scrolling textures, reactive FX):** Either (a) composite clean particles/UI *after* the upscaler (if depth-occluded cleanly or unaliased), or (b) tag only the affected pixels with a reactive / bias-current-color mask so DLSS doesn't smear them along background geometry MVs — **established** (AMD FSR 3.1 manual; NVIDIA DLSS Programming Guide). *Failure mode:* masking large regions of animated foliage or ground destroys temporal AA and causes shimmer; Chlorophyll's developer confirmed reactive masks did not help on foliage (F16).

---

### C4. Volumetric Clouds and the Upscaler (Before vs. After DLSS)

**Answer & Evidence**
* **The fundamental problem with stacking two temporal resolvers:** Meridian runs a quarter-res cloud march with a heavy temporal history (`clouds_resolve.comp`, current weight **0.03** still / **0.10** moving, `kVarianceGamma = 2.0`), sharpens it with an unsharp mask (`cloudOutline = 1.0`), composites it *before* DLAA, AND sets `transparencyMask = 1.0` on the cloud pixels.
  * When the camera moves, `clouds_resolve.comp` has to reproject a 33-frame history at quarter-res through Catmull-Rom, blurring small cloud detail; then `cloudOutline` sharpens the quarter-res texels (causing G16 blocky distant clouds and G19 moving jitter); then DLSS receives an already-temporally-accumulated, sharpened signal with `transparencyMask = 1.0` — **established** (code inspection of `clouds_resolve.comp` & `clouds_composite.comp`).
* **What shipping titles do:**
  * *Horizon Forbidden West / Nubis$^3$* (Schneider, SIGGRAPH 2023) and *Frostbite* (Hillaire 2016) composite volumetric clouds **before** TAA/upscaling only when cloud motion vectors are accurate AND the cloud pass does **not** fight the upscaler with an over-sharpened long-history mask, OR they render clouds at higher resolution / adaptive checkerboard with tight neighborhood clamping — **established** (Schneider, SIGGRAPH 2023 Advances; Hillaire 2016).
  * *Unreal Engine* supports compositing volumetric clouds either before TSR/DLSS (for correct aerial/translucent sorting, writing cloud velocity) or after TAA for sky-only clouds if temporal ghosting/boiling appears; UE 5.6+ specifically flagged temporal reconstruction artifacts in volumetric cloud render targets when stacked with upscalers — **established** (UE forums & docs cited in Meridian's M4 spec).

**Implications for Meridian**
* **Plain contradiction check:** Stacking (1) quarter-res 33-frame temporal resolve (`w = 0.03`) + (2) unsharp mask `cloudOutline = 1.0` on quarter-res texels + (3) `transparencyMask = 1.0` into DLSS Preset M is a textbook recipe for G16 + G19.
* **Three clean A/Bs to run in `image-stability`:**
  1. Fix `transparencyMask` in `clouds_composite.comp` so solid clouds (`opacity > 0.5`) write `0.0` (letting DLAA trust the cloud motion vectors), marking only soft cloud edges (`1.0 - abs(2.0 * opacity - 1.0)`).
  2. Test `--effects cloud-outline=0` (and/or attenuate `cloudOutline` to 0 in motion, and disable it on distant clouds via depth).
  3. Test compositing volumetric clouds **after** DLAA (or raising the march to half-res with a shorter history `w = 0.15–0.25` when moving), measuring whether removing double temporal accumulation eliminates small-detail cloud smear.

---

### C5. Measuring Motion Clarity (Auditing Meridian's Metrics)

**Answer & Evidence**
* **Current industry/research state of the art (2024–2026):**
  * **ColorVideoVDP** (Mantiuk et al., SIGGRAPH 2024, PyTorch/CUDA, MIT/BSD, https://github.com/gfxdisp/colorvideovdp): Full-reference spatiotemporal video quality metric modeling chromatic spatial + temporal contrast sensitivity (4 temporal channels) for both SDR and HDR, outputting JOD (Just-Objectionable-Difference) scores and per-pixel temporal distortion heatmaps — **established** (Mantiuk et al., SIGGRAPH 2024, https://www.cl.cam.ac.uk/~rkm38/pdfs/mantiuk2024_ColorVideoVDP.pdf).
  * **CGVQM+D** (Intel Labs, June 2025, https://github.com/IntelLabs/CGVQM): 3D-CNN full-reference video quality metric trained specifically on **neural upscaling (ghosting, flicker), path tracing noise, and denoising** against $16\times$ SSAA reference videos — **established** (Intel Labs, arXiv 2506.11546, June 2025, https://arxiv.org/html/2506.11546v1).
  * **FLIP / HDR-FLIP** (Andersson et al., NVIDIA, HPG 2020/2021): Standard per-frame perceptual error metric used by NVIDIA and engine teams against SSAA/offline references — **established** (classic still standing).
  * **Digital Foundry:** Uses slowed-down frame-stepping of slow pans, fast camera whips, and strafes past foliage/wires/disocclusions to inspect boiling, disocclusion pixelation, and trailing — **established** (Digital Foundry, Jan–Feb 2026).
* **Audit of Meridian's `tools/motion-clarity.sh` metric:**
  * *What's clever about Meridian's metric:* Reaching the **exact same pose on frame 120 and frame 121** while moving (`scriptedPosition` ending at `--camera`) cancels camera parallax so `|frame120 - frame121|` isolates pure temporal-phase instability (jitter/shimmer) during motion.
  * *Its blind spot:* Because `frame120` and `frame121` land on **different Halton jitter phases** (phase `120 % 8 + 1 = 1` vs `121 % 8 + 1 = 2`), a single pair can over- or under-report shimmer depending on those two specific jitter offsets, and it cannot distinguish **temporal boiling/trailing** over a 10–30 frame sequence from single-frame jitter — **speculative**.
  * *Upgrade:* Keep `motion-clarity.sh` as your fast ~30 s regression gate (average over 4 consecutive ending frames `120..123` to remove phase bias), and add an offline **$8\times$/$16\times$ SSAA reference clip + ColorVideoVDP (or CGVQM)** check for major AA/foliage/cloud milestones — **speculative**.

---

### C6. Beyond the Upscaler: Engine-Side Causes of Motion Smear & Jitter

**Answer & Evidence**
No upscaler (even DLAA) can stabilize an input whose pixel value changes stochastically or discontinuously from frame to frame — **established** (NVIDIA DLSS Programming Guide; Digital Foundry Feb 2026). Auditing Meridian's actual shaders reveals **five concrete upstream instability sources** currently feeding DLAA:

1. **Binary per-frame jittered contact shadows (`contact_shadows.comp`):** Runs a 16-step binary hit/miss march with per-frame `interleavedGradientNoise(pixel, frameIndex)` and subtracts the full sun radiance on hit. Even while still, Meridian measured this as ~25% of grass shimmer; in motion, DLSS 4.5 Preset M actively **boils** on un-denoised binary stochastic signals — **established** (code inspection + Digital Foundry Feb 2026).
2. **Binary hashed prototype voxels (`voxel_bricks.comp`):** Draws 2 px quads with binary `noise < opacity` acceptance, causing sub-pixel stipple/crawl during camera rotation — **established** (code inspection).
3. **Dithered IGN cluster-to-voxel switch band (`mesh.frag` + `voxel_band.frag`):** Uses `interleavedGradientNoise(gl_FragCoord.xy, scene.frameIndex)` to flip pixels every frame between clusters and voxels across a 10% distance band.
4. **Unfiltered specular highlights on leaves (`mesh.frag`):** No geometric specular AA (`fwidth(N)`) or Toksvig roughness widening, creating 1-pixel fireflies that move across pixels every frame and get bloomed (G18) — **established** (code inspection).
5. **Stacked cloud temporal history + `cloudOutline` + `transparencyMask = 1.0` (`clouds_resolve.comp`, `clouds_composite.comp`):** Feeds double-accumulated, sharpened quarter-res clouds into DLAA with history disabled — **established** (code inspection).

**How shipping teams audit for these:** Disable passes one by one in a moving sequence (`--effects contact=0`, `--effects cloud-outline=0`, `--clouds off`, `--voxel-trees off`, specular forced to 0) and rank each pass by its delta on temporal jitter (`motion-clarity.sh` or ColorVideoVDP).

---

# Ranked Action Plan (Across Briefs A, B, and C)

| Rank | Change | Expected Visible Gain | Cost (1440p RTX 4080) | Risk | Confirming Measurement |
|---|---|---|---|---|---|
| **1** | **Fix `clouds_composite.comp` `transparencyMask` + A/B `cloudOutline=0` & Preset K vs. M (DLAA)** | Kills cloud jitter/blockiness (G16/G19); Preset K may dramatically cut motion boiling/shimmer on foliage & ground | **-1.6 ms** if Preset K wins (`0.75` vs `2.36 ms`); `0 ms` for mask fix | Preset K may trade a little forward sharpness for much better stability (matches user priority) | `tools/motion-clarity.sh` (jitter & error on forest, ground, and cloud-detail crop) comparing Preset K vs M, mask fixed vs current, `cloudOutline` 0 vs 1 |
| **2** | **Fix `mesh.frag` leaf backface dead zone + tinted two-sided thin-leaf shading + Geometric Specular AA (`fwidth(N)`)** | Fixes black back faces (G13), adds luminous yellow-green backlit glow (G3), kills leaf fireflies/sparkle (G15/G18) | **+0.05–0.15 ms** ALU in `mesh.frag`, 0 VRAM | Over-bright backlit crowns if energy ($R + T \le 0.9$) isn't clamped | Backlit sunset & meadow captures + `region-luminance.py` + `motion-clarity.sh` forest jitter |
| **3** | **Replace binary jittered `contact_shadows.comp` with deterministic Bend-style wavefront SSS** | Removes ~25% of meadow/foliage shimmer and eliminates DLSS 4.5 boiling on binary shadow noise (G5/G19) | **~0.2–0.4 ms** (comparable to current pass) | Thin-grass over-darkening if thickness tolerance isn't tuned | `motion-clarity.sh` meadow jitter (`contact` Bend vs current vs `contact=0`) |
| **4** | **Add leaf transmission, crown-ellipsoid normal + spread, and filtered/larger (3–4 px) voxels to `voxel_instance` / `voxel.frag`** | Eliminates salt-and-pepper grey forest speckle (G1), matches cluster->voxel boundary, unlocks earlier voxel switch | **-1.0 to -2.0 ms** net at forest views if voxel size rises 2->3–4 px | Edge/depth blending complexity against clusters in switch band | Aerial & forest-overview captures vs Chlorophyll refs (`resemblance.py`) + `motion-clarity.sh` turn jitter |
| **5** | **Blend grass normals toward `terrainNormal()` by 15–30 m (not only during `inStreamFade`)** | Major reduction in alpha-tested grass shimmer during 5 m/s strafe & 1 rad/s turn (G4/G19) | **~0.0 ms** (already computed in `mesh.frag`) | Slightly flatter grazing look on mid-distance grass | `motion-clarity.sh` `ground_side5` & `ground_turn1` jitter |
| **6** | **Re-evaluate Visibility Buffer before building 8-attachment forward GTAO (and use Visibility Bitmask AO, not raw GTAO)** | Avoids 8-attachment forward bandwidth tax, removes ~3 ms sub-pixel foliage overshading (G10), prevents thin-foliage AO halos (G2) | **-2 to -3 ms** at forest edge (vis-buffer); **+0.3–0.4 ms** for bitmask AO | Larger refactor (~1–2 weeks) vs. quick forward GTAO | `parity.route` avg FPS & 1% lows; forest-interior & meadow AO captures without sky-edge halos |
| **7** | **Add area-preserving leaf pruning per DAG level (or earlier voxel switch) + 16 B quantized cluster vertices** | Prevents traced opaque leaf trees (`leaf-geometry`) from thinning at coarse LOD (Gate 2) and cuts 30M tree VRAM from ~3.5 GB to ~0.7 GB | **-2.5+ GB VRAM**; ~0 ms raster (potentially faster from cache bandwidth) | Precision check needed on 16-bit local vertex quantization at seams | Lone-tree silhouette coverage at 10/30/60/120 m ($\le 5\%$ drift); `nvidia-smi` VRAM |
| **8** | **Add ColorVideoVDP (against $8\times$/$16\times$ SSAA reference clips) + 4-frame jitter-averaged `motion-clarity.sh`** | Catches multi-frame temporal boiling and trailing that single-pair metrics miss | Offline tooling only (0 runtime ms) | Extra capture time for milestone checks | Run on Preset K vs M and before/after cloud/contact-shadow fixes |