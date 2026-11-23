// Far-field forests: brick columns of 4^3 voxels per 64 m cell at three
// levels (farfield.h, renderer_farfield.cpp). Layouts must match the C++
// declarations named below.
#ifndef FARFIELD_GLSL
#define FARFIELD_GLSL

#extension GL_EXT_buffer_reference : require

// Must match farfield.h.
const uint kFarLevels = 3u;
const uint kBrickEdge = 4u;
const uint kBrickVoxels = 64u;
const float kFarHeightUnit = 0.25;
const uint kFarChannels = 7u;

float farVoxelSize(uint level) {
    return 4.0 * float(1u << level);
}

// How far a level's voxels reach past FarCell.bottom/top: the extent bounds
// the deposits' heights plus half a voxel, and the trilinear splat writes
// voxels centered up to one voxel from a deposit, each spanning half a
// voxel around its center.
float farCanopyPad(uint level) {
    return farVoxelSize(level);
}

uint farColumnsPerCell(uint level) {
    return 4u >> level;
}

// BrickColumn: (firstBrick, baseHeight | brickCount << 16).
layout(buffer_reference, std430) buffer BrickColumnBuffer {
    uvec2 columns[];
};

uint columnBaseHeight(uvec2 column) {
    return column.y & 0xFFFFu;
}

uint columnBrickCount(uvec2 column) {
    return column.y >> 16u;
}

// Global column (x, z) of a level: cell-major, then row-major in the cell.
uint farColumnIndex(uint cellsX, uint level, uint x, uint z) {
    uint n = farColumnsPerCell(level);
    uint cell = (z / n) * cellsX + x / n;
    return cell * n * n + (z % n) * n + x % n;
}

// Voxel x, layer k, z inside its brick.
uint brickVoxelIndex(uint x, uint k, uint z) {
    return (x & 3u) + 4u * ((k & 3u) + 4u * (z & 3u));
}

// A level's brick pool: a header per brick (maskLo, maskHi, firstVoxel), then
// the occupied voxels. Must match BrickHeader and farBrickVoxel in farfield.h.
layout(buffer_reference, std430) buffer FarVoxelBuffer {
    uint voxels[];
};

const uint kBrickHeaderWords = 3u;

// Must match FarCell in gpu_types.h.
struct FarCell {
    float bottom;
    float top;
    uint treeCount;
    float treeHeight;
};

layout(buffer_reference, std430) buffer FarCellBuffer {
    FarCell cells[];
};

layout(buffer_reference, std430) readonly buffer FarPyramidBuffer {
    float tops[];
};

// Entry i < paletteSize: rgb the mean reflected diffuse albedo, a the
// diffuse transmission factor of the baked material; entry paletteSize + i:
// rgb its transmitted albedo (factor x transmission colour).
layout(buffer_reference, std430) readonly buffer FarPaletteBuffer {
    vec4 albedo[];
};

// Must match GpuFarField in gpu_types.h.
struct FarField {
    BrickColumnBuffer columns[3];
    FarVoxelBuffer voxels[3];
    FarCellBuffer cells;
    // Canopy top max over 2^k x 2^k cells, level k (1..pyramidLevels) from
    // pyramidOffsets[k].
    FarPyramidBuffer pyramid;
    FarPaletteBuffer palette;
    float originX;
    float originZ;
    float heightBase;
    uint cellsX;
    uint cellsZ;
    uint pyramidLevels;
    uint paletteSize;
    uint pad;
    uint pyramidOffsets[16];
};

layout(buffer_reference, std430) readonly buffer FarFieldBuffer {
    FarField field;
};

// Voxel `local` (brickVoxelIndex) of a brick; 0 where unoccupied.
uint farBrickVoxel(FarFieldBuffer farField, uint level, uint brick, uint local) {
    uint header = brick * kBrickHeaderWords;
    uint upper = local >> 5u;
    uint mask = farField.field.voxels[level].voxels[header + upper];
    uint bit = local & 31u;
    if (((mask >> bit) & 1u) == 0u) {
        return 0u;
    }
    uint rank = bitCount(mask & ((1u << bit) - 1u));
    if (upper != 0u) {
        rank += bitCount(farField.field.voxels[level].voxels[header]);
    }
    return farField.field.voxels[level].voxels[farField.field.voxels[level].voxels[header + 2u] + rank];
}

