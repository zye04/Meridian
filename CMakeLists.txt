cmake_minimum_required(VERSION 3.28)
project(Meridian LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

add_compile_options(-Wall -Wextra -Wpedantic)
add_compile_options($<$<CONFIG:Debug>:-fsanitize=address,undefined>)
add_link_options($<$<CONFIG:Debug>:-fsanitize=address,undefined>)

add_subdirectory(engine)
add_subdirectory(game)
add_subdirectory(tools/cook)
add_subdirectory(tests)
