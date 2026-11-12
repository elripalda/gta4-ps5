/**
 * @file        ui/rex_app.cpp
 * @brief       ReXApp implementation - compiled as part of the consumer executable
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/rex_app.h>

#include <cstdlib>

#include <rex/assert.h>
#include <rex/platform.h>
#if REX_PLATFORM_PS5
#include <rex/ps5_diag.h>
#endif
#include <rex/cvar.h>
#include <rex/ui/flags.h>
#include <rex/kernel/crt/heap.h>
#include <rex/filesystem.h>
#include <rex/logging/sink.h>
#include <rex/logging.h>
#include <rex/ui/overlay/achievement_toast.h>
#include <rex/ui/overlay/achievements_overlay.h>
#include <rex/ui/overlay/console_overlay.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/audio/audio_system.h>
#if REX_PLATFORM_MAC && !REX_PLATFORM_IOS
#include <rex/audio/coreaudio/coreaudio_audio_system.h>
#else
#include <rex/audio/sdl/sdl_audio_system.h>
#endif
#include <rex/input/input_system.h>
#include <rex/input/input_trace.h>
#include <rex/kernel/init.h>
#include <rex/system.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/cooperative_shutdown.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/kernel_state.h>
#include <rex/system/session_restart.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/keybinds.h>
#include <rex/version.h>

#include <fmt/format.h>
#if REX_PLATFORM_ANDROID
#include <SDL3/SDL_messagebox.h>
#endif
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string_view>

#if REX_PLATFORM_PS5
REXCVAR_DEFINE_BOOL(ps5_save_logs, true, "PS5", "Write the log to /app0/logs");
REXCVAR_DEFINE_BOOL(ps5_perf_capture, false, "PS5",
                    "Profile the renderer for a few minutes (read at startup, see windowed_app_main_sdl.cpp)");
#endif
REXCVAR_DEFINE_STRING(gpu_plugin, "", "GPU",
                      "Graphics plugin to load at startup (for example, 'xenos'); empty lets "
                      "the title choose; changes take effect after restart")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex {

// --- ReXApp ---

ReXApp::~ReXApp() = default;

ReXApp::ReXApp(ui::WindowedAppContext& ctx, std::string_view name, PPCImageInfo ppc_info,
               std::string_view usage)
    : WindowedApp(ctx, name, usage), ppc_info_(ppc_info) {}

std::unique_ptr<ui::ImGuiDialog> ReXApp::CreateAchievementsOverlay() {
  if (!runtime_ || !runtime_->kernel_state() || !imgui_drawer_ || !immediate_drawer_) {
    return nullptr;
  }
  return std::make_unique<ui::AchievementsOverlayDialog>(
      imgui_drawer_.get(), immediate_drawer_.get(), runtime_.get(), &achievements());
}

std::unique_ptr<ui::AchievementNotificationDialog> ReXApp::CreateAchievementNotificationDialog() {
  if (!imgui_drawer_ || !immediate_drawer_ || !runtime_) {
    return nullptr;
  }
  return std::make_unique<ui::AchievementToastDialog>(imgui_drawer_.get(), immediate_drawer_.get(),
                                                      runtime_.get());
}

system::AchievementManager& ReXApp::achievements() const {
  assert_not_null(runtime_);
  assert_not_null(runtime_->kernel_state());
  return runtime_->kernel_state()->achievements();
}

bool ReXApp::OnInitialize() {
  if (!SetupEnvironment())
    return false;
  if (!SetupPresentation())
    return false;

  auto paths = OnFinalizePaths(resolved_defaults_, MakeResumeCallback());
  if (!paths) {
    // Async: consumer will invoke resume when ready. OnInitialize returns
    // true so the event loop keeps pumping (wizard dialogs render).
    return true;
  }

  if (!ConstructRuntime(*paths))
    return false;
  LaunchModule();
  return true;
}

bool ReXApp::SetupEnvironment() {
  auto exe_dir = rex::filesystem::GetExecutableFolder();

  std::filesystem::path game_dir;
  std::string game_data_cvar = REXCVAR_GET(game_data_root);
  if (!game_data_cvar.empty()) {
    game_dir = game_data_cvar;
  }

  // User data: cvar override, or platform user directory
  std::filesystem::path user_dir;
  std::string user_data_cvar = REXCVAR_GET(user_data_root);
  if (!user_data_cvar.empty()) {
    user_dir = user_data_cvar;
  } else {
    user_dir = rex::filesystem::GetUserFolder() / GetName();
  }

  // Update data: cvar override, or empty (opt-in)
  std::filesystem::path update_dir;
  std::string update_data_cvar = REXCVAR_GET(update_data_root);
  if (!update_data_cvar.empty()) {
    update_dir = update_data_cvar;
  }

  // Cache: cvar override, or user_dir/cache
  std::filesystem::path cache_dir;
  std::string cache_root_cvar = REXCVAR_GET(cache_root);
  if (!cache_root_cvar.empty()) {
    cache_dir = cache_root_cvar;
  } else {
    cache_dir = user_dir / "cache";
  }

  std::filesystem::path metadata_dir;
  std::string metadata_root_cvar = REXCVAR_GET(metadata_root);
  if (!metadata_root_cvar.empty()) {
    metadata_dir = metadata_root_cvar;
  }

  std::filesystem::path saved_game_dir;
  std::string saved_game_root_cvar = REXCVAR_GET(saved_game_root);
  if (!saved_game_root_cvar.empty()) {
    saved_game_dir = saved_game_root_cvar;
  }

  PathConfig path_config{
      .game_data_root = game_dir,
      .user_data_root = user_dir,
      .update_data_root = update_dir,
      .cache_root = cache_dir,
      .metadata_root = metadata_dir,
      .marketplace_content_root = {},
      .saved_game_root = saved_game_dir,
      .config_path = exe_dir / (std::string(GetName()) + ".toml"),
  };
  OnConfigurePaths(path_config);
  game_data_root_ = path_config.game_data_root;
  user_data_root_ = path_config.user_data_root;
  update_data_root_ = path_config.update_data_root;
  cache_root_ = path_config.cache_root;
  metadata_root_ = path_config.metadata_root;
  marketplace_content_root_ = path_config.marketplace_content_root;
  saved_game_root_ = path_config.saved_game_root;
  config_path_ = path_config.config_path;
  resolved_defaults_ = std::move(path_config);

  // Load config FIRST so log cvars have final values
  if (std::filesystem::exists(config_path_))
    rex::cvar::LoadConfig(config_path_);

  // Late-phase logging
  std::string log_file_cvar = REXCVAR_GET(log_file);
  std::string log_level_str = REXCVAR_GET(log_level);
  if (REXCVAR_GET(log_verbose) && log_level_str == "info")
    log_level_str = "trace";

  auto category_levels = rex::ParseCategoryLevelsFromConfig(config_path_);
  auto log_config = rex::BuildLogConfig(log_file_cvar.empty() ? nullptr : log_file_cvar.c_str(),
                                        log_level_str, category_levels);
#if REX_PLATFORM_PS5
  // [ps5 port] Save Logs (the hub's Debug settings): off, no log file at all.
  if (!REXCVAR_GET(ps5_save_logs)) {
    log_file_cvar.clear();
  } else
#endif
  if (log_file_cvar.empty()) {
    log_config.app_name = std::string(GetName());
#if REX_PLATFORM_MAC || defined(__ANDROID__)
    // Android's library directory is read-only.
    // A macOS bundle's Contents/MacOS directory is code-signing territory.
    // Runtime logs placed there make a subsequent incremental build fail to
    // seal the bundle.  Keep mutable diagnostics with the rest of the title's
    // user data instead.
    log_config.log_dir = (user_dir / "logs").string();
#else
    log_config.log_dir = (exe_dir / "logs").string();
#endif
  }

#if REX_PLATFORM_PS5
  // [ps5 port] The log file is written every 2 s, not after every line: each
  // line was a write to the drive by whichever thread logged it, sometimes one
  // holding a lock the game's render thread then waited on (a beta tester's
  // game stalled for 0.8-0.9 s at a time, again and again). Errors too: a
  // tester's cutscenes ran at 8 FPS with the renderer writing out an error
  // line per draw (0.5b6). A crash writes its own report and then the rest
  // (ps5_diag.cpp, the crash hook).
  log_config.flush_level = spdlog::level::critical;
#endif
  rex::InitLogging(log_config);
#if REX_PLATFORM_PS5
  if (REXCVAR_GET(log_flush_interval) <= 0) spdlog::flush_every(std::chrono::seconds(2));
#endif
  rex::RegisterLogLevelCallback();

  log_sink_ = std::make_shared<rex::LogCaptureSink>();
  rex::AddSink(log_sink_);

  OnPostInitLogging();

  if (std::filesystem::exists(config_path_))
    REXLOG_INFO("Loaded config: {}", config_path_.filename().string());

  REXLOG_INFO("{} starting", GetName());
  if (!game_data_root_.empty()) {
    REXLOG_INFO("  Game directory: {}", game_data_root_.string());
  }
  if (!user_data_root_.empty()) {
    REXLOG_INFO("  User data:      {}", user_data_root_.string());
  }
  if (!update_data_root_.empty()) {
    REXLOG_INFO("  Update data:    {}", update_data_root_.string());
  }
  REXLOG_INFO("  Cache root:     {}", cache_root_.string());
  if (!metadata_root_.empty()) {
    REXLOG_INFO("  Metadata root:  {}", metadata_root_.string());
  }
  if (!marketplace_content_root_.empty()) {
    REXLOG_INFO("  Marketplace:    {}", marketplace_content_root_.string());
  }
  if (!saved_game_root_.empty()) {
    REXLOG_INFO("  Saved games:    {}", saved_game_root_.string());
  }

  return true;
}

bool ReXApp::ConstructRuntime(const PathConfig& paths) {
  if (runtime_) {
    // A live runtime owns guest memory, kernel objects, and the graphics
    // backend. Replacing it here would destroy them without a cooperative
    // drain. A relaunch must shut the old runtime down first.
    REXLOG_ERROR("ConstructRuntime refused: a runtime is already active");
    return false;
  }
  if (paths.game_data_root.empty()) {
    auto msg = std::string("--game_data_root was not provided.");
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }
  if (!std::filesystem::is_directory(paths.game_data_root)) {
    auto msg = fmt::format("--game_data_root does not exist: {}", paths.game_data_root.string());
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  game_data_root_ = paths.game_data_root;
  user_data_root_ = paths.user_data_root;
  update_data_root_ = paths.update_data_root;
  cache_root_ = paths.cache_root;
  metadata_root_ = paths.metadata_root;
  marketplace_content_root_ = paths.marketplace_content_root;
  saved_game_root_ = paths.saved_game_root;

  runtime_ = std::make_unique<rex::Runtime>(
      paths.game_data_root, paths.user_data_root, paths.update_data_root, paths.cache_root,
      paths.metadata_root, paths.marketplace_content_root, paths.saved_game_root);
  runtime_->set_app_context(&app_context());

  // Window and ImGui drawer already exist from SetupPresentation; publish them
  // to the runtime before Setup so hooks and native rendering see them.
  if (window_) {
    runtime_->set_display_window(window_.get());
  }
  if (imgui_drawer_) {
    runtime_->set_imgui_drawer(imgui_drawer_.get());
  }

  auto status = runtime_->Setup(ppc_info_, std::move(config_));
  if (XFAILED(status)) {
    REXLOG_ERROR("Runtime setup failed: {:08X}", status);
#if REX_PLATFORM_ANDROID
    // Say so instead of closing silently, then leave without tearing down the
    // half-initialized runtime and presentation: that teardown has crashed
    // (a double free under the ImGui drawer) and there is nothing to save.
    const std::string message =
        fmt::format("The game could not start on this device (runtime setup failed, status "
                    "{:08X}).\n\nThe reason is in the crash report the launcher shows next "
                    "and in Android/data/com.libertyrecomp/files/Liberty Recompiled/logs.",
                    status);
    // SDL shows a real Android dialog and waits for OK; the POSIX message box
    // only prints to stderr.
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Liberty Recompiled", message.c_str(), nullptr);
    rex::FlushLogging();
    std::_Exit(3);
#endif
    return false;
  }

  if (window_ && runtime_->input_system()) {
    static_cast<rex::input::InputSystem*>(runtime_->input_system())->AttachWindow(window_.get());
  }

  if (ppc_info_.register_modules) {
    ppc_info_.register_modules(runtime_->kernel_state());
  }

  if (imgui_drawer_) {
    auto* input_sys = static_cast<rex::input::InputSystem*>(runtime_->input_system());
    if (input_sys) {
      input_sys->SetActiveCallback([capture = input_capture_,
                                    imgui_capture = imgui_drawer_->input_capture_snapshot()]() {
        const bool title_captured = capture->title.load(std::memory_order_acquire);
        const bool overlay_present = capture->overlay.load(std::memory_order_acquire);
        const bool imgui_captured = imgui_capture->load(std::memory_order_acquire);
        const bool active = !capture->stopping.load(std::memory_order_acquire) &&
                            !title_captured && !(overlay_present && imgui_captured);
        if (rex::input::IsInputTraceEnabled()) {
          const int32_t state = active ? 1 : 0;
          const int32_t previous =
              capture->last_trace.exchange(state, std::memory_order_acq_rel);
          if (previous != state) {
            REXLOG_INFO(
                "input-e2e: seq={} stage=focus-capture owner=app active={} title-captured={} "
                "overlay-present={} imgui-captured={}",
                rex::input::NextInputTraceSequence(), active, title_captured, overlay_present,
                imgui_captured);
          }
        }
        return active;
      });
    }
  }

  std::string xex_image = "game:\\default.xex";
  OnLoadXexImage(xex_image);

  // Mirrors the game:\ / d:\ -> game_data_root mapping in Runtime::SetupVfs.
  {
    constexpr std::string_view kGameDevice = "game:\\";
    constexpr std::string_view kDDevice = "d:\\";
    std::string_view tail = xex_image;
    if (tail.starts_with(kGameDevice)) {
      tail.remove_prefix(kGameDevice.size());
    } else if (tail.starts_with(kDDevice)) {
      tail.remove_prefix(kDDevice.size());
    }
    std::string host_tail{tail};
    std::replace(host_tail.begin(), host_tail.end(), '\\', '/');
    auto xex_host = paths.game_data_root / host_tail;
    if (!std::filesystem::is_regular_file(xex_host)) {
      auto msg = fmt::format("Entrypoint XEX not found: {}", xex_host.string());
      REXLOG_ERROR("{}", msg);
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
      return false;
    }
  }

  status = runtime_->LoadXexImage(xex_image);
  if (XFAILED(status)) {
    auto msg = fmt::format("Failed to load XEX ({}): {:08X}", xex_image, status);
    REXLOG_ERROR("{}", msg);
    rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
    return false;
  }

  OnPostLoadXexImage();

  if (ppc_info_.rexcrt_heap) {
    if (!rex::kernel::crt::InitHeap(REXCVAR_GET(rexcrt_heap_size_mb), runtime_->memory())) {
      REXLOG_ERROR("Failed to initialize rexcrt heap");
      return false;
    }
  }

  OnPostSetup();

  return true;
}

bool ReXApp::ConfigureGraphicsBackend() {
  // RuntimeConfig is consumed by the first Runtime::Setup (graphics is moved
  // out), so both the first launch and a return-to-hub rebuild start from a
  // fresh config and a fresh backend plugin instance.
  config_ = RuntimeConfig{};
  config_.gpu_plugin = REXCVAR_GET(gpu_plugin);
#if REX_PLATFORM_MAC && !REX_PLATFORM_IOS
  config_.audio_factory = REX_AUDIO_BACKEND(rex::audio::coreaudio::CoreAudioAudioSystem);
#else
  config_.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
#endif
  config_.input_factory = REX_INPUT_BACKEND(rex::input::CreateDefaultInputSystem);
  config_.kernel_init = rex::kernel::InitializeKernel;

  OnPreSetup(config_);

  if (!config_.graphics && !config_.gpu_plugin.empty()) {
    config_.graphics = rex::system::LoadGpuPlugin(config_.gpu_plugin);
    if (!config_.graphics) {
      // Fatal by design: no silent headless fallback.
      auto msg =
          fmt::format("Failed to load GPU plugin '{}'. See log for details.", config_.gpu_plugin);
      REXLOG_ERROR("{}", msg);
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, msg);
      return false;
    }
  }

  if (config_.graphics) {
    X_STATUS status = config_.graphics->SetupPresentation(&app_context());
    if (XFAILED(status)) {
      REXLOG_ERROR("Graphics presentation setup failed: {:08X}", status);
      return false;
    }
  }
  return true;
}

bool ReXApp::CreateMainWindow() {
  if (window_) {
    // The window, its listeners, and its input binding outlive guest sessions;
    // only the presenter attached to it is rebuilt.
    return true;
  }
  window_ = rex::ui::Window::Create(app_context(), GetName(), 1280, 720);
  if (!window_) {
    REXLOG_ERROR("Failed to create window");
    return false;
  }

  // Set window title with SDK build stamp
  std::string title = std::string(GetName()) + " " + REXGLUE_BUILD_TITLE;
  window_->SetTitle(title);

  window_->AddListener(this);
  window_->AddInputListener(this, 0);

  if (REXCVAR_GET(fullscreen)) {
    window_->SetFullscreen(true);
  }
  if (!window_->Open()) {
    REXLOG_ERROR("Failed to open the main window");
    return false;
  }
  return true;
}

bool ReXApp::AttachPresentationStack() {
  auto* graphics_system = config_.graphics.get();
  if (graphics_system && graphics_system->presenter()) {
    // SDK mode: the emulated-Xenos presenter drives the overlays.
    auto* presenter = graphics_system->presenter();
    auto* provider = graphics_system->provider();
    if (provider) {
      immediate_drawer_ = provider->CreateImmediateDrawer();
      if (immediate_drawer_) {
        immediate_drawer_->SetPresenter(presenter);
        SetupOverlays(presenter, immediate_drawer_.get());
      }
    }
    window_->SetPresenter(presenter);
  } else if (!graphics_system) {
    // Detached mode: the app brings its own renderer and drives its own paint
    // loop. ReXApp owns the returned drawer via immediate_drawer_.
    immediate_drawer_ = OnCreateImmediateDrawer();
    if (immediate_drawer_) {
      SetupOverlays(/*presenter=*/nullptr, immediate_drawer_.get());
      // No window_->SetPresenter, no drawer SetPresenter: the app owns the
      // surface and the present cadence.
    }
  }
  return true;
}

