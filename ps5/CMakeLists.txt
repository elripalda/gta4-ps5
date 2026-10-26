# GTA IV for PS5: the Graine ReXGlue consumer (glue/) built as one static PS5
# homebrew program. Build steps: ps5/BUILD.md.
cmake_minimum_required(VERSION 3.25)
project(gta4-ps5 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS OFF)
add_compile_definitions(FMT_USE_CONSTEVAL=0)

# Keep the build machine's source paths out of diagnostics and debug information.
get_filename_component(PS5_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
add_compile_options("-ffile-prefix-map=${PS5_SOURCE_ROOT}=/src")

set(LIBERTY_RECOMP_TARGET_PLATFORM "ps5" CACHE STRING "" FORCE)
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../glue/rexglue-sdk-main" CACHE PATH "")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../glue" glue)