// The far switch (cell_cull.comp): a cell draws its voxels instead of its
// trees once its tallest tree projects under kFarSwitchPixels, at the
// coarsest level whose voxel projects to at most kFarLevelPixels.
const float kFarSwitchPixels = 6.0;
const float kFarLevelPixels = 1.0;

// One occupied template voxel. Must match FarTemplateVoxel in farfield.h.
struct FarTemplateVoxel {
    vec3 center;
    float extinction;
    vec3 normal;
    uint albedo;
};

layout(buffer_reference, std430) readonly buffer FarTemplateVoxelBuffer {
    FarTemplateVoxel voxels[];
};

// Per (prototype, level). Must match FarTemplateInfo in gpu_types.h.
struct FarTemplateInfo {
    vec3 centerMin;
    uint firstVoxel;
    vec3 centerMax;
    uint voxelCount;
};

layout(buffer_reference, std430) readonly buffer FarTemplateBuffer {
    FarTemplateInfo infos[];
};

// Voxel packing; must match packVoxel / unpackVoxel in farfield.cpp.
float signNotZero(float value) {
    return value >= 0.0 ? 1.0 : -1.0;
}

uint packVoxel(float extinction, vec3 normal, uint albedo) {
    uint code = 0u;
    if (extinction > 0.0) {
        float c = floor(18.0 * (log2(extinction) + 10.0) + 0.5) + 1.0;
        code = c < 1.0 ? 0u : uint(min(c, 255.0));
    }
    vec2 p = normal.xy / (abs(normal.x) + abs(normal.y) + abs(normal.z));
    if (normal.z < 0.0) {
        p = (1.0 - abs(p.yx)) * vec2(signNotZero(p.x), signNotZero(p.y));
    }
    uvec2 octahedral = uvec2(floor((p * 0.5 + 0.5) * 255.0 + 0.5));
    return code | octahedral.x << 8u | octahedral.y << 16u | albedo << 24u;
}

float voxelExtinction(uint voxel) {
    uint code = voxel & 0xFFu;
    return code == 0u ? 0.0 : exp2(float(code - 1u) / 18.0 - 10.0);
}

