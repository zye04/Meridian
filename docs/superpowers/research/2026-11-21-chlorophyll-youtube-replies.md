# Toni Macaroni: owner replies (last 2 videos)

* c = comment
* R = response

Collected 21 Nov 2026. Includes every comment and reply on both videos. Where Toni replied inside a thread by @-mentioning someone, "c" is that person's comment, not the top comment of the thread.

---

## 1. [Chlorophyll Engine] 10M Trees - 15km² Test
https://www.youtube.com/watch?v=VC_NUxbeA0E (2,425 of 2,426 comments checked, 5 owner replies)

**Toni's pinned comment:**
"First of all thank you for the (quite unexpected) attention the previous video was getting. I'm in the process of writing up an FAQ with all the questions I have seen flying around and I will update the description once it ready. I try to read and answer all the questions, but some are asked twice or thrice so an FAQ makes more sense."

c -> "I am stunned. And I have just one big pile of questions in one go: What is your goal with this engine, how can it be used and.. will there be a way to use it, getting hands on it to either help you making your game or using it for own projects? I really wanna dig my nose into building landscapes with your engine - and eventually helping on tweaking parts or adding assets. I love Kingdom Come Deliverance 1 and 2 just for walking around in their forests for fun and relax. And you just build the ground for the future of these experiences." (@sithmaster)
r -> "@sithmaster I'm developing a game with this engine. From the start I wanted it the game to be highly moddable, although only with limited world editing capabilities. However I recently stumbled upon the fast erosion filter video again and thought it would be nice to have some experimental option to create and populate landscapes directly at runtime. So I'm starting to port the placement logic from Houdini to an in-engine library and maybe full on runtime generation will be possible sooner than later."

c -> "Mind you, this is just a graphics showcase, not object physics." (@ParkIngLot4039, in a thread under "You're putting most modern day engines to shame, please don't stop developing this")
r -> "@ParkIngLot4039 This is a GPU driven renderer and pretty much all physics run on the CPU, so those two don't really interfere with each other. Currently the CPU is pretty much idling in most cases. I also don't really plan on having a whole physics playground, and for the couple of characters I have more than enough headroom."

c -> "Also static lighting. Add raytracing to this and you won't get a frame. If this wants to catch up with latest engine standards it needs A LOT more work. Currently it looks like Cryengine from 10 years ago." (@dzezonja3558, same thread)
r -> "@dzezonja3558 Not sure I'm getting the argument here. Adding something just because other engines have it sort of defeats the purpose of making a custom engine in the first place. I have some visual goals and hardware constraints in mind and I want to develop the most efficient architecture around that. Games like KCD use some pretty old tech, but I haven't heard anyone say it's unplayable because of the bad graphics."

c -> "Absolutely incredible! At 4:08 you can see where the grass starts fading in, is it possible to apply the same tree rendering magic to the grass?" (@Vercidium)
r -> "The grass goes through the same voxel rendering path as the trees. However I experimented with faking the grass mesh shading (mainly normals, sss response and a custom brdf) on the terrain surface and it worked pretty well. I since changed the grass mesh shading a bit (and didn't update the terrain path) so they don't agree again, thus resulting in a somewhat noticeable transition (it's only really noticeable when flying or looking against a steep hill, walking around an a flat section it's pretty seamless). I found an interesting technique some while ago of smearing shaded pixels upwards to fake grass blades in the distance, however I don't know if this would work in this kind of grass as well. Rendering some terrain tiles twice (the second time with a top down grass look) would also be some worth trying, although I'm not sure if it would be cheaper than just straight up drawing the grass in the distance."

c -> "Hi @tonimacaroniy i would like to talk about your project in my youtube chanell, can i use these videos about yout chlorophyll engine?" (@everyeye)
r -> "Sure!"

---

## 2. Custom Engine Forest Showcase
https://www.youtube.com/watch?v=uR5EvBWxN14 (5,963 of 5,975 comments checked, 30 owner replies)

