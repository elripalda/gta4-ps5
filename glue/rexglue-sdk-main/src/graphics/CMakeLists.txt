# rexgpu-xenos: Xenos GPU emulation plugin (ported from Xenia)
# Namespace: rex::graphics
#
# Runtime-loaded via rex::system::LoadGpuPlugin; consumers never link this
# target. The Xenos format layer is compiled into rexcore.

set(REXGPU_XENOS_SOURCES
    # Core GPU types
    flags.cpp
    register_file.cpp
    registers.cpp
    pipeline/shader/shader.cpp
    format/ucode.cpp
    sampler_info.cpp

    # GPU system
    graphics_system.cpp
    command_processor.cpp
    trace_writer.cpp
    util/draw_extent_estimator.cpp
    util/draw.cpp
    packet_disassembler.cpp
    primitive_processor.cpp
    pipeline/render_target/cache.cpp
    shared_memory.cpp
    pipeline/texture/cache.cpp
    pipeline/shader/interpreter.cpp
    pipeline/shader/translator.cpp
    pipeline/shader/translator_disasm.cpp
)

# SPIR-V shader translation
set(REXGPU_XENOS_SPIRV_SOURCES
    pipeline/shader/spirv_builder.cpp
    pipeline/shader/spirv.cpp
    pipeline/shader/spirv_translator.cpp
    pipeline/shader/spirv_translator_alu.cpp
    pipeline/shader/spirv_translator_fetch.cpp
    pipeline/shader/spirv_translator_memexport.cpp
    pipeline/shader/spirv_translator_rb.cpp
)

# Vulkan backend
set(REXGPU_XENOS_VULKAN_SOURCES
    vulkan/graphics_system.cpp
    vulkan/deferred_command_buffer.cpp
    vulkan/command_processor.cpp
    vulkan/pipeline_cache.cpp
    vulkan/primitive_processor.cpp
    vulkan/render_target_cache.cpp
    vulkan/shader.cpp
    vulkan/shared_memory.cpp
    vulkan/texture_cache.cpp
)

# D3D12 backend
if(REXGLUE_USE_D3D12)
    # DXBC third-party dependency
    set(REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES
        ${REXGLUE_ROOT}/thirdparty/dxbc/DXBCChecksum.cpp
    )

    # DXBC shader translation (required for D3D12)
    set(REXGPU_XENOS_DXBC_SOURCES
        pipeline/shader/dxbc.cpp
        pipeline/shader/dxbc_translator.cpp
        pipeline/shader/dxbc_translator_alu.cpp
        pipeline/shader/dxbc_translator_fetch.cpp
        pipeline/shader/dxbc_translator_memexport.cpp
        pipeline/shader/dxbc_translator_om.cpp
    )

    # D3D12 GPU backend
    set(REXGPU_XENOS_D3D12_SOURCES
        d3d12/graphics_system.cpp
        d3d12/command_processor.cpp
        d3d12/primitive_processor.cpp
        d3d12/render_target_cache.cpp
        d3d12/shader.cpp
        d3d12/shared_memory.cpp
        d3d12/texture_cache.cpp
        d3d12/deferred_command_list.cpp
        d3d12/pipeline_cache.cpp
    )
endif()

if(REXGLUE_USE_VULKAN)
    list(APPEND REXGPU_XENOS_SOURCES ${REXGPU_XENOS_SPIRV_SOURCES} ${REXGPU_XENOS_VULKAN_SOURCES})
endif()

if(REXGLUE_USE_D3D12)
    list(APPEND REXGPU_XENOS_SOURCES
        ${REXGPU_XENOS_DXBC_THIRDPARTY_SOURCES}
        ${REXGPU_XENOS_DXBC_SOURCES}
        ${REXGPU_XENOS_D3D12_SOURCES})
endif()

