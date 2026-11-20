// Placement at load (setPlacement): placement.comp counts the kept points
// per (cell, layer), scan.comp turns the counts into offsets, and
// placement.comp writes the instances and the cell table. The CPU reads the
// total back between the two submits to size the table. Debug builds
// compare samples with the CPU reference (evaluatePoint) and print a hash of
// the tables.
#include "renderer_impl.h"

#include <meridian/placement.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace meridian {
namespace {

// The bounding sphere of a prototype's meshes.
std::array<float, 4> prototypeSphere(const Prototype &prototype,
                                     const std::vector<GpuMesh> &meshes) {
  if (prototype.meshes.empty()) {
    throw std::runtime_error("placement prototype without meshes");
  }
  Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
  for (MeshId id : prototype.meshes) {
    const GpuMesh &mesh = meshes.at(id.index);
    const Vec3 r{mesh.boundsRadius, mesh.boundsRadius, mesh.boundsRadius};
    const Vec3 a = mesh.boundsCenter - r, b = mesh.boundsCenter + r;
    lo = {std::min(lo.x, a.x), std::min(lo.y, a.y), std::min(lo.z, a.z)};
    hi = {std::max(hi.x, b.x), std::max(hi.y, b.y), std::max(hi.z, b.z)};
  }
  const Vec3 center = (lo + hi) * 0.5f;
  float radius = 0.0f;
  for (MeshId id : prototype.meshes) {
    const GpuMesh &mesh = meshes[id.index];
    radius = std::max(radius, length(mesh.boundsCenter - center) + mesh.boundsRadius);
  }
  return {center.x, center.y, center.z, radius};
}

// Debug: whole (cell, layer) runs of the GPU table against evaluatePoint
// until at least 4096 points are compared: keep decisions, prototypes and
// x/z exactly (order included), the float attributes within tolerance.
void checkPlacement(const Heightfield &field, std::span<const PlacementLayer> layers,
                    const std::vector<DitherPattern> &patterns, uint32_t seed,
                    const PlacementGrid &grid, const std::vector<uint32_t> &offsets,
                    const std::vector<std::array<float, 4>> &spheres,
                    const PlacedInstance *instances, const Cell *cells) {
  const auto layerCount = static_cast<uint32_t>(layers.size());
  const uint32_t cellCount = grid.columns * grid.rows;
  uint32_t evaluated = 0, kept = 0, mismatches = 0;
  // Point UINT32_MAX: the run as a whole; layer UINT32_MAX: the cell record.
  const auto report = [&](int32_t cellX, int32_t cellZ, uint32_t layer, uint32_t point) {
    if (mismatches++ == 0) {
      std::printf("placement check: first mismatch at cell (%d, %d), layer %d, point %d\n",
                  cellX, cellZ, static_cast<int>(layer), static_cast<int>(point));
    }
  };
  // The crop's corners and an edge cell (every layer), then random runs.
  const uint32_t last = grid.columns - 1, bottom = (grid.rows - 1) * grid.columns;
  const uint32_t fixedCells[] = {0, last, bottom, bottom + last, grid.columns / 2};
  const auto fixedRuns = static_cast<uint32_t>(std::size(fixedCells)) * layerCount;
  float worstHeight = 0.0f;
  for (uint32_t sample = 0; sample < fixedRuns || evaluated < 4096; ++sample) {
    const uint32_t cell = sample < fixedRuns ? fixedCells[sample / layerCount]
                                             : placementHash(sample, 0x636b, 0) % cellCount;
    const uint32_t l = sample < fixedRuns ? sample % layerCount
                                          : placementHash(sample, 0x636b, 1) % layerCount;
    const int32_t cellX = grid.firstX + static_cast<int32_t>(cell % grid.columns);
    const int32_t cellZ = grid.firstZ + static_cast<int32_t>(cell / grid.columns);
    uint32_t next = offsets[cell * layerCount + l];
    const uint32_t end = offsets[cell * layerCount + l + 1];
    for (uint32_t p = 0; p < patterns[l].points.size(); ++p, ++evaluated) {
      const auto point = evaluatePoint(field, layers[l], patterns[l], seed, cellX, cellZ, p);
      if (!point) {
        continue;
      }
      ++kept;
      if (next >= end) {
        report(cellX, cellZ, l, p);
        continue;
      }
      const PlacedInstance expected = encodePlacedInstance(*point);
      const PlacedInstance &gpu = instances[next++];
      worstHeight = std::max(worstHeight, std::abs(gpu.position[1] - expected.position[1]));
      bool same = gpu.prototype == expected.prototype &&
                  gpu.position[0] == expected.position[0] &&
                  gpu.position[2] == expected.position[2] &&
                  std::abs(gpu.position[1] - expected.position[1]) <= 1e-3f &&
                  std::abs(gpu.scaleHalf - expected.scaleHalf) <= 1;
      for (int i = 0; i < 3; ++i) {
        same = same && std::abs(gpu.rotation[i] - expected.rotation[i]) <= 2;
      }
      if (!same) {
        report(cellX, cellZ, l, p);
      }
    }
    if (end > next) { // the GPU kept points the CPU rejected
      report(cellX, cellZ, l, UINT32_MAX);
    }
    const Cell &c = cells[cell];
    bool cellOk = c.firstInstance == offsets[cell * layerCount] &&
                  c.instanceCount == offsets[(cell + 1) * layerCount] - c.firstInstance;
    const bool empty = c.instanceCount == 0;
    float minY = empty ? 0.0f : 1e30f, maxY = empty ? 0.0f : -1e30f;
    for (uint32_t i = c.firstInstance; cellOk && i < c.firstInstance + c.instanceCount; ++i) {
      const DecodedInstance d = decodePlacedInstance(instances[i]);
      const auto &[cx, cy, cz, radius] = spheres[d.prototype];
      const auto &[x, y, z, w] = d.rotation;
      // y of the sphere center rotated by the quaternion.
      const float rotatedY = 2.0f * (x * y + w * z) * cx + (1.0f - 2.0f * (x * x + z * z)) * cy +
                             2.0f * (y * z - w * x) * cz;
      minY = std::min(minY, d.position.y + d.scale * (rotatedY - radius));
      maxY = std::max(maxY, d.position.y + d.scale * (rotatedY + radius));
    }
    cellOk = cellOk && std::abs(c.minY - minY) <= 1e-2f && std::abs(c.maxY - maxY) <= 1e-2f;
    if (!cellOk) {
      report(cellX, cellZ, UINT32_MAX, UINT32_MAX);
    }
  }
  std::printf("placement check: %u points (%u kept) compared with the CPU reference, "
              "%u mismatches (height within %g m)\n",
              evaluated, kept, mismatches, worstHeight);
  if (mismatches > 0) {
    throw std::runtime_error("GPU placement differs from the CPU reference");
  }
}

} // namespace

