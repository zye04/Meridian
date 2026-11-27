// The far field at load (setPlacement): each far-field prototype's 1 m
// template comes from its meshes' far voxels (the cluster build voxelized
// them, ClusterLod::farBricks; farBakeGridFromVoxels), filtered on the CPU
// to each level (filterFarTemplate). farfield_splat.comp then
// splats the placed table level by level: occupancy (read back; the CPU
// allocates the brick columns), fixed-point accumulation into transient
// buffers, pack. Debug builds compare sample cells with the CPU replica
// (splatFarField) and print a hash of the result.
#include "renderer_impl.h"

#include <meridian/farfield.h>
#include <meridian/placement.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
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
// 16 m) plus its sink depth.
constexpr float kFarBaseMargin = 32.0f;
// A splat slice's accumulators stay within this many bytes.
// 256 MiB, not 1 GiB: the splat's transient buffers set the startup memory
// peak, and more slices cost little (decisions.md "Cull list caps from measured
// need").
constexpr VkDeviceSize kFarSliceBytes = VkDeviceSize{1} << 28;
constexpr uint64_t kFarSliceBricks =
    kFarSliceBytes / (kBrickVoxels * kFarChannels * sizeof(uint32_t));
// The transient placement runs in chunks of cell rows whose instances, if
// every pattern point were kept, stay within this many bytes.
constexpr VkDeviceSize kFarPlacementChunkBytes = VkDeviceSize{1} << 26;
// The levels each cell stores, by its distance from the placement grid (a
// square band per level, kFarBandMeters past the grid's edge): from a view
// at the crop's centre (2 km from its edge at the relief patch), a band's
// outer edge is about kFarBandMeters + 2 km away, where the next coarser
// level's voxel is 8 m / 4 km = 3.7 px (8 m past the 4 m band) and 16 m /
// 8 km = 3.7 px (16 m past the 8 m band) at 1440p and 70 degrees; finer
// levels everywhere would cost about 4x memory per halving. The fine
// levels' bands are the camera window instead (farFineHalf).
constexpr float kFarBandMeters[kFarLevels - 1 - kFarFineLevels] = {2000.0f, 6000.0f};

// Dispatch dimensions for `groups` workgroups (x at most 65535).
std::array<uint32_t, 2> groups2d(uint64_t groups) {
  const auto x = static_cast<uint32_t>(std::clamp<uint64_t>(groups, 1, 65535));
  return {x, static_cast<uint32_t>((groups + x - 1) / x)};
}