bool ReXApp::SetupPresentation() {
  if (!ConfigureGraphicsBackend()) {
    return false;
  }
  if (!CreateMainWindow()) {
    return false;
  }
  return AttachPresentationStack();
}

void ReXApp::SetupOverlays(rex::ui::Presenter* presenter, rex::ui::ImmediateDrawer* drawer) {
  imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(
      window_.get(), 64, [this](ImFontAtlas* atlas) { OnConfigureFonts(atlas); });
  // presenter is nullptr in detached mode; ImGuiDrawer tolerates that and the
  // gated eager font upload in SetImmediateDrawer is skipped (font uploads
  // lazily on the first Draw instead).
  imgui_drawer_->SetPresenterAndImmediateDrawer(presenter, drawer);
  if (!overlay_binds_registered_) {
    // The bind registry is global and ignores duplicate names, so register the
    // callbacks once; they reach the current session's dialogs through `this`.
    overlay_binds_registered_ = true;
    rex::ui::RegisterBind("bind_debug_overlay", "F3", "Toggle debug overlay", [this] {
      if (debug_overlay_) {
        debug_overlay_.reset();
      } else {
        debug_overlay_ =
            std::make_unique<ui::DebugOverlayDialog>(imgui_drawer_.get(), frame_stats_provider_);
      }
      PublishInputOverlayState();
    });
    rex::ui::RegisterBind("bind_console", "Backtick", "Toggle console overlay", [this] {
      if (console_overlay_) {
        console_overlay_.reset();
      } else {
        console_overlay_ = std::make_unique<ui::ConsoleDialog>(imgui_drawer_.get(), log_sink_);
      }
      PublishInputOverlayState();
    });
    rex::ui::RegisterBind("bind_settings", "F4", "Toggle settings overlay", [this] {
      if (settings_overlay_) {
        settings_overlay_.reset();
      } else {
        settings_overlay_ = std::make_unique<ui::SettingsDialog>(imgui_drawer_.get(), config_path_);
      }
      PublishInputOverlayState();
    });
    rex::ui::RegisterBind("bind_achievements", "F7", "Toggle achievements overlay", [this] {
      if (achievements_overlay_) {
        achievements_overlay_.reset();
      } else {
        achievements_overlay_ = CreateAchievementsOverlay();
      }
      PublishInputOverlayState();
    });
  }

  OnCreateDialogs(imgui_drawer_.get());
}