void Renderer::setPlacement(std::span<const Prototype> prototypes,
                            std::span<const PlacementLayer> layers, uint32_t seed) {
  Impl &r = *impl_;
  const Heightfield &field = r.heightfield;
  if (field.samples.empty()) {
    throw std::runtime_error("setPlacement needs setTerrain first");
  }
  if (layers.empty()) {
    throw std::runtime_error("setPlacement needs a layer");
  }
  const auto start = std::chrono::steady_clock::now();
  const PlacementGrid grid = placementGrid(field);
  const auto layerCount = static_cast<uint32_t>(layers.size());
  const uint32_t cellCount = grid.columns * grid.rows;

  std::vector<GpuPlacementLayer> gpuLayers;
  std::vector<DitherPattern> patterns;
  std::vector<uint32_t> points; // (point, threshold) pairs
  std::vector<uint32_t> picks;  // (cumulative threshold, prototype) pairs
  for (const PlacementLayer &layer : layers) {
    const std::vector<uint32_t> thresholds = prototypeThresholds(layer);
    GpuPlacementLayer gpu;
    gpu.rule = placementRule(field, layer.rule);
    gpu.id = layer.id;
    gpu.firstPrototype = static_cast<uint32_t>(picks.size() / 2);
    for (size_t i = 0; i < thresholds.size(); ++i) {
      if (layer.prototypes[i].first >= prototypes.size()) {
        throw std::runtime_error("placement layer names a missing prototype");
      }
      picks.insert(picks.end(), {thresholds[i], layer.prototypes[i].first});
    }
    // Fixed per layer id: the pattern is part of a placed object's identity.
    patterns.push_back(makeDitherPattern(layer.id, ditherPointCount(layer.footprint)));
    gpu.firstPoint = static_cast<uint32_t>(points.size() / 2);
    gpu.pointCount = static_cast<uint32_t>(patterns.back().points.size());
    for (uint32_t p = 0; p < gpu.pointCount; ++p) {
      points.insert(points.end(), {patterns.back().points[p], patterns.back().thresholds[p]});
    }
    gpu.scaleMin = layer.scaleMin;
    gpu.scaleMax = layer.scaleMax;
    gpu.sinkDepth = layer.sinkDepth;
    gpu.fullRotation = layer.rotation == Rotation::Full ? 1 : 0;
    gpuLayers.push_back(gpu);
  }
  // Far-field layers first (FarCell::treeCount counts the start of each
  // cell's run), and their prototypes in no other layer.
  std::vector<uint8_t> farUse(prototypes.size(), 0);
  for (uint32_t l = 0; l < layerCount; ++l) {
    if (layers[l].farField && l > 0 && !layers[l - 1].farField) {
      throw std::runtime_error("far-field placement layers must come first");
    }
    for (const auto &[prototype, weight] : layers[l].prototypes) {
      farUse[prototype] |= layers[l].farField ? 1 : 2;
    }
  }
  if (std::find(farUse.begin(), farUse.end(), 3) != farUse.end()) {
    throw std::runtime_error("a far-field prototype is also in another placement layer");
  }
  std::vector<std::array<float, 4>> bounds;
  for (const Prototype &prototype : prototypes) {
    bounds.push_back(prototypeSphere(prototype, r.meshes));
    for (MeshId id : prototype.meshes) {
      // The cluster path draws opaque and masked meshes only.
      if (r.meshes[id.index].alphaMode == AlphaMode::Blend) {
        throw std::runtime_error("placement prototype with a blended mesh");
      }
    }
  }

  // The samples two per uint (the last one padded).
  std::vector<uint16_t> words(field.samples);
  words.resize((words.size() + 1) & ~size_t{1}, 0);
  Buffer heights = r.uploadStorage(words.data(), words.size() * sizeof(uint16_t));
  Buffer layerBuffer =
      r.uploadStorage(gpuLayers.data(), gpuLayers.size() * sizeof(GpuPlacementLayer));
  Buffer pointBuffer = r.uploadStorage(points.data(), points.size() * sizeof(uint32_t));
  Buffer pickBuffer = r.uploadStorage(picks.data(), picks.size() * sizeof(uint32_t));
  Buffer boundsBuffer = r.uploadStorage(bounds.data(), bounds.size() * sizeof(bounds[0]));
  // Per (cell, layer), plus the scan's total.
  const VkDeviceSize countBytes = (VkDeviceSize{cellCount} * layerCount + 1) * sizeof(uint32_t);
  Buffer counts = r.createBuffer(countBytes,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                 0);
  Buffer countReadback =
      r.createBuffer(countBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                         VMA_ALLOCATION_CREATE_MAPPED_BIT);

  VkDevice dev = r.device.device;
  const auto makeLayout = [&](uint32_t bytes) {
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, bytes};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    check(vkCreatePipelineLayout(dev, &info, nullptr, &layout), "vkCreatePipelineLayout");
    return layout;
  };
  VkPipelineLayout placementLayout = makeLayout(sizeof(PlacementConstants));
  VkPipelineLayout scanLayout = makeLayout(sizeof(ScanConstants));
  VkPipeline countPipeline = createComputePipeline(dev, "placement_count.comp", placementLayout);
  VkPipeline writePipeline = createComputePipeline(dev, "placement_write.comp", placementLayout);
  VkPipeline scanPipeline = createComputePipeline(dev, "scan.comp", scanLayout);

  const auto releaseTemporaries = [&] {
    vkDestroyPipeline(dev, countPipeline, nullptr);
    vkDestroyPipeline(dev, writePipeline, nullptr);
    vkDestroyPipeline(dev, scanPipeline, nullptr);
    vkDestroyPipelineLayout(dev, placementLayout, nullptr);
    vkDestroyPipelineLayout(dev, scanLayout, nullptr);
    for (Buffer *buffer :
         {&heights, &layerBuffer, &pointBuffer, &pickBuffer, &boundsBuffer, &counts,
          &countReadback}) {
      r.destroyBuffer(*buffer);
    }
  };

  check(vkDeviceWaitIdle(dev), "vkDeviceWaitIdle");

  PlacementConstants constants;
  constants.heights = heights.address;
  constants.layers = layerBuffer.address;
  constants.points = pointBuffer.address;
  constants.prototypes = pickBuffer.address;
  constants.bounds = boundsBuffer.address;
  constants.counts = counts.address;
  constants.field = placementField(field);
  constants.gridFirst[0] = grid.firstX;
  constants.gridFirst[1] = grid.firstZ;
  constants.gridColumns = grid.columns;
  constants.layerCount = layerCount;
  constants.seed = seed;
  constants.minHeight = field.minHeight;
  constants.heightScale = field.heightScale;
  constants.spacing = field.spacing;

  r.submitNow([&](VkCommandBuffer cmd) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, countPipeline);
    vkCmdPushConstants(cmd, placementLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                       &constants);
    vkCmdDispatch(cmd, grid.columns, grid.rows, 1);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    const ScanConstants scan{counts.address, cellCount * layerCount, 0};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, scanPipeline);
    vkCmdPushConstants(cmd, scanLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(scan), &scan);
    vkCmdDispatch(cmd, 1, 1, 1);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_TRANSFER_READ_BIT);
    const VkBufferCopy copy{0, 0, countBytes};
    vkCmdCopyBuffer(cmd, counts.buffer, countReadback.buffer, 1, &copy);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
  });
  check(vmaInvalidateAllocation(r.allocator, countReadback.allocation, 0, VK_WHOLE_SIZE),
        "vmaInvalidateAllocation");
  std::vector<uint32_t> offsets(cellCount * layerCount + 1);
  std::memcpy(offsets.data(), countReadback.mapped, countBytes);
  const uint32_t total = offsets.back();
  if (total > kMaxInstanceId - kPlacedBase + 1) {
    releaseTemporaries();
    throw std::runtime_error("placement exceeds the 24-bit instance id space");
  }

  // Only now: a throw above keeps the earlier placement and its TLAS valid.
  r.destroyBuffer(r.placedInstances);
  r.destroyBuffer(r.cells);
  r.destroyBuffer(r.prototypeTable);
  r.destroyBuffer(r.prototypeMeshTable);
  for (AccelerationStructure &blas : r.prototypeBlas) {
    r.destroyAccelerationStructure(blas);
  }
  r.prototypeBlas.clear();

  constexpr VkBufferUsageFlags kTableUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  r.placedInstances = r.createBuffer(
      std::max<VkDeviceSize>(total, 1) * sizeof(PlacedInstance), kTableUsage, 0);
  r.cells = r.createBuffer(VkDeviceSize{cellCount} * sizeof(Cell), kTableUsage, 0);
  constants.instances = r.placedInstances.address;
  constants.cells = r.cells.address;
  r.submitNow([&](VkCommandBuffer cmd) {
    // The fence made the scan's offsets available; this makes them visible.
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, writePipeline);
    vkCmdPushConstants(cmd, placementLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                       &constants);
    vkCmdDispatch(cmd, grid.columns, grid.rows, 1);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT |
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
  });
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
          .count();

  releaseTemporaries();

  r.placementGrid = grid;
  r.placedCount = total;
  std::printf("Placed %u instances in %.0f ms (layer id: count", total, ms);
  for (uint32_t l = 0; l < layerCount; ++l) {
    uint32_t count = 0;
    for (uint32_t c = 0; c < cellCount; ++c) {
      count += offsets[c * layerCount + l + 1] - offsets[c * layerCount + l];
    }
    std::printf("%s%u: %u", l == 0 ? "; " : ", ", layers[l].id, count);
  }
  std::printf(")\n");

  r.buildPlacedTlas(prototypes, layers, bounds);

  if (kEnableValidation) {
    const VkDeviceSize instanceBytes = VkDeviceSize{total} * sizeof(PlacedInstance);
    const VkDeviceSize cellBytes = VkDeviceSize{cellCount} * sizeof(Cell);
    Buffer readback = r.createBuffer(instanceBytes + cellBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                         VMA_ALLOCATION_CREATE_MAPPED_BIT);
    r.submitNow([&](VkCommandBuffer cmd) {
      if (total > 0) {
        const VkBufferCopy instanceCopy{0, 0, instanceBytes};
        vkCmdCopyBuffer(cmd, r.placedInstances.buffer, readback.buffer, 1, &instanceCopy);
      }
      const VkBufferCopy cellCopy{0, instanceBytes, cellBytes};
      vkCmdCopyBuffer(cmd, r.cells.buffer, readback.buffer, 1, &cellCopy);
      memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    });
    check(vmaInvalidateAllocation(r.allocator, readback.allocation, 0, VK_WHOLE_SIZE),
          "vmaInvalidateAllocation");
    const auto *bytes = static_cast<const uint8_t *>(readback.mapped);
    std::printf("placement hash 0x%016llx\n",
                static_cast<unsigned long long>(
                    fnv1a(0xcbf29ce484222325ull, bytes, instanceBytes + cellBytes)));
    checkPlacement(field, layers, patterns, seed, grid, offsets, bounds,
                   reinterpret_cast<const PlacedInstance *>(bytes),
                   reinterpret_cast<const Cell *>(bytes + instanceBytes));
    r.destroyBuffer(readback);
  }

  r.buildFarField(prototypes, layers, offsets);
}

