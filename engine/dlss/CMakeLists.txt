# The DLSS module: NVIDIA's NGX SDK behind meridian's UpscalerProvider. Kept
# out of the core library because the SDK licence forbids making the SDK
# subject to an open-source licence; MERIDIAN_DLSS=OFF builds without it.

find_package(Vulkan REQUIRED)
include(FetchContent)
# NVIDIA DLSS SDK v310.9.16, the newest 310.x tag on 2026-11-20
# (git ls-remote --tags), commit 9981c6350dc58c0ce21f024fd65e31e851c31689.
# A commit tarball: a git clone would fetch every release's binaries.
FetchContent_Declare(dlss_sdk
    URL https://github.com/NVIDIA/DLSS/archive/9981c6350dc58c0ce21f024fd65e31e851c31689.tar.gz
    URL_HASH SHA256=701cec0cd869566b9f3deb7a30b318521f8f1e2b2fb73081741ea5427ae94dca)
FetchContent_MakeAvailable(dlss_sdk)

set(DLSS_SDK_LIB ${dlss_sdk_SOURCE_DIR}/lib/Linux_x86_64)

add_library(meridian_dlss STATIC src/dlss.cpp)
target_include_directories(meridian_dlss PUBLIC include)
target_include_directories(meridian_dlss SYSTEM PRIVATE ${dlss_sdk_SOURCE_DIR}/include)
target_link_libraries(meridian_dlss
    PUBLIC meridian Vulkan::Vulkan
    PRIVATE ${DLSS_SDK_LIB}/libnvsdk_ngx.a ${CMAKE_DL_LIBS})

# Loaded by the driver's NGX core from the binary's directory: Super
# Resolution and Ray Reconstruction (release builds of the snippets; the
# dev ones draw an on-screen watermark and must not ship).
set(MERIDIAN_DLSS_RUNTIME_FILES
    ${DLSS_SDK_LIB}/rel/libnvidia-ngx-dlss.so.310.9.16
    ${DLSS_SDK_LIB}/rel/libnvidia-ngx-dlssd.so.310.9.16
    ${CMAKE_CURRENT_SOURCE_DIR}/dlss_third_party_notices.txt
    PARENT_SCOPE)