void ReXApp::PublishInputOverlayState() {
  input_capture_->overlay.store(debug_overlay_ || console_overlay_ || settings_overlay_ ||
                                 achievements_overlay_, std::memory_order_release);
}

void ReXApp::LaunchModule() {
  app_context().CallInUIThreadDeferred([this]() {
    // A second launch is only allowed once the previous module thread has
    // finished. Joining a live thread here would block the UI event loop, so a
    // live thread is refused instead; the caller must drain it first.
    if (module_thread_.joinable()) {
      if (module_thread_live_.load(std::memory_order_acquire)) {
        REXLOG_ERROR("LaunchModule refused: the previous module thread is still running");
        return;
      }
      module_thread_.join();
    }

    // Register the achievement notification callback now that the runtime and
    // KernelState are guaranteed to exist. Done here (not OnCreateDialogs)
    // because KernelState is null during SetupPresentation.
    if (!achievement_notification_) {
      achievement_notification_ =
          std::shared_ptr<ui::AchievementNotificationDialog>(CreateAchievementNotificationDialog());
    }
    if (achievement_notification_ && achievement_notification_listener_ == 0 && runtime_ &&
        runtime_->kernel_state()) {
      std::weak_ptr<ui::AchievementNotificationDialog> notification = achievement_notification_;
      achievement_notification_listener_ = achievements().RegisterNotificationCallback(
          [notification](const rex::system::AchievementEvent& event) {
            if (auto dialog = notification.lock()) {
              dialog->Push(event);
            }
          });
    }

    OnPreLaunchModule();

    auto main_thread = runtime_->PrepareModuleLaunch();
    if (!main_thread) {
      REXLOG_ERROR("Failed to launch module");
      app_context().QuitFromUIThread();
      return;
    }

    auto* graphics_system = runtime_->graphics_system();
    if (graphics_system && !runtime_->cache_root().empty()) {
      uint32_t title_id = runtime_->kernel_state()->title_id();
      if (title_id != 0) {
        REXLOG_INFO("Initializing shader storage for title {:08X}...", title_id);
        graphics_system->InitializeShaderStorage(runtime_->cache_root(), title_id, true);
      }
    }

    OnPostLaunchModule(main_thread.get());
    const X_STATUS resume_status = RequiresSynchronizedInitialThreadResume()
                                       ? main_thread->ResumeFromInitialSuspension()
                                       : main_thread->Resume();
    if (XFAILED(resume_status)) {
      REXLOG_ERROR("Failed to resume the main guest thread: {:08X}", resume_status);
      app_context().QuitFromUIThread();
      return;
    }

    module_thread_live_.store(true, std::memory_order_release);
    module_thread_ = std::thread([this, main_thread = std::move(main_thread)]() mutable {
      main_thread->Wait(0, 0, 0, nullptr);
      OnGuestThreadExit(main_thread.get());
      REXLOG_INFO("Execution complete");
      module_thread_live_.store(false, std::memory_order_release);
      // An intentional return to the hub ends the guest thread too; the
      // process must survive so the hub can start the next session.
      if (!shutting_down_.load(std::memory_order_acquire) &&
          !restart_in_progress_.load(std::memory_order_acquire)) {
        app_context().CallInUIThread([this]() { app_context().QuitFromUIThread(); });
      }
    });
  });
}

