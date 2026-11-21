# Topic C: foliage and grass shading (research, 2026-11-21)

Scope: gaps G3 (foliage does not glow backlit), G4 (grass bland), G8 (terrain vs grass colour), Megascans inputs (G6). Current model (mesh.frag ~l.995): `leaf = diffuseTransmission > 0`; when `N.L < 0` and the geometric normal faces away from the sun, adds `albedo/pi * sunAtSurface * saturate(-N.L) * sunVisibility(-Ng) * diffuseTransmission`, plus a sky back term. Untinted, no thickness map, no view dependence, no wrap.

Verification (coordinator requirement): every URL below was fetched on 2026-11-21 (HTTP status checked). "Content verified" means I read text on the page supporting the claim; "unverified" means I could not. Slides of the Ghost of Tsushima and Horizon talks are not publicly readable, so their details are second-hand. All cost numbers are either quoted from a source or marked "our estimate (unmeasured)".

## 1. Sources, newest first

| Year | Source (authors) | Solves / how | Cost | Status |
|---|---|---|---|---|
| 2026 | "Grass in Ghost of Tsushima" (Tiger Abrodi, blog, 2026-02) and "Emulating Ghost of Tsushima's Grass in Godot" (Jair Santana) | Third-party summaries of Wohllaib's talk. Content verified: curved normals (tilt outward to fake a round blade, cheaper than geometry); per-instance colour per blade; AO from grass density and height along the blade (Santana). Roughly 83,000 blades in ~2.5 ms (blog snippet, PS4 per the summary; talk itself not read) | 2.5 ms (second-hand) | Verified as summaries only |
| 2025 | Ghost of Yotei tech dive, PlayStation Blog | Not read in detail; no shading information used | n/a | unverified for shading |
| 2024-25 | Epic, "Shading models in Unreal Engine" and "Overview of Substrate materials" | Two Sided Foliage: inputs Base Color, Subsurface Color, AO, opacity; light transmitted through the surface tinted by Subsurface Color. Substrate: set the root node two-sided and masked. Content verified from search snippets (pages return 200; body not fully read) | Not stated | Shipped reference |
| 2024 | Megascans/Fab listings, Epic forum "Updates: New in Megascans", Twinmotion foliage docs | Foliage assets ship Albedo, Normal, Opacity, Roughness, AO, Translucency (varies per asset); newer exports use ORM packing without displacement. Content verified only via search snippets. The exact Mask-texture channel layout and `MA_Foliage` internals: unverified (not documented in anything found) | n/a | Partly unverified |
| 2022 | McLaren, Adventures with Deferred Texturing in Horizon Forbidden West (GDC) | Deferred texturing for foliage / alpha-tested geometry (cost of overdraw), per search snippet. Shading model: unverified. guerrilla-games.com returned 504 at check time; GDC Vault URL exists | n/a | Verified abstract only |
| 2021 | Wohllaib, Procedural Grass in Ghost of Tsushima (GDC Advanced Graphics Summit) | Abstract verified (GDC Vault page 200). Per search-result summaries (not read in the slides): normals lerp to terrain normal at distance; AO at blade base fades with distance; distant terrain colour blended to blade-top colour; translucency and AO per blade | Unverified | Summary-level |
| 2020 | Patry, Samurai Shading in Ghost of Tsushima (SIGGRAPH Physically Based Shading course) | Content verified (slides fetched): fuzz/asperity BRDF for moss and felt; wrap with n=2, w=0.2 in the simplified fuzz shader; "optional translucency" only mentioned. Not a foliage model | Not stated | Verified |
| 2019 | Serious Sam 4 grass talk (GPUOpen GDC 2019) | PDF fetched; lists grass material inputs "Albedo, subsurface, normal, AO, depth" (verified line). Further detail not read | n/a | Verified line only |
| 2018 | Far Cry 5 open world (Ubisoft, Advances 2018 PDF) | PDF downloaded (16.8 MB) and text-searched: no grass, foliage, vegetation or translucency hits. The "terrain humidity through grasses" claim came from a search snippet (PlayStation Blog), not from this PDF. Terrain-to-grass colour matching in FC5: unverified | n/a | Unverified for this topic |
| 2018 | Sanders, Vegetation of Horizon Zero Dawn (GDC) | Gamedeveloper.com page 200; abstract only (shaders, meshes, passes). The two-sided / thickness details I recalled are unverified | n/a | Abstract only |
| 2017 | Activision, Multilayered materials (PBS course slides) | URL resolves (200); not read; only mention of thin-surface geometric opacity from a search snippet. Not recommended as a source for us | n/a | unverified |
| 2011 | Barre-Brisebois and Bouchard, Approximating Translucency for a Fast, Cheap and Convincing Subsurface Scattering Look (Frostbite, GDC 2011; GPU Pro 2) | Content verified via a 2020s comparative paper (bentobaux.github.io) and a search snippet: back-light `T_dir = saturate(V . -(L + N*delta))^p * s`, times thickness `t` and attenuation, plus ambient `a_T`; constant per-pixel cost, no extra maps required. EA's own page returns 404 now; slides on SlideShare resolve (200) | Constant per pixel (verified); ALU count unverified | Classic |
| ~2010 | Wrap lighting / half-Lambert (GPU Gems skin chapter; Valve) | `saturate((N.L + w)/(1+w))`. Formula is standard; I did not re-fetch a source | 2 ALU, our estimate | Standard (source not re-fetched) |
| 2019 (book) / 1990 | Jacquemoud and Ustin, Leaf Optical Properties; Jacquemoud and Baret, PROSPECT, Remote Sens. Environ. 1990 | Both PDFs resolve (200). Search snippets verified qualitatively: chlorophyll absorption minimum near 550 nm; NIR R and T both about 0.5; N-layer plate model. Numeric visible-band R and T I wrote earlier (R 0.10-0.17, T 0.05-0.15 at 550 nm) are from memory: unverified, used only as order of magnitude | Offline | Authoritative, numbers unverified |

