cmake_minimum_required(VERSION 3.28)
project(Meridian LANGUAGES C CXX)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  if(NOT CMAKE_C_COMPILER_LAUNCHER)
    set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
  endif()
  if(NOT CMAKE_CXX_COMPILER_LAUNCHER)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
  endif()
endif()

find_program(MOLD_PROGRAM mold)
if(MOLD_PROGRAM)
  add_link_options(-fuse-ld=mold)
endif()

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
add_subdirectory(tools/cpuvox)
add_subdirectory(tests)
