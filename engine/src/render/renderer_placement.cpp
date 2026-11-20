// Placement at load (setPlacement): placement.comp counts the kept points
// per (cell, layer), scan.comp turns the counts into offsets, and
// placement.comp writes the instances and the cell table. The CPU reads the
// total back between the two submits to size the table. Debug builds
// compare samples with the CPU reference (evaluatePoint) and print a hash of
// the tables. Layers with streamFullDensity > 0 are not placed here: their
// inputs stay on the GPU and placement.comp's STREAM variant places the
// cells around the camera into a ring after the static instances, per frame
// (planStreamUpdates, recordStreamUpdate).
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

// Mismatch counters of the Debug checks; label names the check in its
// messages.
struct RunStats {
  const char *label;
  uint32_t evaluated = 0, kept = 0, mismatches = 0;
  float worstHeight = 0.0f;
  // Point UINT32_MAX: the run as a whole; layer UINT32_MAX: the cell record.
  void report(int32_t cellX, int32_t cellZ, uint32_t layer, uint32_t point) {
    if (mismatches++ == 0) {
      std::printf("%s: first mismatch at cell (%d, %d), layer %d, point %d\n", label, cellX,
                  cellZ, static_cast<int>(layer), static_cast<int>(point));
    }
  }
};

// One (cell, layer) run of GPU instances against evaluatePoint: keep
// decisions, prototypes and x/z exactly (order included), the float
// attributes within tolerance. tags (optional, streamed runs): the dither
// thresholds, which must equal the pattern's with the layer index above.
void compareRun(const Heightfield &field, const PlacementLayer &layer,
                const DitherPattern &pattern, uint32_t seed, int32_t cellX, int32_t cellZ,
                uint32_t layerIndex, const PlacedInstance *run, uint32_t count,
                const uint32_t *tags, RunStats &stats) {
  uint32_t next = 0;
  for (uint32_t p = 0; p < pattern.points.size(); ++p, ++stats.evaluated) {
    const auto point = evaluatePoint(field, layer, pattern, seed, cellX, cellZ, p);
    if (!point) {
      continue;
    }
    ++stats.kept;
    if (next >= count) {
      stats.report(cellX, cellZ, layerIndex, p);
      continue;
    }
    const PlacedInstance expected = encodePlacedInstance(*point);
    const PlacedInstance &gpu = run[next];
    stats.worstHeight =
        std::max(stats.worstHeight, std::abs(gpu.position[1] - expected.position[1]));
    bool same = gpu.prototype == expected.prototype && gpu.position[0] == expected.position[0] &&
                gpu.position[2] == expected.position[2] &&
                std::abs(gpu.position[1] - expected.position[1]) <= 1e-3f &&
                std::abs(gpu.scaleHalf - expected.scaleHalf) <= 1;
    for (int i = 0; i < 3; ++i) {
      same = same && std::abs(gpu.rotation[i] - expected.rotation[i]) <= 2;
    }
    if (tags) {
      same = same && tags[next] == (pattern.thresholds[p] | layerIndex << 16);
    }
    ++next;
    if (!same) {
      stats.report(cellX, cellZ, layerIndex, p);
    }
  }
  if (count > next) { // the GPU kept points the CPU rejected
    stats.report(cellX, cellZ, layerIndex, UINT32_MAX);
  }
}

// The height range of instances' bounding spheres, as placement.comp
// computes it: (0, 0) for none.
std::pair<float, float> instanceHeights(const PlacedInstance *instances, uint32_t count,
                                        const std::vector<std::array<float, 4>> &spheres,
                                        bool streamed = false) {
  float minY = count == 0 ? 0.0f : 1e30f, maxY = count == 0 ? 0.0f : -1e30f;
  for (uint32_t i = 0; i < count; ++i) {
    const DecodedInstance d = decodePlacedInstance(instances[i]);
    const auto &[cx, cy, cz, radius] = spheres[d.prototype];
    const auto &[x, y, z, w] = d.rotation;
    // y of the sphere center rotated by the quaternion.
    const float rotatedY = 2.0f * (x * y + w * z) * cx + (1.0f - 2.0f * (x * x + z * z)) * cy +
                           2.0f * (y * z - w * x) * cz;
    minY = std::min(minY, d.position.y + d.scale * (rotatedY - radius));
    maxY = std::max(maxY, d.position.y + d.scale * (rotatedY + radius));
    if (streamed) {
      minY = std::min(minY, d.position.y);
      maxY = std::max(maxY, d.position.y);
    }
  }
  return {minY, maxY};
}

