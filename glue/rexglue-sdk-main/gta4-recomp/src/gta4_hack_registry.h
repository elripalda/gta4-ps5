#pragma once

// Every setting that makes the port behave differently from the original game
// or from a straightforward renderer: speed shortcuts, gameplay changes, crash
// guards and architecture choices, with what each does and, where one exists,
// the value that turns it back to the plain behaviour.
//
// Safe mode (gta4_safe_mode) applies those values at launch, so a bug report
// can be checked against the port without its shortcuts in one run, and then
// narrowed down one entry at a time. tools/check_hack_registry.py fails the
// build check when a cvar of a registered category is missing here.

#include <array>
#include <string_view>

namespace gta4::hacks {

enum class Kind {
  kShortcut,      // speed: same result intended, a shorter path to it
  kGameplay,      // changes what the game does (streaming, automatic reductions)
  kGuard,         // keeps the game from crashing or hanging; stays on
  kArchitecture,  // a renderer design choice; the alternative is not a plain fallback
  kSetting,       // a user choice or tuning value, not a hack
  kDiagnostic,    // measurement or logging
};

void ApplySafeModeIfRequested();

struct Entry {
  std::string_view cvar;
  Kind kind;
  // Value safe mode applies; empty = left as it is.
  std::string_view safe_value;
  std::string_view summary;
};

inline constexpr auto kRegistry = std::to_array<Entry>({
    // --- Renderer shortcuts that can change the image when they are wrong ---
    {"gta4_native_fold_depth_clears", Kind::kShortcut, "false", "depth clears as load-op clears (LRZ)"},
    {"gta4_native_drop_inert_blending", Kind::kShortcut, "false", "no-op blending dropped (LRZ)"},
    {"gta4_native_materialize_by_copy", Kind::kShortcut, "false", "same-format materialization as an image copy"},
    {"gta4_native_worker_constant_ring", Kind::kShortcut, "false", "draw constants written by the worker"},
    {"gta4_native_upload_memo_first", Kind::kShortcut, "false", "buffers bound from their memoized GPU copy"},
    {"gta4_native_shared_key_memo", Kind::kShortcut, "false", "shared constants reused from the previous draw"},
    {"gta4_native_gpu_detile", Kind::kShortcut, "false", "textures untiled on the GPU"},
    {"gta4_native_ubo_shared_constants", Kind::kShortcut, "false", "shared constants through a uniform buffer"},
    {"gta4_native_sampled_stage_biases", Kind::kShortcut, "false", "LOD biases only for sampled stages"},
    {"gta4_native_prepared_binding_cache", Kind::kShortcut, "false", "texture bindings reused across draws"},
    {"gta4_native_merge_up_draws", Kind::kShortcut, "false", "water patches merged into one draw"},
    {"gta4_native_merge_indexed_draws", Kind::kShortcut, "false", "adjacent indexed draws merged"},
    {"gta4_native_fast_legacy_mul", Kind::kShortcut, "false", "cheaper form of Xenos zero-preserving multiplies"},
    {"gta4_native_ieee_mul", Kind::kShortcut, "0", "plain IEEE multiplies (not bit-exact)"},
    {"gta4_native_skip_repeated_clears", Kind::kShortcut, "false", "clears of already-clear targets skipped"},
    {"gta4_native_skip_invisible_draws", Kind::kShortcut, "false", "draws that change no pixel skipped"},
    // Safe mode sets this one to an empty list (special-cased: "" means no value elsewhere).
    {"gta4_native_skip_pixel_shaders", Kind::kShortcut, "", "draws of listed pixel shaders skipped"},
    {"gta4_native_empty_fragment", Kind::kShortcut, "false", "empty fragment shader for shaderless color draws"},
    {"gta4_native_colorless_fragment", Kind::kShortcut, "false", "color math dropped from depth-only pipelines"},
    {"gta4_native_early_fragment_tests", Kind::kShortcut, "false", "depth test before shading where nothing writes depth"},
    {"gta4_native_reuse_lighting_passes", Kind::kShortcut, "false", "light stencil setups kept in the lighting pass"},
    {"gta4_native_reuse_lighting_real", Kind::kShortcut, "false", "lighting reuse with the draws' own shaders"},
    {"gta4_native_clear_skips_materialization", Kind::kShortcut, "false", "no EDRAM copy into a surface about to be cleared"},
    {"gta4_native_lossless_resolve_optimization", Kind::kShortcut, "false", "prior resolve contents discarded on full overwrite"},
    {"gta4_native_present_from_surface", Kind::kShortcut, "false", "present straight from the final surface"},
    {"gta4_native_dedupe_vertex_bindings", Kind::kShortcut, "false", "repeated vertex buffer binds skipped"},
    {"gta4_native_keep_draw_state", Kind::kShortcut, "false", "draw state kept across clears and markers"},
    {"gta4_native_vertex_cache_order", Kind::kShortcut, "false", "triangles reordered for the vertex cache"},
    {"gta4_native_partial_constant_upload", Kind::kShortcut, "false", "only readable constants uploaded"},
    {"gta4_native_partial_constant_snapshot", Kind::kShortcut, "false", "only readable constants snapshotted"},
    {"gta4_native_lazy_constant_hash", Kind::kShortcut, "false", "constants hashed only where uploaded"},
    {"gta4_native_buffer_validation_per_frame", Kind::kShortcut, "false", "buffer shadow checked once per frame"},
    {"gta4_native_recheck_vertex_textures", Kind::kGuard, "", "vertex-shader textures re-read every frame"},
    {"gta4_native_stamped_protection", Kind::kShortcut, "false", "texture protection once per frame"},
    {"gta4_native_assembled_frame_scans", Kind::kShortcut, "false", "frame scans taken from the worker's assembly"},
    {"gta4_native_assembly_constant_bound", Kind::kShortcut, "false", "constant arena sized during assembly"},
    {"gta4_native_direct_constant_apply", Kind::kShortcut, "false", "constant changes applied without a delta copy"},
    {"gta4_native_worker_constant_hash", Kind::kShortcut, "false", "constant hashes taken on the worker"},
    {"gta4_native_coalesce_state_commands", Kind::kShortcut, "false", "state commands carried in the next command"},
    {"gta4_native_recycle_commands", Kind::kShortcut, "false", "retired commands reused"},
    {"gta4_native_batched_worker_wake", Kind::kShortcut, "false", "worker woken per batch"},
    {"gta4_native_stage_draw_commands", Kind::kShortcut, "false", "commands published in batches"},
    {"gta4_native_async_command_retire", Kind::kShortcut, "false", "commands destroyed on a background thread"},
    {"gta4_native_async_constant_release", Kind::kShortcut, "false", "constant versions released on a background thread"},
    {"gta4_native_resolve_swap", Kind::kShortcut, "false", "resolve-then-clear as an image swap"},
    {"gta4_native_lazy_resolves", Kind::kShortcut, "false", "unused resolves skipped"},
    {"gta4_native_skip_attachment_barriers", Kind::kShortcut, "false", "same-attachment barriers skipped"},
    {"gta4_native_fs_relaxed_precision", Kind::kShortcut, "false", "FP16 pixel shader math"},
    // --- Renderer shortcuts with no effect on the result ---
    {"gta4_native_recorder_prefetch", Kind::kShortcut, "", "prefetch of upcoming commands"},
    {"gta4_native_fast_capacity", Kind::kShortcut, "", "upload capacity planned with flat sets"},
    {"gta4_native_sparse_capacity_plan", Kind::kShortcut, "", "staging arena re-planned every 30 frames"},
    {"gta4_native_batch_texture_uploads", Kind::kShortcut, "", "texture uploads batched"},
    {"gta4_native_pipeline_prewarm", Kind::kShortcut, "", "pipelines compiled ahead on the worker"},
    // --- Renderer architecture ---
    {"gta4_native_pipelined_recording", Kind::kArchitecture, "", "frames recorded on a second thread"},
    {"gta4_native_ubo_constants", Kind::kArchitecture, "", "constants through dynamic uniform buffers"},
    {"gta4_native_persistent_buffers", Kind::kArchitecture, "", "converted geometry kept in GPU blocks"},
    {"gta4_native_descriptor_backend", Kind::kArchitecture, "", "descriptor backend"},
    {"gta4_native_descriptor_paging", Kind::kArchitecture, "", "fence-owned descriptor working sets"},
    {"gta4_native_state_transport", Kind::kArchitecture, "", "draw-state transport"},
    {"gta4_depth_handoff_transport", Kind::kArchitecture, "", "forward-depth handoff transport"},
    {"gta4_native_frames_in_flight", Kind::kArchitecture, "", "GPU frames in flight"},
    {"gta4_native_max_queued_frames", Kind::kArchitecture, "", "frames the producer may run ahead"},
    {"gta4_native_frame_complete_signal", Kind::kArchitecture, "", "when the title sees a frame as complete"},
    {"gta4_native_merge_up_max", Kind::kSetting, "", "most patches per merged water draw"},
    {"gta4_native_texture_budget_poll_frames", Kind::kSetting, "", "texture budget check interval"},
    {"gta4_native_texture_memory_limit_mb", Kind::kSetting, "", "texture heap cap"},
    {"gta4_native_texture_packs", Kind::kSetting, "", "custom <xxh64>.dds texture packs"},
    {"gta4_native_texture_packs_dir", Kind::kSetting, "", "custom texture packs directory"},
    {"gta4_native_texture_dump", Kind::kSetting, "", "dump Xbox 360 textures as <xxh64>.dds"},
    {"gta4_native_texture_dump_dir", Kind::kSetting, "", "texture dump output directory"},
    {"gta4_native_host_fog", Kind::kSetting, "", "host fog"},
    {"gta4_native_host_sun_shafts", Kind::kSetting, "", "host sun shafts"},
    {"gta4_native_light_overrides", Kind::kSetting, "", "light shader overrides"},
    // --- Renderer guards ---
    {"gta4_native_loop_watchdog", Kind::kGuard, "", "runaway shader loops end instead of hanging the GPU"},
    {"gta4_native_loop_watchdog_budget", Kind::kGuard, "", "shader loop iteration budget"},
    // --- Renderer diagnostics ---
    {"gta4_native_debug_force_depth_clamp", Kind::kDiagnostic, "", ""},
    {"gta4_native_debug_gpu_attribution", Kind::kDiagnostic, "", ""},
    {"gta4_native_depth_bias_constant_scale", Kind::kDiagnostic, "", ""},
    {"gta4_native_depth_bias_slope_scale", Kind::kDiagnostic, "", ""},
    // --- Runtime, threads and power ---
    {"gta4_frame_wait_sleep", Kind::kShortcut, "false", "sleep instead of spin while waiting for a frame slot"},
    {"gta4_extra_frames_in_flight", Kind::kShortcut, "0", "title runs ahead of its two pending frames"},
    {"gta4_thread_pinning", Kind::kShortcut, "0", "busiest threads pinned to big cores"},
    {"android_pin_hot_threads", Kind::kShortcut, "false", "hot threads pinned"},
    {"kernel_spinlock_spins", Kind::kShortcut, "0", "guest spinlocks spin before yielding"},
    {"android_perf_hint", Kind::kShortcut, "false", "Android performance hint session"},
    {"android_perf_hint_target_ms", Kind::kSetting, "", "performance hint target"},
    {"android_gpu_turbo", Kind::kSetting, "false", "GPU held at its highest clock"},
    {"android_thermal_guard", Kind::kGameplay, "false", "draw distance lowered when hot"},
    {"android_thermal_guard_hot_c", Kind::kSetting, "", "thermal guard start temperature"},
    {"android_thermal_guard_cool_c", Kind::kSetting, "", "thermal guard end temperature"},
    {"android_thermal_log", Kind::kDiagnostic, "", ""},
    {"gta4_fps_guard", Kind::kGameplay, "0", "environment reflections reduced when frames run long"},
    // --- Gameplay (densities are user settings: safe mode keeps them) ---
    {"gta4_ped_density_scale", Kind::kSetting, "", "pedestrian density"},
    {"gta4_scenario_ped_density_scale", Kind::kSetting, "", "scenario pedestrian density"},
    {"gta4_traffic_density_scale", Kind::kSetting, "", "traffic density"},
    {"gta4_parked_car_density_scale", Kind::kSetting, "", "parked car density"},
    {"gta4_streaming_modern", Kind::kGameplay, "false", "earlier world requests, deadline queue"},
    {"gta4_streaming_deadline_order", Kind::kGameplay, "false", "requests picked by readiness deadline"},
    {"gta4_streaming_budget_scale", Kind::kGameplay, "1.0", "larger streaming budgets"},
    {"gta4_streaming_physical_reserve_mb", Kind::kSetting, "", "streaming physical memory reserve"},
    {"gta4_streaming_prefetch_pending_limit", Kind::kSetting, "", "streaming prefetch limit"},
    {"gta4_streaming_preload_max", Kind::kSetting, "", "streaming preload distance (max)"},
    {"gta4_streaming_preload_min", Kind::kSetting, "", "streaming preload distance (min)"},
    // --- Guards ---
    {"gta4_stale_object_guard", Kind::kGuard, "", "stale entity pointers skipped instead of crashing"},
    // --- Diagnostics elsewhere ---
    {"vulkan_command_counters", Kind::kDiagnostic, "", ""},
    {"gta4_native_gpu_breadcrumbs", Kind::kDiagnostic, "", ""},
    {"gta4_native_vertex_texture_trace", Kind::kDiagnostic, "", ""},
});

}  // namespace gta4::hacks
