add_library(meridian STATIC src/hello.cpp)

target_include_directories(meridian PUBLIC include)
target_compile_features(meridian PUBLIC cxx_std_20)