**Toni's pinned comment:**
"Because the question will probably come up often: This showcase was running on an RTX 3080 and I get an average of 130 fps uncapped (aerial views can go to 200 fps while in the deeper dense forest it can drop to 80 fps). My laptop with an RTX 3070 mobile can maintain 60 fps in most views as well. Cutting on some effects lower end hardware should be able to run this as well. RTX 2060 and RX 6400 is the theoretical limit because of the the GPU features I'm using."

### Replies in the pinned thread

c -> "whats the render resolution ?" (@mimosaSHORTSss)
r -> "@mimosaSHORTSss 1440p"

c -> "Did you see performance improvements switched from godot? I assume gpu timings would be similar?" (@peter2112)
r -> "@peter2112 I mean you can't make an apples to apples comparison because of the completely different architecture, but it's a massive performance boost and way smoother frames with no micro stutter."

c -> "do you use meshopt for the clod/voxelisation? or a custom impl" (@noone191)
r -> "@noone191 I use meshopt but slightly modified so it can also emit voxels during the clod building step"

c -> "You mention no raytracing, but I assume you do use hardware RT (hence the min spec requirement). I suppose you only use hardware RT for rendering the geometry and not for the lighting itself then? Can you give some details on how you approach lighting? Very impressive demo btw, I'm looking forward to seeing more! Also, are you planning on making some more technical videos/blog posts talking about the implemented features or future plans? Edit: The lighting looks incredibly stable and I was not able to spot any smearing or flickering artifacts (except maybe some small flickering in the water reflections :). What sort of AA solution do you run?" (@Gikster007)
r -> "@Gikster007 I don't use hardware RT anywhere. I would need to build a different scene representation for hardware RT first. I'm quite happy with the screen space + voxel approach for GI (more details in a future video) but raytraced reflections would be nice for water (I have planar reflections and SSR for now). The min spec is because of features like mesh shaders that I'm using."

c -> "Looks awesome. I have to ask though, why does the intro say "no DLSS 5"? How is that relevant?" (@Commentator-jh7wl)
r -> "@Commentator-jh7wl There is a wave of people slapping dlss 5 on every game to make it look real. Here it would defeat the purpose of showing some rendering tech when it's just a filter applied to the whole image that makes it look real. While it is interesting tech it isn't really runnable on the average pc and it tends to look generic and unstable. I just wanted to make sure people wouldn't get the idea that I just slapped a filter on it and called it a day."

### Tech / rendering

c -> "This is brilliant, so uncanny seeing such high fidelity visuals without the temporal artifacts we're used to seeing associated with these sort of dense foliage scenes. I work in foliage asset production, so I don't know too much about the technical side of it but how did you manage to make it so temporally stable and sharp looking where other engines like unreal can struggle on scenes exactly like these?" (@JeMeNommeGnome)
r -> "I have been fighting with this a lot as well and working with the general recommendations of DLSS and FSR you can only get so far. In the end I ended up tweaking the mip bias on the alpha tested grass and creating a "smear mask" based on movement and selectively sharpen it which does help a lot. While it of course doesn't recover the full detail, foliage captured with a real camera is never super sharp as well. The trees aren't a big problem, but the alpha tested grass can still smear (although not as much as most modern games I've played like KCD2). All in all it's not distracting anymore which is what's important to me, but I will keep improving on it if I can. I have also tested other features like reactive masks but it didn't really help."

c -> "Those trees look great. What tone mapping and BRDF are you using?" (@ThreatInteractive)
r -> "Thanks! For tonemapping I use AgX with contrast control. The BRDF is Lambert + GGX, with EON for some materials. I tried Burley before, but against real world captures it made no measurable difference for foliage and bark. The shading additionally uses models fitted to measured real world data for grass (both terrain material as well as grass meshes) and canopy. Since I voxelize the assets for GI already, the same voxel opacity volume is used to get the light's optical depth through each crown and add light scattered between leaves, which brings far forests closer to a pathtraced reference (this is just for realism sake, but the forest already looks good without it so it can be turned off for weaker machines)."

