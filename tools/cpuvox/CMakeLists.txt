add_executable(meridian-cpuvox cpuvox.cpp)
target_compile_features(meridian-cpuvox PRIVATE cxx_std_20)
target_link_libraries(meridian-cpuvox PRIVATE fastgltf zstd)
