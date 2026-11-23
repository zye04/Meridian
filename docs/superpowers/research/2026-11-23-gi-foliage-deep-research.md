# Probe GI + Screen-Space GI Hybrid for Pure-Raster Daylight Forests (Neon deep research, 2026-11-23)

Prompt: `2026-11-23-gi-foliage-deep-research-prompt.md`. Triage:
`2026-11-23-gi-foliage-triage.md`. Pasted by the user; kept as received
except formatting (math kept in LaTeX). Labels are the report's own:
VERIFIED (read the primary source), SECONDARY, INFERRED; not re-checked here.

## 1. Foliage in probe / voxel GI without ray tracing

### 1.1 Leaf single-scattering albedo and directional extinction
- A leaf volume is a turbid medium of bi-Lambertian scatterers with single-scattering albedo $\omega(\lambda) = \rho(\lambda) + \tau(\lambda) = 1 - \alpha(\lambda)$ (absorptance $\alpha$). VERIFIED: [Sellers 1985](https://biocycle.atmos.colostate.edu/Documents/SiB/Sellers_1985.pdf); [Pinty et al., JGR 2006](https://misr.jpl.nasa.gov/media/documents/pinty2006.pdf).
- Direct beam transmittance over path $s$: $T_{dir}(s,\Omega) = \exp(-\Omega_c\,G(\Omega)\,\mathrm{LAD}\,s) = \exp(-K(\Omega)\,L(s))$, $L(s) = \int_0^s \mathrm{LAD}\,dl$, $K = \Omega_c G$ (clumping $\Omega_c \in (0,1]$). VERIFIED: Pinty 2006; [Béland stem-canopy RT preprint 2026](https://www.researchgate.net/publication/404656229_Consideration_of_radiation_absorption_by_stems_in_forests_for_microclimate_modeling).
- Spherical leaf angle distribution: $G \equiv 0.5$. VERIFIED (Pinty 2006; Sellers 1985). Ross linear approximation: $G(\mu) = \phi_1 + \phi_2\mu$, $\phi_1 = 0.5 - 0.633\chi_L - 0.33\chi_L^2$, $\phi_2 = 0.877(1 - 2\phi_1)$, $\chi_L \in [-0.4, 0.6]$ (0 spherical, > 0 planophile). VERIFIED (Sellers 1985 Eq. 12-13).
- Reflectance-only voxel albedo drops the transmitted flux; in temperate deciduous leaves $\tau \geq \rho$ in the visible green, so $\omega \approx 2\rho$-$3\rho$ and $\rho$ alone cuts single scatter by 50-67%. INFERRED. $\tau$ is not a scalar multiple of $\rho$: transmission is greener/warmer (blue absorbed more; reflection carries a neutral cuticle part). INFERRED from Pilarski 2008.

### 1.2 Multiple scattering inside a crown
- Two-stream canopy models (Dickinson 1983; Verhoef SAIL; Sellers 1985): coupled ODEs in $I^\uparrow$, $I^\downarrow$ driven by $\omega$ and $e^{-KL}$ (upscatter fractions $\beta$, $\beta_0$). VERIFIED: Sellers 1985 Eq. 1-2; [Yuan et al. 2016](https://www.researchgate.net/publication/311940757_Reexamination_and_further_development_of_two-stream_canopy_radiative_transfer_models_for_global_land_modeling).
- Real-time options:
  1. Recollision ($p$-theory, Smolander & Stenberg 2003): with recollision probability $p \approx 1 - \bar T_{hemi}$, total scattered per intercepted photon $\omega_{ms} = \omega / (1 - p\,\omega)$; split form: first-order sun scatter $\frac{\Phi_{bi}}{\pi} T_{sun} E_{sun}$ + higher orders $\frac{p\,\omega^2}{4\pi(1 - p\omega)}(\bar T_{sun} E_{sun} + T_{sky} E_{sky})$ + $\frac{\omega}{4\pi} E_{sky}$. VERIFIED for $p$-theory (Béland 2026; [Chen & Leblanc GO multiple scattering](https://www.researchgate.net/publication/3202609_Multiple-scattering_scheme_useful_for_geometric_optical_modeling)); INFERRED for the probe-hit collapse. Green shift then follows the true spectral $\omega$ and crown depth only.
  2. Multi-octave volumetric approximation (Wrenninge 2013 / Frostbite clouds): 2-3 octaves $a^n\omega^n\exp(-b^n\tau_{sun})$, $b \approx 0.5$, along the sun through a per-asset opacity volume. INFERRED; Chlorophyll's exact equation unknown.

### 1.3 Shipped engines
- UE5 Lumen software RT: surface cache (mesh cards) with multi-bounce feedback; foliage uses two-sided distance fields and two-sided materials. VERIFIED: [Wright et al., Lumen, SIGGRAPH 2022](https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Lumen-Wright%20et%20al.pdf); [UE5 mesh distance fields](https://dev.epicgames.com/documentation/unreal-engine/mesh-distance-fields-in-unreal-engine).
- Snowdrop (Avatar, GDC 2024): skips alpha test in GI traversal for foliage; opaque vegetation "gives expected amount of occlusion and saturation from trees". VERIFIED: [Koshlo GDC 2024](https://media.gdcvault.com/gdc2024/Slides/GDC+slide+presentations/Koshlo_Oleksandr_Snowdrop_Custom_BVH.pdf).
- Per-asset opacity volume (16^3-32^3 per prototype): 3-6 steps along the sun give intra-crown sun optical depth (replacing a `leafSun` heuristic); a pre-integrated hemisphere gives sky escape $T_{sky}$ and $p \approx 1 - T_{hemi}$. INFERRED.

## 2. Combining occlusion scales without double counting
- Rule 1, frequency partition. UE5 Lumen Short Range AO (screen bent normal, detail below screen-probe spacing) replaces SSAO/DFAO rather than multiplying. VERIFIED: [UE5 cvars](https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-console-variables-reference), [Lumen technical details](https://dev.epicgames.com/documentation/unreal-engine/lumen-technical-details-in-unreal-engine). DDGI + SSAO: AO radius at most ~0.5x the finest probe spacing. INFERRED from [Majercik et al., JCGT 2021](https://jcgt.org/published/0010/02/01/paper-lowres.pdf). Our crown visibility x probes double counted one 4-15 m band: disable the crown term under GI, or turn it into a sub-voxel contrast term with mean 1. INFERRED.
- Rule 2, bent normals + multi-bounce AO (Jimenez 2016): $AO_{mb} = ao\,((a\,ao + b)\,ao + c)$, $a = 2.0404\rho - 0.3324$, $b = -4.7951\rho + 0.6417$, $c = 2.7552\rho + 0.6903$; specular occlusion from bent-normal cones. VERIFIED: [Filament surface_ambient_occlusion.fs](https://github.com/google/filament/blob/main/shaders/src/surface_ambient_occlusion.fs); [XeGTAO](https://github.com/gameTechDev/XeGTAO). Frostbite: min(baked, screen) AO, Lagarde specular occlusion. SECONDARY: [Frostbite PBR notes](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/course-notes-moving-frostbite-to-pbr-v2.pdf).
- Rule 3, sector masking (Therrien et al. 2023): 4 angular sub-regions per slice, SH probe sampled at each centre, weighted by its unoccluded bit fraction. VERIFIED: [SSILVB slides p. 19-20](https://cdrinmatane.github.io/cgspotlight-slides/ssilvb_slides.pdf).
- Ghost of Tsushima: SH irradiance from baked sky visibility, SSAO only for contact. VERIFIED (abstract): [Patry 2021](https://www.glowybits.com/blog/2022/12/18/ghost_talks/). Horizon Forbidden West and Avatar: exact combination unknown.

## 3. Visibility bitmask combined with world-space probes
- Per slice a 32-bit mask; each sample's sector mask $M$ from front angle $\theta_f$ and thickness-offset back angle $\theta_b$; newly visible bits $M \,\&\, \sim V$ add on-screen radiance x bitCount/32 (cosine-weighted in GT-VBAO); $V \mathrel{|}= M$. VERIFIED: SSILVB slides p. 12, 24; [arXiv 2301.11376](https://www.alphaxiv.org/abs/2301.11376).
- Remaining clear bits sample the probes (4 sub-sectors, or the unoccluded bent normal x bitCount(~V)/32): each direction gets either on-screen bounce or probe light, never both. VERIFIED for sub-sector evaluation; INFERRED for exact conservation with SH L1. Screen radius at most 1.5-2 m (the finest probe spacing), faded at the boundary. INFERRED.
- Thickness: a constant world thickness trades halos behind thin leaves (small t, 0.05-0.15 m) against leaks behind trunks. VERIFIED (slides p. 12, 17). Upgrade: depth-scaled thickness or a foliage flag (0.05-0.10 m leaves/grass, 0.4-0.8 m terrain/trunks). INFERRED.
- GT-VBAO (Mirko Salm, 2024): cosine-weighted sectors, perspective-correct slices; slightly noisier in dark regions at low counts. VERIFIED: [norio/three-gtvbao](https://github.com/norio/three-gtvbao); SECONDARY: [UE forum VBAO](https://forums.unrealengine.com/t/ark-kra-vbao-visibility-bitmask-ambient-occlusion/2705204). Open code: SSRT3 (MIT), GT-VBAO (CC0/MIT), Intel XeGTAO (MIT).
- Costs: 1.13 ms bitmask AO vs 1.12 ms GTAO, 1920x1080, RTX 2080, radius 2, 16 samples, 32 sectors, no denoiser. VERIFIED (paper). UE plugin 1080p 0.45-1.60 ms (GPU unstated). SECONDARY. 1440p RTX 4080 half-res estimate 0.4-0.8 ms AO + probe masking, 0.8-1.5 ms with bounce. INFERRED.
- Under DLAA with swaying leaves: avoid long temporal accumulation (residual noise boils under DLSS 4.x, preset M); 2-3 slices x 4-6 steps at half res, single-frame 3x3/5x5 depth+normal-aware spatial filter, history at most ~0.5 with strict rejection. INFERRED (matches XeGTAO's spatial-first design). Half res + XeGTAO depth MIPs + bilateral upsample is standard. VERIFIED (slides p. 31).

## 4. Leaking and darkness in probe GI under dense canopies
- DDGI (Majercik 2021): 8x8 octahedral irradiance, 16x16 visibility moments; weights: trilinear, smooth backface $((\mathbf{d}\cdot\mathbf{N} + 1)/2)^2 + 0.2$, Chebyshev $\sigma^2/(\sigma^2 + (r - \mu)^2)$ at a normal/view-biased point; backface hits record 0 and shorten distance to 0.2r; relocation pushes probes out of solids. VERIFIED: [JCGT 2021](https://jcgt.org/published/0010/02/01/paper-lowres.pdf); [DDGI resampling 2021](https://tom94.net/data/publications/majercik21dynamic/majercik21dynamic.pdf).
- Forest failure modes (INFERRED): (1) backface rules applied to two-sided foliage zero probes inside crowns: apply them to solids only; (2) slice 0 at the terrain height can sit inside the heightfield on slopes: offset 0.5-1 m; (3) probes inside their own 4 m foliage voxel attenuate every ray from t = 0: start after 0.5-1 m or weight the first voxel by the remaining fraction; two-sided leaf ambient $E(+N)\rho + E(-N)\tau$; (4) SH L1 rings at high contrast: non-negative evaluation (Geomerics) or ZH3/octahedral.

## 5. Calibration data
- Use visible/PAR (400-700 nm) values, never broadband (NIR plateau inflates them). VERIFIED: [Pilarski, Tokarz, Kocurek 2008](http://www.pjoes.com/pdf-88167-22025?filename=22025.pdf).
- Endler 1993, forest light environments: large gaps white; small gaps yellow-orange; woodland shade (sky through high gaps, no sun) blue-grey; forest shade (closed canopy) yellow-green peaking ~550 nm. VERIFIED: [Endler 1993](https://www.researchgate.net/publication/201997380_The_Color_of_Light_in_Forests_and_Its_Implications).
- Chiao, Osorio, Vorobyev, Cronin, JOSA A 2000: 238 forest illuminants lie in the CIE 1931 triangle (0.250, 0.270) skylight/woodland shade, (0.325, 0.380) leaf-filtered forest shade, (0.375, 0.360) direct/low sun. VERIFIED: [Chiao et al. 2000](https://opg.optica.org/abstract.cfm?uri=josaa-17-10-1713). In linear Rec.709 normalized to G = 1: forest shade ~(0.83, 1.00, 0.62), woodland shade ~(0.64, 1.00, 1.65), D65 (1, 1, 1). INFERRED (matrix conversion).
- Closed beech/hornbeam canopies: forest-floor median 1-3% of open PAR (vs ~15% under birch). VERIFIED: [Hagemeier & Leuschner, Forests 2019](https://www.mdpi.com/1999-4907/10/10/925). Sunflecks 10-100x diffuse shade. VERIFIED: [Eliáš et al. 2017](https://www.researchgate.net/publication/313530322_Sunflecks_in_forest_communities_and_their_importance_for_plant_life_in_a_forest_understorey).

| Material (Fagus sylvatica unless stated) | Blue 400-500 | Green 500-600 | Red 600-700 | PAR mean | Status |
|---|---|---|---|---|---|
| Leaf reflectance rho | 0.060 | 0.189 | 0.123 | 0.124 | VERIFIED, Pilarski 2008 Fig. 1 |
| Leaf transmittance tau (same leaves, PAR absorptance 62.9%) | 0.103 | 0.373 | 0.268 | 0.247 | VERIFIED, Pilarski 2008 Fig. 1 |
| Beech and hornbeam crown leaves, 10 stands | - | - | - | absorptance 0.70-0.77, so omega 0.23-0.30 | VERIFIED, Hagemeier & Leuschner 2019 |
| Black alder leaf (LOPEX93) | ~0.04-0.06 | ~0.10-0.15 | ~0.04-0.07 | not excerpted | curve VERIFIED, values INFERRED |
| Mature beech bark | 0.131 | 0.149 | 0.167 | 0.149 | VERIFIED, Pilarski 2008 Fig. 2 |
| 10-year beech stem bark | 0.076 | 0.109 | 0.152 | 0.101 | VERIFIED, Pilarski 2008 Fig. 2 |
| Dry beech litter | ~0.05-0.08 | ~0.10-0.15 | ~0.18-0.28 | - | INFERRED (verify against EcoSIS/LOPEX) |

- Against our textures (INFERRED by the report): beech bark (0.129, 0.117, 0.087) within the stem-to-trunk PAR range, slightly browner; the beech leaf atlas (0.038, 0.046, 0.008, Y 0.041) is 2-3x darker than integrating-sphere leaf reflectance, plausibly cavity darkening baked into photogrammetry cards; GI wants omega ~0.20-0.30 in green.

## 6. Engine-side material validation
- Unity HDRP Rendering Debugger material validator (false colour outside albedo thresholds, colour picker, albedo/smoothness/normal overrides). VERIFIED: [HDRP debugger](https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@13.1/manual/Render-Pipeline-Debug-Window.html).
- Unreal buffer visualization (base colour, roughness, specular, metallic, AO, lighting only) VERIFIED: [UE5 viewport modes](https://dev.epicgames.com/documentation/unreal-engine/viewport-modes-in-unreal-engine); custom range validators (dielectrics 60-240 sRGB, 0.04-0.87 linear) SECONDARY: [Shinsoj 2023](https://medium.com/@shinsoj/the-diary-of-ta-ue5-custom-view-modes-4843660c7924); Material Analyzer audits instance hierarchies VERIFIED: [docs](https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-material-analyzer-tool).
- Calibration practice: ColorChecker under overcast RAW, lighting-only (neutral albedo) and albedo-only views, material-ID/foliage masks. VERIFIED: [Lagarde 2011](https://seblagarde.wordpress.com/2011/08/17/feeding-a-physical-based-lighting-mode/); [Filament](https://google.github.io/filament/Filament.md.html).
- Linear albedo ranges: dielectric floor 0.02-0.04; beech leaf rho 0.06-0.19 (PAR mean ~0.12); beech bark 0.10-0.17; worn asphalt / dark damp rock 0.12; bare soil 0.17; grass 0.25 per Lagarde 2011 (includes some specular / broadband; keep diffuse grass ~0.06-0.18); desert sand 0.40; new concrete 0.55; fresh snow 0.80-0.90.

## Ranked list (report's)
1. omega = rho + tau with recollision multi-scatter omega/(1 - p omega) in probe foliage hits, two-sided leaf ambient E(+N) rho + E(-N) tau (Q1, Q5; ~0.05 ms).
2. Crown visibility off under GI (or mean-1 sub-voxel contrast); skip first-half-voxel self-extinction; no backface killing on foliage (Q2, Q4; ~0 ms).
3. Per-asset opacity volume for intra-crown sun transmittance (Q1; ~0.2-0.4 ms).
4. Half-res GT-VBAO, radius <= 1.5 m, 4-sector probe masking, on-screen bounce, material-aware thickness, spatial-first filter (Q2, Q3; ~0.5-1.2 ms).
5. Jimenez multi-bounce AO + bent-normal specular occlusion (Q2; ~0.03-0.05 ms).
6. Probe robustness: slice-0 clearance 0.5-1 m, Chebyshev on solids only, non-negative SH (Q4; ~0.05-0.15 ms).
7. Material validator + albedo override + probe chromaticity view (Q5, Q6; debug only).
