#include "cached_cvar.h"
#include "modern_shader_options.h"
#include "modern_shader_diagnostics.h"
#include <rex/cvar.h>
#include <rex/diagnostics/policy.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(gta4_modern_shaders, false, "GTA IV/Graphics/Post-Processing",
                    "Use modern visual shader replacements; applies on the next complete frame")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(gta4_trace_modern_shaders, false, "GTA IV/Diagnostics",
                    "Log Modern shader execution, DoF passes, inputs, skips and periodic counts");
REXCVAR_DEFINE_BOOL(gta4_trace_modern_shader_gpu, false, "GTA IV/Diagnostics",
                    "Probe Metal sky draws asynchronously: raster visibility and before/after pixel samples");

namespace rex::graphics::gta4_native {
ModernShaderSettings ReadModernShaderSettings() {
  return {CachedQuery<bool>("gta4_modern_shaders"),
          CachedQuery<bool>("gta4_disable_tlad_film_grain")};
}
bool ModernShaderTraceEnabled() {
  return CachedQuery<bool>("gta4_trace_modern_shaders") || ModernShaderGpuProbeEnabled() ||
      rex::diagnostics::IsEnabled(rex::diagnostics::Category::kNativeTrace);
}
bool ModernShaderGpuProbeEnabled() { return CachedQuery<bool>("gta4_trace_modern_shader_gpu"); }
}  // namespace rex::graphics::gta4_native