c -> "I dream of optimisations like this, the render distance is incredible! I need to look into CLODs, do you have any recommendations for places to start?" (@Vercidium)
r -> "I actually use meshoptimizer for cluster generation. Meshoptimizer has a pretty helpful clod helper and a nanite example in the repo as well to experiment with. In my engine I have a custom dag builder for emitting voxels as well during the build step. There's also the vk_lod_clusters example from nvidia which uses meshoptimizer as well. The concept itself is pretty simple imo, but the correct implementation can be a bit tricky so I prefer the code examples. Thankfully you can gradually implement and test clods and keep everything resident as well as evaluating every cluster at first. Streaming and efficient hierarchy traversal is pretty important later on when you actually want render heavy geometry. Another trick I use here is to bake proxies for tiles that get used after ~1km (based on screen error). Those completely drop the instance data and traversal of the dag and only get rendered through voxels. Maybe a future video? :) Would love to see an explanation of yours for this, since quite a lot of modern games started to use a "micro geometry" architecture."

c -> "At what distance do you swap to voxel representation of the geometry? Do you scale the voxel count down as distance goes up as well? I suppose this keeps the illusion better than billboard imposters." (@chrisrose708)
r -> "Triangles swap to voxels based on screen error so the perceived screen size detail is always the same and you have no pop in (in practice there can still be a visible transition due to normal and shading differences). This was actually the initial reason why I started on a custom engine. I hated how the billboards would always look flat because they couldn't receive proper shadows or GI response. I initially thought about doing something like gaussian splat aggregates, but settled on voxels (and UE has shown that it worked). I did start implementing it in Godot first but ran into a lot of issues because of their renderer design. Going with a custom engine was such a good decision because I can carefully develop the systems around my constraints and ultimately have a way faster renderer then I could ever get with Godot. I do miss the Godot editor though."

c -> "Are you using MSAA A2C for more distant trees? (I assume it's forward+ due to your Godot experience) I generally think A2C is an overlooked feature that can be used to great effect for foliage. DayZ Standalone is a great example of it iirc. Also, I'd loove if you could get a farther tree animation distance. It seems limited by distance to camera, and that is usually what I find missing in the UE5 Nanite Forest type environments. Otherwise this looks almost perfect. I really like how you place foliage. It's somewhat closer to KCD than the usual open world game and the way they did their foliage layout is as close to reality as I have ever seen in games." (@deRNmEpRrMm)
r -> "Every tree is either fully represented through triangles or voxels, there are no alpha tested impostors or leaf cards here so no MSAA A2C. I'm using a vis buffer based deferred path. For the animation UE chooses between a set of precomputed bone animations afaik. in my case I'm more limited by the shadows. Shadows in my engine are semi static (VSM like) with a small dynamic shadow map around the player for the tree animations and character. The far animations wouldn't have correct shadows other than the contact shadows. I can probably get away with warping them or only use contact shadows for leaf sized details which shouldn't be too noticeable."
r -> "For all the scattering work I use Houdini (like always) but I make sure to have a size gradient from outer skirt to inner forest with full canopy trees being on the outside and "sparse bottom" trees being on the inside. This is mostly what makes the forest look realistic and dense without bumping the density unrealistically high. The sparse bottom trees and the ~2m spacing between the trees is super important for gameplay sake when you want to walk around in the forest, otherwise you get stuck on colliders every step lol."

c -> "Wow! This looks incredible! Are you utilizing geometry virtualization and how are you managing overdraw?" (@Ray3D)
r -> "yes, CLODs and virtual textures on the terrain and I use a vis buffer approach which also helps with the overdraw"

