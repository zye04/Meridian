include(FetchContent)
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.5.3)
FetchContent_MakeAvailable(doctest)

add_executable(tests vec3_test.cpp mat4_test.cpp texture_test.cpp terrain_test.cpp)
target_link_libraries(tests PRIVATE meridian basisu_encoder doctest::doctest_with_main)
add_test(NAME tests COMMAND tests)
