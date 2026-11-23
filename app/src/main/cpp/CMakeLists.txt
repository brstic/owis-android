cmake_minimum_required(VERSION 3.22.1)
project(owis LANGUAGES CXX)

add_library(owis SHARED owis_jni.cpp)

target_compile_features(owis PRIVATE cxx_std_17)
set_target_properties(owis PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(owis PRIVATE -O2 -fexceptions -Wall -Wno-unused-function)
target_include_directories(owis PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_library(log-lib log)
target_link_libraries(owis ${log-lib})
