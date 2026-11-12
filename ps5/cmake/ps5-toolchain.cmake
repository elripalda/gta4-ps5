# [ps5 port] CMake toolchain for building GTA IV as a native PS5 homebrew app
# (the whole command: ps5/BUILD.md).
#
#   cmake -S . -B out/ps5 -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=ps5/cmake/ps5-toolchain.cmake \
#     -DPS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk \
#     -DREXSDK_DIR=/path/to/rexglue-sdk
#
# Uses the PS5 payload SDK (mihawk-99 fork, as pinned by UnleashedRecomp-
# Prospero) for clang, headers, libc++ 18 and the kernel stubs.

if(NOT PS5_PAYLOAD_SDK)
  set(PS5_PAYLOAD_SDK "$ENV{PS5_PAYLOAD_SDK}")
endif()
if(NOT PS5_PAYLOAD_SDK OR NOT EXISTS "${PS5_PAYLOAD_SDK}/toolchain/prospero.cmake")
  message(FATAL_ERROR "Set PS5_PAYLOAD_SDK to the PS5 payload SDK root")
endif()
find_program(PS5_F02_PYTHON NAMES python3 REQUIRED)
execute_process(
  COMMAND "${PS5_F02_PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../tools/prepare_f02_sdk.py"
          --verify "${PS5_PAYLOAD_SDK}"
  RESULT_VARIABLE PS5_F02_SDK_RESULT)
if(NOT PS5_F02_SDK_RESULT EQUAL 0)
  message(FATAL_ERROR "Prepare a clean F02 SDK with ps5/tools/prepare_f02_sdk.py")
endif()
# Pass it through to try_compile projects.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PS5_PAYLOAD_SDK PS5_F02_PYTHON)

include("${PS5_PAYLOAD_SDK}/toolchain/prospero.cmake")
set(CMAKE_VERBOSE_MAKEFILE OFF CACHE BOOL "" FORCE)

# Zen 2 (the PS5 CPU); emulated TLS (the PS5 ABI); libc++ 18 needs
# -fexperimental-library for std::jthread / stop_token; Linux names mapped to
# FreeBSD ones (rex/ps5_compat.h, force-included once the SDK path is known).
set(PS5_COMMON_FLAGS
  "-march=znver2 -femulated-tls -fno-stack-protector -fno-plt -fdenormal-fp-math=ieee -fvisibility-nodllstorageclass=default")
set(CMAKE_C_FLAGS_INIT "${PS5_COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${PS5_COMMON_FLAGS} -fexperimental-library -frtti -fexceptions")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fexperimental-library")

# No desktop windowing / audio / input in SDL on the PS5: the app gets the
# display, the DualSense and audio from the PS5 platform layer instead.
set(REX_PS5 ON CACHE BOOL "" FORCE)
