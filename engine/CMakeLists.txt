find_package(glfw3 REQUIRED)
find_package(Vulkan REQUIRED COMPONENTS glslc)

include(FetchContent)
FetchContent_Declare(vk-bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
    GIT_TAG v1.4.363)
FetchContent_Declare(VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.5.0
    SYSTEM)
# Basis Universal: KTX2 textures. The engine only uses the transcoder; the
# encoder (in the same static library, linked only where used) is for tools.
set(BASISU_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(basis_universal
    URL https://github.com/BinomialLLC/basis_universal/archive/refs/tags/v2_50.tar.gz
    URL_HASH SHA256=216e49e1f4213d4bfa4afaa07527e16bac28533dddd444197d3aa19230ac130c
    EXCLUDE_FROM_ALL
    SYSTEM)
FetchContent_Declare(fastgltf
    GIT_REPOSITORY https://github.com/spnda/fastgltf.git
    GIT_TAG v1.0.1
    SYSTEM)
# MikkTSpace: the standard tangent basis that normal-map bakers use.
FetchContent_Declare(mikktspace
    GIT_REPOSITORY https://github.com/mmikk/MikkTSpace.git
    GIT_TAG 3e895b49d05ea07e4c2133156cfa94369e19e409)
# meshoptimizer: simplification and clustering; its demo/clusterlod.h (cluster
# LOD hierarchies, Nanite-style) is vendored in third_party/clusterlod.
FetchContent_Declare(meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG v1.3
    SYSTEM)
# Dear ImGui: the in-game overlay. It has no CMake; the library is built here.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.93.0
    GIT_SHALLOW TRUE
    SYSTEM)
FetchContent_MakeAvailable(vk-bootstrap VulkanMemoryAllocator basis_universal
                           fastgltf mikktspace meshoptimizer imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw Vulkan::Vulkan)
target_compile_options(imgui PRIVATE -w)

add_library(mikktspace STATIC ${mikktspace_SOURCE_DIR}/mikktspace.c)
target_include_directories(mikktspace SYSTEM PUBLIC ${mikktspace_SOURCE_DIR})
target_compile_options(mikktspace PRIVATE -w)
target_compile_options(fastgltf PRIVATE -w)
target_compile_options(fastgltf_simdutf PRIVATE -w)
# Third-party code: no warnings, and always optimized and unsanitized. The
# encoder is unusably slow in Debug, and its UB reports are upstream bugs we
# can't act on.
target_compile_options(basisu_encoder PRIVATE -w -O2 -fno-sanitize=all)
target_compile_options(meshoptimizer PRIVATE -w -O2 -fno-sanitize=all)
target_include_directories(basisu_encoder SYSTEM INTERFACE
    ${basis_universal_SOURCE_DIR}/transcoder
    ${basis_universal_SOURCE_DIR}/encoder)

set(SHADER_DIR ${CMAKE_BINARY_DIR}/shaders)
file(GLOB SHADER_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.glsl)

# Compiles shaders/<source> to <output>.spv; extra arguments become -D
# defines, so one source can produce several variants.
function(add_shader source output)
  set(defines "")
  foreach(define ${ARGN})
    list(APPEND defines -D${define})
  endforeach()
  set(spv ${SHADER_DIR}/${output}.spv)
  add_custom_command(
    OUTPUT ${spv}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
    COMMAND Vulkan::glslc --target-env=vulkan1.3 -O ${defines}
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${source} -o ${spv}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${source} ${SHADER_INCLUDES}
    VERBATIM)
  set_property(GLOBAL APPEND PROPERTY MERIDIAN_SHADERS ${spv})
endfunction()

# Swaps the terrain shadow march for a brute-force linear one, for A/B
# checks.
option(MERIDIAN_TERRAIN_MARCH_BRUTE_FORCE "Brute-force terrain shadow march" OFF)
set(MARCH_DEFINES "")
if(MERIDIAN_TERRAIN_MARCH_BRUTE_FORCE)
  set(MARCH_DEFINES TERRAIN_MARCH_BRUTE_FORCE)
endif()

foreach(source
    mesh.vert fullscreen.vert tonemap.frag taa.comp histogram.comp
    exposure.comp sky.vert sky.frag transmittance_lut.comp
    multiscattering_lut.comp sky_view_lut.comp aerial_perspective_lut.comp
    sky_irradiance.comp dfg_lut.comp cull.comp depth_pyramid.comp
    cluster_traverse.comp mesh.mesh terrain_select.comp
    terrain.mesh terrain_normals.comp scan.comp cell_cull.comp tlas_fill.comp
    grade_lut.comp sharpen.comp contact_shadows.comp cloud_noise.comp weather_map.comp
    clouds_march.comp clouds_resolve.comp clouds_composite.comp cloud_shadow.comp
    cloud_shadow_blur.comp sky_capture.comp voxel.mesh voxel_instance.mesh reference.comp)
  add_shader(${source} ${source})