function(rexgpu_configure_plugin target export_name)
    set_target_properties(${target} PROPERTIES
        EXPORT_NAME ${export_name}
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )

    target_include_directories(${target} PRIVATE
        ${REXGLUE_ROOT}/thirdparty/renderdoc
        ${REXGLUE_ROOT}  # For thirdparty/dxbc/DXBCChecksum.h include
    )

    target_compile_options(${target} PRIVATE -Wno-unused-variable)

    # Everything PRIVATE: plugins have no link-time consumers. Never link the
    # OBJECT libraries (rexcore/rexui) here; rexruntime already contains those
    # objects and relinking them would duplicate global state.
    target_link_libraries(${target} PRIVATE
        rexruntime
        xxHash::xxhash
        snappy
    )

    if(ANDROID)
        # Guest vblanks follow the display through AChoreographer.
        target_link_libraries(${target} PRIVATE android)
    endif()

    if(REXGLUE_USE_VULKAN)
        target_link_libraries(${target} PRIVATE
            volk::volk
            glslang::SPIRV
            GPUOpen::VulkanMemoryAllocator
            glslang
            MachineIndependent
            GenericCodeGen
            OSDependent
            OGLCompiler
        )
    endif()

    if(REXGLUE_USE_D3D12)
        target_link_libraries(${target} PRIVATE
            d3d12
            dxgi
            dxguid
        )
    endif()
endfunction()

# [ps5 port] one static program on the PS5: only the gta4-native renderer,
# linked in (gpu_plugin_loader.cpp calls it directly).
if(NOT PS5)
add_library(rexgpu-xenos SHARED
    plugin_main.cpp
    ${REXGPU_XENOS_SOURCES}
)
add_library(rex::gpu-xenos ALIAS rexgpu-xenos)
rexgpu_configure_plugin(rexgpu-xenos gpu-xenos)
endif()

