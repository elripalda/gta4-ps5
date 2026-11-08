cmake_minimum_required(VERSION 3.20)
project(gta4_frontend_controller_patch LANGUAGES C CXX)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(F01_LIBMSPACK_DIR "${REPO_ROOT}/third_party/libmspack/libmspack/mspack" CACHE PATH
    "libmspack mspack source directory used for LZX decoding")

if(NOT EXISTS "${F01_LIBMSPACK_DIR}/lzxd.c")
    message(FATAL_ERROR
        "libmspack sources were not found at ${F01_LIBMSPACK_DIR}. "
        "Configure through the repository root so the dependency patches run, or set "
        "-DF01_LIBMSPACK_DIR to a patched copy.")
endif()

add_executable(gta4_frontend_controller_patch
    main.cpp
    sha256.cpp
    host_lzx.cpp
    "${REPO_ROOT}/glue/rexglue-sdk-main/gta4-recomp/src/frontend_controller_patch.cpp"
    "${F01_LIBMSPACK_DIR}/lzxd.c"
)

target_compile_features(gta4_frontend_controller_patch PRIVATE cxx_std_20)
target_include_directories(gta4_frontend_controller_patch PRIVATE
    "${REPO_ROOT}/glue/rexglue-sdk-main/include"
    "${REPO_ROOT}/glue/rexglue-sdk-main/gta4-recomp/src"
    "${F01_LIBMSPACK_DIR}"
)
if(MSVC)
    target_compile_options(gta4_frontend_controller_patch PRIVATE /W4)
else()
    target_compile_options(gta4_frontend_controller_patch PRIVATE -Wall -Wextra)
endif()
