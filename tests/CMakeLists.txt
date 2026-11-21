include(FetchContent)
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.5.3)
FetchContent_MakeAvailable(doctest)

add_executable(tests vec3_test.cpp mat4_test.cpp texture_test.cpp terrain_test.cpp
    cluster_test.cpp model_test.cpp tree_cook_test.cpp placement_test.cpp
    grade_test.cpp upscaler_test.cpp frame_stats_test.cpp weather_test.cpp
    clouds_test.cpp farfield_test.cpp gi_test.cpp shadow_cascades_test.cpp
    protovoxel_test.cpp)
target_link_libraries(tests PRIVATE meridian meridian-cook-lib basisu_encoder fastgltf
    doctest::doctest_with_main)
add_test(NAME tests COMMAND tests)