std::function<void(PathConfig)> ReXApp::MakeResumeCallback() {
  return [this](PathConfig paths) {
    if (shutting_down_.load(std::memory_order_acquire))
      return;
    if (!ConstructRuntime(std::move(paths))) {
      app_context().QuitFromUIThread();
      return;
    }
    LaunchModule();
  };
}

void ReXApp::ReleasePresentationStack() {
  // Title globals that own dialogs or immediate textures must go before the
  // drawers they were created from.
  OnPresentationTeardown();

  if (achievement_notification_listener_ != 0) {
    if (runtime_ && runtime_->kernel_state()) {
      achievements().UnregisterCallback(achievement_notification_listener_);
    }
    achievement_notification_listener_ = 0;
  }
  achievement_notification_.reset();
  achievements_overlay_.reset();
  settings_overlay_.reset();
  console_overlay_.reset();
  debug_overlay_.reset();

  if (imgui_drawer_) {
    imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
    imgui_drawer_.reset();
  }
  // The drawer detaches from the presenter before the runtime destroys it; in
  // detached mode it never had one.
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(nullptr);
    immediate_drawer_.reset();
  }
  if (runtime_) {
    runtime_->set_display_window(nullptr);
    runtime_->set_imgui_drawer(nullptr);
  }
  if (window_) {
    window_->SetPresenter(nullptr);
  }
  PublishInputOverlayState();
}