Gaps with no public text found: Ghost of Tsushima specular and clump variation, Horizon Forbidden West shading model, Frostbite/Activision leaf-specific models. Not relied on.

## 2. What Chlorophyll probably does (inference, not documented)

- R5 ("luminous yellow-green toward the sun, blue-green away") is the signature of two-sided diffuse with a transmission colour yellower and brighter than the reflected colour, plus a cooler sky-lit side. No special grass BSDF needed.
- F14 (earlier Godot demo, Houdini colour map and grass-density map) suggests an engine without a native foliage SSS model, so most likely a custom shader: wrap/Lambert plus a backlit translucency term. Confidence: 60% for custom wrap plus backlit translucency; 70% for grass colour driven by the terrain colour map (R4); 25% for anything physically measured. Root-dark / tip-bright grass (R3) is almost certainly a per-blade vertical gradient (same trick as Tsushima).

## 3. Recommendation for Meridian (ranked)

### Rank 1: a two-sided thin-leaf model replacing `diffuseTransmission` (G3)

Inputs: albedo `A` (linear), thickness/translucency `t` in [0,1] (Megascans Translucency map, else a per-material constant), `ao`, `N` facing the viewer, `L`, `V`.

```
w     = 0.25 (grass 0.35)
Rdiff = A/pi * saturate((N.L + w)/(1+w)) * sunVis * ao
Tc    = saturate(A * vec3(1.15, 1.00, 0.65))      // chlorophyll: transmitted light greener/yellower; values tunable
tau   = tauMax * t                                // tauMax ~0.5 leaves, ~0.3 grass (starting points, tune against frames)
Tdiff = Tc/pi * saturate(-N.L) * tau * sunVis(-Ng)
Tfwd  = Tc/pi * pow(saturate(dot(V, -normalize(L + N*delta))), p) * tau * sunVis(-Ng)   // p 4..8, delta 0.2..0.5
```

`Tfwd` is the Frostbite lobe (distorted light direction, power) and is what makes grass light up against the sun (user-04). `Tdiff` lights the shaded side of crowns. Energy: `A` (reflect) + `tau * Tc` must stay below ~0.9 per channel; tying `Tc` to `A` enforces it. Ambient: tint the existing sky back term by `Tc * tau`.

Cost: our estimate (unmeasured) ~25 ALU and at most one fetch; budget 0.15 ms at 1440p, to be measured with the capture's GPU passes line.

### Rank 2: cook-side texture inputs

- Pack translucency with AO in the cooked material; `meridian-cook` reads Megascans Translucency (or a constant per prototype). Force metallic 0 for foliage (G6).
- Vertex colour: `MA_Foliage` blends trunk/tileable/decoration by vertex colour (from the audit; not independently verified). If the cook ignores it, white bases appear. Thickness from vertex colour is a fallback when no map exists.

### Rank 3: grass-specific (G4, G8)