// Debug: sample cells of the GPU splat against splatFarField over the cells
// within `ring` of each (every instance that can reach it): columns
// exactly; voxels within a code step (GPU log2 and division round
// differently) or 16 fixed-point units, and where well resolved, normals
// within 3 degrees and palette colors within 0.05: a voxel's mean
// albedo can sit halfway between two entries (a textured and a flat
// green differ by 0.03), and the GPU's division rounds it to either.
// cos 3 degrees: about 2 octahedral steps where the encoding is coarsest.
constexpr float kNormalTolerance = 0.99863f;
// samples: far cells; windowInstances(x0, z0, x1, z1): the GPU's instances
// placed in those cells (inclusive), already checked against the CPU.
void checkFarField(const FarFieldGrid &grid, uint32_t ring,
                   const std::vector<FarTemplates> &templates, const std::vector<Vec3> &palette,
                   const std::array<float, kFarLevels> &scales,
                   const std::array<std::vector<BrickColumn>, kFarLevels> &columns,
                   const std::array<const uint32_t *, kFarLevels> &voxels,
                   const std::vector<uint32_t> &samples,
                   const std::function<std::vector<DecodedInstance>(uint32_t, uint32_t, uint32_t,
                                                                    uint32_t)> &windowInstances) {
  uint32_t compared = 0, occupied = 0, mismatches = 0;
  for (uint32_t cell : samples) {
    const uint32_t cx = cell % grid.cellsX, cz = cell / grid.cellsX;
    const uint32_t x0 = cx >= ring ? cx - ring : 0, z0 = cz >= ring ? cz - ring : 0;
    const uint32_t x1 = std::min(cx + ring, grid.cellsX - 1);
    const uint32_t z1 = std::min(cz + ring, grid.cellsZ - 1);
    FarFieldGrid window{grid.originX + x0 * kCellSize, grid.originZ + z0 * kCellSize,
                        grid.heightBase, x1 - x0 + 1, z1 - z0 + 1};
    for (uint32_t level = 0; level + 1 < kFarLevels; ++level) {
      const int32_t *b = grid.bands[level];
      const int32_t x = static_cast<int32_t>(x0), z = static_cast<int32_t>(z0);
      window.bands[level][0] = b[0] - x;
      window.bands[level][1] = b[1] - z;
      window.bands[level][2] = b[2] - x;
      window.bands[level][3] = b[3] - z;
    }
    window.anchorX = grid.anchorX - static_cast<int32_t>(x0);
    window.anchorZ = grid.anchorZ - static_cast<int32_t>(z0);
    const std::vector<DecodedInstance> decoded = windowInstances(x0, z0, x1, z1);
    const FarFieldCpu cpu = splatFarField(window, templates, palette, decoded, scales);
    const auto report = [&](uint32_t level, const char *what) {
      if (mismatches++ == 0) {
        std::printf("far field check: first mismatch at cell (%u, %u), level %u: %s\n", cx, cz,
                    level, what);
      }
    };
    for (uint32_t level = 0; level < kFarLevels; ++level) {
      if (farFinestLevel(grid.bands, static_cast<int32_t>(cx), static_cast<int32_t>(cz)) > level) {
        continue;
      }
      const uint32_t n = farColumnsPerCell(level);
      const FarRect gridRect = farColumnRect(grid.bands, grid.cellsX, grid.cellsZ, level);
      const FarRect windowRect = farColumnRect(window.bands, window.cellsX, window.cellsZ, level);
      for (uint32_t lz = 0; lz < n; ++lz) {
        for (uint32_t lx = 0; lx < n; ++lx) {
          const BrickColumn &g =
              columns[level][farColumnIndex(gridRect, level, cx * n + lx, cz * n + lz)];
          const BrickColumn &c = cpu.columns[level][farColumnIndex(
              windowRect, level, (cx - x0) * n + lx, (cz - z0) * n + lz)];
          if (g.heightAndCount != c.heightAndCount) {
            report(level, "column");
            continue;
          }
          for (uint32_t i = 0; i < columnBrickCount(g) * kBrickVoxels; ++i) {
            const uint32_t a =
                farBrickVoxel(voxels[level], g.firstBrick + i / kBrickVoxels, i % kBrickVoxels);
            const uint32_t b = farBrickVoxel(cpu.voxels[level].data(),
                                             c.firstBrick + i / kBrickVoxels, i % kBrickVoxels);
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
              // Normals by angle, not by octahedral byte: across the fold
              // (z < 0) one normal has several encodings bytes apart.
              same = same && dot(va.normal, vb.normal) >= kNormalTolerance &&
                     dot(albedo, albedo) < 2.5e-3f;
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

// Debug: sample shadow-like rays for the GPU farFieldTransmittance vs
// farFieldTransmittanceCpu check. Per ray: origin + start distance,
// direction + max distance. They start inside sample cells' canopy and leave
// at elevations from grazing to steep, or come down into it from above.
std::vector<std::array<float, 8>> farCheckRays(const FarFieldGrid &grid,
                                               const std::vector<FarCell> &farCells) {
  const uint32_t cellCount = grid.cellsX * grid.cellsZ;
  constexpr float kElevations[] = {0.035f, 0.09f, 0.21f, 0.52f, -0.05f};
  std::vector<std::array<float, 8>> rays;
  for (uint32_t s = 0; s < 1u << 20 && rays.size() < 256; ++s) {
    const uint32_t cell = placementHash(s, 0x7261, 0) % cellCount;
    if (farCells[cell].treeCount == 0) {
      continue;
    }
    const FarCell &far = farCells[cell];
    const float elevation = kElevations[rays.size() % std::size(kElevations)];
    const float azimuth =
        static_cast<float>(placementHash(s, 0x7261, 1) % 4096) * (6.2831853f / 4096.0f);
    const Vec3 direction{std::cos(elevation) * std::cos(azimuth), std::sin(elevation),
                         std::cos(elevation) * std::sin(azimuth)};
    Vec3 origin{grid.originX + (static_cast<float>(cell % grid.cellsX) + 0.5f) * kCellSize,
                far.bottom + 0.3f * (far.top - far.bottom),
                grid.originZ + (static_cast<float>(cell / grid.cellsX) + 0.5f) * kCellSize};
    if (elevation < 0.0f) {
      // From 2 km back along the ray, so it enters the canopy from above.
      origin = origin - direction * 2000.0f;
    }
    const float start = rays.size() % 2 == 0 ? 0.0f : 4.0f;
    rays.push_back({origin.x, origin.y, origin.z, start, direction.x, direction.y, direction.z,
                    elevation < 0.0f ? 2500.0f : 20000.0f});
  }
  return rays;
}

} // namespace

void Renderer::Impl::destroyFarField() {
  for (uint32_t level = 0; level < kFarLevels; ++level) {
    destroyBuffer(farColumns[level]);
    destroyBuffer(farVoxels[level]);
  }
  for (Buffer *buffer : {&farCells, &farPyramid, &farPalette, &farFieldInfo, &farCellList}) {
    destroyBuffer(*buffer);
  }
  farGridCells = 0;
  destroyFarShadow();
}

// After the early clusters, so the terrain and near geometry reject hidden
// far cells by early depth testing.
void Renderer::Impl::drawFarField(VkCommandBuffer cmd) {
  if (farFieldInfo.buffer == VK_NULL_HANDLE) {
    return;
  }
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, voxelPipeline);
  vkCmdSetCullMode(cmd, VK_CULL_MODE_BACK_BIT);
  dispatch.cmdDrawMeshTasksIndirectEXT(cmd, drawCounters.buffer, offsetof(DrawCounters, farTasks),
                                       1, sizeof(DrawCounters::farTasks));
}

void Renderer::Impl::buildFarField(std::span<const Prototype> prototypes,
                                   std::span<const PlacementLayer> layers,
                                   const std::vector<uint32_t> &offsets,
                                   const FarPlacement &placement) {
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

  // ------------------------------------------------------------ Templates

  // Each far prototype's 1 m template from its meshes' far voxels (the
  // cluster build voxelized them; slot = mesh, up to kFarSlots, the rest
  // share the last slot), each voxel with its own colour (times the
  // prototype's tint, as the cluster voxels); one palette over all of them.
  constexpr uint32_t kFarSlots = 8;
  // The voxel word's albedo index is 8 bit.
  constexpr uint32_t kFarPaletteEntries = 256;
  std::vector<FarTemplates> templates(prototypes.size());
  std::vector<FarBakeGrid> bakes(prototypes.size());
  std::vector<FarBakeGrid *> baked;
  const auto bakeStart = Clock::now();
  for (uint32_t p = 0; p < prototypes.size(); ++p) {
    if (!far[p]) {
      continue;
    }
    const auto meshCount = static_cast<uint32_t>(prototypes[p].meshes.size());
    const uint32_t slots = std::min(meshCount, kFarSlots);
    std::vector<VoxelGrid> grids(slots);
    for (uint32_t m = 0; m < meshCount; ++m) {
      const uint32_t index = prototypes[p].meshes[m].index;
      const MeshFarVoxels &source = meshFarVoxels[index];
      VoxelGrid grid = unpackVoxelBricks(source.bricks, source.voxels, kFarTemplateVoxel);
      const uint32_t slot = std::min(m, slots - 1);
      grids[slot].edge = kFarTemplateVoxel;
      grids[slot].bricks.insert(grids[slot].bricks.end(), grid.bricks.begin(), grid.bricks.end());
    }
    std::vector<FarSlotColor> slotColors(slots);
    FarBakeGrid &grid = bakes[p];
    grid = farBakeGridFromVoxels(grids, 0, slotColors);
    if (grid.extinction.empty()) {
      std::fprintf(stderr, "Far field: prototype %u has no far voxels (not voxelized)\n", p);
      continue;
    }
    const Vec3 tint = prototypes[p].tint;
    for (size_t i = 0; i < grid.reflected.size(); ++i) {
      const Vec3 r = grid.reflected[i], t = grid.transmitted[i];
      grid.reflected[i] = {r.x * tint.x, r.y * tint.y, r.z * tint.z};
      grid.transmitted[i] = {t.x * tint.x, t.y * tint.y, t.z * tint.z};
    }
    baked.push_back(&grid);
  }
  const FarPalette farColors = buildFarPalette(baked, kFarPaletteEntries);
  for (uint32_t p = 0; p < prototypes.size(); ++p) {
    if (bakes[p].extinction.empty()) {
      continue;
    }
    FarTemplates &t = templates[p];
    t = filterFarTemplate(bakes[p], farColors.reflected);
    std::printf("Far field template: prototype %u, %ux%ux%u m; %zu/%zu/%zu/%zu/%zu template "
                "voxels 1/2/4/8/16 m\n",
                p, bakes[p].size[0], bakes[p].size[1], bakes[p].size[2], t[0].voxels.size(),
                t[1].voxels.size(), t[2].voxels.size(), t[3].voxels.size(), t[4].voxels.size());
  }
  bakes.clear();
  const double bakeMs = msSince(bakeStart);
  const auto paletteSize = static_cast<uint32_t>(farColors.reflected.size());
  std::printf("Far field palette: %u entries\n", paletteSize);

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
  // Entries 0..paletteSize-1: (reflected diffuse rgb, 0); entries
  // paletteSize..: (transmitted rgb, 0), the factor times the transmission
  // colour (farfield.glsl).
  const std::vector<Vec3> &palette = farColors.reflected;
  std::vector<float> paletteData, transmittedData;
  for (uint32_t e = 0; e < paletteSize; ++e) {
    const Vec3 r = farColors.reflected[e], t = farColors.transmitted[e];
    paletteData.insert(paletteData.end(), {r.x, r.y, r.z, 0.0f});
    transmittedData.insert(transmittedData.end(), {t.x, t.y, t.z, 0.0f});
  }
  paletteData.insert(paletteData.end(), transmittedData.begin(), transmittedData.end());

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

  // The far grid: the cells within farExtent of the crop's centre (and the
  // placement grid), inside placement's fixed-point range.
  const PlacementGrid &pg = placementGrid;
  const auto half = static_cast<int32_t>(std::ceil(farExtent / kCellSize));
  const auto span = [&](int32_t first, uint32_t count) {
    const int32_t lo = std::clamp(std::min(-half, first), -kPlacementCellRange, first);
    const int32_t hi = std::clamp(std::max(half - 1, first + static_cast<int32_t>(count) - 1),
                                  first + static_cast<int32_t>(count) - 1, kPlacementCellRange - 1);
    return std::pair{lo, static_cast<uint32_t>(hi - lo + 1)};
  };
  const auto [firstX, cellsX] = span(pg.firstX, pg.columns);
  const auto [firstZ, cellsZ] = span(pg.firstZ, pg.rows);
  FarFieldGrid grid{static_cast<float>(firstX) * kCellSize, static_cast<float>(firstZ) * kCellSize,
                    heightfield.minHeight - kFarBaseMargin, cellsX, cellsZ};
  const auto offsetX = static_cast<uint32_t>(pg.firstX - firstX);
  const auto offsetZ = static_cast<uint32_t>(pg.firstZ - firstZ);
  grid.anchorX = static_cast<int32_t>(offsetX);
  grid.anchorZ = static_cast<int32_t>(offsetZ);
  for (uint32_t level = kFarFineLevels; level + 1 < kFarLevels; ++level) {
    const auto margin =
        static_cast<int32_t>(std::ceil(kFarBandMeters[level - kFarFineLevels] / kCellSize));
    grid.bands[level][0] = std::max(static_cast<int32_t>(offsetX) - margin, 0);
    grid.bands[level][1] = std::max(static_cast<int32_t>(offsetZ) - margin, 0);
    grid.bands[level][2] =
        std::min(static_cast<int32_t>(offsetX + pg.columns) + margin, static_cast<int32_t>(cellsX));
    grid.bands[level][3] =
        std::min(static_cast<int32_t>(offsetZ + pg.rows) + margin, static_cast<int32_t>(cellsZ));
  }
  // The fine levels over the static window: farFineHalf (1 m) and twice
  // that (2 m) around farFineCenter, inside the placement grid: they exist
  // for the switch, which only cells with trees have (outside the grid the
  // static levels draw as before; at the measured 0.25 MiB per cell at 1 m
  // the mirrored forest around the crop would not fit).
  const int32_t placed[4] = {static_cast<int32_t>(offsetX), static_cast<int32_t>(offsetZ),
                             static_cast<int32_t>(offsetX + pg.columns),
                             static_cast<int32_t>(offsetZ + pg.rows)};
  for (uint32_t level = kFarFineLevels; level-- > 0;) {
    int32_t *b = grid.bands[level];
    if (farFineHalf <= 0.0f) {
      std::fill(b, b + 4, 0);
      continue;
    }
    const float half = farFineHalf * static_cast<float>(1u << level);
    const int32_t *outer = level + 1 < kFarFineLevels ? grid.bands[level + 1] : placed;
    const auto cell = [&](float world, float origin) {
      return static_cast<int32_t>(std::floor((world - origin) / kCellSize));
    };
    b[0] = std::max(cell(farFineCenter[0] - half, grid.originX), outer[0]);
    b[1] = std::max(cell(farFineCenter[1] - half, grid.originZ), outer[1]);
    b[2] = std::max(std::min(cell(farFineCenter[0] + half, grid.originX) + 1, outer[2]), b[0]);
    b[3] = std::max(std::min(cell(farFineCenter[1] + half, grid.originZ) + 1, outer[3]), b[1]);
  }
  const uint32_t cellCount = grid.cellsX * grid.cellsZ;
  const auto levelColumnRect = [&](uint32_t level) {
    return farColumnRect(grid.bands, grid.cellsX, grid.cellsZ, level);
  };
  const auto rectCells = [](const FarRect &r) {
    return static_cast<uint32_t>((r[2] - r[0]) * (r[3] - r[1]));
  };

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
  VkDeviceSize maxOccupancyBytes = 0;
  for (uint32_t level = 0; level < kFarLevels; ++level) {
    maxOccupancyBytes =
        std::max<VkDeviceSize>(maxOccupancyBytes, VkDeviceSize{rectCells(levelColumnRect(level))} *
                                                      farColumnsPerCell(level) *
                                                      farColumnsPerCell(level) * 2 * sizeof(uint32_t));
  }
  Buffer readback =
      createBuffer(maxOccupancyBytes + cellDataBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);

  VkPipelineLayout splatLayout = makeLayout(sizeof(FarSplatConstants), false);
  VkPipeline occupancyPipeline = createComputePipeline(dev, "farfield_occupancy.comp", splatLayout);
  VkPipeline accumulatePipeline =
      createComputePipeline(dev, "farfield_accumulate.comp", splatLayout);
  VkPipeline maskPipeline = createComputePipeline(dev, "farfield_mask.comp", splatLayout);
  VkPipeline packPipeline = createComputePipeline(dev, "farfield_pack.comp", splatLayout);

  FarSplatConstants constants;
  constants.templates = templateBuffer.address;
  constants.templateVoxels = templateVoxelBuffer.address;
  constants.palette = farPalette.address;
  constants.cellData = cellData.address;
  constants.originX = farAnchorX(grid);
  constants.originZ = farAnchorZ(grid);
  constants.anchorX = grid.anchorX;
  constants.anchorZ = grid.anchorZ;
  constants.heightBase = grid.heightBase;
  constants.cellsX = grid.cellsX;
  constants.cellsZ = grid.cellsZ;
  constants.paletteSize = paletteSize;
  const auto layerCount = static_cast<uint32_t>(layers.size());

  // The far-field layers placed again over a rect [x0, x1) x [z0, z1) of far
  // cells, mirrored beyond the placement grid, in chunks of rows; each
  // chunk's instances go to use(address, count, first row, rows), its
  // offsets per (cell, layer) stay in chunkCounts. Inside the placement grid
  // they equal the placed table's.
  struct CellRect {
    uint32_t x0, z0, x1, z1;
  };
  const VkDeviceSize rowBytes =
      VkDeviceSize{grid.cellsX} * placement.pointsPerCell * sizeof(PlacedInstance);
  const auto chunkRows = static_cast<uint32_t>(
      std::clamp<VkDeviceSize>(kFarPlacementChunkBytes / rowBytes, 1, grid.cellsZ));
  const VkDeviceSize chunkCountBytes =
      (VkDeviceSize{chunkRows} * grid.cellsX * placement.layerCount + 1) * sizeof(uint32_t);
  Buffer chunkInstances = createBuffer(chunkRows * rowBytes, kUsage, 0);
  Buffer chunkCounts = createBuffer(chunkCountBytes, kUsage, 0);
  Buffer chunkCells =
      createBuffer(VkDeviceSize{chunkRows} * grid.cellsX * sizeof(Cell), kUsage, 0);
  Buffer chunkTotal = createBuffer(sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                       VMA_ALLOCATION_CREATE_MAPPED_BIT);
  uint64_t transientPlaced = 0;
  double placeMs = 0.0;
  const auto place = [&](CellRect rect, auto &&use) {
    for (uint32_t z = rect.z0; z < rect.z1; z += chunkRows) {
      const auto placeStart = Clock::now();
      const uint32_t rows = std::min(chunkRows, rect.z1 - z), columns = rect.x1 - rect.x0;
      PlacementConstants c = placement.constants;
      c.counts = chunkCounts.address;
      c.instances = chunkInstances.address;
      c.cells = chunkCells.address;
      c.gridFirst[0] = firstX + static_cast<int32_t>(rect.x0);
      c.gridFirst[1] = firstZ + static_cast<int32_t>(z);
      c.gridColumns = columns;
      c.layerCount = placement.layerCount;
      c.mirror = 1;
      c.placedFirst[0] = pg.firstX;
      c.placedFirst[1] = pg.firstZ;
      c.placedSize[0] = pg.columns;
      c.placedSize[1] = pg.rows;
      const uint32_t entries = columns * rows * placement.layerCount;
      submitNow([&](VkCommandBuffer cmd) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, placement.countPipeline);
        vkCmdPushConstants(cmd, placement.placementLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(c), &c);
        vkCmdDispatch(cmd, columns, rows, 1);
        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        const ScanConstants scan{chunkCounts.address, entries, 0};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, placement.scanPipeline);
        vkCmdPushConstants(cmd, placement.scanLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(scan), &scan);
        vkCmdDispatch(cmd, 1, 1, 1);
        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, placement.writePipeline);
        vkCmdPushConstants(cmd, placement.placementLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(c), &c);
        vkCmdDispatch(cmd, columns, rows, 1);
        const VkBufferCopy copy{VkDeviceSize{entries} * sizeof(uint32_t), 0, sizeof(uint32_t)};
        vkCmdCopyBuffer(cmd, chunkCounts.buffer, chunkTotal.buffer, 1, &copy);
        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_HOST_READ_BIT);
      });
      check(vmaInvalidateAllocation(allocator, chunkTotal.allocation, 0, VK_WHOLE_SIZE),
            "vmaInvalidateAllocation");
      const uint32_t total = *static_cast<const uint32_t *>(chunkTotal.mapped);
      transientPlaced += total;
      placeMs += msSince(placeStart);
      if (total > 0) {
        use(chunkInstances.address, total, z, rows);
      }
    }
  };
  // The cells a level's instances can reach its bricks from: its band and
  // `ring` cells around.
  const auto levelBand = [&](uint32_t level) {
    if (level + 1 < kFarLevels) {
      const int32_t *b = grid.bands[level];
      return std::array<int32_t, 4>{b[0], b[1], b[2], b[3]};
    }
    return std::array<int32_t, 4>{0, 0, static_cast<int32_t>(cellsX),
                                  static_cast<int32_t>(cellsZ)};
  };
  const auto levelRect = [&](uint32_t level) {
    const std::array<int32_t, 4> band = levelBand(level);
    const auto r = static_cast<int32_t>(ring);
    return CellRect{static_cast<uint32_t>(std::max(band[0] - r, 0)),
                    static_cast<uint32_t>(std::max(band[1] - r, 0)),
                    static_cast<uint32_t>(std::min(band[2] + r, static_cast<int32_t>(cellsX))),
                    static_cast<uint32_t>(std::min(band[3] + r, static_cast<int32_t>(cellsZ)))};
  };

  std::array<std::vector<BrickColumn>, kFarLevels> columns;
  std::array<uint32_t, kFarLevels> brickCounts{}, occupiedColumns{}, sliceCounts{};
  std::array<uint64_t, kFarLevels> poolWords{};
  std::array<double, kFarLevels> splatMs{};
  std::vector<float> cellBottom(cellCount, std::numeric_limits<float>::max());
  std::vector<float> cellTop(cellCount, -std::numeric_limits<float>::max());
  VkDeviceSize peakTransient = 0, peakAllocated = 0, peakAll = 0;
  const auto samplePeak = [&](VkDeviceSize transient) {
    peakTransient = std::max(peakTransient, transient);
    const MemoryUse use = memoryUse();
    peakAllocated = std::max(peakAllocated, use.deviceLocal);
    peakAll = std::max(peakAll, use.all);
  };
  const VkDeviceSize fixedTransient = templateBuffer.size + templateVoxelBuffer.size +
                                      cellData.size + readback.size + chunkInstances.size +
                                      chunkCounts.size + chunkCells.size;
  const auto *mapped = static_cast<const uint32_t *>(readback.mapped);
  submitNow([&](VkCommandBuffer cmd) {
    vkCmdFillBuffer(cmd, cellData.buffer, 0, VK_WHOLE_SIZE, 0);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  });
  for (uint32_t level = 0; level < kFarLevels; ++level) {
    const auto start = Clock::now();
    const uint32_t n = farColumnsPerCell(level);
    const FarRect rect = levelColumnRect(level);
    const auto rectX = static_cast<uint32_t>(rect[2] - rect[0]);
    const auto rectZ = static_cast<uint32_t>(rect[3] - rect[1]);
    const size_t columnCount = size_t{rectX} * rectZ * n * n;
    if (columnCount == 0) {
      // A fine level outside the window: never read (no cell's finest).
      farColumns[level] = createBuffer(sizeof(BrickColumn), kUsage, 0);
      farVoxels[level] = createBuffer(sizeof(uint32_t), kUsage, 0);
      continue;
    }
    const VkDeviceSize occupancyBytes = columnCount * 2 * sizeof(uint32_t);
    Buffer occupancy = createBuffer(occupancyBytes, kUsage, 0);
    constants.columns = occupancy.address;
    constants.level = level;
    constants.fixedScale = scales[level];
    const std::array<int32_t, 4> band = levelBand(level);
    std::copy(band.begin(), band.end(), constants.band);
    submitNow([&](VkCommandBuffer cmd) {
      vkCmdFillBuffer(cmd, occupancy.buffer, 0, VK_WHOLE_SIZE, 0xFFFFFFFFu);
      memoryBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    });
    place(levelRect(level), [&](VkDeviceAddress instances, uint32_t count, uint32_t, uint32_t) {
      FarSplatConstants occupancyConstants = constants;
      occupancyConstants.placed = instances;
      occupancyConstants.count = count;
      submitNow([&](VkCommandBuffer cmd) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, occupancyPipeline);
        vkCmdPushConstants(cmd, splatLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(occupancyConstants), &occupancyConstants);
        const auto occupancyGroups = groups2d((uint64_t{count} + 63) / 64);
        vkCmdDispatch(cmd, occupancyGroups[0], occupancyGroups[1], 1);
        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
      });
    });
    submitNow([&](VkCommandBuffer cmd) {
      const VkBufferCopy copy{0, 0, occupancyBytes};
      vkCmdCopyBuffer(cmd, occupancy.buffer, readback.buffer, 1, &copy);
      memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    });
    check(vmaInvalidateAllocation(allocator, readback.allocation, 0, VK_WHOLE_SIZE),
          "vmaInvalidateAllocation");
    destroyBuffer(occupancy);
    std::vector<uint32_t> loHi(columnCount * 2);
    for (size_t c = 0; c < columnCount; ++c) {
      loHi[2 * c] = mapped[2 * c];
      loHi[2 * c + 1] = ~mapped[2 * c + 1];
      if (loHi[2 * c] != UINT32_MAX) {
        const size_t local = c / (n * n);
        const size_t cell = (local / rectX + rect[1]) * grid.cellsX + local % rectX + rect[0];
        cellBottom[cell] =
            std::min(cellBottom[cell], grid.heightBase + loHi[2 * c] * kFarHeightUnit);
        cellTop[cell] = std::max(cellTop[cell], grid.heightBase + loHi[2 * c + 1] * kFarHeightUnit);
      }
    }
    uint32_t &brickTotal = brickCounts[level];
    columns[level] = farBrickColumns(loHi, level, brickTotal);
    for (const BrickColumn &column : columns[level]) {
      occupiedColumns[level] += columnBrickCount(column) > 0 ? 1 : 0;
    }
    farColumns[level] =
        uploadStorage(columns[level].data(), columns[level].size() * sizeof(BrickColumn));

    // Slices of cell rows, each with its own accumulators.
    // Rows of the level's rect.
    const std::vector<uint32_t> rows =
        farSliceRows(columns[level], level, rectX, rectZ, kFarSliceBricks);
    const size_t columnsPerRow = size_t{rectX} * n * n;
    std::vector<uint32_t> sliceBricks(rows.size() - 1, 0);
    uint32_t maxSliceBricks = 0;
    for (size_t sl = 0; sl + 1 < rows.size(); ++sl) {
      for (size_t c = rows[sl] * columnsPerRow; c < rows[sl + 1] * columnsPerRow; ++c) {
        sliceBricks[sl] += columnBrickCount(columns[level][c]);
      }
      maxSliceBricks = std::max(maxSliceBricks, sliceBricks[sl]);
    }
    sliceCounts[level] = static_cast<uint32_t>(sliceBricks.size());
    const VkDeviceSize maxAccumulatorBytes =
        VkDeviceSize{maxSliceBricks} * kBrickVoxels * kFarChannels * sizeof(uint32_t);
    if (maxAccumulatorBytes > maxBufferSize ||
        VkDeviceSize{maxSliceBricks} * kBrickVoxels * kFarChannels > UINT32_MAX) {
      throw std::runtime_error("far field accumulators exceed the largest buffer");
    }
    Buffer accumulators = createBuffer(std::max<VkDeviceSize>(maxAccumulatorBytes, 4), kUsage, 0);
    Buffer headers = createBuffer(
        std::max<VkDeviceSize>(VkDeviceSize{maxSliceBricks} * sizeof(BrickHeader), 4), kUsage,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    auto *headerWords = static_cast<uint32_t *>(headers.mapped);
    constants.columns = farColumns[level].address;
    constants.accumulators = accumulators.address;
    constants.headers = headers.address;

    // The level's headers (firstVoxel absolute in the pool) and, per slice,
    // its occupied voxels.
    std::vector<uint32_t> poolHeaders(size_t{brickTotal} * kBrickHeaderWords, 0);
    std::vector<Buffer> chunks;
    std::vector<uint64_t> chunkWords;
    uint64_t voxelTotal = 0;
    uint32_t firstBrick = 0;
    VkDeviceSize chunkBytes = 0;
    for (size_t sl = 0; sl + 1 < rows.size(); ++sl) {
      const uint32_t count = sliceBricks[sl];
      if (count == 0) {
        continue;
      }
      constants.firstBrick = firstBrick;
      constants.rowBegin = rows[sl] + static_cast<uint32_t>(rect[1]);
      constants.rowEnd = rows[sl + 1] + static_cast<uint32_t>(rect[1]);
      // The instances within reach of the slice's rows (one more row as
      // margin), inside the level's rect.
      CellRect reach = levelRect(level);
      reach.z0 = std::max(reach.z0, constants.rowBegin > ring + 1 ? constants.rowBegin - ring - 1 : 0);
      reach.z1 = std::min(reach.z1, constants.rowEnd + ring + 1);
      FarSplatConstants perBrick = constants;
      perBrick.count = count;
      samplePeak(fixedTransient + accumulators.size + headers.size + chunkBytes);
      submitNow([&](VkCommandBuffer cmd) {
        vkCmdFillBuffer(cmd, accumulators.buffer, 0,
                        VkDeviceSize{count} * kBrickVoxels * kFarChannels * sizeof(uint32_t), 0);
        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
      });
      if (reach.z0 < reach.z1) {
        place(reach, [&](VkDeviceAddress instances, uint32_t instanceCount, uint32_t, uint32_t) {
          FarSplatConstants accumulate = constants;
          accumulate.placed = instances;
          accumulate.count = instanceCount;
          submitNow([&](VkCommandBuffer cmd) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, accumulatePipeline);
            vkCmdPushConstants(cmd, splatLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(accumulate), &accumulate);
            const auto instanceGroups = groups2d(instanceCount);
            vkCmdDispatch(cmd, instanceGroups[0], instanceGroups[1], 1);
            memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
          });
        });
      }
      submitNow([&](VkCommandBuffer cmd) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, maskPipeline);
        vkCmdPushConstants(cmd, splatLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(perBrick),
                           &perBrick);
        const auto brickGroups = groups2d(count); // one workgroup per brick
        vkCmdDispatch(cmd, brickGroups[0], brickGroups[1], 1);
        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                      VK_ACCESS_2_HOST_READ_BIT);
      });
      check(vmaInvalidateAllocation(allocator, headers.allocation, 0, VK_WHOLE_SIZE),
            "vmaInvalidateAllocation");
      // Scan the occupied counts: firstVoxel relative to this slice's voxels
      // for the pack, absolute (after every header) in the level's pool.
      uint32_t sliceVoxels = 0;
      for (uint32_t b = 0; b < count; ++b) {
        const uint32_t lo = headerWords[b * kBrickHeaderWords];
        const uint32_t hi = headerWords[b * kBrickHeaderWords + 1];
        uint32_t *out = &poolHeaders[size_t{firstBrick + b} * kBrickHeaderWords];
        out[0] = lo;
        out[1] = hi;
        out[2] = static_cast<uint32_t>(size_t{brickTotal} * kBrickHeaderWords + voxelTotal +
                                       sliceVoxels);
        headerWords[b * kBrickHeaderWords + 2] = sliceVoxels;
        sliceVoxels += static_cast<uint32_t>(std::popcount(lo) + std::popcount(hi));
      }
      check(vmaFlushAllocation(allocator, headers.allocation, 0, VK_WHOLE_SIZE),
            "vmaFlushAllocation");
      if (sliceVoxels > 0) {
        Buffer chunk = createBuffer(VkDeviceSize{sliceVoxels} * sizeof(uint32_t), kUsage, 0);
        perBrick.voxels = chunk.address;
        submitNow([&](VkCommandBuffer cmd) {
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, packPipeline);
          vkCmdPushConstants(cmd, splatLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(perBrick),
                             &perBrick);
          const auto brickGroups = groups2d(count);
          vkCmdDispatch(cmd, brickGroups[0], brickGroups[1], 1);
          memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT);
        });
        chunkBytes += chunk.size;
        chunks.push_back(chunk);
        chunkWords.push_back(sliceVoxels);
      }
      voxelTotal += sliceVoxels;
      firstBrick += count;
    }
    destroyBuffer(accumulators);
    destroyBuffer(headers);

    // The pool: every header, then the slices' voxels in order.
    poolWords[level] = size_t{brickTotal} * kBrickHeaderWords + voxelTotal;
    if (poolWords[level] > UINT32_MAX || poolWords[level] * sizeof(uint32_t) > maxBufferSize) {
      throw std::runtime_error("far field bricks exceed the largest buffer");
    }
    farVoxels[level] =
        createBuffer(std::max<VkDeviceSize>(poolWords[level] * sizeof(uint32_t), 4), kUsage, 0);
    Buffer headerUpload;
    if (brickTotal > 0) {
      headerUpload = createBuffer(poolHeaders.size() * sizeof(uint32_t),
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                      VMA_ALLOCATION_CREATE_MAPPED_BIT);
      std::memcpy(headerUpload.mapped, poolHeaders.data(), headerUpload.size);
      check(vmaFlushAllocation(allocator, headerUpload.allocation, 0, VK_WHOLE_SIZE),
            "vmaFlushAllocation");
    }
    samplePeak(fixedTransient + chunkBytes + headerUpload.size);
    submitNow([&](VkCommandBuffer cmd) {
      if (brickTotal > 0) {
        const VkBufferCopy copy{0, 0, poolHeaders.size() * sizeof(uint32_t)};
        vkCmdCopyBuffer(cmd, headerUpload.buffer, farVoxels[level].buffer, 1, &copy);
      }
      VkDeviceSize at = poolHeaders.size() * sizeof(uint32_t);
      for (size_t i = 0; i < chunks.size(); ++i) {
        const VkBufferCopy copy{0, at, chunkWords[i] * sizeof(uint32_t)};
        vkCmdCopyBuffer(cmd, chunks[i].buffer, farVoxels[level].buffer, 1, &copy);
        at += copy.size;
      }
      memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT |
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    });
    for (Buffer &chunk : chunks) {
      destroyBuffer(chunk);
    }
    if (brickTotal > 0) {
      destroyBuffer(headerUpload);
    }
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
  std::vector<FarCell> farCellData(cellCount);
  uint32_t treeCells = 0;
  for (uint32_t c = 0; c < cellCount; ++c) {
    FarCell &cell = farCellData[c];
    cell.bottom = cellBottom[c];
    cell.top = cellTop[c];
    // Placed trees only inside the placement grid (placement cell p).
    const uint32_t x = c % cellsX - offsetX, z = c / cellsX - offsetZ;
    if (x < pg.columns && z < pg.rows) {
      const uint32_t p = z * pg.columns + x;
      for (uint32_t l = 0; l < layerCount && layers[l].farField; ++l) {
        cell.treeCount += offsets[p * layerCount + l + 1] - offsets[p * layerCount + l];
      }
    }
    std::memcpy(&cell.treeHeight, &mapped[c], sizeof(float));
    treeCells += cell.treeCount > 0 ? 1 : 0;
  }
  destroyBuffer(readback);
  destroyBuffer(cellData);
  vkDestroyPipeline(dev, occupancyPipeline, nullptr);
  vkDestroyPipeline(dev, accumulatePipeline, nullptr);
  vkDestroyPipeline(dev, maskPipeline, nullptr);
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
  for (uint32_t level = 0; level < kFarLevels; ++level) {
    double sum = 0.0, weight = 0.0;
    for (const FarTemplates &t : templates) {
      sum += double{t[level].alignment} * t[level].extinctionSum;
      weight += t[level].extinctionSum;
    }
    info.alignment[level] = weight > 0.0 ? static_cast<float>(sum / weight) : 1.0f / 3.0f;
  }
  std::printf("Far field: leaf alignment 1/2/4/8/16 m %.3f/%.3f/%.3f/%.3f/%.3f\n",
              info.alignment[0], info.alignment[1], info.alignment[2], info.alignment[3],
              info.alignment[4]);
  std::memcpy(info.bands, grid.bands, sizeof(info.bands));
  info.placedOffset[0] = offsetX;
  info.placedOffset[1] = offsetZ;
  farGridCells = cellCount;
  farFieldInfo = uploadStorage(&info, sizeof(info));
  farCellList = createBuffer(VkDeviceSize{cellCount} * sizeof(uint32_t),
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                             0);

  VkDeviceSize resident = farCells.size + farPyramid.size + farPalette.size + farFieldInfo.size +
                          createFarShadow(grid, farCellData);
  for (uint32_t level = 0; level < kFarLevels; ++level) {
    resident += farColumns[level].size + farVoxels[level].size;
  }
  double splatTotal = 0.0;
  for (double ms : splatMs) {
    splatTotal += ms;
  }
  const auto voxelsIn = [&](uint32_t level) {
    return static_cast<unsigned long long>(poolWords[level] -
                                           uint64_t{brickCounts[level]} * kBrickHeaderWords);
  };
  const auto bandSize = [&](uint32_t level) {
    const FarRect r = levelColumnRect(level);
    return std::pair{r[2] - r[0], r[3] - r[1]};
  };
  std::printf("Far field grid: %ux%u cells from cell (%d, %d), 1/2/4/8 m bands "
              "%dx%d/%dx%d/%dx%d/%dx%d cells; transient placement %llu instances in %.0f ms "
              "(chunks of %u rows)\n",
              cellsX, cellsZ, firstX, firstZ, bandSize(0).first, bandSize(0).second,
              bandSize(1).first, bandSize(1).second,
              grid.bands[2][2] - grid.bands[2][0], grid.bands[2][3] - grid.bands[2][1],
              grid.bands[3][2] - grid.bands[3][0], grid.bands[3][3] - grid.bands[3][1],
              static_cast<unsigned long long>(transientPlaced), placeMs, chunkRows);
  std::printf("Far field: resident %.0f MiB; peak transient %.0f MiB, device memory allocated "
              "at peak %.0f MiB (all heaps %.0f MiB); %u cells with trees; templates %.0f ms, "
              "splat %.0f ms, total %.0f ms; %u lost deposits\n",
              resident * kMiB, peakTransient * kMiB, peakAllocated * kMiB, peakAll * kMiB,
              treeCells, bakeMs, splatTotal, msSince(loadStart), lostDeposits);
  for (uint32_t level = 0; level < kFarLevels; ++level) {
    std::printf("Far field %.0f m: %u bricks (%llu occupied voxels) in %u columns, %u slices; "
                "bricks %.1f MiB, columns %.1f MiB; splat %.0f ms\n",
                kFarVoxelSize[level], brickCounts[level], voxelsIn(level), occupiedColumns[level],
                sliceCounts[level], farVoxels[level].size * kMiB, farColumns[level].size * kMiB,
                splatMs[level]);
  }

  destroyBuffer(templateBuffer);
  destroyBuffer(templateVoxelBuffer);

  if (kEnableValidation) {
    VkDeviceSize total = 0;
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
      for (uint32_t level = 0; level < kFarLevels; ++level) {
        copy(farVoxels[level], farVoxels[level].size);
      }
      memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    });
    vmaInvalidateAllocation(allocator, check.allocation, 0, VK_WHOLE_SIZE);
    const auto *bytes = static_cast<const uint8_t *>(check.mapped);
    std::array<const uint32_t *, kFarLevels> voxels{};
    // The static levels' hash compares with commits before the fine levels.
    uint64_t hash = 0xcbf29ce484222325ull, fineHash = hash;
    VkDeviceSize at = 0;
    for (uint32_t level = 0; level < kFarLevels; ++level) {
      voxels[level] = reinterpret_cast<const uint32_t *>(bytes + at);
      if (!columns[level].empty()) {
        uint64_t &h = level < kFarFineLevels ? fineHash : hash;
        h = fnv1a(h, columns[level].data(), columns[level].size() * sizeof(BrickColumn));
        h = fnv1a(h, bytes + at, farVoxels[level].size);
      }
      at += farVoxels[level].size;
    }
    // The placement grid's cells only, by value (column extent, then every
    // voxel), so it compares across far grids of any extent.
    uint64_t cropHash = 0xcbf29ce484222325ull;
    for (uint32_t level = kFarFineLevels; level < kFarLevels; ++level) {
      const uint32_t n = farColumnsPerCell(level);
      for (uint32_t z = 0; z < pg.rows * n; ++z) {
        for (uint32_t x = 0; x < pg.columns * n; ++x) {
          // Crowns from mirrored trees past the crop reach the outer ring.
          const uint32_t cx = x / n, cz = z / n, m = ring + 1;
          if (cx < m || cz < m || cx + m >= pg.columns || cz + m >= pg.rows) {
            continue;
          }
          const BrickColumn &column = columns[level][farColumnIndex(
              levelColumnRect(level), level, offsetX * n + x, offsetZ * n + z)];
          cropHash = fnv1a(cropHash, &column.heightAndCount, sizeof(uint32_t));
          for (uint32_t i = 0; i < columnBrickCount(column) * kBrickVoxels; ++i) {
            const uint32_t voxel = farBrickVoxel(voxels[level], column.firstBrick + i / kBrickVoxels,
                                                 i % kBrickVoxels);
            cropHash = fnv1a(cropHash, &voxel, sizeof(voxel));
          }
        }
      }
    }
    std::printf("far field hash 0x%016llx, placement grid inside its outer ring+1 cells 0x%016llx "
                "(static levels); fine levels 0x%016llx\n",
                static_cast<unsigned long long>(hash), static_cast<unsigned long long>(cropHash),
                static_cast<unsigned long long>(fineHash));
    // What the brick count holds: bricks with no occupied voxel are
    // footprint slack (the rotated hull's AABB plus the trilinear margin).
    std::printf("far field bricks (empty, occupied voxels per brick):");
    for (uint32_t level = 0; level < kFarLevels; ++level) {
      uint64_t empty = 0, occupied = 0;
      for (uint32_t brick = 0; brick < brickCounts[level]; ++brick) {
        const uint32_t *header = voxels[level] + size_t{brick} * kBrickHeaderWords;
        const auto count =
            static_cast<uint32_t>(std::popcount(header[0]) + std::popcount(header[1]));
        empty += count == 0 ? 1 : 0;
        occupied += count;
      }
      std::printf(" L%u %llu (%.1f%%), %.1f", level, static_cast<unsigned long long>(empty),
                  100.0 * static_cast<double>(empty) / std::max(brickCounts[level], 1u),
                  static_cast<double>(occupied) / std::max(brickCounts[level], 1u));
    }
    std::printf("\n");
    // Sample cells: in the placement grid, and outside it per band (the
    // finest level the cell stores), three each.
    // Inside it, three more per fine level.
    std::vector<uint32_t> samples = {offsetZ * cellsX + offsetX};
    std::array<uint32_t, kFarLevels + 1 + kFarFineLevels> found{};
    for (uint32_t s = 0; s < 1u << 20 && samples.size() < 1 + 3 * found.size(); ++s) {
      const uint32_t cell = placementHash(s, 0x6661, 0) % cellCount;
      const uint32_t x = cell % cellsX - offsetX, z = cell / cellsX - offsetZ;
      const bool placed = x < pg.columns && z < pg.rows;
      const uint32_t finest = farFinestLevel(grid.bands, static_cast<int32_t>(cell % cellsX),
                                             static_cast<int32_t>(cell / cellsX));
      const uint32_t kind = !placed                    ? finest
                            : finest < kFarFineLevels ? kFarLevels + 1 + finest
                                                      : kFarLevels;
      if (found[kind] < 3 && farCellData[cell].bottom <= farCellData[cell].top &&
          (!placed || farCellData[cell].treeCount > 0)) {
        ++found[kind];
        samples.push_back(cell);
      }
    }
    // The GPU's transient instances of a window against evaluatePoint
    // (mirrored outside the placement grid), run by run.
    RunStats stats{"far placement check"};
    const GeoChannels *geoChannels = geo ? &*geo : nullptr;
    const auto windowInstances = [&](uint32_t x0, uint32_t z0, uint32_t x1, uint32_t z1) {
      std::vector<DecodedInstance> out;
      Buffer host = createBuffer(chunkInstances.size + chunkCounts.size,
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                     VMA_ALLOCATION_CREATE_MAPPED_BIT);
      place(CellRect{x0, z0, x1 + 1, z1 + 1},
            [&](VkDeviceAddress, uint32_t count, uint32_t zFirst, uint32_t rows) {
              submitNow([&](VkCommandBuffer cmd) {
                const VkBufferCopy copies[] = {
                    {0, 0, VkDeviceSize{count} * sizeof(PlacedInstance)},
                    {0, chunkInstances.size, chunkCounts.size}};
                vkCmdCopyBuffer(cmd, chunkInstances.buffer, host.buffer, 1, &copies[0]);
                vkCmdCopyBuffer(cmd, chunkCounts.buffer, host.buffer, 1, &copies[1]);
                memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
              });
              vmaInvalidateAllocation(allocator, host.allocation, 0, VK_WHOLE_SIZE);
              const auto *instances = static_cast<const PlacedInstance *>(host.mapped);
              const auto *counts = reinterpret_cast<const uint32_t *>(
                  static_cast<const uint8_t *>(host.mapped) + chunkInstances.size);
              const uint32_t columnsInRect = x1 + 1 - x0, layersHere = placement.layerCount;
              for (uint32_t r = 0; r < rows; ++r) {
                for (uint32_t x = 0; x < columnsInRect; ++x) {
                  const int32_t cellX = firstX + static_cast<int32_t>(x0 + x);
                  const int32_t cellZ = firstZ + static_cast<int32_t>(zFirst + r);
                  const bool mirrored = cellX < pg.firstX || cellZ < pg.firstZ ||
                                        cellX >= pg.firstX + static_cast<int32_t>(pg.columns) ||
                                        cellZ >= pg.firstZ + static_cast<int32_t>(pg.rows);
                  for (uint32_t l = 0; l < layersHere; ++l) {
                    const uint32_t entry = (r * columnsInRect + x) * layersHere + l;
                    const uint32_t first = counts[entry];
                    compareRun(heightfield, geoChannels, layers[l], placement.patterns[l],
                               placement.seed, cellX, cellZ, l, instances + first,
                               counts[entry + 1] - first, nullptr, stats, mirrored);
                  }
                }
              }
              for (uint32_t i = 0; i < count; ++i) {
                out.push_back(decodePlacedInstance(instances[i]));
              }
            });
      destroyBuffer(host);
      return out;
    };
    checkFarField(grid, ring, templates, palette, scales, columns, voxels, samples,
                  windowInstances);
    std::printf("far placement check: %u points (%u kept) in the sample windows compared with the "
                "CPU reference, %u mismatches\n",
                stats.evaluated, stats.kept, stats.mismatches);
    if (stats.mismatches > 0) {
      throw std::runtime_error("GPU far field placement differs from the CPU reference");
    }

    // farFieldTransmittance (farfield.glsl) against the CPU replica on the
    // same bricks.
    FarFieldCpu cpu;
    cpu.origin = {grid.originX, grid.heightBase, grid.originZ};
    cpu.cellsX = grid.cellsX;
    cpu.cellsZ = grid.cellsZ;
    cpu.heightBase = grid.heightBase;
    std::memcpy(cpu.bands, grid.bands, sizeof(cpu.bands));
    for (uint32_t level = 0; level < kFarLevels; ++level) {
      cpu.columns[level] = columns[level];
      cpu.voxels[level].assign(voxels[level], voxels[level] + poolWords[level]);
      cpu.brickCount[level] = brickCounts[level];
    }
    destroyBuffer(check);
    checkFarShadow(cpu);
    const std::vector<std::array<float, 8>> rays = farCheckRays(grid, farCellData);
    const auto rayCount = static_cast<uint32_t>(rays.size());
    Buffer rayBuffer = uploadStorage(rays.data(), rays.size() * sizeof(rays[0]));
    Buffer results = createBuffer(rayCount * sizeof(float),
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                  VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                      VMA_ALLOCATION_CREATE_MAPPED_BIT);
    VkPipelineLayout checkLayout = makeLayout(sizeof(FarCheckConstants), false);
    VkPipeline checkPipeline = createComputePipeline(dev, "farfield_check.comp", checkLayout);
    const FarCheckConstants constants{farFieldInfo.address, rayBuffer.address, results.address,
                                      rayCount, 0};
    submitNow([&](VkCommandBuffer cmd) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, checkPipeline);
      vkCmdPushConstants(cmd, checkLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                         &constants);
      vkCmdDispatch(cmd, (rayCount + 63) / 64, 1, 1);
      memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                    VK_ACCESS_2_HOST_READ_BIT);
    });
    vmaInvalidateAllocation(allocator, results.allocation, 0, VK_WHOLE_SIZE);
    const auto *gpu = static_cast<const float *>(results.mapped);
    float largest = 0.0f;
    uint32_t partial = 0;
    for (uint32_t i = 0; i < rayCount; ++i) {
      const std::array<float, 8> &ray = rays[i];
      const float expected = farFieldTransmittanceCpu(cpu, {ray[0], ray[1], ray[2]},
                                                      {ray[4], ray[5], ray[6]}, ray[3], ray[7],
                                                      nullptr);
      largest = std::max(largest, std::abs(gpu[i] - expected));
      partial += expected > 1e-3f && expected < 0.999f ? 1 : 0;
    }
    std::printf("far field ray check: %u rays (%u partly transmitted) compared "
                "with the CPU "
                "replica, max difference %.2g\n",
                rayCount, partial, largest);
    vkDestroyPipeline(dev, checkPipeline, nullptr);
    vkDestroyPipelineLayout(dev, checkLayout, nullptr);
    destroyBuffer(results);
    destroyBuffer(rayBuffer);
    if (largest > 1e-3f) {
      throw std::runtime_error("GPU far field transmittance differs from the CPU replica");
    }
  }
  for (Buffer *buffer : {&chunkInstances, &chunkCounts, &chunkCells, &chunkTotal}) {
    destroyBuffer(*buffer);
  }
}

} // namespace meridian
