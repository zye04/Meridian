### Task 7: Cloud look development (user, 2026-11-19)

The clouds work but look wrong against real photos: grey instead of white, smoke-puff shapes, uniform small cells, flat color, low contrast. This task makes them match the references through a measured comparison loop.

**Files:**
- Modify: `engine/shaders/clouds.glsl`, `clouds_march.comp`, `weather_map.comp`, `cloud_noise.comp` (as the loop requires), `game/src/main.cpp` (only if a capture flag is missing), `CLAUDE.md` (look-dev views), `docs/decisions.md` (every look choice with sources)
- Create: `tools/cloud-compare.sh` (builds the side-by-side comparison images and prints the measured targets)

**Inputs:** reference photos in `assets/reference/clouds/` (git-ignored: `cloud1.jpg`..`cloud6.jpg`, `images.jpg`); source notes (HZD 2015, Nubis 2017, Frostbite 2016, Wrenninge et al. 2015, Revision 2013 course notes, the GameDev.net HZD thread, Sebastian Lague's Clouds).

**Targets (user):**
1. White, not grey: albedo ~0.99 (scattering ≈ extinction), cumulus extinction ~0.04-0.1 /m; dual-lobe Henyey-Greenstein (forward ~0.8 blended with back ~-0.3) for silver linings; Wrenninge multiple-scattering octaves (3-4; extinction, scattering and eccentricity each attenuated ~0.5 per octave) instead of the powder hack.
2. Shape: flat sharp bases, billowing cauliflower tops, crisp edges: per-type height profile (sharp base, rounded top); base shape eroded by high-frequency Worley detail (inverted toward the base for wisps, billowy toward the top); curl-noise distortion at edges; the temporal upsample must not soften edges.
3. Scale: fair-weather cumulus 0.5-2 km wide with real vertical development; weather-map cells sized to match, clustered with gaps rather than uniformly spaced.
4. Color: sun light at each sample tinted by the atmosphere transmittance at that sample (pink/orange tops at sunset); ambient from skylight above (blue) vs ground bounce below by height in the cloud; aerial perspective on distant clouds.
5. Contrast: shaded bases darker than the adjacent sky, sunlit tops near-white.

- [ ] **Step 1:** `tools/cloud-compare.sh`: captures (under the GPU lock) matched to the references — midday ground view looking up (vs `cloud1.jpg`, `cloud5.jpg`), a scattered-cumulus wide view toward the horizon (vs `cloud3.jpg`, `cloud4.jpg`), sunset from above the layer (vs `cloud2.jpg`) — each placed beside its reference in one image; prints measured targets: sunlit-top luminance / adjacent clear sky (target 1.5-3x at midday), base luminance / adjacent sky (target < 1), base flatness (vertical spread of base heights in the frame).
- [ ] **Step 2: Loop**, one change at a time: change, rebuild, run the script, record the numbers and the comparison image path in the report, keep or revert. Until all five targets hold and the comparisons read as the same kind of cloud.
- [ ] **Step 3: Verify** nothing regressed: Task 4's motion checks (silhouette crop, turn sky crop), clear vs `--clouds off` within noise, 0 validation errors; "clouds" ms before/after (Debug).
- [ ] **Step 4:** Publish the final comparison images to the user (artifact page or files) and get their OK on the look before Task 8. Then commit ("Develop the cloud look against reference photos").

---