// Debug: whole (cell, layer) runs of the GPU table against evaluatePoint
// until at least 4096 points are compared.
void checkPlacement(const Heightfield &field, std::span<const PlacementLayer> layers,
                    const std::vector<DitherPattern> &patterns, uint32_t seed,
                    const PlacementGrid &grid, const std::vector<uint32_t> &offsets,
                    const std::vector<std::array<float, 4>> &spheres,
                    const PlacedInstance *instances, const Cell *cells) {
  const auto layerCount = static_cast<uint32_t>(layers.size());
  const uint32_t cellCount = grid.columns * grid.rows;
  RunStats stats{"placement check"};
  // The crop's corners and an edge cell (every layer), then random runs.
  const uint32_t last = grid.columns - 1, bottom = (grid.rows - 1) * grid.columns;
  const uint32_t fixedCells[] = {0, last, bottom, bottom + last, grid.columns / 2};
  const auto fixedRuns = static_cast<uint32_t>(std::size(fixedCells)) * layerCount;
  for (uint32_t sample = 0; sample < fixedRuns || stats.evaluated < 4096; ++sample) {
    const uint32_t cell = sample < fixedRuns ? fixedCells[sample / layerCount]
                                             : placementHash(sample, 0x636b, 0) % cellCount;
    const uint32_t l = sample < fixedRuns ? sample % layerCount
                                          : placementHash(sample, 0x636b, 1) % layerCount;
    const int32_t cellX = grid.firstX + static_cast<int32_t>(cell % grid.columns);
    const int32_t cellZ = grid.firstZ + static_cast<int32_t>(cell / grid.columns);
    const uint32_t first = offsets[cell * layerCount + l];
    compareRun(field, layers[l], patterns[l], seed, cellX, cellZ, l, instances + first,
               offsets[cell * layerCount + l + 1] - first, nullptr, stats);
    const Cell &c = cells[cell];
    bool cellOk = c.firstInstance == offsets[cell * layerCount] &&
                  c.instanceCount == offsets[(cell + 1) * layerCount] - c.firstInstance;
    if (cellOk) {
      const auto [minY, maxY] = instanceHeights(instances + c.firstInstance, c.instanceCount,
                                                spheres);
      cellOk = std::abs(c.minY - minY) <= 1e-2f && std::abs(c.maxY - maxY) <= 1e-2f;
    }
    if (!cellOk) {
      stats.report(cellX, cellZ, UINT32_MAX, UINT32_MAX);
    }
  }
  std::printf("placement check: %u points (%u kept) compared with the CPU reference, "
              "%u mismatches (height within %g m)\n",
              stats.evaluated, stats.kept, stats.mismatches, stats.worstHeight);
  if (stats.mismatches > 0) {
    throw std::runtime_error("GPU placement differs from the CPU reference");
  }
}

// How far a layer's instances' bounds reach past their cell horizontally:
// their sphere's extent from the origin (any direction under full rotation).
float layerMargin(const PlacementLayer &layer, const std::vector<std::array<float, 4>> &bounds) {
  float margin = 0.0f;
  for (const auto &[prototype, weight] : layer.prototypes) {
    const auto &[x, y, z, radius] = bounds[prototype];
    const float extent = layer.rotation == Rotation::Full ? std::sqrt(x * x + y * y + z * z)
                                                          : std::sqrt(x * x + z * z);
    // 1.001: the half-float scale rounds up by at most 2^-11.
    margin = std::max(margin, (extent + radius) * layer.scaleMax * 1.001f);
  }
  return margin;
}

