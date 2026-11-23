# Deep research prompt: physically grounded procedural terrain (2026-11-23)

Sent to Neon by the user after the reference path tracer lane. Report:
`2026-11-23-terrain-pcg-deep-research.md` (triage pending).

```
Deep research request: physically grounded procedural terrain for a real-time engine (state of practice, 2025-2026)

CONTEXT (our engine, Meridian)
- C++20 / Vulkan 1.3 engine, raster-first (hardware ray tracing available but not on the default path), target RTX 4080 at native 1440p, 180 fps goal on a reference route.
- Goal: photorealistic, first-person, open world; first showcase is a ~15x15 km natural scene (meadows, beech/hornbeam/alder forests, rock, valleys). Visual target: ultra-realistic ground at walking distance AND at multi-km vistas.
- Current terrain: a 16-bit heightfield (real DEM: swissALTI3D 2 m, Napf region, 7500x7500), CDLOD quadtree rendered with mesh shaders, morphing LODs, up to 0.3 m procedural detail noise in the two finest levels, mirrored outside the crop. Normal map, a height pyramid used for ray marching (sun shadows, GI probe rays, terrain shadow-height map).
- Current ground material: a few Megascans surface sets (grass, forest floor, rock, cliff), blended per pixel by slope/height/noise masks, hex-tiled to hide repetition, triplanar on steep slopes, parallax occlusion mapping.
- Scatter: GPU placement of rocks, plants, trees and streamed grass by integer, bit-exact rules (slope, masks, density) from the same heightfield; forests from a procedural mask.
- Principles: the engine stays asset-agnostic and parametric (other planets possible); asset and authoring knowledge lives in tools/cook, not the engine. Roadmap: phase 1 = engine capabilities, phase 2 = environment built in an authoring tool (Houdini as the lead candidate). Dynamic weather and time of day are planned, so terrain materials must respond to wetness, snow and seasons at runtime, not only through baked values.

QUESTION
How should a modern engine get ultra-realistic, real-world-plausible terrain that is textured correctly by its own geology and processes? Specifically:
1. Generation: compare offline authoring (Houdini Heightfields/Terrain tools, Gaea 2, World Creator, World Machine, Instant Terra), runtime GPU generation and amplification (erosion on the GPU, detail synthesis on top of a coarse DEM), and learned methods (diffusion or GAN terrain synthesis, DEM super-resolution, example-based synthesis from real DEMs). Which processes matter visually at 1 m to 10 km scales: hydraulic and thermal erosion, sediment deposition, fluvial networks, glacial forms, talus and scree, rock strata. What is fast enough at what resolution, and what is "real-life aware" in a measurable sense (statistics versus real DEMs: slope and curvature distributions, drainage density, hypsometry)?
2. Terrain-aware texturing: how state-of-the-art pipelines derive material masks from simulation outputs (flow and wetness, deposition, curvature/convexity, exposure/aspect, altitude/insolation, geology layers) instead of hand-painted masks or plain slope/height rules. How these masks are stored and streamed (splat maps, virtual texturing, material ID plus weights, runtime procedural evaluation), and how they stay consistent with scatter (rocks on scree, ferns in wet hollows, trees on soil depth).
3. Runtime dynamics: wetness and puddles from rain, snow accumulation by slope/exposure, seasonal changes, all without re-cooking. What parameters belong in the baked data versus a runtime material layer (Unreal Material Parameter Collections, runtime virtual texturing, Frostbite/Decima/Horizon practices).
4. Rendering at scale: geometry (displacement versus mesh LOD versus parallax), virtual texturing or texture arrays, mipmapping and filtering of blended material masks at distance (the distant-terrain "flat colour" problem), and how leading games handle 1 m to 20 km in one view (Horizon Forbidden West, Microsoft Flight Simulator 2024, Death Stranding 2, Ghost of Yotei, Red Dead Redemption 2, Unreal 5.x landscape/PCG, Unity terrain).
5. Integration fit: which approaches keep a single heightfield (or a small set of layers) as the source of truth that ray marching, GI, shadows and bit-exact CPU/GPU placement can all read. Flag any approach that breaks that.

DELIVERABLE
- A ranked comparison of 3-5 concrete pipeline architectures (offline tool plus engine runtime split), each with: what is baked versus runtime, data formats and sizes for 15x15 km, GPU cost estimates at 1440p, authoring effort, realism evidence, risks.
- A recommended architecture for our constraints with reasons, and what NOT to do.
- For texturing: the specific mask channels and their derivation, with formulas or papers.
- Primary sources (papers 2020-2026, GDC/SIGGRAPH talks, official docs), with links and dates; mark anything unverified or vendor marketing. Prefer measured numbers over claims.
```
