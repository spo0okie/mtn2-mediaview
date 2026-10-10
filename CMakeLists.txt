cmake_minimum_required(VERSION 3.20)
project(mediaview CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(mediaview SHARED src/plugin.cpp)
target_include_directories(mediaview PRIVATE include src)
target_compile_definitions(mediaview PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(mediaview PRIVATE /W4 /EHsc /utf-8)
target_link_libraries(mediaview PRIVATE user32 gdi32)

# MTN2 loads plugins\<id>\*.dll; dist\mtn.mediaview is that folder, ready to copy.
set(DIST "${CMAKE_SOURCE_DIR}/dist/mtn.mediaview")
set_target_properties(mediaview PROPERTIES
  PREFIX ""
  RUNTIME_OUTPUT_DIRECTORY "${DIST}"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${DIST}"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${DIST}")