bool ReXApp::RebuildPresentationStack() {
  if (!window_) {
    REXLOG_ERROR("F02 restart: cannot rebuild presentation without a window");
    return false;
  }
  if (!ConfigureGraphicsBackend()) {
    return false;
  }
  return AttachPresentationStack();
}

bool ReXApp::RestartSessionForHub(std::function<void(bool)> completion) {
  if (!app_context().IsInUIThread()) {
    REXLOG_ERROR("F02 restart refused: not on the UI thread");
    return false;
  }
  if (shutting_down_.load(std::memory_order_acquire)) {
    REXLOG_ERROR("F02 restart refused: the process is closing");
    return false;
  }
  if (!runtime_ || !window_) {
    REXLOG_WARN("F02 restart refused: no active guest session");
    return false;
  }
  bool expected = false;
  if (!restart_in_progress_.compare_exchange_strong(expected, true)) {
    REXLOG_WARN("F02 restart refused: a return is already in progress");
    return false;
  }

  const uint32_t generation = session_generation_.load(std::memory_order_acquire);
  REXLOG_INFO("F02 restart: begin generation={} runtime={}", generation + 1,
              fmt::ptr(runtime_.get()));

  // A thread left over from a session that ended by itself can be joined now;
  // a live one is the expected case and exits during the drain.
  if (module_thread_.joinable() && !module_thread_live_.load(std::memory_order_acquire)) {
    module_thread_.join();
  }

  auto sequence = std::make_shared<system::SessionRestartSequence>();
  sequence->Begin();

  // The drain and save barrier happen on a worker so the UI thread can serve
  // the graphics system's synchronous presenter teardown. The completion is
  // always posted back to the UI thread.
  std::thread worker([this, sequence, generation,
                      completion = std::move(completion)]() mutable {
    system::ShutdownReport report;
    report.status = system::ShutdownStatus::kComplete;
    if (runtime_) {
      auto release_presentation = [this, sequence] {
        // Only after the drain and the save barrier proved clean: stop the
        // title's workers and detach every object that still points at the
        // graphics system. A blocked result never reaches this point, so the
        // title keeps its session resources for a retry.
        if (module_thread_.joinable()) {
          module_thread_.join();
        }
        app_context().CallInUIThreadSynchronous([this, sequence] {
          OnSessionEnding();
          sequence->CompleteStep(true);  // quiesce-session
          ReleasePresentationStack();
          sequence->CompleteStep(true);  // release-presentation
        });
      };
      // One retry: the first attempt already asked the guest to terminate, so
      // a drain or save barrier that was still busy usually finishes shortly.
      // A blocked result destroyed nothing and kept the save gate closed, so a
      // second attempt cannot start new work behind the gate.
      for (int attempt = 0; attempt < 2; ++attempt) {
        const uint32_t guest_ms = attempt == 0 ? 15000 : 5000;
        const auto save_ms = std::chrono::milliseconds(attempt == 0 ? 15000 : 5000);
        report = runtime_->ShutdownCooperatively(guest_ms, save_ms, release_presentation);
        if (report.complete() ||
            report.status == system::ShutdownStatus::kReleasedIncomplete) {
          // The presentation is gone; report incomplete teardown without relaunching.
          break;
        }
        REXLOG_WARN(
            "F02 restart: shutdown blocked (status={}) on attempt {}; the guest was already "
            "asked to terminate, retrying",
            static_cast<int>(report.status), attempt + 1);
      }
    }

    if (!report.complete()) {
      const bool release_incomplete =
          report.status == system::ShutdownStatus::kReleasedIncomplete;
      sequence->CompleteStep(false);  // stop-runtime → kShutdownBlocked
      if (release_incomplete) {
        REXLOG_ERROR(
            "F02 restart: teardown retained the memory owner (status={}); "
            "the presentation is gone and no new session may start",
            static_cast<int>(report.status));
      } else {
        REXLOG_ERROR(
            "F02 restart: runtime did not shut down cleanly (status={}); "
            "keeping the process on the existing session resources",
            static_cast<int>(report.status));
      }
      app_context().CallInUIThread([this, release_incomplete, failure = sequence->failure(),
                                    completion = std::move(completion)]() mutable {
        REXLOG_ERROR("F02 restart: blocked ({})",
                     system::SessionRestartFailureName(failure));
        restart_in_progress_.store(false, std::memory_order_release);
        completion(false);
        if (release_incomplete) {
          // Retained kernel resources must not be reused by another title.
          REXLOG_ERROR("F02 restart: closing the app instead of returning to the hub");
          app_context().QuitFromUIThread();
        }
      });
      return;
    }
    sequence->CompleteStep(true);  // stop-runtime

    if (module_thread_.joinable()) {
      module_thread_.join();
    }
    sequence->CompleteStep(true);  // join-module-thread

    // Shutdown() already destroyed the subsystems; the Runtime object itself
    // must go so ConstructRuntime's single-session guard can admit the next
    // title. This runs on the worker, which owns no UI objects.
    runtime_.reset();
    sequence->CompleteStep(true);  // reset-runtime

    app_context().CallInUIThread(
        [this, sequence, generation, completion = std::move(completion)]() mutable {
          const bool rebuilt = RebuildPresentationStack();
          sequence->CompleteStep(rebuilt);  // rebuild-presentation
          restart_in_progress_.store(false, std::memory_order_release);
          if (!rebuilt) {
            // The old session is gone and the presentation cannot come back:
            // the process has nothing left to show or resume. Quit cleanly
            // instead of leaving a window with no drawer.
            REXLOG_ERROR("F02 restart: presentation rebuild failed ({}); closing the app",
                         system::SessionRestartFailureName(sequence->failure()));
            app_context().QuitFromUIThread();
            return;
          }
          sequence->CompleteStep(true);  // resume-host-ui
          session_generation_.store(generation + 1, std::memory_order_release);
          REXLOG_INFO("F02 restart: session {} is ready for the hub", generation + 1);
          completion(true);
        });
  });
  worker.detach();
  return true;
}

