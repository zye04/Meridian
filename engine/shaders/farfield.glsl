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

layout(buffer_reference, std430) buffer FarVoxelBuffer {
    uint voxels[];
};

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

#endif
