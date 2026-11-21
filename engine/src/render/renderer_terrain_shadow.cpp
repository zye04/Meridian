// The terrain shadow-height map (terrain_shadow.comp): per texel of a
// sun-aligned window around the camera, the height above which the
// heightfield lets the sun through, so receivers fetch it instead of
// marching the height pyramid (lighting.glsl terrainShadowVisibility).
#include "renderer_impl.h"

#include <cmath>

namespace meridian {
namespace {

// 8.2 km of 4 m texels: twice the heightfield's spacing, whose samples the
// lines step through.
constexpr uint32_t kTerrainShadowSize = 2048;
constexpr float kTerrainShadowTexel = 4.0f;
// Below it receivers keep the march: a grazing sun's shadow heights change
// faster than the map resolves.
constexpr float kMinSunElevation = 1.0f * 3.14159265f / 180.0f;
constexpr float kRebuildAngle = 0.02f * 3.14159265f / 180.0f;
constexpr float kRebuildDistance = 1000.0f;
constexpr bool kAlwaysRebuild = false;

} // namespace

void Renderer::Impl::destroyTerrainShadow() {
  if (terrainShadowPipeline != VK_NULL_HANDLE) {
    vkDestroyPipeline(device.device, terrainShadowPipeline, nullptr);
    vkDestroyPipelineLayout(device.device, terrainShadowLayout, nullptr);
    terrainShadowPipeline = VK_NULL_HANDLE;
    terrainShadowLayout = VK_NULL_HANDLE;
  }
  destroyBuffer(terrainShadowMap);
  terrainShadowValid = false;
}

void Renderer::Impl::updateTerrainShadow(Vec3 sun, Vec3 cameraPosition,
                                         VkDeviceAddress sceneAddress, SceneData &scene) {
  terrainShadowPending = false;
  sun = normalize(sun);
  const float horizontal = std::sqrt(sun.x * sun.x + sun.z * sun.z);
  if (terrain.width == 0 || std::atan2(sun.y, horizontal) < kMinSunElevation) {
    return;
  }
  if (terrainShadowPipeline == VK_NULL_HANDLE) {
    terrainShadowMap = createBuffer(VkDeviceSize{kTerrainShadowSize} * kTerrainShadowSize * 8,
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                    0);
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
  TerrainShadowConstants &built = terrainShadowBuild;
  const float half = 0.5f * static_cast<float>(kTerrainShadowSize) * kTerrainShadowTexel;
  const float centreX = built.origin[0] + half * (built.along[0] + built.across[0]);
  const float centreZ = built.origin[1] + half * (built.along[1] + built.across[1]);
  const bool stale =
      kAlwaysRebuild || !terrainShadowValid || length(cross(sun, terrainShadowSun)) > std::sin(kRebuildAngle) ||
      std::hypot(cameraPosition.x - centreX, cameraPosition.z - centreZ) > kRebuildDistance;
  if (stale) {
    // Texel-snapped in the sun's frame, so the map doesn't swim as the
    // camera moves.
    const auto snap = [](float v) {
      return std::round(v / kTerrainShadowTexel) * kTerrainShadowTexel;
    };
    const float a = snap(cameraPosition.x * alongX + cameraPosition.z * alongZ) - half;
    const float c = snap(cameraPosition.x * acrossX + cameraPosition.z * acrossZ) - half;
    built.scene = sceneAddress;
    built.texels = terrainShadowMap.address;
    built.origin[0] = a * alongX + c * acrossX;
    built.origin[1] = a * alongZ + c * acrossZ;
    built.along[0] = alongX;
    built.along[1] = alongZ;
    built.across[0] = acrossX;
    built.across[1] = acrossZ;
    built.texel = kTerrainShadowTexel;
    built.size = kTerrainShadowSize;
    built.tanE = sun.y / horizontal;
    built.maxLength = static_cast<float>(kShadowRayLength);
    terrainShadowSun = sun;
    terrainShadowValid = true;
    terrainShadowPending = true;
  }
  // Each frame's scene buffer (the terrain) is the one the build reads.
  built.scene = sceneAddress;
  scene.terrainShadowMode = 1;
  scene.terrainShadowTexels = terrainShadowMap.address;
  scene.terrainShadowOrigin[0] = built.origin[0];
  scene.terrainShadowOrigin[1] = built.origin[1];
  scene.terrainShadowAlong[0] = built.along[0];
  scene.terrainShadowAlong[1] = built.along[1];
  scene.terrainShadowAcross[0] = built.across[0];
  scene.terrainShadowAcross[1] = built.across[1];
  scene.terrainShadowTexel = built.texel;
  scene.terrainShadowSize = built.size;
  // The occluder line's height change across the sun disk's diameter, per
  // meter of horizontal occluder distance.
  const float cosE = horizontal / length(sun);
  scene.terrainShadowPenumbra = 2.0f * std::tan(scene.sunAngularRadius) / (cosE * cosE);
}

void Renderer::Impl::recordTerrainShadow(VkCommandBuffer cmd) {
  if (!terrainShadowPending) {
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
  vkCmdPushConstants(cmd, terrainShadowLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(TerrainShadowConstants), &terrainShadowBuild);
  vkCmdDispatch(cmd, kTerrainShadowSize, 1, 1);
  memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
  terrainShadowPending = false;
}

} // namespace meridian