namespace rex::graphics::gta4_native {
namespace {
const char* EffectName(ModernEffect effect) {
  constexpr const char* names[] = {"depth-of-field", "fog", "bloom", "tone-mapping", "sun-shafts",
      "cloud-mask", "sky", "motion-blur", "tlad-film-grain", "water", "light-volume"};
  return names[size_t(effect)];
}
const char* ExecutionName(ModernExecution result) {
  constexpr const char* names[] = {"encoded", "skipped", "fallback", "stock", "reused"};
  return names[size_t(result)];
}
}
void ModernShaderDiagnostics::BeginFrame(bool enabled, ModernShaderSettings settings, const char* backend) {
  ++frame_;
  const bool changed = enabled != enabled_ || settings != settings_;
  const bool interval = frame_ - window_start_ >= 300;
  if (enabled_ && (changed || interval)) Summary();
  if (changed || interval) {
    counts_ = {}; changes_ = {}; last_.fill(ModernExecution::kCount); window_start_ = frame_;
    failures_.Reset(); sky_.Reset();
  }
  enabled_ = enabled; settings_ = settings; backend_ = backend;
  if (enabled_ && changed) {
    REXLOG_INFO("gta4-modern-exec: backend={} event=settings trace-frame={} enabled={} disable-tlad-grain={} "
        "evidence=command-encoding detail-limit=2-per-result transitions=8 summary-every=300-guest-frames",
        backend_, frame_, settings.enabled, settings.disable_tlad_grain);
    REXLOG_INFO("gta4-modern-exec: backend={} event=algorithms dof=stipple,bokeh16,tent4,depth-combine "
        "fog=radial-height-and-sky-color bloom=resolution-scaled-hue-preserving "
        "tone=gamma2.2-PQ-Frostbite32 sun=prepass,radial24,radial24,add "
        "note=encoding-does-not-prove-visible-pixel-contribution", backend_);
  }
}
bool ModernShaderDiagnostics::Observe(ModernEffect effect, ModernExecution result) {
  if (!enabled_) return false;
  const size_t index = size_t(effect);
  const bool changed = last_[index] != result;
  last_[index] = result;
  const uint64_t count = ++counts_[index][size_t(result)];
  // Bound both normal repetition and rapidly alternating fallback/success.
  const bool transition = changed && changes_[index]++ < 8;
  return count <= 2 || transition;
}
void ModernShaderDiagnostics::Summary() {
  REXLOG_INFO("gta4-modern-exec: backend={} event=failure-summary trace-frames={}-{} total={} signatures={} untracked={}",
      backend_, window_start_, frame_, failures_.total(), failures_.entries().size(), failures_.overflow());
  for (const auto& entry : failures_.entries())
    REXLOG_INFO("gta4-modern-exec: backend={} event=failure-count signature={} count={}", backend_, entry.key, entry.count);
  for (size_t i = 0; i < counts_.size(); ++i) {
    const auto& c = counts_[i];
    REXLOG_INFO("gta4-modern-exec: backend={} event=summary trace-frames={}-{} effect={} "
        "encoded={} skipped={} fallback={} stock={} reused={}", backend_, window_start_, frame_,
        EffectName(ModernEffect(i)), c[0], c[1], c[2], c[3], c[4]);
  }
}
bool ModernShaderDiagnostics::Failure(std::string_view point, uint64_t command, uint32_t type,
    uint32_t phase, uint64_t vertex, uint64_t pixel, std::string_view reason) {
  if (!enabled_) return false;
  // Resource addresses/generations after ':' belong to the detail record, not
  // the signature. Resource churn must not defeat repetition limits.
  const auto key = fmt::format("{}|{}|{}|{:016X}|{:016X}|{}", point, type, phase,
      vertex, pixel, reason.substr(0, reason.find(':')));
  const auto result = failures_.Observe(key);
  if (!result.report) return false;
  REXLOG_ERROR("gta4-modern-exec: backend={} event=failure trace-frame={} command={} type={} phase={} "
      "point={} vs={:016X} ps={:016X} modern={} count={} overflow={} reason={}",
      backend_, frame_, command, type, phase, point, vertex, pixel, settings_.enabled,
      result.count, result.overflow, reason);
  return true;
}
bool ModernShaderDiagnostics::Sky(uint64_t vertex, uint64_t pixel, uint32_t samples,
    uint64_t variant, uint32_t target) {
  if (!enabled_ || !IsFusionSkyShader(pixel)) return false;
  const auto key = fmt::format("{:016X}|{:016X}|{}|{}|{:08X}", vertex, pixel, samples, variant, target);
  const auto result = sky_.Observe(key);
  // At most eight distinct sky contracts per settings/300-frame window.
  return !result.overflow && result.count == 1 && sky_.entries().size() <= 8;
}
void ModernShaderDiagnostics::SkySkipped(uint64_t command,uint64_t pixel,uint32_t phase,std::string_view reason) {
  if(!enabled_ || !IsFusionSkyShader(pixel))return;
  const auto result=sky_.Observe(fmt::format("skip|{:016X}|{}|{}",pixel,phase,reason));
  if(result.report)REXLOG_INFO("gta4-modern-exec: backend={} event=sky-skipped trace-frame={} command={} "
      "ps={:016X} phase={} count={} reason={}",backend_,frame_,command,pixel,phase,result.count,reason);
}
void ModernShaderDiagnostics::Draw(const ModernDrawTrace& d) {
  if (!enabled_) return;
  const auto family = ClassifyModernShader(d.pixel);
  const auto* e = d.environment;
  const uint64_t valid = e ? e->valid_fields : 0;
  auto report = [&](ModernEffect effect, bool replacement, std::string_view action, uint64_t required = 0) {
    auto result = replacement ? ModernExecution::kEncoded : ModernExecution::kStock;
    if (replacement && (valid & required) != required) result = ModernExecution::kFallback;
    if (!Observe(effect, result)) return false;
    REXLOG_INFO("gta4-modern-exec: backend={} event=draw trace-frame={} effect={} result={} "
        "action={} master={} phase={} vs={:016X}:override={} ps={:016X}:override={} "
        "target={}x{} samples={} environment-seq={} valid={:016X} missing={:016X}",
        backend_, frame_, EffectName(effect), ExecutionName(result), action, settings_.enabled,
        d.phase, d.vertex, d.vertex_override, d.pixel, d.pixel_override, d.width, d.height, d.samples,
        e ? e->source_sequence : 0, valid, required & ~valid);
    return true;
  };
  if (SupportsSplitPostFx(d.pixel)) {
    if (report(ModernEffect::kTone, settings_.enabled && d.pixel_override && d.tone_lut,
               "gamma2.2-to-PQ-to-Frostbite-LUT"))
      REXLOG_INFO("gta4-modern-exec: backend={} event=composite ps={:016X} lut-bound={} "
          "upgraded-dof-input={} title-dof-bypassed={} bloom-threshold={} alpha=preserved",
          backend_, d.pixel, d.tone_lut, d.dof_bypass, d.dof_bypass,
          settings_.enabled && d.pixel_override ? "hue-preserving" : "title");
  }
  switch (family) {
    case ModernShaderFamily::kFog: {
      const bool deferred = d.pixel == 0x1A6669BBAFDC43E7ull;
      const uint64_t required = deferred ? 0x001CB6F0u : 0x009CB6F0u;
      if (report(ModernEffect::kFog, settings_.enabled && d.pixel_override,
          deferred ? "deferred-radial-height-fog" : "forward-radial-height-fog", required) && e)
        REXLOG_INFO("gta4-modern-exec: backend={} event=fog-inputs density={} height-falloff={} "
            "altitude-tweak={} power={} camera-z={} far-clip={} sky-exposure={} "
            "pixel-guards=viewport,depth,world-w,material-fog-start",
            backend_, e->fog_density, e->fog_height_falloff, e->fog_altitude_tweak, e->fog_power,
            e->camera_altitude, e->fog_far_clip, e->sky_color_exposure[3]);
      break;
    }
    case ModernShaderFamily::kBloom:
      report(ModernEffect::kBloom, d.pixel_override, "normalized-center9-broad25-resolution-scaled"); break;
    case ModernShaderFamily::kSky:
      report(ModernEffect::kSky, d.pixel_override, "sky-cloud-fog-and-reflection"); break;
    case ModernShaderFamily::kMotionBlur:
      report(ModernEffect::kMotion, d.pixel_override, "motion-blur-composite-compatibility"); break;
    case ModernShaderFamily::kTladGrain:
      report(ModernEffect::kGrain, d.pixel_override, "tlad-noise-composite"); break;
    case ModernShaderFamily::kWater:
      report(ModernEffect::kWater, d.pixel_override, "water-replacement"); break;
    case ModernShaderFamily::kLightVolume:
      report(ModernEffect::kLight, d.pixel_override && d.vertex_override, "paired-light-volume"); break;
    default: break;
  }
}
void ModernShaderDiagnostics::Dof(const ModernDofTrace& t) {
  if (!enabled_) return;
  const bool zero = NativeDofCanBeElided(t.parameters.dof_projection, t.parameters.dof_distance,
                                        t.parameters.dof_blur);
  const auto result = t.encoded ? (zero ? ModernExecution::kSkipped : ModernExecution::kEncoded)
                                : ModernExecution::kFallback;
  if (!Observe(ModernEffect::kDof, result)) return;
  const auto& p = t.parameters;
  REXLOG_INFO("gta4-modern-exec: backend={} event=dof trace-frame={} ps={:016X} phase={} "
      "result={} reason={} published-passes={} kernel={} composite-bypass={} "
      "projection={},{},{},{} distance={},{},{},{} blur={},{},{},{} depth-source={}",
      backend_, frame_, t.pixel, t.phase, ExecutionName(result), t.encoded && zero ? "zero-blur-constants" : t.reason,
      t.encoded ? (zero ? 1 : 4) : 0, zero ? "stipple-only-zero-blur" : "stipple,bokeh16,tent4,depth-combine",
      t.encoded, p.dof_projection[0], p.dof_projection[1], p.dof_projection[2], p.dof_projection[3],
      p.dof_distance[0], p.dof_distance[1], p.dof_distance[2], p.dof_distance[3],
      p.dof_blur[0], p.dof_blur[1], p.dof_blur[2], p.dof_blur[3], uint32_t(p.depth_source));
  const auto texture = [&](const char* role, const ModernTextureTrace& image) {
    REXLOG_INFO("gta4-modern-exec: backend={} event=dof-input role={} handle={:08X} generation={} "
        "size={}x{} ready={}", backend_, role, image.handle, image.generation, image.width, image.height, image.ready);
  };
  texture("scene", t.scene); texture("half-scene", t.half_scene);
  texture("depth", t.depth); texture("stipple-mask", t.mask);
}
void ModernShaderDiagnostics::SkipPostFx(uint64_t pixel, uint32_t phase, std::string_view reason, bool reused) {
  const auto result = reused ? ModernExecution::kReused : ModernExecution::kSkipped;
  if (Observe(ModernEffect::kDof, result))
    REXLOG_INFO("gta4-modern-exec: backend={} event=dof trace-frame={} ps={:016X} phase={} result={} reason={}",
        backend_, frame_, pixel, phase, ExecutionName(result), reason);
}
void ModernShaderDiagnostics::Sun(uint64_t pixel, const SunShaftParameters& p,
    const EnvironmentalDataV2* environment, bool encoded, std::string_view reason) {
  const auto result = encoded ? ModernExecution::kEncoded :
      (p.valid ? ModernExecution::kFallback : ModernExecution::kSkipped);
  if (!Observe(ModernEffect::kSun, result)) return;
  REXLOG_INFO("gta4-modern-exec: backend={} event=sun-shafts trace-frame={} ps={:016X} result={} reason={} "
      "passes={} uv={},{} intensity={} authored-intensity={} horizon-fade={} view-sun={},{},{} "
      "cloud={}x{} cloud-bound={} depth-near={} depth-far={} fog-depth={} environment-valid={:016X}",
      backend_, frame_, pixel, ExecutionName(result), reason, encoded ? 4 : 0,
      p.screen_position[0], p.screen_position[1], p.intensity,
      environment ? environment->sun_shafts_intensity : 0, p.horizon_fade,
      p.view_sun_projection[0], p.view_sun_projection[1], p.view_sun_projection[2],
      p.cloud_width, p.cloud_height, p.cloud_mask_address != 0,
      p.depth_projection[1], p.depth_projection[2], p.depth_projection[3],
      environment ? environment->valid_fields : 0);
}
void ModernShaderDiagnostics::Cloud(uint64_t pixel, bool ready, uint32_t width, uint32_t height) {
  const auto result = ready ? ModernExecution::kEncoded : ModernExecution::kFallback;
  if (Observe(ModernEffect::kCloudMask, result))
    REXLOG_INFO("gta4-modern-exec: backend={} event=cloud-mask trace-frame={} ps={:016X} "
        "result={} size={}x{} action=write-one-minus-cloud-opacity",
        backend_, frame_, pixel, ExecutionName(result), width, height);
}
}  // namespace rex::graphics::gta4_native