// The GPU inputs of a set of layers: their rules, patterns, points and
// prototype picks (placement.comp).
struct PlacementTables {
  std::vector<GpuPlacementLayer> layers;
  std::vector<DitherPattern> patterns;
  std::vector<uint32_t> points; // (point, threshold) pairs
  std::vector<uint32_t> picks;  // (cumulative threshold, prototype) pairs
};

PlacementTables buildTables(const Heightfield &field, std::span<const PlacementLayer> layers,
                            size_t prototypeCount, const std::filesystem::path &patternCache) {
  PlacementTables t;
  for (const PlacementLayer &layer : layers) {
    const std::vector<uint32_t> thresholds = prototypeThresholds(layer);
    GpuPlacementLayer gpu;
    gpu.rule = placementRule(field, layer.rule);
    gpu.id = layer.id;
    gpu.firstPrototype = static_cast<uint32_t>(t.picks.size() / 2);
    for (size_t i = 0; i < thresholds.size(); ++i) {
      if (layer.prototypes[i].first >= prototypeCount) {
        throw std::runtime_error("placement layer names a missing prototype");
      }
      t.picks.insert(t.picks.end(), {thresholds[i], layer.prototypes[i].first});
    }
    // Fixed per layer id: the pattern is part of a placed object's identity.
    t.patterns.push_back(cachedDitherPattern(layer.id, ditherPointCount(layer.footprint), patternCache));
    gpu.firstPoint = static_cast<uint32_t>(t.points.size() / 2);
    gpu.pointCount = static_cast<uint32_t>(t.patterns.back().points.size());
    for (uint32_t p = 0; p < gpu.pointCount; ++p) {
      t.points.insert(t.points.end(),
                      {t.patterns.back().points[p], t.patterns.back().thresholds[p]});
    }
    gpu.scaleMin = layer.scaleMin;
    gpu.scaleMax = layer.scaleMax;
    gpu.sinkDepth = layer.sinkDepth;
    gpu.fullRotation = layer.rotation == Rotation::Full ? 1 : 0;
    t.layers.push_back(gpu);
  }
  return t;
}

} // namespace