vec3 voxelNormal(uint voxel) {
    vec2 p = vec2((voxel >> 8u) & 0xFFu, (voxel >> 16u) & 0xFFu) / 255.0 * 2.0 - 1.0;
    vec3 n = vec3(p, 1.0 - abs(p.x) - abs(p.y));
    float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

uint voxelAlbedo(uint voxel) {
    return voxel >> 24u;
}

// ---------------------------------------------------------------- Marching
// In field coordinates: world minus (originX, heightBase, originZ).

const uint kFarMaxCellSteps = 512u;

// Marches a cell's brick columns at `level` along origin + t direction over
// [tStart, tEnd], voxels constant inside, adding to opticalDepth: a DDA over
// the cell's voxel columns in x/z, and in each column over its layers (the
// column's own vertical lattice), one voxel segment per step in one loop
// (nested loops cost registers in the shaders that inline this). Returns
// where the optical depth reaches `limit` (hitVoxel: the voxel there), or
// -1 if it doesn't. With wantAlbedo (a constant at every call site, so the
// plain march compiles unchanged) each segment also adds its absorbed share,
// exp(-opticalDepth) (1 - exp(-segment)), times the voxel's palette scatter
// albedo (reflected + transmitted) to albedoSum.
float farMarchCellImpl(FarFieldBuffer farField, uvec2 cell, uint level, vec3 origin,
                       vec3 direction, float tStart, float tEnd, float limit,
                       inout float opticalDepth, out uint hitVoxel, const bool wantAlbedo,
                       inout vec3 albedoSum) {
    float v = farVoxelSize(level);
    int n = int(kBrickEdge * farColumnsPerCell(level));
    ivec2 lo = ivec2(cell) * n;
    ivec2 voxel = clamp(ivec2(floor((origin.xz + direction.xz * tStart) / v)), lo, lo + n - 1);
    ivec2 stepXZ = ivec2(direction.x >= 0.0 ? 1 : -1, direction.z >= 0.0 ? 1 : -1);
    vec2 inverseXZ = 1.0 / vec2(abs(direction.x) > 1e-12 ? direction.x : 1e-12,
                              abs(direction.z) > 1e-12 ? direction.z : 1e-12);
    vec2 tMax = (vec2(voxel + max(stepXZ, ivec2(0))) * v - origin.xz) * inverseXZ;
    float inverseY = 1.0 / (abs(direction.y) > 1e-12 ? direction.y : 1e-12);
    int stepY = direction.y >= 0.0 ? 1 : -1;

    // t: where the ray enters the current voxel column; [a, b]: the rest of
    // the column's segment inside its layers.
    float t = tStart;
    float a = 0.0;
    float b = -1.0;
    bool newColumn = true;
    uint firstBrick = 0u;
    int layers = 0;
    float base = 0.0;
    int k = 0;
    for (uint steps = 0u; steps < kFarMaxCellSteps && t < tEnd; ++steps) {
        float columnEnd = min(min(tMax.x, tMax.y), tEnd);
        if (newColumn) {
            uvec2 column = farField.field.columns[level].columns[farColumnIndex(
                farField.field.cellsX, level, uint(voxel.x) / kBrickEdge,
                uint(voxel.y) / kBrickEdge)];
            firstBrick = column.x;
            layers = int(columnBrickCount(column) * kBrickEdge);
            base = float(columnBaseHeight(column)) * kFarHeightUnit;
            vec2 slab = (vec2(base, base + float(layers) * v) - origin.y) * inverseY;
            a = max(t, min(slab.x, slab.y));
            b = min(columnEnd, max(slab.x, slab.y));
            k = clamp(int(floor((origin.y + direction.y * a - base) / v)), 0, max(layers - 1, 0));
            newColumn = false;
        }
        if (a < b) {
            float layerEnd = (base + float(k + max(stepY, 0)) * v - origin.y) * inverseY;
            float end = min(layerEnd, b);
            uint packed = farBrickVoxel(farField, level,
                                        firstBrick + uint(k) / kBrickEdge,
                                        brickVoxelIndex(uint(voxel.x), uint(k), uint(voxel.y)));
            float extinction = voxelExtinction(packed);
            float segment = extinction * max(end - a, 0.0);
            if (wantAlbedo) {
                float taken = min(segment, limit - opticalDepth);
                uint entry = voxelAlbedo(packed);
                albedoSum += exp(-opticalDepth) * (1.0 - exp(-taken)) *
                             (farField.field.palette.albedo[entry].rgb +
                              farField.field.palette.albedo[farField.field.paletteSize + entry].rgb);
            }
            if (opticalDepth + segment >= limit) {
                hitVoxel = packed;
                return a + (limit - opticalDepth) / extinction;
            }
            opticalDepth += segment;
            a = end;
            k += stepY;
            if (a < b && k >= 0 && k < layers) {
                continue;
            }
        }
        // To the next voxel column.
        if (columnEnd >= tEnd) {
            break;
        }
        if (tMax.x < tMax.y) {
            voxel.x += stepXZ.x;
            tMax.x += abs(v * inverseXZ.x);
        } else {
            voxel.y += stepXZ.y;
            tMax.y += abs(v * inverseXZ.y);
        }
        if (any(lessThan(voxel, lo)) || any(greaterThanEqual(voxel, lo + n))) {
            break;
        }
        t = columnEnd;
        newColumn = true;
    }
    return -1.0;
}

float farMarchCell(FarFieldBuffer farField, uvec2 cell, uint level, vec3 origin, vec3 direction,
                   float tStart, float tEnd, float limit, inout float opticalDepth,
                   out uint hitVoxel) {
    vec3 unused = vec3(0.0);
    return farMarchCellImpl(farField, cell, level, origin, direction, tStart, tEnd, limit,
                            opticalDepth, hitVoxel, false, unused);
}

// Rays march L0 for their first 256 m, L1 to 1 km, L2 beyond. Must match
// kFarRayLevelEnd in farfield.h.
const float kFarRayLevelEnd0 = 256.0;
const float kFarRayLevelEnd1 = 1000.0;
// Past this optical depth (transmittance 1e-6) a ray counts as blocked.
const float kFarOpaqueDepth = 13.815511;
const uint kFarMaxRaySteps = 1024u;

// Transmittance through the far field along worldOrigin + t direction (unit),
// t in [startDistance, maxDistance] (startDistance one L0 voxel for rays
// from voxel surfaces, so they don't shadow themselves); the CPU replica is
// farFieldTransmittanceCpu. A DDA over the 64 m cells with the canopy top
// pyramid (Tevs 2008, as terrainOccludes): a block of 2^k cells whose
// canopy stays below the ray is skipped and the next tried one level
// coarser; otherwise it descends, and a cell the ray passes through at
// canopy height marches its bricks at the ray's level (by distance).
// skipTlasCells: M2b skips the cells its TLAS holds; in M2a no far ray
// traces trees in the TLAS, so there is nothing to skip. wantAlbedo: see
// farMarchCellImpl; albedoSum is then the absorbed-weighted albedo sum.
float farFieldMarch(FarFieldBuffer farField, vec3 worldOrigin, vec3 direction,
                    float startDistance, float maxDistance, bool skipTlasCells,
                    const bool wantAlbedo, inout vec3 albedoSum) {
    // Heights (canopy extents included) relative to heightBase from here on.
    float heightBase = farField.field.heightBase;
    uint levels = farField.field.pyramidLevels;
    vec3 origin = worldOrigin - vec3(farField.field.originX, heightBase, farField.field.originZ);
    float top = (levels > 0u ? farField.field.pyramid.tops[farField.field.pyramidOffsets[levels]]
                             : farField.field.cells.cells[0].top) -
                heightBase;
    float tEnd = maxDistance;
    if (direction.y > 0.0) {
        tEnd = min(tEnd, (top + farCanopyPad(kFarLevels - 1u) - origin.y) / direction.y);
    }
    vec2 inverseXZ = 1.0 / vec2(abs(direction.x) > 1e-12 ? direction.x : 1e-12,
                              abs(direction.z) > 1e-12 ? direction.z : 1e-12);
    ivec2 cellCount = ivec2(farField.field.cellsX, farField.field.cellsZ);
    vec2 gridT0 = -origin.xz * inverseXZ;
    vec2 gridT1 = (vec2(cellCount) * kCellSize - origin.xz) * inverseXZ;
    vec2 enter = min(gridT0, gridT1), leave = max(gridT0, gridT1);
    float t = max(startDistance, max(enter.x, enter.y));
    tEnd = min(tEnd, min(leave.x, leave.y));
    if (t >= tEnd) {
        return 1.0;
    }
    ivec2 stepXZ = ivec2(direction.x >= 0.0 ? 1 : -1, direction.z >= 0.0 ? 1 : -1);
    ivec2 cell = clamp(ivec2(floor((origin.xz + direction.xz * t) / kCellSize)), ivec2(0),
                       cellCount - 1);
    float opticalDepth = 0.0;
    uint k = 0u;
    for (uint i = 0u; i < kFarMaxRaySteps && t < tEnd; ++i) {
        ivec2 block = cell >> k;
        float blockTop;
        FarCell far;
        if (k == 0u) {
            far = farField.field.cells.cells[cell.y * cellCount.x + cell.x];
            blockTop = far.top - heightBase;
        } else {
            int width = (cellCount.x + (1 << k) - 1) >> k;
            blockTop = farField.field.pyramid.tops[farField.field.pyramidOffsets[k] +
                                                   uint(block.y * width + block.x)] -
                       heightBase;
        }
        ivec2 lo = block << k;
        ivec2 hi = ((block + 1) << k) - 1;
        vec2 axisExit = (vec2(mix(lo, hi + 1, greaterThan(stepXZ, ivec2(0)))) * kCellSize -
                         origin.xz) * inverseXZ;
        float tExit = min(min(axisExit.x, axisExit.y), tEnd);
        float y0 = origin.y + direction.y * t;
        float y1 = origin.y + direction.y * tExit;
        float pad = farCanopyPad(tExit < kFarRayLevelEnd0 ? 0u
                                 : tExit < kFarRayLevelEnd1 ? 1u
                                                            : 2u);
        bool below = blockTop + pad <= min(y0, y1);
        if (!below) {
            if (k > 0u) {
                --k;
                continue;
            }
            if (far.bottom - heightBase - pad < max(y0, y1)) {
                // One level per march (a single call site, as the march is
                // inlined); a cell the level changes in is revisited from
                // the change on.
                uint level = t < kFarRayLevelEnd0 ? 0u : t < kFarRayLevelEnd1 ? 1u : 2u;
                float levelEnd = level == 0u ? kFarRayLevelEnd0
                                 : level == 1u ? kFarRayLevelEnd1
                                               : tExit;
                float b = min(tExit, levelEnd);
                uint voxel;
                if (farMarchCellImpl(farField, uvec2(cell), level, origin, direction, t, b,
                                     kFarOpaqueDepth, opticalDepth, voxel, wantAlbedo,
                                     albedoSum) >= 0.0) {
                    return 0.0;
                }
                if (b < tExit) {
                    t = b;
                    continue;
                }
            }
        }
        // To the next block: across the side the ray leaves through (both
        // on a tie), the other axis from the exit point kept inside the
        // block, so rounding can't step backwards.
        ivec2 next = clamp(ivec2(floor((origin.xz + direction.xz * tExit) / kCellSize)), lo, hi);
        if (axisExit.x <= axisExit.y) {
            next.x = stepXZ.x > 0 ? hi.x + 1 : lo.x - 1;
        }
        if (axisExit.y <= axisExit.x) {
            next.y = stepXZ.y > 0 ? hi.y + 1 : lo.y - 1;
        }
        if (any(lessThan(next, ivec2(0))) || any(greaterThanEqual(next, cellCount))) {
            break;
        }
        if (below && k < levels) {
            ++k;
        }
        cell = next;
        t = tExit;
    }
    return exp(-opticalDepth);
}

float farFieldTransmittance(FarFieldBuffer farField, vec3 worldOrigin, vec3 direction,
                            float startDistance, float maxDistance, bool skipTlasCells) {
    vec3 unused = vec3(0.0);
    return farFieldMarch(farField, worldOrigin, direction, startDistance, maxDistance,
                         skipTlasCells, false, unused);
}

// Transmittance plus the mean scatter albedo (reflected + transmitted) of
// what the ray crossed, weighted by the light each voxel absorbed (a ray that
// crosses nothing returns albedo 0).
float farFieldTransmittanceAlbedo(FarFieldBuffer farField, vec3 worldOrigin, vec3 direction,
                                  float startDistance, float maxDistance, out vec3 albedo) {
    vec3 sum = vec3(0.0);
    float transmittance = farFieldMarch(farField, worldOrigin, direction, startDistance,
                                        maxDistance, false, true, sum);
    albedo = sum / max(1.0 - transmittance, 1e-6);
    return transmittance;
}

// ------------------------------------------------------------ Sun shadow map
// Must match farfield.h.
const uint kFarShadowLevel = 1u;
const float kFarShadowOpticalDepth = 0.6931472;
const uint kFarShadowEmpty = 0xFFFFu;
const uint kFarShadowStepCap = 0xFFFEu;

// Where the opacity of the kFarShadowLevel voxels along worldOrigin + t
// direction (unit) reaches kFarShadowOpticalDepth, t in [0, maxDistance], or
// -1; the CPU twin is farFieldShadowDistanceCpu. farFieldTransmittance's
// cell DDA and canopy pyramid at one level, which builds the map
// (farfield_shadow.comp).
float farFieldShadowDistance(FarFieldBuffer farField, vec3 worldOrigin, vec3 direction,
                             float maxDistance) {
    float heightBase = farField.field.heightBase;
    uint levels = farField.field.pyramidLevels;
    vec3 origin = worldOrigin - vec3(farField.field.originX, heightBase, farField.field.originZ);
    float top = (levels > 0u ? farField.field.pyramid.tops[farField.field.pyramidOffsets[levels]]
                             : farField.field.cells.cells[0].top) -
                heightBase;
    float pad = farCanopyPad(kFarShadowLevel);
    float tEnd = maxDistance;
    if (direction.y > 0.0) {
        tEnd = min(tEnd, (top + pad - origin.y) / direction.y);
    }
    vec2 inverseXZ = 1.0 / vec2(abs(direction.x) > 1e-12 ? direction.x : 1e-12,
                              abs(direction.z) > 1e-12 ? direction.z : 1e-12);
    ivec2 cellCount = ivec2(farField.field.cellsX, farField.field.cellsZ);
    vec2 gridT0 = -origin.xz * inverseXZ;
    vec2 gridT1 = (vec2(cellCount) * kCellSize - origin.xz) * inverseXZ;
    vec2 enter = min(gridT0, gridT1), leave = max(gridT0, gridT1);
    float t = max(0.0, max(enter.x, enter.y));
    tEnd = min(tEnd, min(leave.x, leave.y));
    if (t >= tEnd) {
        return -1.0;
    }
    ivec2 stepXZ = ivec2(direction.x >= 0.0 ? 1 : -1, direction.z >= 0.0 ? 1 : -1);
    ivec2 cell = clamp(ivec2(floor((origin.xz + direction.xz * t) / kCellSize)), ivec2(0),
                       cellCount - 1);
    float opticalDepth = 0.0;
    uint k = 0u;
    uint i = 0u;
    for (; i < kFarMaxRaySteps && t < tEnd; ++i) {
        ivec2 block = cell >> k;
        float blockTop;
        FarCell far;
        if (k == 0u) {
            far = farField.field.cells.cells[cell.y * cellCount.x + cell.x];
            blockTop = far.top - heightBase;
        } else {
            int width = (cellCount.x + (1 << k) - 1) >> k;
            blockTop = farField.field.pyramid.tops[farField.field.pyramidOffsets[k] +
                                                   uint(block.y * width + block.x)] -
                       heightBase;
        }
        ivec2 lo = block << k;
        ivec2 hi = ((block + 1) << k) - 1;
        vec2 axisExit = (vec2(mix(lo, hi + 1, greaterThan(stepXZ, ivec2(0)))) * kCellSize -
                         origin.xz) * inverseXZ;
        float tExit = min(min(axisExit.x, axisExit.y), tEnd);
        float y0 = origin.y + direction.y * t;
        float y1 = origin.y + direction.y * tExit;
        bool below = blockTop + pad <= min(y0, y1);
        if (!below) {
            if (k > 0u) {
                --k;
                continue;
            }
            if (far.bottom - heightBase - pad < max(y0, y1)) {
                uint voxel;
                float hit = farMarchCell(farField, uvec2(cell), kFarShadowLevel, origin, direction,
                                         t, tExit, kFarShadowOpticalDepth, opticalDepth, voxel);
                if (hit >= 0.0) {
                    return hit;
                }
            }
        }
        ivec2 next = clamp(ivec2(floor((origin.xz + direction.xz * tExit) / kCellSize)), lo, hi);
        if (axisExit.x <= axisExit.y) {
            next.x = stepXZ.x > 0 ? hi.x + 1 : lo.x - 1;
        }
        if (axisExit.y <= axisExit.x) {
            next.y = stepXZ.y > 0 ? hi.y + 1 : lo.y - 1;
        }
        if (any(lessThan(next, ivec2(0))) || any(greaterThanEqual(next, cellCount))) {
            break;
        }
        if (below && k < levels) {
            ++k;
        }
        cell = next;
        t = tExit;
    }
    // -2: out of steps before the end of the ray.
    return i >= kFarMaxRaySteps && t < tEnd ? -2.0 : -1.0;
}

#endif
