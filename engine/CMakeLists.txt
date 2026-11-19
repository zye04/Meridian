find_package(glfw3 REQUIRED)
find_package(Vulkan REQUIRED)

include(FetchContent)
FetchContent_Declare(vk-bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
    GIT_TAG v1.4.363)
FetchContent_MakeAvailable(vk-bootstrap)

add_library(meridian STATIC src/window.cpp src/renderer.cpp)

target_include_directories(meridian PUBLIC include)
target_compile_features(meridian PUBLIC cxx_std_20)
target_link_libraries(meridian PRIVATE glfw Vulkan::Vulkan vk-bootstrap::vk-bootstrap)