void Renderer::setPlacement(std::span<const Prototype> prototypes,
                            std::span<const PlacementLayer> allLayers, uint32_t seed,
                          const std::filesystem::path &patternCache) {
  Impl &r = *impl_;
  const Heightfield &field = r.heightfield;
  if (field.samples.empty()) {
    throw std::runtime_error("setPlacement needs setTerrain first");
  }
  if (allLayers.empty()) {
    throw std::runtime_error("setPlacement needs a layer");
  }
  const auto start = std::chrono::steady_clock::now();
  // Static layers are placed over the whole crop now, streamed ones around
  // the camera every frame.
  std::vector<PlacementLayer> staticList, streamList;
  for (const PlacementLayer &layer : allLayers) {
    if (layer.streamFullDensity > 0.0f && layer.maxDrawDistance <= 0.0f) {
      throw std::runtime_error("a streamed placement layer needs a maxDrawDistance");
    }
    (layer.streamFullDensity > 0.0f ? streamList : staticList).push_back(layer);
  }
  if (staticList.empty()) {
    throw std::runtime_error("setPlacement needs a static layer");
  }
  const std::span<const PlacementLayer> layers = staticList;
  const PlacementGrid grid = placementGrid(field);
  const auto layerCount = static_cast<uint32_t>(layers.size());
  const uint32_t cellCount = grid.columns * grid.rows;

  PlacementTables tables = buildTables(field, layers, prototypes.size(), patternCache);
  const std::vector<DitherPattern> &patterns = tables.patterns;
  // Far-field layers first (FarCell::treeCount counts the start of each
  // cell's run), and their prototypes in no other layer.
  std::vector<uint8_t> farUse(prototypes.size(), 0);
  for (uint32_t l = 0; l < layerCount; ++l) {
    if (layers[l].farField && l > 0 && !layers[l - 1].farField) {
      throw std::runtime_error("far-field placement layers must come first");
    }
  }
  for (const PlacementLayer &layer : allLayers) {
    for (const auto &[prototype, weight] : layer.prototypes) {
      farUse[prototype] |= layer.farField ? 1 : 2;
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
      r.uploadStorage(tables.layers.data(), tables.layers.size() * sizeof(GpuPlacementLayer));
  Buffer pointBuffer =
      r.uploadStorage(tables.points.data(), tables.points.size() * sizeof(uint32_t));
  Buffer pickBuffer =
      r.uploadStorage(tables.picks.data(), tables.picks.size() * sizeof(uint32_t));
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

  // Streamed layers: the same inputs for the per-frame updates, in their own
  // tables (point and prototype offsets are per table). The window covers
  // the largest draw distance plus the bounds' reach past a cell.
  Impl::Stream next;
  if (!streamList.empty()) {
    PlacementTables streamTables = buildTables(field, streamList, prototypes.size(), patternCache);
    float radius = 0.0f;
    std::vector<float> layerTable;
    for (const PlacementLayer &layer : streamList) {
      radius = std::max(radius, layer.maxDrawDistance + layerMargin(layer, bounds));
      layerTable.insert(layerTable.end(), {layer.streamFullDensity, layer.maxDrawDistance});
    }
    next.layerCount = static_cast<uint32_t>(streamList.size());
    next.window = streamWindowCells(radius);
    for (const GpuPlacementLayer &layer : streamTables.layers) {
      next.runStride += layer.pointCount;
    }
    const uint32_t slots = next.window * next.window;
    next.ringCount = slots * next.runStride;
    next.recordCount = slots * next.layerCount;
    next.gridCells = cellCount;
    next.seed = seed;
    next.layers = r.uploadStorage(streamTables.layers.data(),
                                  streamTables.layers.size() * sizeof(GpuPlacementLayer));
    next.points = r.uploadStorage(streamTables.points.data(),
                                  streamTables.points.size() * sizeof(uint32_t));
    next.picks = r.uploadStorage(streamTables.picks.data(),
                                 streamTables.picks.size() * sizeof(uint32_t));
    next.bounds = r.uploadStorage(bounds.data(), bounds.size() * sizeof(bounds[0]));
    next.layerTable = r.uploadStorage(layerTable.data(), layerTable.size() * sizeof(float));
    constexpr VkBufferUsageFlags kStreamUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    next.thresholds = r.createBuffer(VkDeviceSize{next.ringCount} * sizeof(uint32_t),
                                     kStreamUsage, 0);
    next.coords = r.createBuffer(VkDeviceSize{next.recordCount} * sizeof(int32_t) * 2,
                                 kStreamUsage, 0);
    next.layout = makeLayout(sizeof(StreamConstants));
    next.pipeline = createComputePipeline(dev, "placement_stream.comp", next.layout);
    next.held.assign(slots, {INT32_MIN, INT32_MIN});
    next.layerList = streamList;
    next.patterns = std::move(streamTables.patterns);
    next.spheres = bounds;
  }

  const auto releaseTemporaries = [&](bool keepHeights) {
    vkDestroyPipeline(dev, countPipeline, nullptr);
    vkDestroyPipeline(dev, writePipeline, nullptr);
    vkDestroyPipeline(dev, scanPipeline, nullptr);
    vkDestroyPipelineLayout(dev, placementLayout, nullptr);
    vkDestroyPipelineLayout(dev, scanLayout, nullptr);
    for (Buffer *buffer :
         {&layerBuffer, &pointBuffer, &pickBuffer, &boundsBuffer, &counts, &countReadback}) {
      r.destroyBuffer(*buffer);
    }
    if (!keepHeights) {
      r.destroyBuffer(heights);
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
  if (uint64_t{total} + next.ringCount > kMaxInstanceId - kPlacedBase + 1) {
    releaseTemporaries(false);
    r.destroyStreamBuffers(next);
    throw std::runtime_error("placement exceeds the 24-bit instance id space");
  }

  // Only now: a throw above keeps the earlier placement and its TLAS valid.
  r.destroyStream();
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
  // Static instances, then the streamed ring; the grid's cells, then the
  // streamed records.
  r.placedInstances = r.createBuffer(
      std::max<VkDeviceSize>(total + next.ringCount, 1) * sizeof(PlacedInstance), kTableUsage, 0);
  r.cells = r.createBuffer((VkDeviceSize{cellCount} + next.recordCount) * sizeof(Cell),
                           kTableUsage, 0);
  if (next.active()) {
    next.heights = heights; // shared with the static placement above
    r.stream = std::move(next);
    for (FrameData &frame : r.frames) {
      frame.streamUpdates = r.createBuffer(
          VkDeviceSize{r.stream.window} * r.stream.window * sizeof(StreamUpdate),
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
              VMA_ALLOCATION_CREATE_MAPPED_BIT);
    }
  }
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

  releaseTemporaries(r.stream.active());

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

  if (r.stream.active()) {
    std::printf("Streamed %u layers: window %ux%u cells, ring %u instances (%u per slot)\n",
                r.stream.layerCount, r.stream.window, r.stream.window, r.stream.ringCount,
                r.stream.runStride);
  }

  r.buildPlacedTlas(prototypes, allLayers, bounds);

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
    // The largest draw distance of its layers; 0 if any has none. A
    // far-field layer's prototype is in no other layer (setPlacement).
    bool unlimited = false;
    info.tlasMask = kShadowCasterMask;
    for (const PlacementLayer &layer : layers) {
      for (const auto &[prototype, weight] : layer.prototypes) {
        if (prototype == p) {
          info.tlasMask = layer.farField ? kFarFieldCasterMask : kShadowCasterMask;
          unlimited = unlimited || layer.maxDrawDistance <= 0.0f;
          info.maxDrawDistance = std::max(info.maxDrawDistance, layer.maxDrawDistance);
        }
      }
    }
    if (unlimited) {
      info.maxDrawDistance = 0.0f;
    }
    const Vec3 &tint = prototypes[p].tint;
    for (int c = 0; c < 3; ++c) {
      const float t = std::clamp((&tint.x)[c] * 0.5f, 0.0f, 1.0f);
      info.tint |= static_cast<uint32_t>(std::lround(t * 255.0f)) << (8 * c);
    }
    infos.push_back(info);
  }
  const double blasMs = msSince(blasStart);
  r.prototypeTable = r.uploadStorage(infos.data(), infos.size() * sizeof(PrototypeInfo));
  r.prototypeMeshTable = r.uploadStorage(meshList.data(), meshList.size() * sizeof(uint32_t));

  float margin = 0.0f, drawDistance = 0.0f;
  bool unlimited = false;
  for (const PlacementLayer &layer : layers) {
    unlimited = unlimited || layer.maxDrawDistance <= 0.0f;
    drawDistance = std::max(drawDistance, layer.maxDrawDistance);
    margin = std::max(margin, layerMargin(layer, bounds));
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

void Renderer::Impl::destroyStreamBuffers(Stream &s) {
  for (Buffer *buffer : {&s.heights, &s.layers, &s.points, &s.picks, &s.bounds, &s.layerTable,
                         &s.thresholds, &s.coords}) {
    destroyBuffer(*buffer);
  }
  vkDestroyPipeline(device.device, s.pipeline, nullptr);
  vkDestroyPipelineLayout(device.device, s.layout, nullptr);
  s.pipeline = VK_NULL_HANDLE;
  s.layout = VK_NULL_HANDLE;
}

void Renderer::Impl::destroyStream() {
  destroyStreamBuffers(stream);
  stream = {};
  for (FrameData &frame : frames) {
    destroyBuffer(frame.streamUpdates);
  }
}

uint32_t Renderer::Impl::planStreamUpdates(const Vec3 &camera, FrameData &frame) {
  Stream &s = stream;
  if (!s.active()) {
    return 0;
  }
  const auto cameraCellX = static_cast<int32_t>(std::floor(camera.x / kCellSize));
  const auto cameraCellZ = static_cast<int32_t>(std::floor(camera.z / kCellSize));
  auto *updates = static_cast<StreamUpdate *>(frame.streamUpdates.mapped);
  uint32_t count = 0;
  for (uint32_t z = 0; z < s.window; ++z) {
    for (uint32_t x = 0; x < s.window; ++x) {
      const auto [cellX, cellZ] = streamSlotCell(x, z, cameraCellX, cameraCellZ, s.window);
      const uint32_t slot = z * s.window + x;
      if (s.held[slot] == std::array<int32_t, 2>{cellX, cellZ}) {
        continue;
      }
      s.held[slot] = {cellX, cellZ};
      updates[count++] = {slot * s.runStride, slot * s.layerCount, cellX, cellZ};
    }
  }
  if (count > 0) {
    check(vmaFlushAllocation(allocator, frame.streamUpdates.allocation, 0, VK_WHOLE_SIZE),
          "vmaFlushAllocation");
  }
  return count;
}

// One workgroup per (updated slot, streamed layer). The first barrier waits
// for earlier frames' reads of the ring and the records on this queue (the
// culling, the mesh shaders drawing, ray hits in fragment shaders; the
// update list is per frame, so no earlier frame still reads it); the second
// makes the writes visible to this frame's culling and drawing.
void Renderer::Impl::recordStreamUpdate(VkCommandBuffer cmd, const FrameData &frame,
                                        uint32_t updates) {
  if (updates == 0) {
    return;
  }
  const Stream &s = stream;
  constexpr VkPipelineStageFlags2 kReaders =
      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
      VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
      VK_PIPELINE_STAGE_2_COPY_BIT;
  memoryBarrier(cmd, kReaders, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  const Heightfield &field = heightfield;
  StreamConstants constants;
  constants.heights = s.heights.address;
  constants.layers = s.layers.address;
  constants.points = s.points.address;
  constants.prototypes = s.picks.address;
  constants.bounds = s.bounds.address;
  constants.updates = frame.streamUpdates.address;
  constants.thresholds = s.thresholds.address;
  constants.coords = s.coords.address;
  constants.instances = placedInstances.address;
  constants.cells = cells.address;
  constants.field = placementField(field);
  constants.seed = s.seed;
  constants.staticCount = placedCount;
  constants.gridCells = s.gridCells;
  constants.minHeight = field.minHeight;
  constants.heightScale = field.heightScale;
  constants.spacing = field.spacing;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipeline);
  vkCmdPushConstants(cmd, s.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                     &constants);
  vkCmdDispatch(cmd, updates, s.layerCount, 1);
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kReaders,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
}

// Debug: reads back the runs the update wrote and compares every point of
// every updated (cell, layer) with evaluatePoint, besides the thresholds,
// records and coordinates.
void Renderer::Impl::checkStreamUpdate(const FrameData &frame, uint32_t updates) {
  const Stream &s = stream;
  const VkDeviceSize instanceBytes = VkDeviceSize{s.ringCount} * sizeof(PlacedInstance);
  const VkDeviceSize tagBytes = VkDeviceSize{s.ringCount} * sizeof(uint32_t);
  const VkDeviceSize recordBytes = VkDeviceSize{s.recordCount} * sizeof(Cell);
  const VkDeviceSize coordBytes = VkDeviceSize{s.recordCount} * 2 * sizeof(int32_t);
  Buffer readback = createBuffer(instanceBytes + tagBytes + recordBytes + coordBytes,
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                     VMA_ALLOCATION_CREATE_MAPPED_BIT);
  submitNow([&](VkCommandBuffer cmd) {
    const VkBufferCopy instanceCopy{VkDeviceSize{placedCount} * sizeof(PlacedInstance), 0,
                                    instanceBytes};
    vkCmdCopyBuffer(cmd, placedInstances.buffer, readback.buffer, 1, &instanceCopy);
    const VkBufferCopy tagCopy{0, instanceBytes, tagBytes};
    vkCmdCopyBuffer(cmd, s.thresholds.buffer, readback.buffer, 1, &tagCopy);
    const VkBufferCopy recordCopy{VkDeviceSize{s.gridCells} * sizeof(Cell),
                                  instanceBytes + tagBytes, recordBytes};
    vkCmdCopyBuffer(cmd, cells.buffer, readback.buffer, 1, &recordCopy);
    const VkBufferCopy coordCopy{0, instanceBytes + tagBytes + recordBytes, coordBytes};
    vkCmdCopyBuffer(cmd, s.coords.buffer, readback.buffer, 1, &coordCopy);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
  });
  check(vmaInvalidateAllocation(allocator, readback.allocation, 0, VK_WHOLE_SIZE),
        "vmaInvalidateAllocation");
  const auto *bytes = static_cast<const uint8_t *>(readback.mapped);
  const auto *ring = reinterpret_cast<const PlacedInstance *>(bytes);
  const auto *tags = reinterpret_cast<const uint32_t *>(bytes + instanceBytes);
  const auto *records = reinterpret_cast<const Cell *>(bytes + instanceBytes + tagBytes);
  const auto *coords =
      reinterpret_cast<const int32_t *>(bytes + instanceBytes + tagBytes + recordBytes);
  const auto *list = static_cast<const StreamUpdate *>(frame.streamUpdates.mapped);

  RunStats stats{"stream check"};
  for (uint32_t u = 0; u < updates; ++u) {
    uint32_t runOffset = 0;
    for (uint32_t k = 0; k < s.layerCount; ++k) {
      const auto pointCount = static_cast<uint32_t>(s.patterns[k].points.size());
      const StreamUpdate &update = list[u];
      const Cell &c = records[update.record + k];
      const uint32_t start = update.ringStart + runOffset;
      runOffset += pointCount;
      bool recordOk = c.instanceCount <= pointCount && c.firstInstance == placedCount + start &&
                      coords[2 * (update.record + k)] == update.cellX &&
                      coords[2 * (update.record + k) + 1] == update.cellZ;
      compareRun(heightfield, s.layerList[k], s.patterns[k], s.seed, update.cellX, update.cellZ,
                 k, ring + start, std::min(c.instanceCount, pointCount), tags + start, stats);
      if (recordOk) {
        const auto [minY, maxY] = instanceHeights(ring + start, c.instanceCount, s.spheres, true);
        recordOk = std::abs(c.minY - minY) <= 1e-2f && std::abs(c.maxY - maxY) <= 1e-2f;
      }
      if (!recordOk || c.instanceCount > pointCount) {
        stats.report(update.cellX, update.cellZ, UINT32_MAX, UINT32_MAX);
      }
    }
  }
  destroyBuffer(readback);
  std::printf("stream check: %u cells, %u points (%u kept), %u mismatches\n", updates,
              stats.evaluated, stats.kept, stats.mismatches);
  if (stats.mismatches > 0) {
    throw std::runtime_error("GPU streamed placement differs from the CPU reference");
  }
}

Renderer::StreamStats Renderer::streamStats() const {
  Impl &r = *impl_;
  StreamStats stats;
  if (!r.stream.active()) {
    return stats;
  }
  stats.layers = r.stream.layerCount;
  stats.window = r.stream.window;
  stats.ringInstances = r.stream.ringCount;
  check(vkDeviceWaitIdle(r.device.device), "vkDeviceWaitIdle");
  const VkDeviceSize bytes = VkDeviceSize{r.stream.recordCount} * sizeof(Cell);
  Buffer readback = r.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                       VMA_ALLOCATION_CREATE_MAPPED_BIT);
  r.submitNow([&](VkCommandBuffer cmd) {
    const VkBufferCopy copy{VkDeviceSize{r.stream.gridCells} * sizeof(Cell), 0, bytes};
    vkCmdCopyBuffer(cmd, r.cells.buffer, readback.buffer, 1, &copy);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
  });
  check(vmaInvalidateAllocation(r.allocator, readback.allocation, 0, VK_WHOLE_SIZE),
        "vmaInvalidateAllocation");
  const auto *records = static_cast<const Cell *>(readback.mapped);
  for (uint32_t i = 0; i < r.stream.recordCount; ++i) {
    stats.live += records[i].instanceCount;
  }
  r.destroyBuffer(readback);
  return stats;
}

uint32_t Renderer::placedInstanceCount() const { return impl_->placedCount; }

} // namespace meridian
