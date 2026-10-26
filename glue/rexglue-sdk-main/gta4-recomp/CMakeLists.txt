cmake_minimum_required(VERSION 3.25)
project(gta4_recomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/sources.cmake")
    message(FATAL_ERROR
        "gta4-recomp generated sources are missing. Build rexglue and run "
        "'rexglue codegen gta4_manifest.toml'.")
endif()

include(generated/sources.cmake)

# Title sources shared by every Graine host (macOS bundle, Android library).
set(GTA4_APP_SOURCES
    src/main.cpp
    src/gta4_app.cpp
    src/gta4_frontend_hooks.cpp
    src/gta4_font_selection_trace.cpp
    src/gta4_presentation_options.cpp
    src/gta4_presentation_hooks.cpp
    src/gta4_aspect_hooks.cpp
    src/gta4_quicksave_hooks.cpp
    src/gta4_camera_hooks.cpp
    src/gta4_input_hooks.cpp
    src/gta4_sony_feedback.cpp
    src/gta4_sony_event_hooks.cpp
    src/gta4_loading_parallax_fix.cpp
    src/gta4_motion_bridge.cpp
    src/gta4_motion_hooks.cpp
    src/gta4_motion_vehicle_hooks.cpp
    src/gta4_audio_lifetime_trace.cpp
    src/gta4_audio_ramped_mix.cpp
    src/gta4_lzx_hooks.cpp
    src/gta4_multiplayer_64_hooks.cpp
    src/gta4_online_hooks.cpp
    src/gta4_native_hooks.cpp
    src/gta4_population_hooks.cpp
    src/gta4_hack_registry.cpp
    src/gta4_streaming_diagnostics.cpp
    src/gta4_streaming_hooks.cpp
    src/gta4_touch_coordinator.cpp
    src/gta4_transition_hooks.cpp
    src/install/gta4_install_dialog.cpp
    src/install/gta4_installer.cpp
    src/install/gta4_rpf_extractor.cpp
    src/install/gta4_source_inspector.cpp
    src/legal_screen_trace.cpp
    src/rpf_button_prompts.cpp
    src/input/text_chat_dialog.cpp
    src/input/context_touch_controls.cpp
    src/input/context_touch_context.cpp
    src/input/context_touch_camera_hooks.cpp
    src/input/context_touch_settings.cpp
    src/input/context_touch_draw.cpp
    src/input/context_touch_layout.cpp
    src/input/context_touch_activity.cpp
    src/input/context_touch_activity_context.cpp
    src/input/context_touch_activity_layout.cpp
    src/input/context_touch_overlay.cpp
    src/input/gamepad_touch_overlay.cpp
    src/gta4_touch_guest_hooks.cpp
    src/network/community_multiplayer.cpp
    src/network/gta4_voice_audio.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/crypto/rijndael-alg-fst.c"
)

if(APPLE AND LIBERTY_RECOMP_TARGET_PLATFORM STREQUAL "desktop")
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    find_package(CURL REQUIRED)
    find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
    # The Apple SDK is already supplied through CMAKE_OSX_SYSROOT. Re-adding
    # its usr/include directory through CURL::libcurl makes Homebrew libc++
    # combine headers outside and inside the sysroot, which breaks fundamental
    # Darwin typedefs with current Xcode SDKs.
    if(CURL_INCLUDE_DIR MATCHES "/MacOSX[^/]*/usr/include$")
        set_property(TARGET CURL::libcurl PROPERTY INTERFACE_INCLUDE_DIRECTORIES "")
    endif()

    set(LIBERTY_GC_MACOS_ENTITLEMENTS
        "${CMAKE_CURRENT_SOURCE_DIR}/res/macos/LibertyRecomp.entitlements")
    set(LIBERTY_BUTTON_PROMPT_SOURCE
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/LibertyRecompLib/private/button_prompts")
    set(LIBERTY_BUTTON_PROMPT_RESOURCES)
    foreach(LIBERTY_BUTTON_PROMPT_VARIANT
            xbox360 xbox_one xbox_series_x ps3 ps4 ps5 switch steam_controller steam_deck)
        set(LIBERTY_BUTTON_PROMPT_RESOURCE
            "${LIBERTY_BUTTON_PROMPT_SOURCE}/${LIBERTY_BUTTON_PROMPT_VARIANT}/buttons_360.xtd")
        list(APPEND LIBERTY_BUTTON_PROMPT_RESOURCES "${LIBERTY_BUTTON_PROMPT_RESOURCE}")
        set_source_files_properties("${LIBERTY_BUTTON_PROMPT_RESOURCE}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources/button_prompts/${LIBERTY_BUTTON_PROMPT_VARIANT}")
    endforeach()

    set(LIBERTY_VECTOR_FONT_RESOURCES)
    set(LIBERTY_VECTOR_FONT_SOURCE
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/LibertyRecompLib/font_atlases")
    foreach(LIBERTY_VECTOR_FONT_NAME font1 font2 font3)
        set(LIBERTY_VECTOR_FONT_RESOURCE
            "${LIBERTY_VECTOR_FONT_SOURCE}/${LIBERTY_VECTOR_FONT_NAME}.png")
        list(APPEND LIBERTY_VECTOR_FONT_RESOURCES "${LIBERTY_VECTOR_FONT_RESOURCE}")
        set_source_files_properties("${LIBERTY_VECTOR_FONT_RESOURCE}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources/font_atlases")
    endforeach()
    set(LIBERTY_VECTOR_FONT_MANIFEST
        "${LIBERTY_VECTOR_FONT_SOURCE}/manifest.json")
    list(APPEND LIBERTY_VECTOR_FONT_RESOURCES "${LIBERTY_VECTOR_FONT_MANIFEST}")
    set_source_files_properties("${LIBERTY_VECTOR_FONT_MANIFEST}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources/font_atlases")
    foreach(LIBERTY_VECTOR_FONT_EPISODE tlad tbogt)
        foreach(LIBERTY_VECTOR_FONT_NAME font1 font2 font3)
            set(LIBERTY_VECTOR_FONT_RESOURCE
                "${LIBERTY_VECTOR_FONT_SOURCE}/${LIBERTY_VECTOR_FONT_EPISODE}/${LIBERTY_VECTOR_FONT_NAME}.png")
            list(APPEND LIBERTY_VECTOR_FONT_RESOURCES "${LIBERTY_VECTOR_FONT_RESOURCE}")
            set_source_files_properties("${LIBERTY_VECTOR_FONT_RESOURCE}" PROPERTIES
                MACOSX_PACKAGE_LOCATION
                    "Resources/font_atlases/${LIBERTY_VECTOR_FONT_EPISODE}")
        endforeach()
        set(LIBERTY_VECTOR_FONT_EPISODE_MANIFEST
            "${LIBERTY_VECTOR_FONT_SOURCE}/${LIBERTY_VECTOR_FONT_EPISODE}/manifest.json")
        list(APPEND LIBERTY_VECTOR_FONT_RESOURCES
            "${LIBERTY_VECTOR_FONT_EPISODE_MANIFEST}")
        set_source_files_properties("${LIBERTY_VECTOR_FONT_EPISODE_MANIFEST}" PROPERTIES
            MACOSX_PACKAGE_LOCATION
                "Resources/font_atlases/${LIBERTY_VECTOR_FONT_EPISODE}")
    endforeach()

    set(LIBERTY_RPF_AES_KEY_SOURCE
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/LibertyRecompLib/aes_key.bin")
    set(LIBERTY_RPF_AES_KEY_RESOURCES)
    if(EXISTS "${LIBERTY_RPF_AES_KEY_SOURCE}")
        list(APPEND LIBERTY_RPF_AES_KEY_RESOURCES
            "${LIBERTY_RPF_AES_KEY_SOURCE}")
        set_source_files_properties("${LIBERTY_RPF_AES_KEY_SOURCE}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources")
    else()
        message(STATUS
            "LibertyRecompLib/aes_key.bin is not present; the app will build "
            "without the optional game-install key resource")
    endif()

    set(LIBERTY_DESKTOP_ICON
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/LibertyRecomp/res/macos/game_icon.icns")
    set_source_files_properties("${LIBERTY_DESKTOP_ICON}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")

    add_executable(LibertyRecomp MACOSX_BUNDLE
        "${LIBERTY_DESKTOP_ICON}"
        ${GTA4_APP_SOURCES}
        src/achievement_bridge_gc.mm
        src/input/user_music_player.mm
        src/network/gta4_microphone_permission.mm
        "${LIBERTY_GC_MACOS_ENTITLEMENTS}"
        ${LIBERTY_BUTTON_PROMPT_RESOURCES}
        ${LIBERTY_VECTOR_FONT_RESOURCES}
        ${LIBERTY_RPF_AES_KEY_RESOURCES}
        ${GENERATED_SOURCES}
    )

    # The startup registrar is one very large straight-line function. Avoid
    # optimizer memory/swap spikes for that file only; all gameplay translation
    # units retain the release optimization level. Generated source is untouched.
    set_property(SOURCE generated/gta4_register.cpp APPEND PROPERTY COMPILE_OPTIONS
        "$<$<CONFIG:Release>:-O0>")

    set_source_files_properties(
        src/achievement_bridge_gc.mm
        src/network/gta4_microphone_permission.mm
        src/input/user_music_player.mm
        PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc"
    )

    target_include_directories(LibertyRecomp PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/crypto
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/imgui
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/inja/third_party/include
    )
    # This hook shares the native temporal camera/history policy headers.
    set_property(SOURCE src/gta4_native_hooks.cpp APPEND PROPERTY INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}/../src")

    set_source_files_properties(src/gta4_sony_feedback.cpp src/gta4_sony_event_hooks.cpp
        PROPERTIES COMPILE_DEFINITIONS GTA4_SONY_PRIMARY_PLAYER_ALIAS)

    target_compile_definitions(LibertyRecomp PRIVATE
        GTA4_TOUCH_PRIMARY_PLAYER_ALIAS=1
        GTA4_RECOMP_ASSET_XEX="${CMAKE_CURRENT_SOURCE_DIR}/assets/default_v8.xex"
        GTA4_BUTTON_PROMPT_ASSET_ROOT="${LIBERTY_BUTTON_PROMPT_SOURCE}"
        GTA4_RPF_AES_KEY_SOURCE="${LIBERTY_RPF_AES_KEY_SOURCE}"
    )

    target_link_libraries(LibertyRecomp PRIVATE
        rex::runtime
        xxHash::xxhash
        CURL::libcurl
        OpenSSL::Crypto
        SDL3::SDL3
        utf8cpp
        z
        "-framework AVFoundation"
        "-framework GameKit"
    )

    include("${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/metal/MetalShaderArchive.cmake")
    liberty_add_metal_shader_archive(LibertyRecomp "${LIBERTY_RECOMP_WORKSPACE_ROOT}")
    liberty_stage_vulkan_temporal_archive(LibertyRecomp)

    get_property(LIBERTY_METAL_PASS_LIBRARY GLOBAL PROPERTY REX_METAL_PASSES_LIBRARY)
    if(LIBERTY_METAL_PASS_LIBRARY)
        set_source_files_properties("${LIBERTY_METAL_PASS_LIBRARY}" PROPERTIES
            GENERATED TRUE MACOSX_PACKAGE_LOCATION "Resources/metal")
        target_sources(LibertyRecomp PRIVATE "${LIBERTY_METAL_PASS_LIBRARY}")
        add_dependencies(LibertyRecomp rexui_metal_passes)
        set_property(TARGET LibertyRecomp APPEND PROPERTY LINK_DEPENDS "${LIBERTY_METAL_PASS_LIBRARY}")
    endif()

    rexglue_configure_target(LibertyRecomp GPU_PLUGINS xenos gta4-native gta4-metal)

    # rexglue_configure_target stages the Vulkan loader and its companion
    # runtime from the selected SDK.  MoltenVK itself is built from the
    # workspace, however, and may contain title-required fixes that are not in
    # the installed SDK copy.  Make the workspace build authoritative in the
    # same runtime directory after the generic staging command has run.
    if(TARGET MoltenVK)
        add_dependencies(LibertyRecomp MoltenVK)
        # The target dependency establishes build order, but by itself it does
        # not rerun this target's POST_BUILD events when only MoltenVK changes.
        # Make the driver a file-level link dependency so the app is relinked
        # and the authoritative workspace driver is restaged after the generic
        # SDK runtime staging step.
        set_property(TARGET LibertyRecomp APPEND PROPERTY LINK_DEPENDS
            "$<TARGET_FILE:MoltenVK>"
        )
        add_custom_command(TARGET LibertyRecomp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_CONTENT_DIR:LibertyRecomp>/Resources/vulkan/lib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:MoltenVK>"
                "$<TARGET_BUNDLE_CONTENT_DIR:LibertyRecomp>/Resources/vulkan/lib/libMoltenVK.dylib"
            COMMENT "Staging the workspace MoltenVK runtime..."
            VERBATIM
        )
    endif()

    set_target_properties(LibertyRecomp PROPERTIES
        OUTPUT_NAME "Liberty Recompiled"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/LibertyRecomp"
        MACOSX_BUNDLE_INFO_PLIST
            "${CMAKE_CURRENT_SOURCE_DIR}/res/macos/MacOSXBundleInfo.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${LIBERTY_MACOS_BUNDLE_IDENTIFIER}"
        MACOSX_BUNDLE_BUNDLE_NAME "Liberty Recompiled"
        MACOSX_BUNDLE_ICON_FILE "game_icon.icns"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.0"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${LIBERTY_MACOS_BUNDLE_IDENTIFIER}"
    )

    # An icon-only update must rerun the bundle signing step too.
    set_property(TARGET LibertyRecomp APPEND PROPERTY LINK_DEPENDS "${LIBERTY_DESKTOP_ICON}")
    # Atlas-only updates also change the bundle seal. Relink so the existing
    # post-build signing step runs after CMake copies the new font resources.
    set_property(TARGET LibertyRecomp APPEND PROPERTY LINK_DEPENDS
        ${LIBERTY_VECTOR_FONT_RESOURCES})

    set(LIBERTY_MACOS_RUNTIME_BUNDLER
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/cmake/BundleMacOSRuntime.cmake")
    set(LIBERTY_MACOS_BUNDLE_VERIFIER
        "${LIBERTY_RECOMP_WORKSPACE_ROOT}/tools/verify_macos_bundle.py")
    set_property(TARGET LibertyRecomp APPEND PROPERTY LINK_DEPENDS
        "${LIBERTY_MACOS_RUNTIME_BUNDLER}" "${LIBERTY_MACOS_BUNDLE_VERIFIER}")
    get_filename_component(_liberty_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_liberty_compiler_prefix "${_liberty_compiler_bin}" DIRECTORY)
    add_custom_command(TARGET LibertyRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DAPP_BUNDLE=$<TARGET_BUNDLE_DIR:LibertyRecomp>"
            "-DSIGN_IDENTITY=${LIBERTY_MACOS_CODE_SIGN_IDENTITY}"
            "-DRUNTIME_SEARCH_DIRS=${_liberty_compiler_prefix}/lib/c++;${_liberty_compiler_prefix}/lib/unwind;$<TARGET_FILE_DIR:OpenSSL::Crypto>"
            -P "${LIBERTY_MACOS_RUNTIME_BUNDLER}"
        COMMENT "Embedding and signing non-system macOS runtime dependencies..."
        VERBATIM)

    if(LIBERTY_MACOS_CODE_SIGN_IDENTITY)
        set_target_properties(LibertyRecomp PROPERTIES
            XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${LIBERTY_GC_MACOS_ENTITLEMENTS}"
        )
        add_custom_command(TARGET LibertyRecomp POST_BUILD
            COMMAND /usr/bin/codesign
                --force
                --sign "${LIBERTY_MACOS_CODE_SIGN_IDENTITY}"
                --entitlements "${LIBERTY_GC_MACOS_ENTITLEMENTS}"
                "$<TARGET_BUNDLE_DIR:LibertyRecomp>"
            COMMENT "Signing macOS bundle with Game Center and audio-input entitlements..."
        )
    else()
        # Game Center is a restricted entitlement. Attaching it to an ad-hoc
        # signature causes AMFI to kill the process before main. Unsandboxed
        # microphone access is authorized by TCC using the bundle usage
        # description, so development bundles must be signed without the
        # restricted entitlement set.
        add_custom_command(TARGET LibertyRecomp POST_BUILD
            COMMAND /usr/bin/codesign
                --force
                --sign "-"
                "$<TARGET_BUNDLE_DIR:LibertyRecomp>"
            COMMENT "Ad-hoc signing macOS development bundle..."
        )
    endif()
    add_custom_command(TARGET LibertyRecomp POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${LIBERTY_MACOS_BUNDLE_VERIFIER}"
            "$<TARGET_BUNDLE_DIR:LibertyRecomp>" --arch "${CMAKE_OSX_ARCHITECTURES}"
        COMMENT "Verifying macOS runtime closure, architecture, minimum OS, and signature..."
        VERBATIM)
elseif(ANDROID)
    # SDL3's SDLActivity loads libmain.so and calls its SDL_main. The runtime,
    # SDL3 and the GPU plugins are unpacked next to it in nativeLibraryDir;
    # title resources are copied out of the APK by the Java host, which points
    # REX_RESOURCES_DIR at them.
    set(GTA4_ANDROID_SOURCES ${GTA4_APP_SOURCES})
    list(REMOVE_ITEM GTA4_ANDROID_SOURCES src/network/community_multiplayer.cpp)

    add_library(LibertyRecomp SHARED
        ${GTA4_ANDROID_SOURCES}
        src/android/android_platform_stubs.cpp
        ${GENERATED_SOURCES}
    )

    set_property(SOURCE generated/gta4_register.cpp APPEND PROPERTY COMPILE_OPTIONS
        "$<$<CONFIG:Release,RelWithDebInfo>:-O0>")

    target_include_directories(LibertyRecomp PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/crypto
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/imgui
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/inja/third_party/include
    )
    set_property(SOURCE src/gta4_native_hooks.cpp APPEND PROPERTY INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}/../src")

    set_source_files_properties(src/gta4_sony_feedback.cpp src/gta4_sony_event_hooks.cpp
        PROPERTIES COMPILE_DEFINITIONS GTA4_SONY_PRIMARY_PLAYER_ALIAS)

    # The asset paths are build-machine fallbacks; on the device every lookup
    # resolves through GetResourcesFolder() first.
    target_compile_definitions(LibertyRecomp PRIVATE
        GTA4_TOUCH_PRIMARY_PLAYER_ALIAS=1
        GTA4_RECOMP_ASSET_XEX="${CMAKE_CURRENT_SOURCE_DIR}/assets/default_v8.xex"
        GTA4_BUTTON_PROMPT_ASSET_ROOT="${LIBERTY_RECOMP_WORKSPACE_ROOT}/LibertyRecompLib/private/button_prompts"
        GTA4_RPF_AES_KEY_SOURCE="${LIBERTY_RECOMP_WORKSPACE_ROOT}/LibertyRecompLib/aes_key.bin"
    )

    target_link_libraries(LibertyRecomp PRIVATE
        rex::runtime
        xxHash::xxhash
        SDL3::SDL3
        utf8cpp
        z
        android
        log
    )

    rexglue_configure_target(LibertyRecomp GPU_PLUGINS xenos gta4-native)

    set_target_properties(LibertyRecomp PROPERTIES OUTPUT_NAME "main")
    # SDL resolves SDL_main with dlsym after dlopen; Android 15 devices need
    # 16 KB-aligned segments.
    target_link_options(LibertyRecomp PRIVATE
        "-Wl,-z,max-page-size=16384"
        "-Wl,--export-dynamic-symbol=SDL_main"
        "-Wl,--export-dynamic-symbol=JNI_OnLoad"
        # Bind calls between the ~24k recompiled game functions directly
        # instead of through this library's own PLT (nothing interposes them).
        "-Wl,-Bsymbolic-functions"
    )
    # ThinLTO across the recompiled game: with direct calls, small guest
    # functions can now be inlined into their callers at link time.
    # Function ordering: the hottest functions first in .text (generated by
    # tools/make_symbol_order.py from a PGO profile). The renderer plugin uses
    # the same list (src/graphics/CMakeLists.txt); lld skips absent names.
    set(LIBERTY_ANDROID_SYMBOL_ORDER "${CMAKE_CURRENT_SOURCE_DIR}/android_symbol_order.txt"
        CACHE FILEPATH "lld symbol ordering file for Android (empty: off)")
    if(LIBERTY_ANDROID_SYMBOL_ORDER AND EXISTS "${LIBERTY_ANDROID_SYMBOL_ORDER}")
        target_link_options(LibertyRecomp PRIVATE
            "-Wl,--symbol-ordering-file=${LIBERTY_ANDROID_SYMBOL_ORDER}"
            "-Wl,--no-warn-symbol-ordering")
        set_property(TARGET LibertyRecomp APPEND PROPERTY LINK_DEPENDS "${LIBERTY_ANDROID_SYMBOL_ORDER}")
    endif()
    # -O3 for the recompiled game (nfsmw-android builds its game code so).
    target_compile_options(LibertyRecomp PRIVATE $<$<CONFIG:RELWITHDEBINFO>:-O3>)
    option(LIBERTY_ANDROID_THINLTO "ThinLTO for libmain on Android" ON)
    if(LIBERTY_ANDROID_THINLTO)
        target_compile_options(LibertyRecomp PRIVATE -flto=thin)
        target_link_options(LibertyRecomp PRIVATE -flto=thin "-Wl,--thinlto-jobs=8"
            "-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache")
    endif()
    # Profile-guided optimization: "generate" builds the recording libmain
    # (counters written to files/pgo every minute); a path to a merged
    # .profdata builds the optimized one. Empty: off.
    set(LIBERTY_ANDROID_PGO "" CACHE STRING "Android libmain PGO: generate | <file.profdata>")
    target_sources(LibertyRecomp PRIVATE src/android/android_pgo_dump.cpp
        src/android/android_frame_wait_hook.cpp
        src/android/android_stale_object_guard.cpp
        src/android/android_thread_pinning.cpp
        src/android/android_power.cpp)
    # The launcher's installer: the same source inspection and installation
    # code as the in-game installer dialog, without the rest of the title, so
    # the launcher process can install the game before it ever starts.
    add_library(liberty_install SHARED
        src/install/gta4_installer.cpp
        src/install/gta4_rpf_extractor.cpp
        src/install/gta4_source_inspector.cpp
        src/android/android_install_jni.cpp
        "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/crypto/rijndael-alg-fst.c"
    )
    target_include_directories(liberty_install PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/crypto
    )
    target_compile_definitions(liberty_install PRIVATE
        GTA4_RECOMP_ASSET_XEX="${CMAKE_CURRENT_SOURCE_DIR}/assets/default_v8.xex"
        GTA4_RPF_AES_KEY_SOURCE="${LIBERTY_RECOMP_WORKSPACE_ROOT}/LibertyRecompLib/aes_key.bin"
    )
    target_link_libraries(liberty_install PRIVATE rex::runtime xxHash::xxhash z log)
    target_link_options(liberty_install PRIVATE "-Wl,-z,max-page-size=16384")
    add_dependencies(LibertyRecomp liberty_install)

    # vmaddfp/vnmsubfp with one rounding (fmla), as the console's VMX does.
    # Only the REX_VMADDFP macro body changes, so a profile stays valid.
    option(LIBERTY_ANDROID_FUSED_VMADDFP "Fused vmaddfp/vnmsubfp in the recompiled code" ON)
    if(LIBERTY_ANDROID_FUSED_VMADDFP)
        target_compile_definitions(LibertyRecomp PRIVATE REX_FUSED_VMADDFP=1)
    endif()

    if(LIBERTY_ANDROID_PGO STREQUAL "generate")
        target_compile_options(LibertyRecomp PRIVATE -fprofile-generate)
        target_link_options(LibertyRecomp PRIVATE -fprofile-generate)
        target_compile_definitions(LibertyRecomp PRIVATE LIBERTY_PGO_GENERATE=1)
    elseif(LIBERTY_ANDROID_PGO)
        target_compile_options(LibertyRecomp PRIVATE "-fprofile-use=${LIBERTY_ANDROID_PGO}"
            -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date
            -Wno-backend-plugin)
        target_link_options(LibertyRecomp PRIVATE "-fprofile-use=${LIBERTY_ANDROID_PGO}")
    endif()
elseif(PS5)
    # [ps5 port] One static PS5 program: the game, the runtime and the
    # gta4-native renderer (no GPU plugin files). Linked and packaged by
    # ps5/tools/ps5_package.sh with the PS5 toolchain's RADV.
    set(GTA4_PS5_SOURCES ${GTA4_APP_SOURCES})
    list(REMOVE_ITEM GTA4_PS5_SOURCES src/network/community_multiplayer.cpp)

    # The hub (the screen the app opens on; fonts built in, SIL Open Font
    # License), trophies, the frame rate counter and watermark, 360 save import.
    set(GTA4_PS5_DIR "${LIBERTY_RECOMP_WORKSPACE_ROOT}/ps5")
    file(STRINGS "${GTA4_PS5_DIR}/VERSION.txt" GTA4_PS5_VERSION LIMIT_COUNT 1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GTA4_PS5_DIR}/VERSION.txt")
    list(APPEND GTA4_PS5_SOURCES
        src/ps5/gta4_ps5_app.cpp
        src/ps5/hub/hub_ui.cpp
        src/ps5/hub/hub_install.cpp
        src/ps5/hub/hub_installer_page.cpp
        src/ps5/hub/hub_lang.cpp
        src/ps5/hub/hub_sfx.cpp
        src/ps5/hub/hub_trophies.cpp
        src/ps5/hub/hub_trophies_page.cpp
        src/ps5/hub/ps5_hub.cpp
        src/ps5/game/sound.cpp
        src/ps5/game/trophies_game.cpp
        src/ps5/game/ps5_display.cpp
        src/ps5/game/ps5_overlay.cpp
        src/ps5/game/mod_files.cpp
        src/ps5/game/mod_loader.cpp
        src/ps5/game/save_import.cpp)
    # (the fonts are built into it: a changed one rebuilds it)
    file(GLOB GTA4_PS5_FONTS CONFIGURE_DEPENDS "${GTA4_PS5_DIR}/assets/fonts/*.ttf"
         "${GTA4_PS5_DIR}/assets/fonts/*.otf")
    # (Helvetica Neue Heavy, the episodes' tabs, only when it's in fonts/: its
    # licence doesn't let it be shared, so the public repository goes without
    # and those tabs use the menu type)
    set(GTA4_PS5_TAB_FONT 0)
    if(EXISTS "${GTA4_PS5_DIR}/assets/fonts/HelveticaNeue-Heavy.ttf")
        set(GTA4_PS5_TAB_FONT 1)
    endif()
    set_source_files_properties(src/ps5/hub/ps5_hub.cpp PROPERTIES
        COMPILE_DEFINITIONS "GTA4PS5_FONTS_DIR=\"${GTA4_PS5_DIR}/assets/fonts\";GTA4PS5_TAB_FONT=${GTA4_PS5_TAB_FONT}"
        OBJECT_DEPENDS "${GTA4_PS5_FONTS}")
    message(STATUS "GTA IV for PS5 ${GTA4_PS5_VERSION}")

    # The title's wait for a free frame slot sleeps instead of spinning a core
    # (written for Android, nothing Android-specific in it).
    add_executable(gta4
        ${GTA4_PS5_SOURCES}
        src/android/android_platform_stubs.cpp
        src/android/android_frame_wait_hook.cpp
        ${GENERATED_SOURCES}
    )
    set_property(SOURCE generated/gta4_register.cpp APPEND PROPERTY COMPILE_OPTIONS
        "$<$<CONFIG:Release,RelWithDebInfo>:-O0>")
    # The recompiled game's own files: mfmsr/mtmsrd without a hardware fence
    # (generated/gta4_init.h).
    set(GTA4_PS5_GUEST_SOURCES ${GENERATED_SOURCES})
    list(FILTER GTA4_PS5_GUEST_SOURCES INCLUDE REGEX "gta4_recomp\\.[0-9]+\\.cpp$")
    set_property(SOURCE ${GTA4_PS5_GUEST_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS REX_PS5_GUEST_CODE=1)
    # vmaddfp/vnmsubfp fused (one rounding, as the console's VMX does; one FMA
    # instruction on the PS5's CPU instead of two or three), as on Android.
    target_compile_definitions(gta4 PRIVATE REX_FUSED_VMADDFP=1)
    target_include_directories(gta4 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/crypto
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/imgui
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/inja/third_party/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/stb
    )
    set_property(SOURCE src/gta4_native_hooks.cpp APPEND PROPERTY INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    set_source_files_properties(src/gta4_sony_feedback.cpp src/gta4_sony_event_hooks.cpp
        PROPERTIES COMPILE_DEFINITIONS GTA4_SONY_PRIMARY_PLAYER_ALIAS)
    # (the version only where it is shown: on the whole target, a version
    # change rebuilt every recompiled file)
    set_property(SOURCE src/ps5/hub/ps5_hub.cpp src/ps5/game/ps5_overlay.cpp src/ps5/game/ps5_display.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS GTA4PS5_VERSION="${GTA4_PS5_VERSION}")
    target_compile_definitions(gta4 PRIVATE
        GTA4_RECOMP_ASSET_XEX="/app0/assets/default_v8.xex"
        GTA4_BUTTON_PROMPT_ASSET_ROOT="/app0/Resources/button_prompts"
        GTA4_RPF_AES_KEY_SOURCE="/app0/Resources/aes_key.bin"
    )
    target_compile_options(gta4 PRIVATE "SHELL:-include ${REXSDK_DIR}/include/rex/ps5_compat.h")
    target_link_libraries(gta4 PRIVATE
        rex::runtime
        rexgpu-gta4-native
        xxHash::xxhash
        SDL3::SDL3
        utf8cpp
        SceUserService ScePad SceAudioOut SceSystemService SceVideoOut
    )
    # The TV's refresh rate and frame pacing (src/ps5/game/ps5_display.cpp):
    # RADV's VideoOut calls go through it. ps5_package.sh's link wraps them too.
    target_link_options(gta4 PRIVATE
        "LINKER:--wrap=sceVideoOutIsOutputSupported,--wrap=sceVideoOutConfigureOutput,--wrap=sceVideoOutSetFlipRate")
    rexglue_configure_target(gta4)
    # zlib for the installer's RPF extraction: the PS5 toolchain's static build.
    set(PS5_ZLIB_ROOT "${PS5_PAYLOAD_SDK}/../zlib/root/usr" CACHE PATH "PS5 zlib (include/, lib/libz.a)")
    target_include_directories(gta4 PRIVATE "${PS5_ZLIB_ROOT}/include")
    target_link_libraries(gta4 PRIVATE "${PS5_ZLIB_ROOT}/lib/libz.a")
    # FSR 3 (with REXGLUE_ENABLE_FIDELITYFX): the motion-vector shader archive
    # next to the binary for ps5_package.sh, and the menus' FSR 3 entries.
    if(COMMAND liberty_stage_vulkan_temporal_archive AND TARGET rexglue_fidelityfx_vulkan)
        liberty_stage_vulkan_temporal_archive(gta4)
        set_property(SOURCE src/ps5/hub/hub_ui.cpp APPEND PROPERTY COMPILE_DEFINITIONS LIBERTY_HAS_FSR3=1)
    endif()
else()
    add_library(gta4_recomp INTERFACE)
    target_include_directories(gta4_recomp INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/generated
    )
    set(GTA4_RECOMP_GENERATED_SOURCES ${GENERATED_SOURCES} PARENT_SCOPE)
endif()

if(TARGET LibertyRecomp AND COMMAND rexglue_stage_dlss_runtime)
    rexglue_stage_dlss_runtime(LibertyRecomp)
endif()

if(TARGET rex::rexglue)
    add_custom_target(gta4_codegen
        COMMAND $<TARGET_FILE:rex::rexglue> codegen
                ${CMAKE_CURRENT_SOURCE_DIR}/gta4_manifest.toml
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating recompiled code for GTA IV"
        VERBATIM
    )
endif()
