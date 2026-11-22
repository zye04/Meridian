# The tree cook and KTX2 encoding are a library so the tests can link them.
add_library(meridian-cook-lib STATIC ktx2.cpp leaf_trace.cpp tree.cpp)
target_compile_features(meridian-cook-lib PUBLIC cxx_std_20)
target_include_directories(meridian-cook-lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(meridian-cook-lib PUBLIC basisu_encoder fastgltf PRIVATE meshoptimizer)

add_executable(meridian-cook cook.cpp)
target_compile_features(meridian-cook PRIVATE cxx_std_20)
target_link_libraries(meridian-cook PRIVATE meridian-cook-lib meridian fastgltf basisu_encoder)
