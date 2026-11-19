add_executable(meridian-cook cook.cpp)
target_compile_features(meridian-cook PRIVATE cxx_std_20)
target_link_libraries(meridian-cook PRIVATE fastgltf basisu_encoder)