endforeach()
add_shader(mesh.frag mesh.frag ${MARCH_DEFINES})
add_shader(mesh.frag mesh_masked.frag ALPHA_MASK ${MARCH_DEFINES})
add_shader(mesh.frag mesh_masked_depth.frag ALPHA_MASK DEPTH_ONLY)
add_shader(mesh.frag mesh_blended.frag ALPHA_BLEND ${MARCH_DEFINES})
add_shader(mesh.frag terrain.frag TERRAIN ${MARCH_DEFINES})
add_shader(shadow.mesh shadow.mesh)
add_shader(shadow_masked.frag shadow_masked.frag)
add_shader(voxel.frag voxel.frag ${MARCH_DEFINES})
add_shader(voxel.frag voxel_shadow.frag SHADOW ${MARCH_DEFINES})
add_shader(gi_probes.comp gi_probes.comp ${MARCH_DEFINES})
add_shader(gi_ground.comp gi_ground.comp ${MARCH_DEFINES})
add_shader(gi_albedo.comp gi_albedo_scatter.comp SCATTER ${MARCH_DEFINES})
add_shader(gi_albedo.comp gi_albedo_resolve.comp ${MARCH_DEFINES})
add_shader(cluster_cull.comp cluster_cull.comp ${MARCH_DEFINES})
add_shader(voxel_instance.frag voxel_instance.frag ${MARCH_DEFINES})
add_shader(voxel_instance.frag voxel_band.frag VOXEL_BAND ${MARCH_DEFINES})
add_shader(voxel_bricks.comp voxel_bricks.comp ${MARCH_DEFINES})
add_shader(placement.comp placement_count.comp PLACEMENT_COUNT)
add_shader(placement.comp placement_write.comp PLACEMENT_WRITE)
add_shader(placement.comp placement_stream.comp PLACEMENT_STREAM)
add_shader(placement.comp placement_forest.comp PLACEMENT_FOREST)
add_shader(bloom.comp bloom_down.comp)
add_shader(bloom.comp bloom_up.comp BLOOM_UPSAMPLE)
add_shader(bloom.comp bloom_composite.comp BLOOM_COMPOSITE)
add_shader(farfield_bake.comp farfield_bake.comp)
add_shader(farfield_splat.comp farfield_occupancy.comp FARFIELD_OCCUPANCY)
add_shader(farfield_splat.comp farfield_accumulate.comp FARFIELD_ACCUMULATE)
add_shader(farfield_splat.comp farfield_mask.comp FARFIELD_MASK)
add_shader(farfield_splat.comp farfield_pack.comp FARFIELD_PACK)
add_shader(farfield_check.comp farfield_check.comp)
add_shader(farfield_shadow.comp farfield_shadow.comp)
add_shader(terrain_shadow.comp terrain_shadow.comp)

get_property(SHADER_OUTPUTS GLOBAL PROPERTY MERIDIAN_SHADERS)
add_custom_target(shaders DEPENDS ${SHADER_OUTPUTS})

add_library(meridian STATIC
    src/cluster.cpp
    src/clusterlod.cpp
    src/frame_stats.cpp
    src/farfield.cpp
    src/protovoxel.cpp
    src/grade.cpp
    src/model.cpp
    src/placement.cpp
    src/shadow_cascades.cpp
    src/render/renderer_clouds.cpp
    src/render/renderer_reference.cpp
    src/render/renderer_gi.cpp
    src/render/renderer_shadows.cpp
    src/render/renderer_farfield.cpp
    src/render/renderer_farshadow.cpp
    src/render/renderer_frame.cpp
    src/render/renderer_placement.cpp
    src/render/renderer_resources.cpp
    src/render/renderer_setup.cpp
    src/render/renderer_terrain.cpp
    src/render/renderer_terrain_shadow.cpp
    src/render/taa_upscaler.cpp
    src/render/vulkan_util.cpp
    src/gi.cpp
    src/terrain.cpp
    src/geo.cpp
    src/texture.cpp
    src/weather.cpp
    src/vma.cpp
    src/window.cpp)
add_dependencies(meridian shaders)

target_include_directories(meridian PUBLIC include)
target_compile_features(meridian PUBLIC cxx_std_20)
target_compile_definitions(meridian PRIVATE MERIDIAN_SHADER_DIR="${SHADER_DIR}")
target_link_libraries(meridian PUBLIC imgui)
target_link_libraries(meridian PRIVATE
    glfw
    Vulkan::Vulkan
    vk-bootstrap::vk-bootstrap
    GPUOpen::VulkanMemoryAllocator
    basisu_encoder
    fastgltf
    mikktspace
    meshoptimizer)
target_include_directories(meridian SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/clusterlod)
set_source_files_properties(src/clusterlod.cpp PROPERTIES
    COMPILE_OPTIONS "-w;-O2;-fno-sanitize=all")
