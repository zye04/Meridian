// The far field at load (setPlacement, after the TLAS): each far-field
// prototype's BLAS is voxelized by farfield_bake.comp into a 1 m template
// (ray pass fractions, normals, mesh slots, base colors), filtered on the
// CPU to the three levels (filterFarTemplate). farfield_splat.comp then
// splats the placed table level by level: occupancy (read back; the CPU
// allocates the brick columns), fixed-point accumulation into transient
// buffers, pack. Debug builds compare sample cells with the CPU replica
// (splatFarField) and print a hash of the result.
#include "renderer_impl.h"

#include <meridian/farfield.h>
#include <meridian/placement.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace meridian {
namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
constexpr double kMiB = 1.0 / (1 << 20);
// Column bases count from this far below the heightfield's minimum: a
// tree's lowest voxels can reach below its origin by half a voxel (8 m at
// L2) plus its sink depth.
constexpr float kFarBaseMargin = 32.0f;

// Dispatch dimensions for `groups` workgroups (x at most 65535).
std::array<uint32_t, 2> groups2d(uint64_t groups) {
  const auto x = static_cast<uint32_t>(std::clamp<uint64_t>(groups, 1, 65535));
  return {x, static_cast<uint32_t>((groups + x - 1) / x)};
}

// Debug: sample cells of the GPU splat against splatFarField over the cells
// within `ring` of each (every instance that can reach it): columns
// exactly; voxels within a code step (GPU log2 and division round
// differently) or 16 fixed-point units, and where well resolved, normals
// within 2 octahedral steps and palette colors within 0.01 (the entries
// can be near-identical, so the index may differ).
void checkFarField(const FarFieldGrid &grid, uint32_t ring,
                   const std::vector<FarTemplates> &templates, const std::vector<Vec3> &palette,
                   const std::array<float, kFarLevels> &scales,
                   const std::array<std::vector<BrickColumn>, kFarLevels> &columns,
                   const std::array<const uint32_t *, kFarLevels> &voxels,
                   const PlacedInstance *instances, const Cell *cells,
                   const std::vector<FarCell> &farCells) {
  const uint32_t cellCount = grid.cellsX * grid.cellsZ;
  std::vector<uint32_t> samples = {0};
  for (uint32_t s = 0; s < 1u << 16 && samples.size() < 9; ++s) {
    const uint32_t cell = placementHash(s, 0x6661, 0) % cellCount;
    if (farCells[cell].treeCount > 0) {
      samples.push_back(cell);
    }
  }
  uint32_t compared = 0, occupied = 0, mismatches = 0;
  for (uint32_t cell : samples) {
    const uint32_t cx = cell % grid.cellsX, cz = cell / grid.cellsX;
    const uint32_t x0 = cx >= ring ? cx - ring : 0, z0 = cz >= ring ? cz - ring : 0;
    const uint32_t x1 = std::min(cx + ring, grid.cellsX - 1);
    const uint32_t z1 = std::min(cz + ring, grid.cellsZ - 1);
    const FarFieldGrid window{grid.originX + x0 * kCellSize, grid.originZ + z0 * kCellSize,
                              grid.heightBase, x1 - x0 + 1, z1 - z0 + 1};
    std::vector<DecodedInstance> decoded;
    for (uint32_t z = z0; z <= z1; ++z) {
      for (uint32_t x = x0; x <= x1; ++x) {
        const Cell &c = cells[z * grid.cellsX + x];
        for (uint32_t i = c.firstInstance; i < c.firstInstance + c.instanceCount; ++i) {
          decoded.push_back(decodePlacedInstance(instances[i]));
        }
      }
    }
    const FarFieldCpu cpu = splatFarField(window, templates, palette, decoded, scales);
    const auto report = [&](uint32_t level, const char *what) {
      if (mismatches++ == 0) {
        std::printf("far field check: first mismatch at cell (%u, %u), level %u: %s\n", cx, cz,
                    level, what);
      }
    };
    for (uint32_t level = 0; level < kFarLevels; ++level) {
      const uint32_t n = farColumnsPerCell(level);
      for (uint32_t lz = 0; lz < n; ++lz) {
        for (uint32_t lx = 0; lx < n; ++lx) {
          const BrickColumn &g =
              columns[level][farColumnIndex(grid.cellsX, level, cx * n + lx, cz * n + lz)];
          const BrickColumn &c = cpu.columns[level][farColumnIndex(
              window.cellsX, level, (cx - x0) * n + lx, (cz - z0) * n + lz)];
          if (g.heightAndCount != c.heightAndCount) {
            report(level, "column");
            continue;
          }
          for (uint32_t i = 0; i < columnBrickCount(g) * kBrickVoxels; ++i) {
            const uint32_t a = voxels[level][size_t{g.firstBrick} * kBrickVoxels + i];
            const uint32_t b = cpu.voxels[level][size_t{c.firstBrick} * kBrickVoxels + i];
            const auto byte = [](uint32_t v, uint32_t k) {
              return static_cast<int32_t>((v >> (8 * k)) & 0xFF);
            };
            ++compared;
            occupied += a != 0 ? 1 : 0;
            // Each deposit may round one fixed-point unit apart (GPU
            // normalize and cross differ in the last bit), so voxels within
            // 64 units of empty compare by extinction only.
            const Voxel va = unpackVoxel(a), vb = unpackVoxel(b);
            const float unit = 1.0f / scales[level];
            bool same = std::abs(byte(a, 0) - byte(b, 0)) <= 1 ||
                        std::abs(va.extinction - vb.extinction) <= 16.0f * unit;
            if (vb.extinction > 64.0f * unit) {
              const Vec3 albedo = palette[va.albedoIndex] - palette[vb.albedoIndex];
              same = same && std::abs(byte(a, 1) - byte(b, 1)) <= 2 &&
                     std::abs(byte(a, 2) - byte(b, 2)) <= 2 && dot(albedo, albedo) < 1e-4f;
            }
            if (!same) {
              report(level, "voxel");
            }
          }
        }
      }
    }
  }
  std::printf("far field check: %zu cells, %u voxels (%u occupied) compared "
              "with the CPU "
              "replica, %u mismatches\n",
              samples.size(), compared, occupied, mismatches);
  if (mismatches > 0) {
    throw std::runtime_error("GPU far field differs from the CPU replica");
  }
}

} // namespace