if(REXGLUE_USE_VULKAN)
    if(REXGLUE_BUILD_TESTS)
        add_executable(rex-vulkan-temporal-contract-test EXCLUDE_FROM_ALL
            "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/tests/vulkan_temporal_contract_test.cpp")
        target_include_directories(rex-vulkan-temporal-contract-test PRIVATE "${REXGLUE_ROOT}/src")
        target_link_libraries(rex-vulkan-temporal-contract-test PRIVATE volk::volk fmt::fmt)
        add_executable(rex-vulkan-temporal-archive-test EXCLUDE_FROM_ALL
            "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/vulkan/shader_archive_test.cpp"
            gta4_native/temporal/shader_archive.cpp)
        target_include_directories(rex-vulkan-temporal-archive-test PRIVATE "${REXGLUE_ROOT}/src")
    endif()
    set(GTA4_NATIVE_SHADER_CACHE_ROOT
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/LibertyRecompLib")
    set(GTA4_NATIVE_SMOLV_ROOT
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/XenosRecomp/thirdparty/smol-v/source")
    set(GTA4_NATIVE_ZSTD_ROOT
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/XenosRecomp/thirdparty/zstd")
    set(GTA4_NATIVE_SHADER_OVERRIDE_ROOT
        "${GTA4_NATIVE_SHADER_CACHE_ROOT}/shader_overrides")
    set(GTA4_NATIVE_SHADER_OVERRIDE_MANIFEST
        "${GTA4_NATIVE_SHADER_OVERRIDE_ROOT}/manifest.json")
    set(GTA4_NATIVE_SHADER_OVERRIDE_COMPILER
        "${REXGLUE_ROOT}/tools/gta4_shader_override_compiler.py")
    set(GTA4_NATIVE_SHADER_OVERRIDE_CPP
        "${CMAKE_CURRENT_BINARY_DIR}/generated/gta4_native/shader_override_cache.cpp")
    set(GTA4_NATIVE_SHADER_OVERRIDE_DEPFILE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/gta4_native/shader_override_cache.d")
    if(NOT EXISTS "${GTA4_NATIVE_SHADER_CACHE_ROOT}/shader/shader_cache.cpp")
        message(FATAL_ERROR "GTA IV native shader cache source was not found")
    endif()
    if(NOT EXISTS "${GTA4_NATIVE_SHADER_OVERRIDE_MANIFEST}" OR
       NOT EXISTS "${GTA4_NATIVE_SHADER_OVERRIDE_COMPILER}")
        message(FATAL_ERROR "GTA IV native shader override inputs were not found")
    endif()

    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    find_program(GTA4_NATIVE_GLSLANG_VALIDATOR
        NAMES glslangValidator
        HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
    find_program(GTA4_NATIVE_SPIRV_VAL
        NAMES spirv-val
        HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")

    string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" GTA4_NATIVE_HOST_PROCESSOR)
    if(GTA4_NATIVE_HOST_PROCESSOR MATCHES "arm64|aarch64")
        set(GTA4_NATIVE_DXC_ARCH arm64)
    else()
        set(GTA4_NATIVE_DXC_ARCH x64)
    endif()
    set(GTA4_NATIVE_DXC_ROOT
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/XenosRecomp/thirdparty/dxc-bin")
    set(GTA4_NATIVE_DXC_LIBRARY_PATH "" CACHE PATH
        "Directory containing a DXC shared library compatible with gta4-native shader overrides")
    set(GTA4_NATIVE_SHADER_COMMON
        "${GTA4_NATIVE_SHADER_OVERRIDE_ROOT}/xenos_shader_common.h")
    if(CMAKE_HOST_WIN32)
        # The bundled DXC 1.8 crashes on the Bink pixel shaders here as well
        # (see the macOS branch). GTA4_NATIVE_DXC_LIBRARY_PATH may name the bin
        # directory of a newer DXC release.
        if(GTA4_NATIVE_DXC_LIBRARY_PATH AND EXISTS "${GTA4_NATIVE_DXC_LIBRARY_PATH}/dxc.exe")
            set(GTA4_NATIVE_DXC "${GTA4_NATIVE_DXC_LIBRARY_PATH}/dxc.exe")
        else()
            set(GTA4_NATIVE_DXC
                "${GTA4_NATIVE_DXC_ROOT}/bin/${GTA4_NATIVE_DXC_ARCH}/dxc.exe")
        endif()
    elseif(CMAKE_HOST_APPLE)
        set(GTA4_NATIVE_DXC
            "${GTA4_NATIVE_DXC_ROOT}/bin/${GTA4_NATIVE_DXC_ARCH}/dxc-macos")
        if(NOT GTA4_NATIVE_DXC_LIBRARY_PATH)
            # The bundled DXC 1.8 crashes in SPIRV-Tools AggressiveDCE for GTA IV's
            # Bink pixel shaders. Prefer a separately installed newer DXC when one
            # is available, while retaining the bundled compiler as a fallback.
            find_library(GTA4_NATIVE_HOST_DXCOMPILER
                NAMES dxcompiler libdxcompiler
                PATHS /usr/local/lib /opt/homebrew/lib
                NO_DEFAULT_PATH
                NO_CACHE)
            if(GTA4_NATIVE_HOST_DXCOMPILER)
                get_filename_component(GTA4_NATIVE_DXC_LIBRARY_PATH
                    "${GTA4_NATIVE_HOST_DXCOMPILER}" DIRECTORY)
            else()
                set(GTA4_NATIVE_DXC_LIBRARY_PATH
                    "${GTA4_NATIVE_DXC_ROOT}/lib/${GTA4_NATIVE_DXC_ARCH}")
            endif()
        endif()
    elseif(CMAKE_HOST_UNIX AND GTA4_NATIVE_DXC_ARCH STREQUAL "x64")
        if(NOT GTA4_NATIVE_DXC)
            set(GTA4_NATIVE_DXC "${GTA4_NATIVE_DXC_ROOT}/bin/x64/dxc-linux")
        endif()
        if(NOT GTA4_NATIVE_DXC_LIBRARY_PATH)
            set(GTA4_NATIVE_DXC_LIBRARY_PATH "${GTA4_NATIVE_DXC_ROOT}/lib/x64")
        endif()
    endif()

    set(GTA4_NATIVE_SHADER_OVERRIDE_COMMAND
        "${Python3_EXECUTABLE}"
        "${GTA4_NATIVE_SHADER_OVERRIDE_COMPILER}"
        --manifest "${GTA4_NATIVE_SHADER_OVERRIDE_MANIFEST}"
        --source-root "${GTA4_NATIVE_SHADER_OVERRIDE_ROOT}"
        --output "${GTA4_NATIVE_SHADER_OVERRIDE_CPP}"
        --depfile "${GTA4_NATIVE_SHADER_OVERRIDE_DEPFILE}")
    if(GTA4_NATIVE_DXC AND EXISTS "${GTA4_NATIVE_DXC}")
        list(APPEND GTA4_NATIVE_SHADER_OVERRIDE_COMMAND --dxc "${GTA4_NATIVE_DXC}")
        if(GTA4_NATIVE_DXC_LIBRARY_PATH)
            list(APPEND GTA4_NATIVE_SHADER_OVERRIDE_COMMAND
                --dxc-library-path "${GTA4_NATIVE_DXC_LIBRARY_PATH}")
        endif()
    endif()
    if(GTA4_NATIVE_GLSLANG_VALIDATOR)
        list(APPEND GTA4_NATIVE_SHADER_OVERRIDE_COMMAND
            --glslang-validator "${GTA4_NATIVE_GLSLANG_VALIDATOR}")
    endif()
    if(GTA4_NATIVE_SPIRV_VAL)
        list(APPEND GTA4_NATIVE_SHADER_OVERRIDE_COMMAND
            --spirv-val "${GTA4_NATIVE_SPIRV_VAL}")
    endif()

    add_custom_command(
        OUTPUT "${GTA4_NATIVE_SHADER_OVERRIDE_CPP}"
        COMMAND ${GTA4_NATIVE_SHADER_OVERRIDE_COMMAND}
        DEPENDS
            "${GTA4_NATIVE_SHADER_OVERRIDE_MANIFEST}"
            "${GTA4_NATIVE_SHADER_OVERRIDE_COMPILER}"
            "${GTA4_NATIVE_SHADER_OVERRIDE_ROOT}/shader_override_cache.h"
            "${GTA4_NATIVE_SHADER_COMMON}"
        DEPFILE "${GTA4_NATIVE_SHADER_OVERRIDE_DEPFILE}"
        COMMENT "Compiling GTA IV hash-specific shader overrides"
        VERBATIM)

    if(NOT TARGET libzstd_static)
        set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
        set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        add_subdirectory("${GTA4_NATIVE_ZSTD_ROOT}/build/cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/gta4_native_zstd")
    endif()

    if(PS5)
        set(_gta4_native_library_type STATIC)
    else()
        set(_gta4_native_library_type SHARED)
    endif()
    add_library(rexgpu-gta4-native ${_gta4_native_library_type}
        gta4_native/graphics_system.cpp
        gta4_native/core/font_atlas.cpp
        gta4_native/shader_override_policy.h
        gta4_native/native_buffer_arena.cpp
        gta4_native/native_descriptor_backend.cpp
        gta4_native/native_frame_context.cpp
        gta4_native/native_gpu_attribution.cpp
        gta4_native/native_memory_samples.cpp
        gta4_native/native_performance_samples.cpp
        gta4_native/native_submission_lifetime.cpp
        gta4_native/postfx_resource_pool.cpp
        gta4_native/smaa_pipeline.cpp
        gta4_native/split_postfx_pass.cpp
        gta4_native/sun_shafts_pass.cpp
        gta4_native/temporal/fsr_vulkan.cpp
        gta4_native/temporal/shader_archive.cpp
        gta4_native/temporal/vulkan_scene.cpp
        gta4_native/temporal/vulkan_frame_generation.cpp
        gta4_native/plugin_main.cpp
        "${GTA4_NATIVE_SHADER_CACHE_ROOT}/shader/shader_cache.cpp"
        "${GTA4_NATIVE_SHADER_OVERRIDE_CPP}"
        "${GTA4_NATIVE_SMOLV_ROOT}/smolv.cpp"
    )
    set_source_files_properties(
        "${GTA4_NATIVE_SHADER_CACHE_ROOT}/shader/shader_cache.cpp"
        PROPERTIES COMPILE_OPTIONS "-Wno-missing-field-initializers"
    )
    if(APPLE)
        target_sources(rexgpu-gta4-native PRIVATE gta4_native/native_resource_reclaimer_mac.mm)
    endif()
    add_library(rex::gpu-gta4-native ALIAS rexgpu-gta4-native)
    rexgpu_configure_plugin(rexgpu-gta4-native gpu-gta4-native)
    target_include_directories(rexgpu-gta4-native PRIVATE
        "${GTA4_NATIVE_SHADER_CACHE_ROOT}"
        "${GTA4_NATIVE_SMOLV_ROOT}"
    )
    target_compile_definitions(rexgpu-gta4-native PRIVATE
        GTA4_NATIVE_FONT_ASSET_ROOT="${LIBERTY_RECOMP_WORKSPACE_ROOT}/LibertyRecompLib/font_atlases"
    )
    target_link_libraries(rexgpu-gta4-native PRIVATE
        libzstd_static
        SPIRV-Headers::SPIRV-Headers
    )
    if(TARGET rexglue_fidelityfx_vulkan)
        target_link_libraries(rexgpu-gta4-native PRIVATE rexglue_fidelityfx_vulkan)
    endif()
    # [ps5 port] Line tables only (no code change): the PS5 sampling
    # profiler's addresses map to source lines in the renderer.
    if(PS5)
        target_compile_options(rexgpu-gta4-native PRIVATE -gline-tables-only)
    endif()
    # ThinLTO inside the renderer plugin (recorder and render worker).
    if(ANDROID)
        option(LIBERTY_ANDROID_RENDERER_THINLTO "ThinLTO for the native renderer on Android" ON)
        if(LIBERTY_ANDROID_RENDERER_THINLTO)
            target_compile_options(rexgpu-gta4-native PRIVATE -flto=thin)
            target_link_options(rexgpu-gta4-native PRIVATE -flto=thin "-Wl,--thinlto-jobs=8"
                "-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache-renderer")
        endif()
    endif()
    # The function order libmain uses (gta4-recomp/CMakeLists.txt).
    if(ANDROID)
        set(_renderer_symbol_order "${CMAKE_CURRENT_SOURCE_DIR}/../../gta4-recomp/android_symbol_order.txt")
        if(EXISTS "${_renderer_symbol_order}")
            target_link_options(rexgpu-gta4-native PRIVATE
                "-Wl,--symbol-ordering-file=${_renderer_symbol_order}"
                "-Wl,--no-warn-symbol-ordering")
            set_property(TARGET rexgpu-gta4-native APPEND PROPERTY LINK_DEPENDS "${_renderer_symbol_order}")
        endif()
    endif()
    # Same switch as libmain (gta4-recomp/CMakeLists.txt): "generate" records
    # files/pgo/rexgpu-*.profraw, a .profdata path optimizes with it.
    if(ANDROID AND LIBERTY_ANDROID_PGO)
        target_sources(rexgpu-gta4-native PRIVATE gta4_native/android_pgo_dump.cpp)
        if(LIBERTY_ANDROID_PGO STREQUAL "generate")
            target_compile_options(rexgpu-gta4-native PRIVATE -fprofile-generate)
            target_link_options(rexgpu-gta4-native PRIVATE -fprofile-generate)
            target_compile_definitions(rexgpu-gta4-native PRIVATE LIBERTY_PGO_GENERATE=1)
        else()
            target_compile_options(rexgpu-gta4-native PRIVATE "-fprofile-use=${LIBERTY_ANDROID_PGO}"
                -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date)
            target_link_options(rexgpu-gta4-native PRIVATE "-fprofile-use=${LIBERTY_ANDROID_PGO}")
        endif()
    endif()
    include("${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/vulkan/VulkanShaderArchive.cmake")
    liberty_add_vulkan_temporal_archive()
endif()

# Native Metal is a peer plugin, not a Vulkan translation path.
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_subdirectory(gta4_metal)
endif()
