/**
 * @file        runtime.h
 * @brief       Runtime subsystem entry point
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 * @remarks     Based on Xenia emulator.h/cc
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include <rex/cvar.h>
#include <rex/embedded_metadata.h>
#include <rex/filesystem/vfs.h>
#include <rex/memory.h>
#include <rex/system/export_resolver.h>
#include <rex/system/interfaces/audio.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/interfaces/input.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/live_compatibility.h>
#include <rex/system/xobject.h>  // object_ref

// Forward declaration for function mapping (defined in rex/ppc/context.h)
struct PPCFuncMapping;

#include <rex/image_info.h>

REXCVAR_DECLARE(std::string, game_data_root);
REXCVAR_DECLARE(std::string, user_data_root);
REXCVAR_DECLARE(std::string, update_data_root);
REXCVAR_DECLARE(std::string, cache_root);
REXCVAR_DECLARE(std::string, metadata_root);
REXCVAR_DECLARE(std::string, saved_game_root);

namespace rex {

// Forward declarations
namespace runtime {
class FunctionDispatcher;
class ExportResolver;
}  // namespace runtime
namespace system {
class KernelState;
class XThread;
struct ShutdownReport;
}  // namespace system
namespace ui {
class WindowedAppContext;
class Window;
class ImGuiDrawer;
}  // namespace ui

/// Configuration for Runtime subsystem injection.
/// Graphics and audio backends are provided by the caller, keeping the runtime
/// library decoupled from concrete backend implementations.
/// Audio uses a factory because AudioSystem requires a FunctionDispatcher* at
/// construction time, which is only available during Setup().
struct RuntimeConfig {
  std::unique_ptr<system::IGraphicsSystem> graphics;
  // GPU emulation plugin loaded by ReXApp when `graphics` is empty
  // (e.g. "xenos"); empty means no GPU emulation.
  std::string gpu_plugin;
  std::function<std::unique_ptr<system::IAudioSystem>(runtime::FunctionDispatcher*)> audio_factory;
  std::function<std::unique_ptr<system::IInputSystem>(bool tool_mode)> input_factory;
  std::function<void(Runtime*, system::KernelState*)> kernel_init;
  system::xam::LiveConfig live;
  bool tool_mode = false;
};

/// Helper macros for populating RuntimeConfig with concrete backends.
/// Usage:
///   rex::RuntimeConfig config;
///   config.graphics      = REX_GRAPHICS_BACKEND(MyCustomGraphicsSystem);
///   config.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
#define REX_GRAPHICS_BACKEND(Type) std::make_unique<Type>()
#define REX_AUDIO_BACKEND(Type)                                                                 \
  [](::rex::runtime::FunctionDispatcher* _fd) -> std::unique_ptr<::rex::system::IAudioSystem> { \
    return Type::Create(_fd);                                                                   \
  }
#define REX_INPUT_BACKEND(SetupFunc)                                    \
  [](bool _tool_mode) -> std::unique_ptr<::rex::system::IInputSystem> { \
    return SetupFunc(_tool_mode);                                       \
  }

/**
 * Runtime class - the main entry point for recompiled applications.
 *
 * Owns all subsystems:
 * - Memory: Virtual address space for guest code
 * - VFS: Virtual file system
 * - KernelState: Kernel objects, threading, etc.
 */
class Runtime {
 public:
  explicit Runtime(const std::filesystem::path& game_data_root,
                   const std::filesystem::path& user_data_root = {},
                   const std::filesystem::path& update_data_root = {},
                   const std::filesystem::path& cache_root = {},
                   const std::filesystem::path& metadata_root = {},
                   const std::filesystem::path& marketplace_content_root = {},
                   const std::filesystem::path& saved_game_root = {});
  ~Runtime();

  // Non-copyable
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  // Global instance accessor - set after Setup() is called
  static Runtime* instance();

  // Monotonic stamp for the graphics system's lifetime. It changes whenever a
  // graphics system is installed or destroyed, so a caller may cache the
  // system for the current generation only; a return to the hub replaces it.
  static uint64_t graphics_generation();

  // Subsystem accessors
  memory::Memory* memory() const { return memory_.get(); }
  rex::filesystem::VirtualFileSystem* file_system() const { return file_system_.get(); }
  system::KernelState* kernel_state() const { return kernel_state_.get(); }
  system::IGraphicsSystem* graphics_system() const { return graphics_system_.get(); }
  system::IAudioSystem* audio_system() const { return audio_system_.get(); }
  system::IInputSystem* input_system() const { return input_system_.get(); }

  // FunctionDispatcher for guest function dispatch and interrupt execution
  runtime::FunctionDispatcher* function_dispatcher() const { return function_dispatcher_.get(); }
  // Export resolver - used for variable import resolution in guest memory
  runtime::ExportResolver* export_resolver() const { return export_resolver_.get(); }