1. Root-to-tip gradient: `ao = mix(0.25, 1, h^0.7)`, fading to 1 with distance; dark saturated root to yellow-green tip. Replaces micro-shadow base darkening for grass (avoid double count).
2. Curved normals tilted outward across the blade width; lerp to terrain up with distance (less shimmer under TAA).
3. Per-clump variation from the existing placement hash: hue +/-0.03, value +/-15%, `t` +/-20% (our starting values).
4. Terrain colour matching: share one macro-colour function between terrain and grass in mesh.frag; at the blade root `A = mix(A, terrainColor, 0.4*(1-h))`; at distance fade terrain colour toward the blade-top colour (Tsushima, second-hand). Placeholder terrain stays until phase 2 colour maps.
5. Sheen: grazing term `0.15 * pow(1-NoV, 4) * skyRadiance(R)` or the fuzz lobe with wrap (n=2, w=0.2, Patry); blades roughness 0.45-0.6. Optional via the Effects panel for A/B (starting values, unverified).

### Not recommended now

Substrate-like layered slabs, spectral leaf BSDFs, screen-space SSS: cost and complexity out of proportion. PROSPECT only informs the default `Tc`/`tau`, set once by hand.

## 4. Verification plan

Backlit gate (sunset view and meadow-toward-sun) with the lobe on/off beside the Chlorophyll frames, luminance ratio sun-facing/away via `tools/region-luminance.py`; furnace test on a backlit quad (out <= in); unchanged motion/shimmer gates; GPU ms of scene passes before/after.

## 5. Sources (all resolved 2026-11-21 unless noted)

- Barre-Brisebois, Bouchard, Approximating Translucency..., GDC 2011: https://es.slideshare.net/colinbb/colin-barrebrisebois-gdc-2011-approximating-translucency-for-a-fast-cheap-and-convincing-subsurfacescattering-look-7170855 (EA page 404); comparative paper confirming the formula: https://bentobaux.github.io/assets/research/realtime-subsurface-scattering/paper.pdf
- Wohllaib, Procedural Grass in Ghost of Tsushima, GDC 2021: https://gdcvault.com/play/1027033/Advanced-Graphics-Summit-Procedural-Grass ; summaries: https://tigerabrodi.blog/grass-in-ghost-of-tsushima , https://jairsantana.com/posts/grass/ ; (http://gamedesignnews.com/... did not resolve, not cited)
- Patry, Samurai Shading in Ghost of Tsushima, SIGGRAPH 2020: https://blog.selfshadow.com/publications/s2020-shading-course/patry/slides
- Epic, Shading models in Unreal Engine: https://dev.epicgames.com/documentation/en-us/unreal-engine/shading-models-in-unreal-engine ; Substrate overview: https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-substrate-materials-in-unreal-engine
- Epic forum, Updates: New in Megascans: https://forums.unrealengine.com/t/updates-new-in-megascans/2067776 ; Twinmotion foliage: https://dev.epicgames.com/documentation/en-us/twinmotion/settings-for-foliage-materials-in-twinmotion
- Horizon Zero Dawn vegetation (Sanders, GDC 2018) abstract: https://gamedeveloper.com/art/video-how-guerrilla-crafted-the-lush-vegetation-of-i-horizon-zero-dawn-i- ; Forbidden West deferred texturing (McLaren, GDC 2022): https://www.guerrilla-games.com/read/adventures-with-deferred-texturing-in-horizon-forbidden-west (504 at check time)
- Far Cry 5 open world, Advances 2018 (no relevant content found): https://www.advances.realtimerendering.com/s2018/The%20Challenges%20of%20Rendering%20an%20Open%20World%20in%20Far%20Cry%205%20(With%20Notes).pdf ; Terrain Rendering in Far Cry 5 (Moore, GDC 2018): https://gdcvault.com/play/1025261/contactUs
- Serious Sam 4 grass, GDC 2019: https://gpuopen.com/gdc-presentations/2019/gdc-2019-agtd2-4-million-acres-serious-sam-4.pdf
- Activision multilayered materials (unread): https://www.activision.com/cdn/research/s2017_pbs_multilayered_slides_final.pdf
- Leaf optics: https://sites.bu.edu/cliveg/files/2023/10/leaf-optics-jacquemoud-ustin.pdf ; https://www.ipgp.fr/~jacquemoud/publications/jacquemoud1990.pdf
- Ghost of Yotei tech dive: https://blog.playstation.com/?p=411201