// Each prototype's BLAS and PrototypeInfo, the cells' reach and draw
// distance, and the static TLAS over the placed and hand-placed instances:
// tlas_fill.comp writes the placed records once, then one build. Prints
// what M2b's ray representations are measured against.
void Renderer::Impl::buildPlacedTlas(std::span<const Prototype> prototypes,
                                     std::span<const PlacementLayer> layers,
                                     const std::vector<std::array<float, 4>> &bounds) {
  Impl &r = *this;
  using Clock = std::chrono::steady_clock;
  const auto msSince = [](Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  };
  const auto blasStart = Clock::now();
  std::vector<PrototypeInfo> infos;
  std::vector<uint32_t> meshList;
  VkDeviceSize builtBytes = 0, blasBytes = 0;
  for (uint32_t p = 0; p < prototypes.size(); ++p) {
    PrototypeInfo info;
    info.boundsCenter = {bounds[p][0], bounds[p][1], bounds[p][2]};
    info.boundsRadius = bounds[p][3];
    info.firstMesh = static_cast<uint32_t>(meshList.size());
    info.meshCount = static_cast<uint32_t>(prototypes[p].meshes.size());
    for (MeshId id : prototypes[p].meshes) {
      meshList.push_back(id.index);
    }
    VkDeviceSize built = 0;
    r.prototypeBlas.push_back(r.buildBlas(
        std::span(meshList).subspan(info.firstMesh, info.meshCount), &built));
    builtBytes += built;
    blasBytes += r.prototypeBlas.back().size;
    info.blasAddress = r.prototypeBlas.back().address;
    // The largest draw distance of its layers; 0 if any has none.
    bool unlimited = false;
    for (const PlacementLayer &layer : layers) {
      for (const auto &[prototype, weight] : layer.prototypes) {
        if (prototype == p) {
          unlimited = unlimited || layer.maxDrawDistance <= 0.0f;
          info.maxDrawDistance = std::max(info.maxDrawDistance, layer.maxDrawDistance);
        }
      }
    }
    if (unlimited) {
      info.maxDrawDistance = 0.0f;
    }
    infos.push_back(info);
  }
  const double blasMs = msSince(blasStart);
  r.prototypeTable = r.uploadStorage(infos.data(), infos.size() * sizeof(PrototypeInfo));
  r.prototypeMeshTable = r.uploadStorage(meshList.data(), meshList.size() * sizeof(uint32_t));

  // An instance's bounds reach past its cell by its sphere's horizontal
  // extent from the origin (any direction under full rotation).
  float margin = 0.0f, drawDistance = 0.0f;
  bool unlimited = false;
  for (const PlacementLayer &layer : layers) {
    unlimited = unlimited || layer.maxDrawDistance <= 0.0f;
    drawDistance = std::max(drawDistance, layer.maxDrawDistance);
    for (const auto &[prototype, weight] : layer.prototypes) {
      const auto &[x, y, z, radius] = bounds[prototype];
      const float extent = layer.rotation == Rotation::Full ? std::sqrt(x * x + y * y + z * z)
                                                            : std::sqrt(x * x + z * z);
      // 1.001: the half-float scale rounds up by at most 2^-11.
      margin = std::max(margin, (extent + radius) * layer.scaleMax * 1.001f);
    }
  }
  r.cellMargin = margin;
  r.cellDrawDistance = unlimited ? 0.0f : drawDistance;

  r.createVisibility();
  r.createTlas(r.placedCount + kMaxInstances);
  VkDevice dev = r.device.device;
  const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TlasFillConstants)};
  VkPipelineLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges = &range;
  VkPipelineLayout fillLayout = VK_NULL_HANDLE;
  check(vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &fillLayout), "vkCreatePipelineLayout");
  VkPipeline fillPipeline = createComputePipeline(dev, "tlas_fill.comp", fillLayout);
  const TlasFillConstants fill{r.placedInstances.address, r.prototypeTable.address,
                               r.tlasRecords.address, r.placedCount, 0};
  r.submitNow([&](VkCommandBuffer cmd) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fillPipeline);
    vkCmdPushConstants(cmd, fillLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(fill), &fill);
    vkCmdDispatch(cmd, (r.placedCount + 63) / 64, 1, 1);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                  VK_ACCESS_2_SHADER_READ_BIT);
  });
  vkDestroyPipeline(dev, fillPipeline, nullptr);
  vkDestroyPipelineLayout(dev, fillLayout, nullptr);

  const auto buildStart = Clock::now();
  r.submitNow([&](VkCommandBuffer cmd) {
    r.uploadInstances(cmd, r.frames[0]);
    r.buildTlas(cmd, r.frames[0]);
  });
  const double buildMs = msSince(buildStart);
  r.instancesDirty = false;

  VmaTotalStatistics stats;
  vmaCalculateStatistics(r.allocator, &stats);
  VmaAllocationInfo scratch;
  vmaGetAllocationInfo(r.allocator, r.tlasScratch.allocation, &scratch);
  constexpr double kMiB = 1.0 / (1 << 20);
  const uint32_t records = r.placedCount + static_cast<uint32_t>(r.instances.size());
  std::printf("TLAS: %u records (%.0f MiB of records), TLAS %.0f MiB, scratch %.0f MiB, "
              "build %.1f ms; %zu prototype BLAS %.0f MiB (%.0f MiB before compaction, "
              "%.0f ms); cell margin %.1f m; device memory allocated %.0f MiB\n",
              records, VkDeviceSize{records} * sizeof(VkAccelerationStructureInstanceKHR) * kMiB,
              r.tlas.size * kMiB, scratch.size * kMiB, buildMs,
              r.prototypeBlas.size(), blasBytes * kMiB, builtBytes * kMiB, blasMs, margin,
              stats.total.statistics.blockBytes * kMiB);
}

uint32_t Renderer::placedInstanceCount() const { return impl_->placedCount; }

} // namespace meridian
