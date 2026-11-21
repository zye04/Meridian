# Toni Macaroni on X (@ToniMacaroniy): Chlorophyll Engine replies

* c = comment
* R = response

Collected 21 Nov 2026 from all of his replies since Feb 2026 (35 total). This file keeps only the replies that explain how the custom engine works. Small talk, thank-yous and off-topic replies (AI coding, DLSS 5 jokes, sound, etc.) are left out. "…" marks a comment that X cut off.

---

## Rendering architecture

c -> "looks absolutely incredible, using tons of custom compute shaders I presume?" (@VRBobbie, Oct 22)
r -> "Hey Bobbie! Yes, the majority of the renderer is running on the compute pipeline. Since it's largely gpu driven with a vis buffer approach most of the culling and shading work is on compute. Only raster into vis buffer, output and some other effects run on the graphics pipeline."

c -> "a million high poly instances is a wild budget for a hand built engine. is it gpu driven culling with meshlets all the way out, or do the far trees swap to impostors? the near ones holding full detail is what sells it" (@m_emanaftab, Oct 23)
r -> "No impostors are being used. It's gpu driven with meshlets and small triangles are rendered as voxels (with minimal to no detail loss)."

c -> "Is this pathtraced?" (@cybrdelic, Nov 16)
r -> "no it's all raster"

c -> "Wow great frame rate, I'm struggling on godot to get it there on weaker gpus." (@ethrenity, Nov 2)
r -> "Yea you practically need to do everything in the renderserver and be smart about culling. But even then you will quickly hit a limit. I virtualize a lot in my engine from the ground up to keep a semi constant workload. Aerial views are very cheap and go up to ~200fps."

## Lighting, GI, shadows, sky

c -> "Hi, may i ask, Is the vegetation and the forest procedurally generated? And also, do GI used RT based or maybe other methods? This is really nice work!" (@e_pteranodon, Oct 23)
r -> "The forest is procedural. The vegetation is only part procedural, I'm working on a fully procedural tree simulation but I need to work on the trunk geo generation more. GI is a voxel/screenspace hybrid, no RT."

c -> "Have you seen the HDDAGI PR?" (@DZuloyan33995, Jun 10)
r -> "I saw it yes. However it wasn't much of interest to me since I have my own GI pipeline that I'm quite happy with."

c -> "I was quite displeased with how the grass shading looked in The Orb demo for Detis, so I've been experimenting with a couple of improvements. First, I reduced the texel size of the cascaded shadow maps to be slightly shorter than the medium-sized grass blades so grass geometry…" (@JKoukourakis, Oct 29)
r -> "I always just let contact shadows handle the smaller ground cover like grass and ignore them in the shadowmap pass. It's not really worth the cost. However the result depends a lot on the grass geometry."

c -> "nailed the photorealism, only really offputting visual is the clouds because those are like impossible to get photorealistic without being atrociously intensive" (@ghostofabaddon, Nov 2)
r -> "Yea I know what you're talking about. I'm still working on them and I think I can get them better looking. Since they are 2.5D and not truly volumetric I can get away with some art direction."

## World building / placement

c -> "I know you made your own engine to make this all possible, but how did you do the placements? They look so natural. Is it rule-based or did you hand-place?" (@OrnDMF, Nov 2)
r -> "All terrain and scattering work is done in houdini and yes it's rule based. For the natural look it's important to have a size gradient on the skirt as well as full canopy trees on the outside and "sparse bottom" trees inside."

## Performance and hardware

c -> "Oh this is beautiful. We ought to compare notes! What's the performance like and what hardware target?" (@Keyframe, Oct 22)
r -> "Depends heavily on the view. In aerial views it's ~5ms gpu time. Close to foliage it can be heavier. I'm using an rtx 3080 and I never really dip below 90 fps with GI and all the bells and whistles on. A laptop with a 3070 can hold up as well."
r -> "Although since I use things like mesh shaders there is a hard limit on what hardware this runs on."