bool ReXApp::StartSessionAfterReturn(const PathConfig& paths) {
  if (!app_context().IsInUIThread()) {
    REXLOG_ERROR("F02 start refused: not on the UI thread");
    return false;
  }
  if (shutting_down_.load(std::memory_order_acquire) ||
      restart_in_progress_.load(std::memory_order_acquire)) {
    REXLOG_ERROR("F02 start refused: a return or shutdown is in progress");
    return false;
  }
  if (runtime_) {
    REXLOG_ERROR("F02 start refused: a runtime is already active");
    return false;
  }
  if (!ConstructRuntime(paths)) {
    REXLOG_ERROR("F02 start: the next session's runtime could not be constructed");
    return false;
  }
  LaunchModule();
  REXLOG_INFO("F02 start: session {} launched", session_generation_.load(std::memory_order_acquire));
  return true;
}

void ReXApp::OnKeyDown(ui::KeyEvent& e) {
  if (input_capture_->title.load(std::memory_order_acquire)) {
    if (rex::input::IsInputTraceEnabled()) {
      REXLOG_INFO("input-e2e: seq={} stage=focus-capture owner=app key=down result=title-captured",
                  e.input_trace_sequence());
    }
    return;
  }
  if (rex::input::IsInputTraceEnabled()) {
    REXLOG_INFO("input-e2e: seq={} stage=focus-capture owner=app key=down result=forwarded",
                e.input_trace_sequence());
  }
  rex::ui::ProcessKeyEvent(e);
}

