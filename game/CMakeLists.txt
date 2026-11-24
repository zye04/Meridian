add_executable(game src/main.cpp src/overlay.cpp src/perf_overlay.cpp)
target_link_libraries(game PRIVATE meridian)
# geo_channels.h: what the geo channels mean (header only, shared with the cook).
target_include_directories(game PRIVATE ${CMAKE_SOURCE_DIR}/tools/cook)
target_compile_definitions(game PRIVATE MERIDIAN_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
set(game_runtime_files ${CMAKE_CURRENT_SOURCE_DIR}/credits.txt)
if(MERIDIAN_DLSS)
  target_link_libraries(game PRIVATE meridian_dlss)
  target_compile_definitions(game PRIVATE MERIDIAN_HAS_DLSS)
  list(APPEND game_runtime_files ${MERIDIAN_DLSS_RUNTIME_FILES})
endif()
# NGX loads the DLSS snippets from the binary's directory.
add_custom_command(TARGET game POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${game_runtime_files} $<TARGET_FILE_DIR:game>)
