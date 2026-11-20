cmake_minimum_required(VERSION 3.28)
project(Meridian LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

add_compile_options(-Wall -Wextra -Wpedantic)
add_compile_options($<$<CONFIG:Debug>:-fsanitize=address,undefined>)
add_link_options($<$<CONFIG:Debug>:-fsanitize=address,undefined>)

option(MERIDIAN_DLSS "Build the DLSS module (NVIDIA SDK licence)" ON)

add_subdirectory(engine)
if(MERIDIAN_DLSS)
  add_subdirectory(engine/dlss)
endif()
add_subdirectory(game)
add_subdirectory(tools/cook)
add_subdirectory(tests)