c -> "how do you get past the massive overdraw from all the alpha clipped foliage (assuming thats how you did the vegetation)" (@chems)
r -> "Trees and bushes are fully triangles or voxels, no alpha tested impostors are used here. Only the grass and smaller ground cover is alpha tested but I use a vis buffer approach which helps with the overdraw."

c -> "looks very good, can you share a breakdown of where the rendering cost is going? how much to pure rendering of the triangles, how much to the voxels in the distance, how much to shadows, how much to the GI? and is any part of the shadows or GI baked?" (@Tystros)
r -> "Yes I'm going to go into detail on the individual systems of the engine. Initially I only planned on creating shorter explanations on X but I should probably also post on them Youtube."

c -> "Stunning! Are you using Vulkan?" (@UriahGnu)
r -> "Only D3D12 for now. I might support Vulkan once I know the renderer won't change much anymore."

c -> "amazing stuff, what language is the engine written in?" (@_3Leon)
r -> "C++ and luau for the scripting backend"

c -> "Dude, it's very cool, but give those rock cliffs some umph, and spawn some rocks. But great job. Are you going to share this someday? A source? What lang it is?" (@MikAlexander)
r -> "That's an ongoing effort and I'm experimenting with some alternative approaches for that. The issue is that scattering a set of cliffs/rocks will always look bad and creating geo from terrain extrusions will result in a lot of unique geo and disk size. I'm thinking about a fast virtual runtime renderer for the cliffs. And yes I'm going to share a demo and in the end ship a game. The engine is completely written in C++ and uses luau as the scripting backend. The full luau game source will be available so people can modify it however they like."

### Demo, game and project

c -> "I would love to experience this myself, any plans for a public demo?" (@SyntaxDaemon)
r -> "Yes, hopefully soon. Since I don't have an AMD card I need feedback as well."

c -> "Incredible! The weather system looks superb. What sort of game are you building this for?" (@blenderguru)
r -> "Thanks, didn't expect to see you here lol! While I don't have a concrete plot or storyline yet, I have always thought about a game with underground megastructures that serve as shelter for heavy storms above the ground. It's mainly a survival game but I want to experiment with different themes surrounding it."

c -> "all hand written?? that's pronominal progress in 8 months" (@kyzabuilds)
r -> "8 months since I started on the engine. However it uses tech that I implemented in the 2 years of using Godot so I guess it's technically a bit more than that."

c -> "It's a shame that youtube's compression ruins the quality as much as I'm sure it does." (@cptairwolf)
r -> "Then you haven't seen the total of 4 individual colors that the twitter compression gives you. I already uploaded in 4K idk if there's more I can do here other than release a demo so people can experience it for themselves."

### Short / casual

c -> "Thank you, now I don't have to go outside anymore." (@CoolLittleDetails)
r -> "Please do it's healthy :)"

c -> "How are we supposed to know you didn't just go outside and film?" (@Dystisis)
r -> "Cause there isn't a big datacenter in the middle of the nowhere"

c -> "Looks great but please stand still for a bit, everytime you want to see trees moving or stuff you run around like a chicken." (@vaulttraveler)
r -> "sorry I got happy feet"

c -> "finally someone remembers Crysis." (@PTsnsudeptofeng, under "Looks as impressive as Crysis back then. I love it!")
r -> "@PTsnsudeptofeng Crysis was one of the main reasons I got into CG and gamedev"

c -> "I actually thought I was being bated for a minute." (@xQds-rt)
r -> "glad you came"

c -> "what" (@Mawntee)
r -> "I've seen this face before"

c -> "cracked" (@SwifterYT)
r -> "omg it's the pretty visuals guy"

c -> "Here in no time. Incredible as always, but here I have to say you stepped up a lot!" (@Im-_-Axel)
r -> "Thanks Axel! I just haven't posted my progress in quite a while. I always wanted to, but then I thought "one last thing before I can post this". Well after 8 months it should be good enough for a little sneak peek :)"

c -> "My man, this is looking absolutely stunning! Amazing work!" (@JohnLogostini)
r -> "Thanks John!"