void ReXApp::OnClosing(ui::UIEvent& e) {
  (void)e;
  REXLOG_INFO("Window closing, shutting down...");
  shutting_down_.store(true, std::memory_order_release);
  input_capture_->stopping.store(true, std::memory_order_release);
  if (runtime_ && runtime_->kernel_state()) {
    runtime_->kernel_state()->TerminateTitle();
  }
  // Hard-exit rather than run subsystem teardown, which can deadlock on a host
  // lock still held by a straggler TerminateTitle left running. Flush (not
  // ShutdownLogging, which frees loggers a straggler may still use); the OS
  // reclaims the rest.
  REXLOG_INFO("Title terminated; hard-exiting process.");
  rex::FlushLogging();
  std::_Exit(0);
}

bool ReXApp::OnCloseRequested(ui::UIEvent& e) {
  (void)e;
  const bool accepted = OnWindowCloseRequested();
  REXLOG_INFO("app-quit-trace: source=window-listener-close-requested accepted={}", accepted);
  return accepted;
}

void ReXApp::OnResize(ui::UISetupEvent& e) {
  (void)e;
  if (!window_) {
    return;
  }
  OnWindowPixelSizeChanged(window_->GetActualPhysicalWidth(), window_->GetActualPhysicalHeight());
  OnWindowResized(window_->GetActualLogicalWidth(), window_->GetActualLogicalHeight());
}