  // Path accessors
  const std::filesystem::path& game_data_root() const { return game_data_root_; }
  const std::filesystem::path& user_data_root() const { return user_data_root_; }
  const std::filesystem::path& update_data_root() const { return update_data_root_; }
  const std::filesystem::path& cache_root() const { return cache_root_; }
  const std::filesystem::path& metadata_root() const { return metadata_root_; }
  // Optional directory containing marketplace packages directly. When empty,
  // ContentManager keeps the standard Xbox hierarchy under user_data_root.
  const std::filesystem::path& marketplace_content_root() const {
    return marketplace_content_root_;
  }
  // Optional host root for loose XContentType::kSavedGame packages. Empty
  // preserves the standard Xbox hierarchy beneath user_data_root.
  const std::filesystem::path& saved_game_root() const { return saved_game_root_; }
  const system::xam::LiveConfig& live_config() const { return live_config_; }

  // Finds a metadata file or directory. An explicit metadata_root disables
  // legacy discovery; otherwise existing project layouts remain supported.
  std::optional<std::filesystem::path> FindMetadataPath(
      const std::filesystem::path& relative_path) const;
  std::optional<EmbeddedMetadataAsset> FindEmbeddedMetadata(
      const std::filesystem::path& relative_path) const;

  // Set the app context for presentation (call before Setup)
  void set_app_context(ui::WindowedAppContext* context) { app_context_ = context; }
  ui::WindowedAppContext* app_context() const { return app_context_; }

  // UI accessors for dialog system
  void set_display_window(ui::Window* window) { display_window_ = window; }
  ui::Window* display_window() const { return display_window_; }
  void set_imgui_drawer(ui::ImGuiDrawer* drawer) { imgui_drawer_ = drawer; }
  ui::ImGuiDrawer* imgui_drawer() const { return imgui_drawer_; }

  // Setup the runtime environment
  // config.tool_mode: If true, skips GPU initialization (for analysis tools)
  X_STATUS Setup(RuntimeConfig config = {});

  // rexglue - initializes per-module function dispatch table
  X_STATUS Setup(const PPCImageInfo& image_info, RuntimeConfig config = {});

  // Check if running in tool mode (no GPU)
  bool is_tool_mode() const { return tool_mode_; }

  void Shutdown();

  // Cooperative teardown for an in-process title transition.
  //
  // Closes save admission, drains guest threads cooperatively (never
  // force-kills one), closes and flushes idle saved-game mounts, waits for
  // save quiescence, and destroys runtime resources only when both phases
  // proved clean. Otherwise it returns a blocked report and leaves every
  // resource the surviving guest or the incomplete save could touch intact;
  // the caller may retry. Never replace this with a process exit.
  //
  // Once begun the transition is committed: guest dispatch admission and save
  // admission stay closed even on a blocked result, so a caller must retry or
  // stop, never resume gameplay from a partially shut-down runtime. Calling
  // from a guest thread is refused (blocked) because teardown would destroy the
  // state the calling thread executes on.
  //
  // before_shutdown (optional) runs on the calling thread after the drain and
  // the save barrier proved clean, immediately before resources are destroyed.
  // It lets the host detach presentation objects that still point at the
  // graphics system; it never runs on a blocked result.
  system::ShutdownReport ShutdownCooperatively(
      uint32_t guest_timeout_ms = 1000,
      std::chrono::milliseconds save_timeout = std::chrono::milliseconds(5000),
      std::function<void()> before_shutdown = {});

  // Load XEX image into guest memory
  X_STATUS LoadXexImage(const std::string_view module_path);

  // Launch XEX module and return main thread
  // Call after LoadXexImage to start execution
  system::object_ref<system::XThread> LaunchModule();

  // Prepare module launch: creates suspended main thread without resuming.
  // Call thread->Resume() after any pre-launch hooks.
  system::object_ref<system::XThread> PrepareModuleLaunch();

  // Access the memory base pointer for recompiled code
  uint8_t* virtual_membase() const;

  // Failed release retains the memory owner and blocks replacement sessions.
  bool last_shutdown_released_everything() const {
    return last_shutdown_released_everything_;
  }

 private:
  // Set up VFS: mounts game_data_root as game:/d:, update_data_root as update:
  bool SetupVfs();

  std::filesystem::path game_data_root_;
  std::filesystem::path user_data_root_;
  std::filesystem::path update_data_root_;
  std::filesystem::path cache_root_;
  std::filesystem::path metadata_root_;
  std::filesystem::path marketplace_content_root_;
  std::filesystem::path saved_game_root_;
  system::xam::LiveConfig live_config_;

  ui::WindowedAppContext* app_context_ = nullptr;
  ui::Window* display_window_ = nullptr;
  ui::ImGuiDrawer* imgui_drawer_ = nullptr;
  bool tool_mode_ = false;
  bool setup_complete_ = false;
  bool last_shutdown_released_everything_ = true;

  std::unique_ptr<memory::Memory> memory_;
  std::unique_ptr<runtime::FunctionDispatcher> function_dispatcher_;
  std::unique_ptr<rex::filesystem::VirtualFileSystem> file_system_;
  std::unique_ptr<system::KernelState> kernel_state_;
  std::unique_ptr<system::IGraphicsSystem> graphics_system_;
  std::unique_ptr<system::IAudioSystem> audio_system_;
  std::unique_ptr<system::IInputSystem> input_system_;
  std::unique_ptr<runtime::ExportResolver> export_resolver_;

  static Runtime* instance_;
};

}  // namespace rex
