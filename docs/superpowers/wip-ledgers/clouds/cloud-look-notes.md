# Cloud look notes: white cumulus, physically lit

Target: Vulkan/GLSL 2.5D cumulus slab, quarter-res march + temporal resolve, Hillaire 2020 LUTs, sun in lux, pre-exposed output.
Everything tagged **[R]** is my recommendation. Untagged statements come from the cited source, which I read in full text unless noted otherwise.

Sources (fetched 2026-11-20):
- HZD 2015 (Schneider/Vos, ARTR): https://www.advances.realtimerendering.com/s2015/The%20Real-time%20Volumetric%20Cloudscapes%20of%20Horizon%20-%20Zero%20Dawn%20-%20ARTR.pdf
- Nubis 2017: https://advances.realtimerendering.com/s2017/Nubis%20-%20Authoring%20Realtime%20Volumetric%20Cloudscapes%20with%20the%20Decima%20Engine%20-%20Final%20.pdf
- Frostbite 2016 (Hillaire), course notes §5: https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016_pbs_frostbite_sky_clouds.pdf
- Oz (Wrenninge/Kulla/Lundqvist 2013): https://fpsunflower.github.io/ckulla/data/oz_volumes.pdf
- Approximate Mie (Jendersie/d'Eon 2023): https://research.nvidia.com/labs/rtr/approximate-mie/publications/approximate-mie.pdf
- Revision 2013 course (Patapom): https://patapom.com/topics/Revision2013/Revision%202013%20-%20Real-time%20Volumetric%20Rendering%20Course%20Notes.pdf
- GameDev.net thread, 8 pages via Wayback (`web/2019id_/`): https://gamedev.net/forums/topic/680832-horizonzero-dawn-cloud-system/
- Lague: https://github.com/SebLague/Clouds, file `Assets/Scripts/Clouds/Shaders/Clouds.shader`

**What I could not verify:**
- (a) The HZD 2015 and Nubis 2017 slide equations are images. The PDF text has the speaker notes and the code captions but not the full "code in the slides". That means the 2015 Beer-powder formula `E = 2·e^{-d}·(1 − e^{-2d})` comes from memory and forum code, not from the slide text.
- (b) GPU Pro 7 (Schneider 2016) is not online. Its `SampleCloudDensity` is quoted below from the forum, where the operators are mangled and I had to reconstruct them.
- (c) No source gives world-space noise scales in metres, and none gives cumulus cell dimensions. Those values are [R].
- (d) Frostbite gives no numeric dual-lobe g/w values.

---

## 1. White, not grey: material and integration

- **Material** (Frostbite §5.2, citing Hess et al. 1998): albedo ρ = 1 and σs = σt. Measured σt at 550 nm:
  - stratus: **0.04–0.06 m⁻¹**
  - cumulus: **0.05–0.12 m⁻¹**

  The mean free path is therefore about 8–20 m, so a 1 km cumulus has an optical depth of roughly 50–120.
- **[R]** Use `σt(x) = density(x) · σt_max` with σt_max ≈ 0.08–0.1 m⁻¹. The density field should be in [0,1]. Set `σs = σt` (no absorption term). Keep one σt for both the view ray and the sun ray.
- **Why single scattering looks dirty:** "With only single scattering … clouds would only look like dirty/smoky element with only scattered light at their surface" (Frostbite §5.2). Multiple scattering is mandatory (§3 below).
- **Energy-conserving step** (Frostbite §5.6.3, eq. 17): treat the scattered source S as constant over the step and integrate the transmittance analytically.

```glsl
// S = σs * (sun_inscatter + ambient_inscatter)   [cd/m² per m]
float Tstep = exp(-sigmaT * ds);
vec3  Sint  = (S - S * Tstep) / max(sigmaT, 1e-7);  // ∫0^ds e^{-σt x} S dx
L += T * Sint;
T *= Tstep;
```

Note that Frostbite's Listing 2 passes `luminance` directly, without multiplying by σs. The S in eq. 17 must include σs. If you omit σs, the result is 1/σt ≈ 10× too bright, or too dark if you compensate somewhere else.

- **Order matters:** "(S) before (T)" is not energy conserving, and "(T) before (S)" over-darkens (Frostbite §5.6.1). Eq. 17 makes the result nearly independent of step count: 21 samples matched a 512-sample reference.
- **Physical units [R]:**
  - The sun term is σs · E_sun[lux] · T_atm · T_cloud · p(θ)[sr⁻¹], which gives cd/m² per metre. With σs = σt and eq. 17, the medium's brightness saturates at about E_sun·p·T_atm·T_cloud, independent of density.
  - Apply pre-exposure once, at write-out. Keep the march accumulation in fp32.
  - Sanity check: a sunlit cumulus side at noon should land in the same range as a white Lambertian card, L ≈ E/π ≈ 30k cd/m² for E ≈ 100k lux. That is brighter than the blue sky beside it (single-digit kcd/m²).
- **No darkness floors.** Lague's `darknessThreshold + T·(1−darknessThreshold)` and `phaseParams.z + hg·phaseParams.w` are artistic floors. Nubis: "we do not clamp any values in our lighting model" (HDR).

## 2. Phase function

- HG, normalised. Use it exactly as written; see the pitfalls about π errors.

$$p_{HG}(\theta,g)=\frac{1}{4\pi}\frac{1-g^2}{(1+g^2-2g\cos\theta)^{3/2}}$$

- **Frostbite dual lobe** (eq. 18): `p = lerp(pHG(θ,g0), pHG(θ,g1), w)`. Frostbite adds the back lobe because a single strong forward lobe leaves sun-behind-camera views "dull" and lit only by ambient.
- **Forum implementation** (thread p.6): `lerp(HG(0.8), HG(−0.5), 0.5)`.
- **[R] starting point:** g0 = 0.8, g1 = −0.3, w = 0.3 toward the back lobe, i.e. `p = 0.7·HG(0.8) + 0.3·HG(−0.3)`. Tune w in 0.2–0.5.
- **Nubis 2017 silver lining** (not energy conserving): `max(HG(cosθ, 0.6), silver_intensity · HG(cosθ, 0.99 − silver_spread))`. Schneider says in the thread (p.7) that the shipped version used a "triple henyey-greenstein".
- **Approximate Mie** (Jendersie/d'Eon, HG + Draine blend, droplet diameter d in µm, valid for 5 < d < 50). It fits the forward 95% of energy and misses the fogbow and glory.

$$\phi_D(\theta)=\frac{1}{4\pi}\frac{1-g^2}{(1+g^2-2g\cos\theta)^{3/2}}\frac{1+\alpha\cos^2\theta}{1+\alpha(1+2g^2)/3}$$
$$\phi=(1-w_D)\,\phi_{HG}(g_{HG})+w_D\,\phi_D(\alpha,g_D)$$
$$g_{HG}=e^{-0.0990567/(d-1.67154)},\; g_D=e^{-2.20679/(d+3.91029)-0.428934},\; \alpha=e^{3.62489-8.29288/(d+5.52825)},\; w_D=e^{-0.599085/(d-0.641583)-0.665888}$$

  The table below gives the values I computed. The 0.99 lobe is essentially the sun aureole: at quarter resolution it is fine, but it adds nothing away from the sun.

  | d (µm) | g_HG | g_D | α | w_D |
  |---|---|---|---|---|
  | 10 | 0.988 | 0.556 | 22.0 | 0.482 |
  | 20 | 0.995 | 0.594 | 27.1 | 0.498 |

  **[R]:** Use it for the single-scatter octave (n = 0) with d ≈ 15–20, and dual HG for the higher octaves.

## 3. Multiple scattering

- **Oz** (eqs. 1–2), with N octaves summed:

$$L=\sum_{i=0}^{N-1}L_i,\quad L_i=\sigma_s\,b^i\,L_{light}\,p(\omega_i,\omega_o,c^i g)\,e^{-a^i\int\sigma_t ds}$$

  Defaults: **N = 8, a = b = c = ½** ("a good starting point for all the cases we tried").
- **Naming clash:** in Oz, a is attenuation and b is contribution. Frostbite (eq. 20) swaps them: σs′ = σs·aⁿ, σe′ = σe·bⁿ, p′ = p(θ·cⁿ), and requires **a ≤ b** for energy conservation. Frostbite ships N = 2–3.
- **Plug-in [R]:** run the light march once to get the sun optical depth τ = ∫σt ds, then loop over the octaves. The view-ray transmittance stays unmodified; only the sun path's τ and the phase are softened.

```glsl
float tauSun = lightMarch(x);              // optical depth to sun (cone + shadow map)
vec3  Lsun = vec3(0); float an = 1, bn = 1, cn = 1;
for (int n = 0; n < 3; ++n) {              // [R] N=3 real-time
    Lsun += an * exp(-bn * tauSun) * phase(cosTheta, cn);  // phase scales g by cn
    an *= 0.5; bn *= 0.5; cn *= 0.5;       // Frostbite naming: a=scatter, b=extinction
}
vec3 S = sigmaS * (E_sun * T_atm(x) * Lsun + ambientInscatter(x));
```

- **Nubis 2017 variant:** `max(exp(-τ), 0.7·exp(-0.25·τ))`, which is a 2-octave approximation with a max(), "similar to Wrenninge". It is ramped down as the view direction approaches the sun.
- **Beer-powder is not physical.** HZD 2015 derives the dark sun-facing edges ("powdered sugar") from a thought experiment: in-scatter potential grows with depth. They multiplied Beer by `(1 − e^{-2d})` and made the term view-dependent, strongest when looking away from the sun. Forum code (p.6): `2·exp(-d)·lerp(1, 1−exp(-2d), saturate(-cosθ·0.5+0.5))`.
  - It is a heuristic: it is not normalised, and its ×2 doubles energy in thick regions.
  - Nubis 2017 replaced it with an "in-scatter probability": `(0.05 + pow(lodDensity, remap(h,0.3,0.85,0.5,2))) · pow(remap(h,0.07,0.14,0.1,1),0.8)`.
  - A forum user (p.6) reports that powder in the shadow path made clouds behind lighter.
  - **[R]:** With octaves plus a proper ambient term, skip powder at first. If you add it, apply it only to the sun term and only as a darkening multiplier ≤ 1, never in the light march.

## 4. Shape: density model

- **Textures** (HZD 2015):
  - base: 128³ RGBA. R is Perlin-Worley; GBA are Worley at increasing frequency.
  - detail: 32³ RGB Worley at increasing frequency.
  - curl: 128² RGB curl noise.

  Frostbite collapses each volume to a single channel for bandwidth. Hillaire's generator is https://github.com/sebh/TileableVolumeNoise.
- **Perlin-Worley** (Nubis): `remap(perlin, 1 − worley, 1, 0, 1)`.
- **GPU Pro 7 sampler** (forum p.1, operators reconstructed):

```glsl
float lowFBM = n.g*0.625 + n.b*0.25 + n.a*0.125;
float base   = remap(n.r, lowFBM - 1.0, 1.0, 0.0, 1.0);      // dilate PW by Worley fBm
base *= heightGradient(h, cloudType);                         // per-type profile
float cov    = weather.r;
base = remap(base, cov, 1.0, 0.0, 1.0) * cov;                 // coverage as erosion, then scale
p.xy += curl.xy * (1.0 - h);                                  // turbulence strongest at base
float hiFBM  = d.r*0.625 + d.g*0.25 + d.b*0.125;              // detail tex sampled at p*0.1
float hiMod  = mix(hiFBM, 1.0 - hiFBM, saturate(h * 10.0));   // wispy at base -> billowy above
float dens   = remap(base, hiMod * 0.2, 1.0, 0.0, 1.0);
```

- **Height profiles** (Nubis): `remap(h,0,0.1,0,1) * remap(h,0.2,0.3,1,0)`, i.e. stratus. Type shifts the in/out points across the three types. A forum user's float4 gradients (p.4):
  - stratus: (0, .07, .08, .15)
  - stratocumulus: (0, .2, .42, .6)
  - cumulus: (0, .08, .75, 1)

  **[R]** For flat bases, use a very short base ramp, `remap(h, 0, 0.03, 0, 1)`, and get the round top by shrinking coverage with height rather than ramping density. A forum user does exactly this (p.1: "modify the coverage", not density). Example: `remap(base, cov_eff, 1,0,1)` with `cov_eff = 1 − (1 − cov)·(1 − 0.6·h²)`.
- **Anvil** (Nubis): `cov = pow(cov, remap(h, 0.7, 0.8, 1.0, mix(1.0, 0.5, anvil_bias)))`.
- **Wind shear** (Nubis): `p += h * windDir * 500.0`.
- **Weather map:** HZD uses R = coverage, G = precipitation, B = type, with precipitation adding absorption to darken rain clouds. Nubis covers ~100 km² with an RGB cloud map: a low-frequency mask for the air-mass regions, Perlin footprints (connected), and Perlin-Worley footprints (clustered). Frostbite uses R = density, G = type index into a type texture, where X is type, Y is height, R is density, and G is erosion amount.
- **Scales [R]** (no source):
  - Fair-weather cumulus: 0.5–2 km wide, 0.5–1.5 km tall, base at a flat condensation level of about 1–2 km.
  - Base 128³ tile: about 4–8 km, so the Worley cells (~4 per tile at the lowest octave) land at about 1–2 km.
  - Detail 32³ tile: about 0.3–0.6 km, which is HZD's ×0.1 relative scale.
  - Weather footprints: clustered Perlin-Worley at ~1–3 km cells, gated by a 10–20 km low-frequency mask so clouds group into fields with blue gaps.

## 5. Lighting colour

- **Sun:** `E_sun · T_atm(x, sunDir)` from the Hillaire 2020 transmittance LUT at the sample's altitude and sun zenith **[R]**. This gives warm low-sun tops and white noon light with no extra tint parameter.
- **Ambient:**
  - HZD's colour model: "ambient sky contribution increases over height; direct dominated by sun colour; atmosphere occludes clouds over depth".
  - Frostbite uses an SH DC term with a linear 0→1 height gradient, optionally biased to [a, 1] for ground bounce, plus artist scale and desaturation. It admits this is not physical and that clouds go blue without multiple scattering.
  - Revision 2013 gives an analytic slab: `IsoTop·max(0, e^{a} − a·Ei(a))` with a = −σt·(top − y), plus the same term for the bottom with ground light.
  - **[R]:** `ambient = σs · [ L̄sky_up·f_top(h) + ρg·Ē_ground/π·f_bot(h) ] / 2`.
    - L̄sky_up is the mean upper-hemisphere sky radiance from the sky-view or multi-scattering LUT; E_up = π·L̄.
    - Ē_ground = E_sun·cosθs·T_atm + sky irradiance, with ρg ≈ 0.1–0.3.
    - f_top and f_bot can be the Revision slab terms, or a smooth gradient.
    - Apply the octave sum to ambient too, using the same aⁿ, or ambient stays bluish.
- **Aerial perspective:**
  - Frostbite eq. 21 takes a transmittance-weighted mean cloud depth, `Σ T(xn)·depth(xn) / Σ T(xn)`, skipping empty samples. It then samples the AP LUT once and applies `L = L_cloud·T_AP + L_AP·(1 − T_cloud)`, which is the composite form.
  - Nubis takes the depth where alpha reaches 0.5.
  - **[R]:** Use the Hillaire 2020 AP froxel volume at that depth. Without AP, the horizon visibly mismatches (Frostbite Fig. 41).

## 6. Light march and temporal

- **HZD 2015:**
  - 6 light samples in a cone toward the sun, the last one placed far away for distant shadows.
  - Once view alpha reaches 0.3, the light samples switch to the cheap (base-only) density, which made the shader 2× faster.
  - The view march takes 64 samples looking up and 128 at the horizon. It uses cheap large steps until density > 0, then steps back one and goes full detail, and returns to cheap after several empty samples.
- **Nubis 2017:**
  - 54–96 view steps.
  - 5 cone samples with decreasing LOD.
  - Back to cheap stepping after 10 empty samples.
- **Frostbite:** 4 shadow samples at `d0·kⁱ` (geometric spacing), plus per-frame jitter. A 2D cloud transmittance shadow map is projected on the world.
- **[R] for Meridian:**
  - 6 geometric steps: t₀ ≈ 30–50 m, k ≈ 2, which reaches ~2–3 km.
  - Then add optical depth from a top-down cloud optical-depth map beyond the last step, for distant clouds. Do not double count the overlap region.
  - Jitter the light-ray start with blue noise per frame.
- **Temporal:**
  - HZD renders a quarter-res buffer and updates 1 of 16 pixels per 4×4 block per frame. It reprojects the previous frame, falls back to the low-res buffer where reprojection fails, and takes 2 ms total.
  - Frostbite uses an EMA with reprojection.
  - Nubis takes a `max()` of low-LOD depth for thin occluders.
  - **[R] to avoid soft edges:**
    - Reproject with the cloud mean depth, not the far plane.
    - Clamp history to the current 3×3 low-res min/max in YCoCg, with transmittance included.
    - Upsample with weights from depth and transmittance, not bilinear on premultiplied colour.
    - Lower the history weight where |ΔT| is large.
    - Never blur T separately from L.

## 7. Pitfalls: why clouds read as grey smoke

1. **Too thin:** σt ≪ 0.05 m⁻¹, or density mostly < 0.3, makes clouds translucent. Fix: σt_max ≈ 0.1 and remaps that saturate cores (Nubis notes that remap, unlike multiply, "prevents a loss of too much density at the core").
2. **Single scattering only:** dark interiors, "dirty/smoky" (Frostbite). Fix: octaves (§3).
3. **Floors and offsets** (Lague's `darknessThreshold`, phase offset `phaseParams.z`): grey haze and lost contrast. Fix: remove them; ambient plus octaves provides the fill.
4. **Two absorption coefficients** (Lague: `lightAbsorptionTowardSun` ≠ `lightAbsorptionThroughCloud`): non-physical. Fix: use one σt.
5. **Phase normalisation bugs:**
   - The Nubis slide text reads `… / 4.0 * PI`, which multiplies by π instead of dividing.
   - The forum HG uses `* 0.25` with no 1/π.
   - Lague uses `4*3.1415`.

   In lux-based lighting these errors are 3–10× brightness errors that then get hand-tuned away. Fix: `1/(4π)`, plus a unit test that the phase integrates to 1 over the sphere.
6. **Missing σs in Frostbite's Listing 2 S term:** see §1.
7. **Powder used as a global multiplier, or inside the light march:** darkens everything, or lightens shadows behind (forum p.6).
8. **Blue ambient:** ambient without octaves or ground bounce. Fix: §5.
9. **Erosion everywhere:** lacy, smoky edges. Fix: erode only near edges. Use remap erosion (GPU Pro 7), or Lague's `(1−shape)³` weighting, with wispy detail near the base and billowy detail above.
10. **No coverage and height structure:** fog-like blobs with no flat bases. Fix: a short base ramp and coverage shrinking with height.
11. **Exposure:** with sky-driven auto exposure, physically correct clouds can still clip or look dim. Fix: verify the absolute luminance against the ~E/π card check before touching any cloud parameter. **[R]**
12. **Temporal smear:** soft or ghosted edges after upsampling. Fix: §6.
