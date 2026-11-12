cmake_minimum_required(VERSION 3.25)
project(f02_host_gates LANGUAGES C CXX)
enable_testing()
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(REX_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../glue/rexglue-sdk-main")
set(F02_SDK_SOURCE "" CACHE PATH "Prepared SDK's _f02_source directory")
if(NOT EXISTS "${F02_SDK_SOURCE}/platform/src/shm.c")
  message(FATAL_ERROR "Set F02_SDK_SOURCE to the prepared SDK's _f02_source directory")
endif()
option(F02_SANITIZE "Enable ASan and UBSan" OFF)
if(F02_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
foreach(suite return_to_hub cooperative_drain save_completion save_quiescence
              cooperative_shutdown return_prompt relaunch_policy session_restart
              game_menu episode_startup keybind_reregistration hsio_watch)
  add_executable(f02_${suite} "${REX_ROOT}/tests/unit/system/f02_${suite}_test.cpp")
  target_compile_definitions(f02_${suite} PRIVATE GTA4_F02_STANDALONE)
  target_compile_options(f02_${suite} PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  target_include_directories(f02_${suite} PRIVATE "${REX_ROOT}/include" "${REX_ROOT}/gta4-recomp/src")
  target_link_libraries(f02_${suite} PRIVATE Threads::Threads)
  add_test(NAME f02.${suite} COMMAND f02_${suite})
endforeach()
target_sources(f02_keybind_reregistration PRIVATE "${REX_ROOT}/src/ui/keybinds.cpp")
target_include_directories(f02_keybind_reregistration BEFORE PRIVATE
  "${REX_ROOT}/tests/unit/system/f02_keybind_stubs")
target_sources(f02_hsio_watch PRIVATE "${REX_ROOT}/src/kernel/xboxkrnl/vdhsio_watch.cpp")
foreach(suite native_confirmation native_teardown native_graphics_cache text_lifetime runtime_inspection)
  set(extra)
  if(suite STREQUAL "native_confirmation")
    list(APPEND extra --simde "${REX_ROOT}/thirdparty/simde")
  endif()
  if(F02_SANITIZE)
    list(APPEND extra --sanitize)
  endif()
  add_test(NAME f02.${suite} COMMAND "${Python3_EXECUTABLE}" -B
    "${REX_ROOT}/tests/unit/system/f02_${suite}_test.py"
    --root "${REX_ROOT}" --cxx "${CMAKE_CXX_COMPILER}" ${extra})
endforeach()
set(motion_extra)
if(F02_SANITIZE)
  list(APPEND motion_extra --sanitize)
endif()
add_test(NAME f02.motion_registration COMMAND "${Python3_EXECUTABLE}" -B
  "${REX_ROOT}/tests/unit/system/gta4_motion_registration_test.py"
  --root "${REX_ROOT}" --cxx "${CMAKE_CXX_COMPILER}" --simde "${REX_ROOT}/thirdparty/simde" ${motion_extra})
add_test(NAME f02.watcher_lifetime COMMAND "${Python3_EXECUTABLE}" -B
  "${REX_ROOT}/tests/unit/system/f02_watcher_lifetime_audit_test.py" --root "${REX_ROOT}")
add_executable(f02_sdk_ownership f02_sdk_ownership_test.c
  "${F02_SDK_SOURCE}/platform/src/shm.c"
  "${F02_SDK_SOURCE}/platform/src/placement.c"
  "${F02_SDK_SOURCE}/platform/tests/host_kernel.c")
target_compile_options(f02_sdk_ownership PRIVATE -Wall -Wextra -Werror -UNDEBUG)
target_include_directories(f02_sdk_ownership PRIVATE
  "${F02_SDK_SOURCE}/platform/include" "${F02_SDK_SOURCE}/platform/src" "${F02_SDK_SOURCE}/platform/tests")
target_link_libraries(f02_sdk_ownership PRIVATE Threads::Threads)
target_link_options(f02_sdk_ownership PRIVATE
  -Wl,--wrap=sceKernelReleaseDirectMemory -Wl,--wrap=sceKernelMunmap -Wl,--wrap=sceKernelReserveVirtualRange)
add_test(NAME f02.sdk_ownership COMMAND f02_sdk_ownership)
set_tests_properties(f02.native_confirmation f02.native_teardown f02.native_graphics_cache
  f02.text_lifetime f02.runtime_inspection f02.motion_registration PROPERTIES TIMEOUT 240)
