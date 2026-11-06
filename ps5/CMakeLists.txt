# GTA IV for PS5: the Graine ReXGlue consumer (glue/) built as one static PS5
# homebrew program. Build steps: ps5/BUILD.md.
cmake_minimum_required(VERSION 3.25)
project(gta4-ps5 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS OFF)
add_compile_definitions(FMT_USE_CONSTEVAL=0)
# ThinLTO across the whole program (ps5_package.sh's link finishes it): the
# ~24k recompiled game functions sit in 90 files, and a call from one file to
# a small function in another could not be inlined without it.
option(GTA4_PS5_THINLTO "ThinLTO for the PS5 program" ON)
if(GTA4_PS5_THINLTO)
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-flto=thin>")
endif()

set(LIBERTY_RECOMP_TARGET_PLATFORM "ps5" CACHE STRING "" FORCE)
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../glue/rexglue-sdk-main" CACHE PATH "")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../glue" glue)
