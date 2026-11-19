add_executable(game src/main.cpp)
target_link_libraries(game PRIVATE meridian)
target_compile_definitions(game PRIVATE MERIDIAN_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
