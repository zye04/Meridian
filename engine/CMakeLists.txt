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
FetchContent_MakeAvailable(vk-bootstrap VulkanMemoryAllocator basis_universal
                           fastgltf mikktspace)

add_library(mikktspace STATIC ${mikktspace_SOURCE_DIR}/mikktspace.c)
target_include_directories(mikktspace SYSTEM PUBLIC ${mikktspace_SOURCE_DIR})
target_compile_options(mikktspace PRIVATE -w)
target_compile_options(fastgltf PRIVATE -w)
target_compile_options(fastgltf_simdutf PRIVATE -w)
# Third-party code: no warnings, and always optimized and unsanitized. The
# encoder is unusably slow in Debug, and its UB reports are upstream bugs we
# can't act on.
target_compile_options(basisu_encoder PRIVATE -w -O2 -fno-sanitize=all)
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

foreach(source
    mesh.vert mesh.frag fullscreen.vert tonemap.frag taa.comp histogram.comp
    exposure.comp sky.vert sky.frag transmittance_lut.comp
    multiscattering_lut.comp sky_view_lut.comp aerial_perspective_lut.comp
    sky_irradiance.comp dfg_lut.comp cull.comp)
  add_shader(${source} ${source})
endforeach()
add_shader(mesh.frag mesh_masked.frag ALPHA_MASK)
add_shader(mesh.frag mesh_blended.frag ALPHA_BLEND)

get_property(SHADER_OUTPUTS GLOBAL PROPERTY MERIDIAN_SHADERS)
add_custom_target(shaders DEPENDS ${SHADER_OUTPUTS})

add_library(meridian STATIC
    src/model.cpp
    src/render/renderer_frame.cpp
    src/render/renderer_resources.cpp
    src/render/renderer_setup.cpp
    src/render/vulkan_util.cpp
    src/terrain.cpp
    src/texture.cpp
    src/vma.cpp
    src/window.cpp)
add_dependencies(meridian shaders)

target_include_directories(meridian PUBLIC include)
target_compile_features(meridian PUBLIC cxx_std_20)
target_compile_definitions(meridian PRIVATE MERIDIAN_SHADER_DIR="${SHADER_DIR}")
target_link_libraries(meridian PRIVATE
    glfw
    Vulkan::Vulkan
    vk-bootstrap::vk-bootstrap
    GPUOpen::VulkanMemoryAllocator
    basisu_encoder
    fastgltf
    mikktspace)
