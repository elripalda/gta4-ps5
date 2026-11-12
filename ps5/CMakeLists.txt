# GTA IV for PS5: the Graine ReXGlue consumer (glue/) built as one static PS5
# homebrew program. Build steps: ps5/BUILD.md.
cmake_minimum_required(VERSION 3.25)
project(gta4-ps5 LANGUAGES C CXX)

include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES ps5platform)
check_c_source_compiles("#include <ps5platform/shm.h>
int main(void) {
  struct ps5_shm shm = {0};
  struct ps5_shm_unmap_report report = {0};
  return ps5_shm_destroy_reported(&shm) + ps5_shm_unmap_reported(0, 0, 0, &report);
}" F02_SDK_REPORTED_APIS_LINK)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT F02_SDK_REPORTED_APIS_LINK)
  message(FATAL_ERROR "The prepared SDK lifecycle APIs do not compile and link")
endif()

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