void ReXApp::OnDpiChanged(ui::UISetupEvent& e) {
  (void)e;
  if (!window_) {
    return;
  }
  OnDpiScaleChanged(float(window_->GetDpi()) / float(window_->GetMediumDpi()));
}

void ReXApp::OnGotFocus(ui::UISetupEvent& e) {
  (void)e;
  OnWindowFocusChanged(true);
}

void ReXApp::OnLostFocus(ui::UISetupEvent& e) {
  (void)e;
  OnWindowFocusChanged(false);
}

void ReXApp::OnMinimized(ui::UIEvent& e) {
  (void)e;
  OnWindowMinimized();
}

void ReXApp::OnRestored(ui::UIEvent& e) {
  (void)e;
  OnWindowRestored();
}

void ReXApp::OnDestroy() {
  input_capture_->stopping.store(true, std::memory_order_release);
  // Notify subclass before cleanup
  OnShutdown();

  // Unregister overlay keybinds before destroying dialogs
  rex::ui::UnregisterBind("bind_debug_overlay");
  rex::ui::UnregisterBind("bind_console");
  rex::ui::UnregisterBind("bind_settings");
  rex::ui::UnregisterBind("bind_achievements");

  // ImGui cleanup (reverse of setup)
  if (achievement_notification_listener_ != 0) {
    if (runtime_ && runtime_->kernel_state()) {
      achievements().UnregisterCallback(achievement_notification_listener_);
    }
    achievement_notification_listener_ = 0;
  }
  achievement_notification_.reset();
  achievements_overlay_.reset();
  settings_overlay_.reset();
  console_overlay_.reset();
  debug_overlay_.reset();
  if (imgui_drawer_) {
    imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
    imgui_drawer_.reset();
  }
  // immediate_drawer_ was already unlinked from imgui_drawer_ above. Detach it
  // from its presenter so SDK mode runs OnLeavePresenter() before disposal; in
  // detached mode the drawer never had a presenter, so SetPresenter(nullptr) is
  // a no-op.
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(nullptr);
    immediate_drawer_.reset();
  }
  if (runtime_) {
    runtime_->set_display_window(nullptr);
    runtime_->set_imgui_drawer(nullptr);
  }
  // Window/runtime cleanup
  if (window_) {
    window_->SetPresenter(nullptr);
  }
  if (module_thread_.joinable()) {
    module_thread_.join();
  }
  if (window_) {
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
  }
  window_.reset();
  runtime_.reset();
}

void ReXApp::SetGuestFrameStats(ui::DebugOverlayDialog::FrameStatsProvider provider) {
  frame_stats_provider_ = provider;
  if (debug_overlay_) {
    debug_overlay_->SetStatsProvider(provider);
  }
}

}  // namespace rex
