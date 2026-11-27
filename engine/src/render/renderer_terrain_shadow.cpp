// The terrain shadow-height map (terrain_shadow.comp): per texel of a
// sun-aligned window around the camera, the height above which the
// heightfield lets the sun through, so receivers fetch it instead of
// marching the height pyramid (lighting.glsl terrainShadowVisibility).
#include "renderer_impl.h"

#include <cmath>

namespace meridian {
namespace {

// The near window: 8.2 km of 4 m texels, twice the heightfield's spacing,
// whose samples the lines step through. The far window: 49 km of 16 m
// texels, so receivers out to the far field's edge (its cells, the terrain;
// up to ~18 km per axis from a camera in the crop) read a texel instead of
// marching the pyramid per pixel (12 ms of the far pass at a1); from 4 km a
// 16 m texel is a few pixels.
struct TerrainShadowWindowSpec {
  uint32_t size;
  float texel;
  // Rebuilt when the camera is this far from the window's centre.
  float rebuildDistance;
};
constexpr TerrainShadowWindowSpec kTerrainShadowWindows[2] = {{2048, 4.0f, 1000.0f},
                                                              {3072, 16.0f, 4000.0f}};
// Below it receivers keep the march: a grazing sun's shadow heights change
// faster than the map resolves.
constexpr float kMinSunElevation = 1.0f * 3.14159265f / 180.0f;
constexpr float kRebuildAngle = 0.02f * 3.14159265f / 180.0f;
constexpr bool kAlwaysRebuild = false;

} // namespace

void Renderer::Impl::destroyTerrainShadow() {
  if (terrainShadowPipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(device.device, terrainShadowPipeline, nullptr);
    vkDestroyPipelineLayout(device.device, terrainShadowLayout, nullptr);
    terrainShadowPipeline = VK_NULL_HANDLE;
    terrainShadowLayout = VK_NULL_HANDLE;
  }
  for (Buffer &map : terrainShadowMap) {
    destroyBuffer(map);
  }
  terrainShadowValid = false;
}

void Renderer::Impl::updateTerrainShadow(Vec3 sun, Vec3 cameraPosition,
                                         VkDeviceAddress sceneAddress, SceneData &scene) {
  terrainShadowPending[0] = terrainShadowPending[1] = false;
  sun = normalize(sun);
  const float horizontal = std::sqrt(sun.x * sun.x + sun.z * sun.z);
  if (terrain.width == 0 || std::atan2(sun.y, horizontal) < kMinSunElevation) {
    return;
  }
  if (terrainShadowPipeline == VK_NULL_HANDLE) {
    for (uint32_t w = 0; w < 2; ++w) {
      const uint32_t size = kTerrainShadowWindows[w].size;
      terrainShadowMap[w] = createBuffer(VkDeviceSize{size} * size * 8,
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                         0);
    }
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                    sizeof(TerrainShadowConstants)};
    // Set 1 (bindless) holds the height image; set 0 is the cull set, bound
    // with it.
    const VkDescriptorSetLayout setLayouts[] = {cullSetLayout, materialSetLayout};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 2;
    info.pSetLayouts = setLayouts;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(device.device, &info, nullptr, &terrainShadowLayout),
          "vkCreatePipelineLayout");
    terrainShadowPipeline =
        createComputePipeline(device.device, "terrain_shadow.comp", terrainShadowLayout);
  }
  // The window's axes: away from the sun, and across it.
  const float alongX = -sun.x / horizontal, alongZ = -sun.z / horizontal;
  const float acrossX = -alongZ, acrossZ = alongX;
  const bool sunTurned =
      !terrainShadowValid || length(cross(sun, terrainShadowSun)) > std::sin(kRebuildAngle);
  for (uint32_t w = 0; w < 2; ++w) {
    const TerrainShadowWindowSpec &spec = kTerrainShadowWindows[w];
    TerrainShadowConstants &built = terrainShadowBuild[w];
    const float half = 0.5f * static_cast<float>(spec.size) * spec.texel;
    const float centreX = built.origin[0] + half * (built.along[0] + built.across[0]);
    const float centreZ = built.origin[1] + half * (built.along[1] + built.across[1]);
    const bool stale =
        kAlwaysRebuild || sunTurned ||
        std::hypot(cameraPosition.x - centreX, cameraPosition.z - centreZ) > spec.rebuildDistance;
    if (stale) {
      // Texel-snapped in the sun's frame, so the map doesn't swim as the
      // camera moves.
      const auto snap = [&](float v) { return std::round(v / spec.texel) * spec.texel; };
      const float a = snap(cameraPosition.x * alongX + cameraPosition.z * alongZ) - half;
      const float c = snap(cameraPosition.x * acrossX + cameraPosition.z * acrossZ) - half;
      built.texels = terrainShadowMap[w].address;
      built.origin[0] = a * alongX + c * acrossX;
      built.origin[1] = a * alongZ + c * acrossZ;
      built.along[0] = alongX;
      built.along[1] = alongZ;
      built.across[0] = acrossX;
      built.across[1] = acrossZ;
      built.texel = spec.texel;
      built.size = spec.size;
      built.tanE = sun.y / horizontal;
      built.maxLength = static_cast<float>(kShadowRayLength);
      terrainShadowPending[w] = true;
    }
    // Each frame's scene buffer (the terrain) is the one the build reads.
    built.scene = sceneAddress;
  }
  terrainShadowSun = sun;
  terrainShadowValid = true;
  const TerrainShadowConstants &nearBuild = terrainShadowBuild[0];
  const TerrainShadowConstants &farBuild = terrainShadowBuild[1];
  scene.terrainShadowMode = 1;
  scene.terrainShadowTexels = nearBuild.texels;
  scene.terrainShadowOrigin[0] = nearBuild.origin[0];
  scene.terrainShadowOrigin[1] = nearBuild.origin[1];
  scene.terrainShadowAlong[0] = nearBuild.along[0];
  scene.terrainShadowAlong[1] = nearBuild.along[1];
  scene.terrainShadowAcross[0] = nearBuild.across[0];
  scene.terrainShadowAcross[1] = nearBuild.across[1];
  scene.terrainShadowTexel = nearBuild.texel;
  scene.terrainShadowSize = nearBuild.size;
  scene.terrainShadowFarTexels = farBuild.texels;
  scene.terrainShadowFarOrigin[0] = farBuild.origin[0];
  scene.terrainShadowFarOrigin[1] = farBuild.origin[1];
  scene.terrainShadowFarTexel = farBuild.texel;
  scene.terrainShadowFarSize = farBuild.size;
  // The occluder line's height change across the sun disk's diameter, per
  // meter of horizontal occluder distance.
  const float cosE = horizontal / length(sun);
  scene.terrainShadowPenumbra = 2.0f * std::tan(scene.sunAngularRadius) / (cosE * cosE);
}

void Renderer::Impl::recordTerrainShadow(VkCommandBuffer cmd) {
  if (!terrainShadowPending[0] && !terrainShadowPending[1]) {
    return;
  }
  // The previous frame in flight may still read the map.
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, terrainShadowPipeline);
  const VkDescriptorSet sets[] = {cullSet, materialSet};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, terrainShadowLayout, 0, 2, sets, 0,
                          nullptr);
  for (uint32_t w = 0; w < 2; ++w) {
    if (terrainShadowPending[w]) {
      vkCmdPushConstants(cmd, terrainShadowLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                         sizeof(TerrainShadowConstants), &terrainShadowBuild[w]);
      vkCmdDispatch(cmd, terrainShadowBuild[w].size, 1, 1);
    }
  }
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
  terrainShadowPending[0] = terrainShadowPending[1] = false;
}

} // namespace meridian
