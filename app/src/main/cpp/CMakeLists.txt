cmake_minimum_required(VERSION 3.22.1)
project(owis LANGUAGES CXX C)

# Vendored Apache-2.0 OpenPort 2.0 library (github.com/roffe/libj2534_openport2), built with the Android
# USB byte-I/O backend instead of its Linux serial transport.
add_library(owis SHARED owis_jni.cpp op2lib/op2.c)

target_compile_features(owis PRIVATE cxx_std_17)
set_target_properties(owis PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(owis PRIVATE -O2 -fexceptions -Wall -Wno-unused-function)
target_compile_definitions(owis PRIVATE OP2_ANDROID_USB)
target_include_directories(owis PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_library(log-lib log)
target_link_libraries(owis ${log-lib})