void Renderer::Impl::destroyFarField() {
  for (uint32_t level = 0; level < kFarLevels; ++level) {
    destroyBuffer(farColumns[level]);
    destroyBuffer(farVoxels[level]);
  }
  for (Buffer *buffer : {&farCells, &farPyramid, &farPalette, &farFieldInfo}) {
    destroyBuffer(*buffer);
  }
}

void Renderer::Impl::buildFarField(std::span<const Prototype> prototypes,
                                   std::span<const PlacementLayer> layers,
                                   const std::vector<uint32_t> &offsets) {
  destroyFarField();
  std::vector<uint8_t> far(prototypes.size(), 0);
  float scaleMin = std::numeric_limits<float>::max(), scaleMax = 0.0f;
  uint32_t pointsPerCell = 0;
  for (const PlacementLayer &layer : layers) {
    if (layer.farField) {
      for (const auto &[prototype, weight] : layer.prototypes) {
        far[prototype] = 1;
      }
      scaleMin = std::min(scaleMin, layer.scaleMin);
      scaleMax = std::max(scaleMax, layer.scaleMax);
      pointsPerCell += ditherPointCount(layer.footprint);
    }
  }
  if (pointsPerCell == 0) {
    return;
  }
  VkDevice dev = device.device;
  const auto loadStart = Clock::now();

  // ---------------------------------------------------------------- Bake

  // Each far prototype at its own x offset in one bake TLAS.
  struct Bake {
    uint32_t prototype;
    uint32_t firstMesh;
    uint32_t paletteBase;
    Vec3 origin;
    uint32_t size[3];
    float offset;
  };
  std::vector<Bake> bakes;
  uint32_t firstMesh = 0, paletteSize = 0;
  float cursor = 0.0f;
  for (uint32_t p = 0; p < prototypes.size(); ++p) {
    const uint32_t meshCount = static_cast<uint32_t>(prototypes[p].meshes.size());
    if (far[p]) {
      Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
      for (MeshId id : prototypes[p].meshes) {
        const GpuMesh &mesh = meshes[id.index];
        const Vec3 r{mesh.boundsRadius, mesh.boundsRadius, mesh.boundsRadius};
        const Vec3 a = mesh.boundsCenter - r, b = mesh.boundsCenter + r;
        lo = {std::min(lo.x, a.x), std::min(lo.y, a.y), std::min(lo.z, a.z)};
        hi = {std::max(hi.x, b.x), std::max(hi.y, b.y), std::max(hi.z, b.z)};
      }
      const Vec3 origin{std::floor(lo.x), std::floor(lo.y), std::floor(lo.z)};
      // The boxes (which hold the meshes' spheres) 16 m apart along x.
      Bake bake{p, firstMesh, paletteSize, origin, {}, cursor - origin.x};
      bake.size[0] = static_cast<uint32_t>(std::ceil(hi.x - origin.x));
      bake.size[1] = static_cast<uint32_t>(std::ceil(hi.y - origin.y));
      bake.size[2] = static_cast<uint32_t>(std::ceil(hi.z - origin.z));
      bakes.push_back(bake);
      paletteSize += std::min(meshCount, kFarBakeSlots);
      cursor += static_cast<float>(bake.size[0]) + 16.0f;
    }
    firstMesh += meshCount;
  }
  if (paletteSize > 256) {
    throw std::runtime_error("far field palette exceeds 256 entries");
  }

  std::vector<VkAccelerationStructureInstanceKHR> records;
  for (const Bake &bake : bakes) {
    VkAccelerationStructureInstanceKHR record{};
    record.transform.matrix[0][0] = record.transform.matrix[1][1] = record.transform.matrix[2][2] =
        1.0f;
    record.transform.matrix[0][3] = bake.offset;
    record.mask = 0xFF;
    record.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    record.accelerationStructureReference = prototypeBlas[bake.prototype].address;
    records.push_back(record);
  }
  Buffer recordBuffer = createBuffer(
      records.size() * sizeof(records[0]),
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
  std::memcpy(recordBuffer.mapped, records.data(), records.size() * sizeof(records[0]));
  check(vmaFlushAllocation(allocator, recordBuffer.allocation, 0, VK_WHOLE_SIZE),
        "vmaFlushAllocation");
  VkAccelerationStructureGeometryKHR geometry{};
  geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
  geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  geometry.geometry.instances.sType =
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  geometry.geometry.instances.data.deviceAddress = recordBuffer.address;
  VkAccelerationStructureBuildGeometryInfoKHR build{};
  build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.geometryCount = 1;
  build.pGeometries = &geometry;
  const auto recordCount = static_cast<uint32_t>(records.size());
  VkAccelerationStructureBuildSizesInfoKHR sizes{};
  sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
  dispatch.getAccelerationStructureBuildSizesKHR(VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                 &build, &recordCount, &sizes);
  AccelerationStructure bakeTlas = createAccelerationStructure(
      VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, sizes.accelerationStructureSize);
  Buffer scratch =
      createBuffer(sizes.buildScratchSize,
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                   0, scratchAlignment);
  build.dstAccelerationStructure = bakeTlas.handle;
  build.scratchData.deviceAddress = scratch.address;
  submitNow([&](VkCommandBuffer cmd) {
    const VkAccelerationStructureBuildRangeInfoKHR range{recordCount, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
    {
      DriverLeakScope driverLeak;
      dispatch.cmdBuildAccelerationStructuresKHR(cmd, 1, &build, &ranges);
    }
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                  VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
  });
  destroyBuffer(scratch);
  const VkDeviceSize bakeTlasBytes = bakeTlas.size + recordBuffer.size + sizes.buildScratchSize;

  const auto makeLayout = [&](uint32_t bytes, bool materials) {
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, bytes};
    const VkDescriptorSetLayout setLayouts[] = {atmosphereSetLayout, materialSetLayout};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = materials ? 2 : 0;
    info.pSetLayouts = setLayouts;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    check(vkCreatePipelineLayout(dev, &info, nullptr, &layout), "vkCreatePipelineLayout");
    return layout;
  };
  VkPipelineLayout bakeLayout = makeLayout(sizeof(FarBakeConstants), true);
  VkPipeline bakePipeline = createComputePipeline(dev, "farfield_bake.comp", bakeLayout);

  std::vector<FarTemplates> templates(prototypes.size());
  std::vector<std::array<double, 3>> colorSums(paletteSize, {0.0, 0.0, 0.0});
  std::vector<double> hitSums(paletteSize, 0.0);
  constexpr uint32_t kRays = 3 * kFarBakeRays * kFarBakeRays;
  double bakeMs = 0.0;
  for (const Bake &bake : bakes) {
    const uint32_t voxelCount = bake.size[0] * bake.size[1] * bake.size[2];
    Buffer output = createBuffer(
        VkDeviceSize{voxelCount} * sizeof(FarBakeVoxel),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    const uint32_t meshCount = static_cast<uint32_t>(prototypes[bake.prototype].meshes.size());
    FarBakeConstants constants;
    constants.tlas = bakeTlas.address;
    constants.vertices = vertexPool.address;
    constants.indices = indexPool.address;
    constants.meshes = meshTable.address;
    constants.materials = materialBuffer.address;
    constants.prototypeMeshes = prototypeMeshTable.address;
    constants.bake = output.address;
    constants.firstMesh = bake.firstMesh;
    constants.meshCount = meshCount;
    constants.origin[0] = bake.origin.x + bake.offset;
    constants.origin[1] = bake.origin.y;
    constants.origin[2] = bake.origin.z;
    constants.raysPerAxis = kFarBakeRays;
    std::copy(std::begin(bake.size), std::end(bake.size), constants.size);
    const auto start = Clock::now();
    submitNow([&](VkCommandBuffer cmd) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bakePipeline);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bakeLayout, 1, 1, &materialSet,
                              0, nullptr);
      vkCmdPushConstants(cmd, bakeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                         &constants);
      vkCmdDispatch(cmd, (voxelCount + 63) / 64, 1, 1);
      memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                    VK_ACCESS_2_HOST_READ_BIT);
    });
    const double ms = msSince(start);
    bakeMs += ms;
    check(vmaInvalidateAllocation(allocator, output.allocation, 0, VK_WHOLE_SIZE),
          "vmaInvalidateAllocation");
    const auto *voxels = static_cast<const FarBakeVoxel *>(output.mapped);

    // Extinction from the mean pass fraction over the voxel's 1 m; a
    // fully blocked voxel counts as half a ray passing.
    FarBakeGrid grid;
    grid.origin = bake.origin;
    std::copy(std::begin(bake.size), std::end(bake.size), grid.size);
    grid.extinction.resize(voxelCount);
    grid.normal.resize(voxelCount);
    grid.albedo.resize(voxelCount);
    const uint32_t slots = std::min(meshCount, kFarBakeSlots);
    uint32_t occupied = 0;
    for (uint32_t i = 0; i < voxelCount; ++i) {
      const FarBakeVoxel &v = voxels[i];
      grid.extinction[i] = farExtinctionFromPasses(v.passes, kRays);
      const Vec3 normal{v.normal[0], v.normal[1], v.normal[2]};
      grid.normal[i] = length(normal) > 0.0f ? normalize(normal) : Vec3{};
      uint32_t best = 0, bestHits = 0;
      for (uint32_t slot = 0; slot < slots; ++slot) {
        const uint32_t hits = (v.slotHits[slot / 4] >> (8 * (slot % 4))) & 0xFF;
        if (hits > bestHits) {
          best = slot;
          bestHits = hits;
        }
        hitSums[bake.paletteBase + slot] += hits;
        for (int c = 0; c < 3; ++c) {
          colorSums[bake.paletteBase + slot][c] += v.slotColor[slot][c];
        }
      }
      grid.albedo[i] = static_cast<uint8_t>(bake.paletteBase + best);
      occupied += grid.extinction[i] > 0.0f ? 1 : 0;
    }
    destroyBuffer(output);
    FarTemplates &t = templates[bake.prototype];
    t = filterFarTemplate(grid);
    std::printf("Far field bake: prototype %u, %ux%ux%u m in %.1f ms, %.1f%% "
                "of 1 m voxels "
                "occupied; %zu/%zu/%zu template voxels L0/L1/L2\n",
                bake.prototype, bake.size[0], bake.size[1], bake.size[2], ms,
                100.0 * occupied / voxelCount, t[0].voxels.size(), t[1].voxels.size(),
                t[2].voxels.size());
  }
  vkDestroyPipeline(dev, bakePipeline, nullptr);
  vkDestroyPipelineLayout(dev, bakeLayout, nullptr);
  destroyAccelerationStructure(bakeTlas);
  destroyBuffer(recordBuffer);

  std::vector<Vec3> palette(paletteSize, Vec3{0.5f, 0.5f, 0.5f});
  std::vector<float> paletteData;
  for (uint32_t e = 0; e < paletteSize; ++e) {
    if (hitSums[e] > 0.0) {
      palette[e] = {static_cast<float>(colorSums[e][0] / hitSums[e]),
                    static_cast<float>(colorSums[e][1] / hitSums[e]),
                    static_cast<float>(colorSums[e][2] / hitSums[e])};
    }
    paletteData.insert(paletteData.end(), {palette[e].x, palette[e].y, palette[e].z, 1.0f});
  }

  // ---------------------------------------------------------------- Splat

  std::vector<FarTemplateInfo> infos(prototypes.size() * kFarLevels);
  std::vector<FarTemplateVoxel> templateVoxels;
  std::array<float, kFarLevels> maxExtinction{};
  // How far a template's deposits reach horizontally from the instance's
  // origin (the trilinear splat adds a voxel, diagonally).
  float reach = 0.0f;
  for (uint32_t p = 0; p < prototypes.size(); ++p) {
    for (uint32_t level = 0; level < kFarLevels; ++level) {
      const FarTemplate &t = templates[p][level];
      FarTemplateInfo &info = infos[p * kFarLevels + level];
      info.centerMin = t.centerMin;
      info.centerMax = t.centerMax;
      info.firstVoxel = static_cast<uint32_t>(templateVoxels.size());
      info.voxelCount = static_cast<uint32_t>(t.voxels.size());
      templateVoxels.insert(templateVoxels.end(), t.voxels.begin(), t.voxels.end());
      for (const FarTemplateVoxel &v : t.voxels) {
        maxExtinction[level] = std::max(maxExtinction[level], v.extinction);
        const float horizontal = std::sqrt(v.center.x * v.center.x + v.center.z * v.center.z);
        reach = std::max(reach, horizontal * scaleMax + std::sqrt(2.0f) * kFarVoxelSize[level]);
      }
    }
  }
  if (templateVoxels.empty()) {
    std::printf("Far field: the bake found no geometry\n");
    return;
  }
  // Cells within `ring` of a voxel's cell hold every instance that can reach
  // it; the fixed-point scale assumes all of their trees in one voxel.
  const auto ring = static_cast<uint32_t>(std::ceil(reach / kCellSize));
  const uint32_t instancesPerVoxel = pointsPerCell * (2 * ring + 1) * (2 * ring + 1);
  std::array<float, kFarLevels> scales{};
  for (uint32_t level = 0; level < kFarLevels; ++level) {
    // 1.001: the half-float scale rounds up by at most 2^-11.
    scales[level] = farFixedScale(maxExtinction[level], scaleMin * 0.999f, scaleMax * 1.001f,
                                  instancesPerVoxel);
  }

  const PlacementGrid &pg = placementGrid;
  const FarFieldGrid grid{static_cast<float>(pg.firstX) * kCellSize,
                          static_cast<float>(pg.firstZ) * kCellSize,
                          heightfield.minHeight - kFarBaseMargin, pg.columns, pg.rows};
  const uint32_t cellCount = grid.cellsX * grid.cellsZ;

  Buffer templateBuffer = uploadStorage(infos.data(), infos.size() * sizeof(infos[0]));
  Buffer templateVoxelBuffer =
      uploadStorage(templateVoxels.data(), templateVoxels.size() * sizeof(templateVoxels[0]));
  farPalette = uploadStorage(paletteData.data(), paletteData.size() * sizeof(float));
  constexpr VkBufferUsageFlags kUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  // Per cell: the tallest tree (float bits); then the lost-deposit count.
  const VkDeviceSize cellDataBytes = (VkDeviceSize{cellCount} + 1) * sizeof(uint32_t);
  Buffer cellData = createBuffer(cellDataBytes, kUsage, 0);
  const VkDeviceSize maxOccupancyBytes =
      VkDeviceSize{cellCount} * farColumnsPerCell(0) * farColumnsPerCell(0) * 2 * sizeof(uint32_t);
  Buffer readback =
      createBuffer(maxOccupancyBytes + cellDataBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);

  VkPipelineLayout splatLayout = makeLayout(sizeof(FarSplatConstants), false);
  VkPipeline occupancyPipeline = createComputePipeline(dev, "farfield_occupancy.comp", splatLayout);
  VkPipeline accumulatePipeline =
      createComputePipeline(dev, "farfield_accumulate.comp", splatLayout);
  VkPipeline packPipeline = createComputePipeline(dev, "farfield_pack.comp", splatLayout);

  FarSplatConstants constants;
  constants.placed = placedInstances.address;
  constants.templates = templateBuffer.address;
  constants.templateVoxels = templateVoxelBuffer.address;
  constants.palette = farPalette.address;
  constants.cellData = cellData.address;
  constants.originX = grid.originX;
  constants.originZ = grid.originZ;
  constants.heightBase = grid.heightBase;
  constants.cellsX = grid.cellsX;
  constants.cellsZ = grid.cellsZ;
  constants.paletteSize = paletteSize;

  std::array<std::vector<BrickColumn>, kFarLevels> columns;
  std::array<uint32_t, kFarLevels> brickCounts{}, occupiedColumns{};
  std::array<double, kFarLevels> splatMs{};
  std::vector<float> cellBottom(cellCount, std::numeric_limits<float>::max());
  std::vector<float> cellTop(cellCount, -std::numeric_limits<float>::max());
  VkDeviceSize peakTransient = 0, peakAllocated = 0;
  const auto *mapped = static_cast<const uint32_t *>(readback.mapped);
  for (uint32_t level = 0; level < kFarLevels; ++level) {
    const auto start = Clock::now();
    const uint32_t n = farColumnsPerCell(level);
    const size_t columnCount = size_t{cellCount} * n * n;
    const VkDeviceSize occupancyBytes = columnCount * 2 * sizeof(uint32_t);
    Buffer occupancy = createBuffer(occupancyBytes, kUsage, 0);
    constants.columns = occupancy.address;
    constants.level = level;
    constants.count = placedCount;
    constants.fixedScale = scales[level];
    submitNow([&](VkCommandBuffer cmd) {
      vkCmdFillBuffer(cmd, occupancy.buffer, 0, VK_WHOLE_SIZE, 0xFFFFFFFFu);
      if (level == 0) {
        vkCmdFillBuffer(cmd, cellData.buffer, 0, VK_WHOLE_SIZE, 0);
      }
      memoryBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, occupancyPipeline);
      vkCmdPushConstants(cmd, splatLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                         &constants);
      const auto occupancyGroups = groups2d((uint64_t{placedCount} + 63) / 64);
      vkCmdDispatch(cmd, occupancyGroups[0], occupancyGroups[1], 1);
      memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
      const VkBufferCopy copy{0, 0, occupancyBytes};
      vkCmdCopyBuffer(cmd, occupancy.buffer, readback.buffer, 1, &copy);
      memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    });
    check(vmaInvalidateAllocation(allocator, readback.allocation, 0, VK_WHOLE_SIZE),
          "vmaInvalidateAllocation");
    std::vector<uint32_t> loHi(columnCount * 2);
    for (size_t c = 0; c < columnCount; ++c) {
      loHi[2 * c] = mapped[2 * c];
      loHi[2 * c + 1] = ~mapped[2 * c + 1];
      if (loHi[2 * c] != UINT32_MAX) {
        const size_t cell = c / (n * n);
        cellBottom[cell] =
            std::min(cellBottom[cell], grid.heightBase + loHi[2 * c] * kFarHeightUnit);
        cellTop[cell] = std::max(cellTop[cell], grid.heightBase + loHi[2 * c + 1] * kFarHeightUnit);
      }
    }
    uint32_t &bricks = brickCounts[level];
    columns[level] = farBrickColumns(loHi, level, bricks);
    for (const BrickColumn &column : columns[level]) {
      occupiedColumns[level] += columnBrickCount(column) > 0 ? 1 : 0;
    }
    const VkDeviceSize accumulatorBytes =
        VkDeviceSize{bricks} * kBrickVoxels * kFarChannels * sizeof(uint32_t);
    if (accumulatorBytes > maxBufferSize) {
      throw std::runtime_error("far field accumulators exceed the largest buffer");
    }
    farColumns[level] =
        uploadStorage(columns[level].data(), columns[level].size() * sizeof(BrickColumn));
    farVoxels[level] = createBuffer(
        std::max<VkDeviceSize>(VkDeviceSize{bricks} * kBrickVoxels * sizeof(uint32_t), 4), kUsage,
        0);
    Buffer accumulators = createBuffer(std::max<VkDeviceSize>(accumulatorBytes, 4), kUsage, 0);
    peakTransient =
        std::max(peakTransient, occupancyBytes + accumulators.size + templateBuffer.size +
                                    templateVoxelBuffer.size + cellData.size);
    VmaTotalStatistics stats;
    vmaCalculateStatistics(allocator, &stats);
    peakAllocated = std::max(peakAllocated, stats.total.statistics.blockBytes);
    constants.columns = farColumns[level].address;
    constants.accumulators = accumulators.address;
    constants.voxels = farVoxels[level].address;
    submitNow([&](VkCommandBuffer cmd) {
      vkCmdFillBuffer(cmd, accumulators.buffer, 0, VK_WHOLE_SIZE, 0);
      memoryBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, accumulatePipeline);
      vkCmdPushConstants(cmd, splatLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                         &constants);
      const auto instanceGroups = groups2d(placedCount);
      vkCmdDispatch(cmd, instanceGroups[0], instanceGroups[1], 1);
      memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
      FarSplatConstants pack = constants;
      pack.count = bricks * kBrickVoxels;
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, packPipeline);
      vkCmdPushConstants(cmd, splatLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pack), &pack);
      const auto brickGroups = groups2d(bricks); // one workgroup per brick
      vkCmdDispatch(cmd, brickGroups[0], brickGroups[1], 1);
      memoryBarrier(
          cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT |
              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
          VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    });
    destroyBuffer(accumulators);
    destroyBuffer(occupancy);
    splatMs[level] = msSince(start);
  }

  // Tallest trees and lost deposits.
  submitNow([&](VkCommandBuffer cmd) {
    const VkBufferCopy copy{0, 0, cellDataBytes};
    vkCmdCopyBuffer(cmd, cellData.buffer, readback.buffer, 1, &copy);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
  });
  check(vmaInvalidateAllocation(allocator, readback.allocation, 0, VK_WHOLE_SIZE),
        "vmaInvalidateAllocation");
  const uint32_t lostDeposits = mapped[cellCount];
  const auto layerCount = static_cast<uint32_t>(layers.size());
  std::vector<FarCell> farCellData(cellCount);
  uint32_t treeCells = 0;
  for (uint32_t c = 0; c < cellCount; ++c) {
    FarCell &cell = farCellData[c];
    cell.bottom = cellBottom[c];
    cell.top = cellTop[c];
    for (uint32_t l = 0; l < layerCount && layers[l].farField; ++l) {
      cell.treeCount += offsets[c * layerCount + l + 1] - offsets[c * layerCount + l];
    }
    std::memcpy(&cell.treeHeight, &mapped[c], sizeof(float));
    treeCells += cell.treeCount > 0 ? 1 : 0;
  }
  destroyBuffer(readback);
  destroyBuffer(cellData);
  vkDestroyPipeline(dev, occupancyPipeline, nullptr);
  vkDestroyPipeline(dev, accumulatePipeline, nullptr);
  vkDestroyPipeline(dev, packPipeline, nullptr);
  vkDestroyPipelineLayout(dev, splatLayout, nullptr);
  farCells = uploadStorage(farCellData.data(), farCellData.size() * sizeof(FarCell));

  // The canopy top pyramid: level k is the max over 2^k x 2^k cells.
  GpuFarField info;
  std::vector<float> pyramid;
  {
    std::vector<float> level = cellTop;
    uint32_t width = grid.cellsX, height = grid.cellsZ;
    while (width > 1 || height > 1) {
      const uint32_t w = (width + 1) / 2, h = (height + 1) / 2;
      std::vector<float> next(size_t{w} * h, -std::numeric_limits<float>::max());
      for (uint32_t z = 0; z < height; ++z) {
        for (uint32_t x = 0; x < width; ++x) {
          float &top = next[size_t{z / 2} * w + x / 2];
          top = std::max(top, level[size_t{z} * width + x]);
        }
      }
      info.pyramidOffsets[++info.pyramidLevels] = static_cast<uint32_t>(pyramid.size());
      pyramid.insert(pyramid.end(), next.begin(), next.end());
      level = std::move(next);
      width = w;
      height = h;
    }
    if (pyramid.empty()) {
      pyramid.push_back(cellTop[0]);
    }
  }
  farPyramid = uploadStorage(pyramid.data(), pyramid.size() * sizeof(float));
  for (uint32_t level = 0; level < kFarLevels; ++level) {
    info.columns[level] = farColumns[level].address;
    info.voxels[level] = farVoxels[level].address;
  }
  info.cells = farCells.address;
  info.pyramid = farPyramid.address;
  info.palette = farPalette.address;
  info.originX = grid.originX;
  info.originZ = grid.originZ;
  info.heightBase = grid.heightBase;
  info.cellsX = grid.cellsX;
  info.cellsZ = grid.cellsZ;
  info.paletteSize = paletteSize;
  farFieldInfo = uploadStorage(&info, sizeof(info));

  VkDeviceSize resident = farCells.size + farPyramid.size + farPalette.size + farFieldInfo.size;
  for (uint32_t level = 0; level < kFarLevels; ++level) {
    resident += farColumns[level].size + farVoxels[level].size;
  }
  peakTransient = std::max(peakTransient, bakeTlasBytes);
  const double splatTotal = splatMs[0] + splatMs[1] + splatMs[2];
  std::printf("Far field: bricks L0/L1/L2 %u/%u/%u in %u/%u/%u columns, %u cells with "
              "trees; resident %.0f MiB "
              "(voxels %.0f/%.0f/%.0f, columns %.0f/%.0f/%.0f); peak transient %.0f "
              "MiB, "
              "device memory allocated at peak %.0f MiB; bake %.0f ms, splat %.0f ms "
              "(L0/L1/L2 %.0f/%.0f/%.0f), total %.0f ms; %u lost deposits\n",
              brickCounts[0], brickCounts[1], brickCounts[2], occupiedColumns[0],
              occupiedColumns[1], occupiedColumns[2], treeCells, resident * kMiB,
              farVoxels[0].size * kMiB, farVoxels[1].size * kMiB, farVoxels[2].size * kMiB,
              farColumns[0].size * kMiB, farColumns[1].size * kMiB, farColumns[2].size * kMiB,
              peakTransient * kMiB, peakAllocated * kMiB, bakeMs, splatTotal, splatMs[0],
              splatMs[1], splatMs[2], msSince(loadStart), lostDeposits);

  destroyBuffer(templateBuffer);
  destroyBuffer(templateVoxelBuffer);

  if (kEnableValidation) {
    const VkDeviceSize instanceBytes = VkDeviceSize{placedCount} * sizeof(PlacedInstance);
    const VkDeviceSize cellBytes = VkDeviceSize{cellCount} * sizeof(Cell);
    VkDeviceSize total = instanceBytes + cellBytes;
    for (uint32_t level = 0; level < kFarLevels; ++level) {
      total += farVoxels[level].size;
    }
    Buffer check = createBuffer(total, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                    VMA_ALLOCATION_CREATE_MAPPED_BIT);
    submitNow([&](VkCommandBuffer cmd) {
      VkDeviceSize at = 0;
      const auto copy = [&](const Buffer &source, VkDeviceSize bytes) {
        const VkBufferCopy region{0, at, bytes};
        vkCmdCopyBuffer(cmd, source.buffer, check.buffer, 1, &region);
        at += bytes;
      };
      copy(placedInstances, instanceBytes);
      copy(cells, cellBytes);
      for (uint32_t level = 0; level < kFarLevels; ++level) {
        copy(farVoxels[level], farVoxels[level].size);
      }
      memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    });
    vmaInvalidateAllocation(allocator, check.allocation, 0, VK_WHOLE_SIZE);
    const auto *bytes = static_cast<const uint8_t *>(check.mapped);
    std::array<const uint32_t *, kFarLevels> voxels{};
    uint64_t hash = 0xcbf29ce484222325ull;
    VkDeviceSize at = instanceBytes + cellBytes;
    for (uint32_t level = 0; level < kFarLevels; ++level) {
      voxels[level] = reinterpret_cast<const uint32_t *>(bytes + at);
      hash = fnv1a(hash, columns[level].data(), columns[level].size() * sizeof(BrickColumn));
      hash = fnv1a(hash, bytes + at, farVoxels[level].size);
      at += farVoxels[level].size;
    }
    std::printf("far field hash 0x%016llx\n", static_cast<unsigned long long>(hash));
    // What the brick count holds: bricks with no occupied voxel are
    // footprint slack (the rotated hull's AABB plus the trilinear margin).
    std::printf("far field bricks (empty, occupied voxels per brick):");
    for (uint32_t level = 0; level < kFarLevels; ++level) {
      uint64_t empty = 0, occupied = 0;
      for (uint32_t brick = 0; brick < brickCounts[level]; ++brick) {
        uint32_t count = 0;
        for (uint32_t v = 0; v < kBrickVoxels; ++v) {
          count += voxels[level][size_t{brick} * kBrickVoxels + v] != 0 ? 1 : 0;
        }
        empty += count == 0 ? 1 : 0;
        occupied += count;
      }
      std::printf(" L%u %llu (%.1f%%), %.1f", level, static_cast<unsigned long long>(empty),
                  100.0 * static_cast<double>(empty) / std::max(brickCounts[level], 1u),
                  static_cast<double>(occupied) / std::max(brickCounts[level], 1u));
    }
    std::printf("\n");
    checkFarField(grid, ring, templates, palette, scales, columns, voxels,
                  reinterpret_cast<const PlacedInstance *>(bytes),
                  reinterpret_cast<const Cell *>(bytes + instanceBytes), farCellData);
    destroyBuffer(check);
  }
}

} // namespace meridian