c -> "Are you also optimizing or testing performance on different computers or just seeing what you can make?" (@ethrenity, Nov 2)
r -> "I'm developing on a 3080 and testing on a 3070 laptop from time to time. With an average of 120 fps I still have quite some headroom down, but I can also still cut down on several effects for lower systems. RTX 2060 or RX 6400 is the minimum because of the features I'm using."

## Why a custom engine (vs Godot / Unreal)

c -> "looks amazing. why u left godot though? i remeber that was the most impressive envrioment i saw on godot" (@arnaucco, Oct 23)
r -> "Because of the overhead you pay for things that you don't use. I wanted to create an engine that does mainly one thing and does it incredibly well (as well as just understanding what every part does). I still love godot and would recommend it to others."

c -> "Download Unreal Engine latest version. Get free megascans trees and foliages, now called megaplants, from Fab. Create sky atmosphere. Create landscape and do simple sculpting. Use procedural vegetation to spread trees and grass on the landscape. Use Lumen, Nanite and…" (@noxflux, Nov 3)
r -> "I ran a similar scene in unreal before with their quixel vegetation and voxels enabled and it did not run at 60 fps on my hardware, close views would even drop to 40 sometimes. Lumen was also temporally unstable and noisy. But I guess I'm just missing out."

---

## Extra: Godot-era replies (before the custom engine)

These are about his earlier Godot forest project, not the Chlorophyll Engine. I kept them because he says the new engine reuses tech he built during those Godot years, and the Houdini pipeline and screen-space shadow approach seem to have carried over.

c -> "That's awesome toni! Is all the generation done in houdini first then?" (@MjTheHunter, Mar 11)
r -> "Thanks! I generate the terrain splat maps and certain masks (e.g. for grass types) in Houdini, as well as a point cloud for the transforms and types of larger objects (rocks, trees, etc.). The output comes from terrain simulations as well as the labs biome tools."
r -> "The assets themselves aren't generated in Houdini (they come from an asset pack). I experimented with procedurally generated trees, but didn't quite like the result. However rocks and cliffs might be a good candidate for procedural generation."

c -> "this is amazing!! are you using screen space shadows here too? I'd love to learn more about this" (@Foyezes, Mar 11)
r -> "Yes! Screen space shadows are used here and are crucial. All the ground clutter isn't rendered into the shadow map, so they shadows come entirely from SSS. The resolution of the shadow map wouldn't be enough for those small objects anyway."

c -> "How are you dealing with landscape and tree/foliage instances? I left Godot after I figured out that it does not come with any landscape tools except random third-party add-ons, where one does not have any instanced foliage support with collisions. Not a good start, sadly." (@home3d, Mar 9)
r -> "All the foliage systems are custom as I have a tight houdini integration. Terrain3d is only used for the terrain geo. Tbh for games where foliage takes up 90% of the game you almost always want a custom system, so you can make it according to your needs."

c -> "I wonder about the stats. 0 drawcalls: how that? fps average 60: Are the fps somehow limited, or is this just the performance in this scene?" (@WuotanStudios, Dec 14 2025)
r -> "FPS is capped to 60, otherwise I experience some instability in the engine as well as my gpu running wild. As for the stats, since with gpu driven rendering and meshlets you can do fine-grained culling the drawn primitives shown by the engine aren't correct anymore."
r -> "That's also why the draw calls are 0 because I have overridden the viewport stats in order to show the correct stats later on."

c -> "That's fine i guess, though they could always do it as a fallback and meshlet for hardware the support it, althout i read someone in rocket that mimicking meshlets with compite seems to have minimal perf differences." (@Saulcava1, Dec 15 2025)
r -> "mimicking mesh shading in compute comes with a little overhead because you have to use the graphics pipeline at some point for drawing the meshlets. With mesh shaders you bypass the input assembler, vertex shader, etc."
