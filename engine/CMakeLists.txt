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
FetchContent_MakeAvailable(vk-bootstrap VulkanMemoryAllocator)

set(SHADER_DIR ${CMAKE_BINARY_DIR}/shaders)
set(SHADER_SOURCES shaders/mesh.vert shaders/mesh.frag)
foreach(source ${SHADER_SOURCES})
  get_filename_component(name ${source} NAME)
  set(output ${SHADER_DIR}/${name}.spv)
  add_custom_command(
    OUTPUT ${output}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
    COMMAND Vulkan::glslc --target-env=vulkan1.3 -O
            ${CMAKE_CURRENT_SOURCE_DIR}/${source} -o ${output}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${source}
    VERBATIM)
  list(APPEND SHADER_OUTPUTS ${output})
endforeach()
add_custom_target(shaders DEPENDS ${SHADER_OUTPUTS})

add_library(meridian STATIC
    src/renderer.cpp
    src/terrain.cpp
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
    GPUOpen::VulkanMemoryAllocator)
