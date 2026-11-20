// The 2.5D slab clouds' column field (decisions.md "2.5D clouds"): the
// analytic columns slabColumn evaluates from the weather map and slices of
// the noise volumes. cloud_slab_bake.comp bakes it per frame; the march
// (clouds_slab.glsl) reads the baked field. Include after clouds.glsl.
#ifndef CLOUDS_SLAB_FIELD_GLSL
#define CLOUDS_SLAB_FIELD_GLSL

// Fraction of the type's layer height a full column reaches: cumulus wider
// than tall, as the loaves in the reference frames.
const float kSlabHeight = 0.4;
// The cut shape s in [0, 1] reaches the full height at 1 / kSlabRimWidth
// (a circular profile over that rim); kSlabTopHeap: the billow noise's
// relative swing of the thickness on cumulus tops.
const float kSlabRimWidth = 2.0;
const float kSlabTopHeap = 0.8;
// Rim fraction over which the density fades in.
const float kSlabRimSoftness = 0.3;
// The rim lifts above the flat base by this fraction of the missing
// thickness, so the edge of a cumulus curls up instead of ending in a
// knife-edge plate along the base.
const float kSlabRimLift = 0.15;
// Density ramps (m) inside the top and base surfaces: soft tops, a firmer
// base.
const vec2 kSlabSoftness = vec2(120.0, 40.0);
// Extinction (1/m) at density 1.
const float kSlabExtinction = 0.04;
// Billows: the detail volume at kBillowScale x its period carves the
// cumulus outline (scaled by type).
const float kSlabBillowErosion = 0.6;
// The noise slice advances with the weather time (wraps with it: an
// integer number of tiles per time period), so clouds form and dissolve.
const float kSlabSliceTiles = 16.0;
// Altitudes in the baked field are stored relative to this (m).
const float kSlabBakeOffset = 1700.0;

struct SlabColumn {
    float base;
    float top;
    // The type's layer (bottom, top), for the ambient height gradient.
    vec2 layer;
    // 0 at the rim to 1 where the full height is reached: the density
    // fades in over the rim, so thin rims are translucent, not walls.
    float rim;
};

// The column at world xz; lod: the noise mip for the search step (coarser
// far away, so features finer than a step blur instead of aliasing).
SlabColumn slabColumn(vec2 xz, float lod) {
    SlabColumn column;
    vec4 weather = weatherMapAt(vec3(xz.x, 0.0, xz.y));
    float coverage = weather.r;
    float type = clamp(weather.g, 0.0, 1.0);
    column.layer = cloudLayer(type, max(coverage, 1e-3));
    column.base = column.layer.x;
    column.top = column.layer.x;
    column.rim = 0.0;
    if (coverage <= 0.0) {
        return column;
    }
    Clouds c = pc.scene.clouds;
    vec2 p = xz - c.windOffset;
    float slice = c.time * kSlabSliceTiles / c.timePeriod;
    vec4 base = textureLod(textures3D[c.baseNoise], vec3(p / c.baseNoisePeriod, slice), lod);
    float worley = dot(base.gba, vec3(0.625, 0.25, 0.125));
    float shape = remap(base.r, worley - 1.0, 1.0, 0.0, 1.0);
    shape = clamp(remap(shape, kShapeRange.x, kShapeRange.y, 0.0, 1.0), 0.0, 1.0);
    // Billow texels are 4x the detail's: two mips fewer for the same step.
    vec3 billowNoise = textureLod(textures3D[c.detailNoise],
                                  vec3(p / (c.detailNoisePeriod * kBillowScale), slice * kBillowScale),
                                  max(0.0, lod - 2.0)).rgb;
    float billows = 1.0 - dot(billowNoise, vec3(0.5, 0.3, 0.2));
    shape = clamp(remap(shape, billows * kSlabBillowErosion * type, 1.0, 0.0, 1.0), 0.0, 1.0);
    shape = mix(shape, 1.0, kStratusShapeFloor * (1.0 - type));
    float cut = mix(kShapeCut.x, kShapeCut.y, type);
    float s = coverage * clamp(remap(shape, cut, 1.0, 0.0, 1.0), 0.0, 1.0);
    // Circular profile over the rim (s up to 1 / kSlabRimWidth): steep
    // sides and a flat-topped loaf; heaped tops from the billow noise,
    // modulating the thickness by a fraction of itself only, since
    // high-frequency height on a height field reads as vertical streaks
    // from the side.
    column.rim = min(s * kSlabRimWidth, 1.0);
    float r = 1.0 - column.rim;
    float dome = sqrt(max(1.0 - r * r, 0.0));
    float heap = 1.0 + kSlabTopHeap * type * (billows - 0.5);
    float height = kSlabHeight * (column.layer.y - column.layer.x);
    float thickness = height * dome * heap * (s > 0.0 ? 1.0 : 0.0);
    column.base = column.layer.x + kSlabRimLift * (height - thickness);
    column.top = column.base + thickness;
    return column;
}


#endif
