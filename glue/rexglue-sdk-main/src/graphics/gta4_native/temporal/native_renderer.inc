// Included in graphics_system.cpp: these methods share the renderer's captured
// command/resource types without exposing them through the public title ABI.
bool Gta4NativeGraphicsSystem::EnsureTemporalAssets() {
  if (temporal_assets_checked_) return temporal_assets_ready_;
  temporal_assets_checked_ = true;
  auto* provider = static_cast<ui::vulkan::VulkanProvider*>(provider_.get());
  const auto* device = provider ? provider->vulkan_device() : nullptr;
  if (!device || device->properties().maxColorAttachments < 7 || !device->properties().independentBlend ||
      !device->properties().shaderStorageImageExtendedFormats) return false;
  std::string error;
  const auto executable_folder = rex::filesystem::GetExecutableFolder();
#if REX_PLATFORM_MAC
  const auto path = executable_folder.parent_path() / "Resources" / "shaders" /
                    "vulkan-temporal.bin";
#else
  const auto path = executable_folder / "shaders" / "vulkan-temporal.bin";
#endif
  if (!temporal_shader_archive_.Load(path, error) ||
      !temporal_scene_.Initialize(device)) {
    REXLOG_WARN("gta4-native-temporal: unavailable: {} {}", error, temporal_scene_.error());
    return false;
  }
  temporal_assets_ready_ = true;
  return true;
}

uint32_t Gta4NativeGraphicsSystem::QueryTemporalCapabilities() {
  // Called under temporal_provider_mutex_. Query independent temporary SDK
  // contexts; never reconfigure history used by an asynchronously recorded CB.
  if (temporal_capabilities_checked_) return temporal_capabilities_;
  temporal_capabilities_checked_ = true;
#if REX_PLATFORM_PS5
  // [ps5 port] FSR 3 on the PS5 is new: probe it (load its 100 MB shader
  // archive, make test contexts) only when it was chosen in the hub.
  if (rex::cvar::GetFlagByName("gta4_native_upscaler") != "fsr3") {
    REXLOG_INFO("gta4-native-temporal: FSR 3 not selected; not probed");
    return 0;
  }
#endif
  if (!EnsureTemporalAssets()) return 0;

  auto* provider = static_cast<ui::vulkan::VulkanProvider*>(provider_.get());
  const auto* device = provider->vulkan_device();
  const temporal::Configuration probe{{128, 128}, {128, 128}, temporal::Quality::kNative, true, false, false};
  temporal::VulkanScene scene_probe;
  if (!scene_probe.Initialize(device) || !scene_probe.Configure(probe)) {
#if REX_PLATFORM_PS5
    REXLOG_WARN("gta4-native-temporal: FSR 3 probe: the temporal scene could not be set up: {}",
                scene_probe.error());
#endif
    scene_probe.Shutdown(); return 0;
  }
  scene_probe.Shutdown();
  const auto resolver = device->vulkan_instance()->functions().vkGetDeviceProcAddr;
  temporal::FsrUpscalerVulkan fsr_probe;
#if REX_PLATFORM_PS5
  {
    // [ps5 port] Say which step turns FSR 3 down.
    std::string reason;
    const bool supported = temporal::FsrSupportsDevice(*device, &reason);
    const bool initialized =
        supported && fsr_probe.Initialize(device->physical_device(), device->device(), resolver);
    const bool configured = initialized && fsr_probe.Configure(probe);
    const bool upscaler =
        configured && fsr_upscaler_.Initialize(device->physical_device(), device->device(), resolver);
    REXLOG_INFO("gta4-native-temporal: FSR 3 probe: device {} ({}), context {}, configure {}, "
                "upscaler {}; {}",
                supported, reason, initialized, configured, upscaler,
                !initialized || !configured ? fsr_probe.last_error() : fsr_upscaler_.last_error());
    if (upscaler) temporal_capabilities_ |= kCapabilityFsr3Upscaling;
  }
#else
  if (temporal::FsrSupportsDevice(*device) &&
      fsr_probe.Initialize(device->physical_device(), device->device(), resolver) &&
      fsr_probe.Configure(probe) &&
      fsr_upscaler_.Initialize(device->physical_device(), device->device(), resolver))
    temporal_capabilities_ |= kCapabilityFsr3Upscaling;
#endif
  if (dlss_upscaler_.Initialize(*device)) temporal_capabilities_ |= kCapabilityDlssUpscaling;
  if (temporal_capabilities_ & (kCapabilityFsr3Upscaling | kCapabilityDlssUpscaling)) {
    temporal_capabilities_ |= kCapabilityTemporalInputs;
    if (temporal::VulkanFrameGeneration::SupportsProvider(*device, temporal::FrameGenerationProvider::kFsr))
      temporal_capabilities_ |= kCapabilityFsrFrameGeneration;
    if (temporal::VulkanFrameGeneration::SupportsProvider(*device, temporal::FrameGenerationProvider::kDlss))
      temporal_capabilities_ |= kCapabilityDlssFrameGeneration;
  }
#if REX_PLATFORM_PS5
  rex::frame_stats::g_frame_generation_active.store(
      rex::cvar::GetFlagByName("gta4_native_frame_generation") == "fsr3" &&
          (temporal_capabilities_ & kCapabilityFsrFrameGeneration),
      std::memory_order_relaxed);
  REXLOG_INFO("gta4-native-temporal: capabilities {:#x}; frame generation {}", temporal_capabilities_,
              rex::frame_stats::g_frame_generation_active.load() ? "on" : "off");
#endif
  return temporal_capabilities_;
}

VkShaderModule Gta4NativeGraphicsSystem::GetTemporalShaderModule(
    const NativeShader* shader, bool overridden, bool late) {
  const auto stage = shader && shader->stage == ShaderStage::kVertex
      ? temporal::ShaderStage::kVertex : temporal::ShaderStage::kPixel;
  const uint64_t hash = shader ? shader->hash : 0;
  const auto key = std::make_tuple(hash, uint32_t(stage), overridden, late);
  if (const auto found = temporal_shader_modules_.find(key); found != temporal_shader_modules_.end())
    return found->second;
  const auto words = temporal_shader_archive_.Find(hash, stage, overridden, late);
  if (words.empty()) return VK_NULL_HANDLE;
  std::vector<uint32_t> module(words.begin(), words.end());
  RemapSpirvDrawDescriptorSets(module);
  if (REXCVAR_GET(gta4_native_loop_watchdog)) {
    if (auto guarded = AddSpirvLoopWatchdog(module, 16384)) module = std::move(guarded->words);
  }
  if (stage == temporal::ShaderStage::kPixel) {
    std::string error;
    auto transformed = AddNativeColorOutputEpilogue(module, &error, 0x70u);
    if (!transformed) {
      REXLOG_ERROR("gta4-native-temporal: shader {:016X} color contract: {}", hash, error);
      return VK_NULL_HANDLE;
    }
    module = std::move(*transformed);
  }
  auto* provider = static_cast<ui::vulkan::VulkanProvider*>(provider_.get());
  const auto handle = ui::vulkan::util::CreateShaderModule(provider->vulkan_device(),
      module.data(), module.size() * sizeof(uint32_t));
  if (handle) temporal_shader_modules_.emplace(key, handle);
  return handle;
}

bool Gta4NativeGraphicsSystem::PrepareTemporalFrame() {
  temporal_frame_leases_[active_frame_slot_].clear();
  temporal_generated_frame_ = {};
  temporal_display_ready_ = false;
  temporal_ui_exact_ = true;
  temporal_composite_source_ = nullptr;
  temporal_display_present_generation_ = 0;
  temporal_frame_prepared_ = temporal_active_ = temporal_resolved_ = temporal_encoded_ = false;
  // A frame without temporal updates has no scene, and its commands still
  // carry default temporal fields unless an earlier pass set them: skip the
  // walk, which read the first cache line of every command.
  if (!frame_has_temporal_updates_ && !temporal_fields_set_) {
    rex::frame_stats::NoteFgDrop(rex::frame_stats::FgDrop::kNoScene);
    return true;
  }
  temporal_fields_set_ = false;
  // One pass, and no stores into commands still at their defaults (every
  // command of a frame without temporal output): dirtying each command's
  // first line cost a cache-line transfer per draw.
  std::optional<TemporalCommand> scene;
  for (auto& command : current_frame_) {
    if (command.temporal_scene_binding || command.temporal_scene_stage != UINT32_MAX ||
        command.temporal_prefilter || command.temporal_composite_mode) {
      command.temporal_scene_binding = nullptr;
      command.temporal_scene_stage = UINT32_MAX;
      command.temporal_prefilter.reset();
      command.temporal_composite_mode = 0;
    }
    if (command.type != CommandType::kTemporalUpdate) continue;
    TemporalCommand candidate{};
    std::memcpy(&candidate, command.bytes.data(), sizeof(candidate));
    if (candidate.event == TemporalEvent::kBeginScene) scene = candidate;
  }
  if (!scene) {
    rex::frame_stats::NoteFgDrop(rex::frame_stats::FgDrop::kNoScene);
    return true;
  }
  std::lock_guard lock(temporal_provider_mutex_);
  const auto selected = temporal_query_selections_.find(scene->sequence);
  if (selected == temporal_query_selections_.end() ||
      selected->second.render != temporal::Extent{scene->width, scene->height} ||
      selected->second.output != temporal::Extent{scene->output_width, scene->output_height}) {
    rex::frame_stats::NoteFgDrop(rex::frame_stats::FgDrop::kSceneSize);
    return true;
  }
  const auto selection = selected->second;
  if (!EnsureTemporalAssets()) {
    rex::frame_stats::NoteFgDrop(rex::frame_stats::FgDrop::kAssets);
    return true;
  }
  if (temporal_provider_recreate_) {
    if (!CompleteSecondaryNativeFrameSlot()) return false;
    temporal_provider_recreate_ = false;
  }
  temporal::Configuration config{{scene->width, scene->height},
      {scene->output_width, scene->output_height}, selection.quality, true,
      bool(scene->flags & kTemporalDepthReversed), false};
  // This helper is called after the active slot was retired. On resize, retire
  // the other slot too before replacing shared SDK history or descriptor views.
  if (!temporal_scene_.Matches(config)) {
    if (!CompleteSecondaryNativeFrameSlot()) return false;
    if (temporal_output_binding_) {
      InvalidateCachedDescriptors(temporal_output_binding_->descriptor_lifetime);
      if (!temporal_output_binding_->descriptor_reclaimed) {
        RetireNativeTextureDescriptor(*temporal_output_binding_);
        if (!ReclaimCompletedNativeTextureDescriptors(completed_command_buffer_submission_)) return false;
      }
      temporal_output_binding_.reset();
    }
    if (!temporal_scene_.Configure(config)) return false;
    temporal_history_valid_ = false;
    temporal_geometry_history_.Reset();
    temporal_output_binding_ = std::make_unique<NativeTextureImage>();
    auto& binding = *temporal_output_binding_;
    const auto& image = temporal_scene_.output();
    binding.resource.image = image.image;
    binding.resource.view = image.view;
    binding.format = image.format;
    binding.usage = image.usage;
    binding.width = image.extent.width; binding.height = image.extent.height;
    binding.logical_width = scene->width; binding.logical_height = scene->height;
    binding.view_type = VK_IMAGE_VIEW_TYPE_2D;
    binding.view_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    binding.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    binding.descriptor_lifetime = ++next_native_image_lifetime_;
  }
  temporal_configuration_ = config;
  temporal_metadata_ = *scene;
#if REX_PLATFORM_PS5
  // [ps5 port] FSR's history (upscaling and frame generation) counts scenes,
  // one after another: the guest's frame count can advance twice per scene.
  if (temporal_last_guest_sequence_ && scene->sequence != temporal_last_guest_sequence_ + 1)
    rex::frame_stats::g_fg_sequence_gaps.fetch_add(1, std::memory_order_relaxed);
  temporal_last_guest_sequence_ = scene->sequence;
  ++temporal_history_sequence_;
#endif
  temporal_provider_ = selection.provider;
  const bool configured = temporal_provider_ == TemporalUpscalerProvider::kFsr3
      ? fsr_upscaler_.Configure(config) : dlss_upscaler_.Configure(config);
  if (!configured) {
    REXLOG_WARN("gta4-native-temporal: provider configuration failed: {}",
        temporal_provider_ == TemporalUpscalerProvider::kFsr3 ? fsr_upscaler_.last_error() : dlss_upscaler_.status());
    // Keep the concrete display-sized composite route alive if creation fails
    // after the title has allocated reduced-resolution render targets. Encode
    // will decline and the spatial fallback supplies that route for this frame.
  }
  for (auto& command : current_frame_) {
    const auto& instance = command.temporal_instance;
    if (!instance || instance->sequence != scene->sequence || instance->epoch != scene->epoch ||
        !(instance->flags & kTemporalFinalCompositeExecution) || !command.pipeline_state ||
        !command.pipeline_state->pixel_shader_resource) continue;
    const auto contract = gta4_metal::temporal::CompositeForShader(
        command.pipeline_state->pixel_shader_resource->hash);
    if (!contract || !command.textures[contract->scene_stage]) continue;
    if (contract->filter == gta4_metal::temporal::CompositeFilter::kStockF6) {
      auto prefilter = std::make_shared<NativeCommand>(command);
      prefilter->temporal_composite_mode = 1;
      prefilter->fixed_function_state.depth_enable = prefilter->fixed_function_state.depth_write_enable = 0;
      prefilter->fixed_function_state.stencil_enable = prefilter->fixed_function_state.alpha_test_enable = 0;
      prefilter->fixed_function_state.alpha_to_mask = prefilter->fixed_function_state.alpha_to_mask_enable = 0;
      prefilter->fixed_function_state.blend_enable = 0;
      prefilter->fixed_function_state.blend_controls.fill(0x00010001u);
      prefilter->fixed_function_state.color_write_mask = 0xFu;
      command.temporal_prefilter = std::move(prefilter);
      command.temporal_composite_mode = 2;
      temporal_fields_set_ = true;
    }
    temporal_output_binding_->source = command.textures[contract->scene_stage];
    command.temporal_scene_binding = temporal_output_binding_.get();
    command.temporal_scene_stage = contract->scene_stage;
    temporal_fields_set_ = true;
  }
  temporal_frame_prepared_ = true;
  return true;
}

void Gta4NativeGraphicsSystem::ObserveTemporalCommand(VkCommandBuffer cb, const TemporalCommand& command) {
  if (!temporal_frame_prepared_ || command.sequence != temporal_metadata_.sequence ||
      command.epoch != temporal_metadata_.epoch) return;
  if (command.event == TemporalEvent::kBeginScene) {
    temporal_active_ = temporal_scene_.Begin(cb);
    temporal_resolved_ = temporal_camera_adopted_ = false;
    temporal_depth_source_ = nullptr;
    temporal_motion_complete_ = true;
    temporal_world_draws_ = temporal_history_draws_ = 0;
    temporal_composite_scope_ = 0;
  } else if (command.event == TemporalEvent::kBeforePostFx &&
             (command.flags & kTemporalCompositeExecution)) {
    temporal_composite_scope_ = command.time_ns;
  } else if (command.event == TemporalEvent::kAfterPostFx &&
             command.time_ns == temporal_composite_scope_) {
    temporal_active_ = false;
    temporal_composite_scope_ = 0;
  } else if (command.event == TemporalEvent::kEndFrame) {
    temporal_active_ = false;
  }
}

void Gta4NativeGraphicsSystem::ClassifyTemporalDraw(const NativeCommand& command,
                                                   NativeRenderingTarget& target) {
  target.temporal_shader = command.temporal_composite_mode != 0;
  if (!temporal_active_ || temporal_resolved_ || target.is_reflection ||
      target.samples != VK_SAMPLE_COUNT_1_BIT || target.width != temporal_metadata_.width ||
      target.height != temporal_metadata_.height || !command.pipeline_state) return;
  const auto* instance = command.temporal_instance.get();
  const bool current = instance && instance->sequence == temporal_metadata_.sequence &&
      instance->epoch == temporal_metadata_.epoch;
  const auto flags = current ? instance->flags : 0;
  const auto* depth = virtual_resource_registry_.Find(command.pipeline_state->depth_stencil.handle);
  const auto* color = virtual_resource_registry_.Find(command.pipeline_state->render_targets[0].handle);
  const bool primary = depth && depth->scale_domain == VirtualResourceScaleDomain::kPrimaryScene &&
      ((color && color->scale_domain == VirtualResourceScaleDomain::kPrimaryScene &&
        (target.color_attachment_mask & 7u) == 7u) ||
       (!target.color_attachment_mask && command.fixed_function_state.depth_write_enable &&
        ((flags & kTemporalSceneGeometry) || command.lighting.stage == RenderExecutionStage::kSceneToGBuffer)));
  if (primary && !temporal_camera_adopted_ && (flags & kTemporalExecutionCamera)) {
    const auto projection = gta4_metal::temporal::DescribeProjection(instance->projection);
    if (projection && projection->reversed == temporal_configuration_.reversed_depth) {
      if (temporal_history_valid_ && gta4_metal::temporal::CameraDiscontinuity(
          temporal_previous_inverse_, instance->view_inverse)) temporal_metadata_.flags |= kTemporalCameraCut;
      temporal_metadata_.projection = instance->projection;
      temporal_metadata_.view_inverse = instance->view_inverse;
      temporal_metadata_.view = instance->view;
      temporal_metadata_.near_plane = projection->near_plane;
      temporal_metadata_.far_plane = projection->far_plane;
      temporal_metadata_.field_of_view = projection->field_of_view;
      temporal_metadata_.aspect = projection->aspect;
      if (projection->reversed) temporal_metadata_.flags |= kTemporalDepthReversed;
      else temporal_metadata_.flags &= ~kTemporalDepthReversed;
      temporal_camera_adopted_ = temporal_geometry_history_.Begin(temporal_metadata_.epoch,
          temporal_metadata_.view, TemporalHistorySequence(),
          !temporal_history_valid_ || bool(temporal_metadata_.flags & kTemporalCameraCut));
    }
  }
  const bool camera_matches = temporal_camera_adopted_ && (flags & kTemporalExecutionCamera) &&
      instance->projection == temporal_metadata_.projection &&
      instance->view_inverse == temporal_metadata_.view_inverse;
  const bool composite = current && (flags & kTemporalFinalCompositeExecution) &&
      instance->time_ns == temporal_composite_scope_;
  const auto policy = gta4_metal::temporal::ClassifySceneDraw(true, false, composite, primary,
      bool(flags & kTemporalSceneGeometry), bool(flags & kTemporalScreenSpace), camera_matches,
      command.lighting);
  const auto& fixed = command.fixed_function_state;
  bool blending = false;
  for (uint32_t i = 0; i < kRenderTargetCount; ++i)
    if ((target.color_attachment_mask & (1u << i)) && IsNativeBlendControlEnabled(fixed.blend_controls[i]))
      blending = true;
  target.temporal_motion = policy.world && fixed.depth_enable && fixed.depth_write_enable && !blending;
  target.temporal_reactive = (policy.world || policy.effect) && !target.temporal_motion && target.color_write_mask;
  target.temporal_jitter = policy.world && camera_matches && (temporal_metadata_.flags & kTemporalJitterApplied);
  target.temporal_shader = target.temporal_motion || target.temporal_reactive || target.temporal_jitter ||
      command.temporal_composite_mode != 0;
  const bool preserves = target.temporal_reactive && !fixed.depth_write_enable &&
      (blending || (command.pipeline_state->pixel_shader_resource &&
       IsFusionSkyShader(command.pipeline_state->pixel_shader_resource->hash)));
  if (policy.motion_relevant && (target.color_write_mask || fixed.depth_write_enable) &&
      ((!target.temporal_motion && !preserves) || (target.temporal_motion && !target.temporal_jitter))) {
#if REX_PLATFORM_PS5
    // [ps5 port] Two kinds of draw no longer cost the frame its generated
    // twin: one with motion but drawn without the frame's jitter (gta_default
    // vs4: at most a sub-pixel shimmer on it), and one with no motion that
    // writes no depth (hair's sorted alpha: drawn as reactive, like the sky).
    // Every frame had one, so frame generation never made a frame.
    const bool tolerated = target.temporal_motion || (target.temporal_reactive && !fixed.depth_write_enable);
    if (tolerated) {
      NOTE_FG_MOTION_LOST(target.temporal_motion ? "motion without jitter, tolerated"
                                                 : "no motion, no depth, tolerated",
                          command.pipeline_state);
    } else {
      if (temporal_motion_complete_) NOTE_FG_MOTION_LOST("no motion, not blended", command.pipeline_state);
      temporal_motion_complete_ = false;
    }
#else
    if (temporal_motion_complete_)
      NOTE_FG_MOTION_LOST(!target.temporal_motion ? "no motion, not blended" : "motion without jitter",
                          command.pipeline_state);
    temporal_motion_complete_ = false;
#endif
  }
}

bool Gta4NativeGraphicsSystem::BindTemporalDraw(VkCommandBuffer cb, const NativeCommand& command,
    const NativeRenderingTarget& target, VkPipelineLayout layout,
    const NativeUploadAllocation& vertex, const NativeUploadAllocation& pixel,
    const NativeUploadAllocation& shared) {
  if (!vertex.mapping || !shared.mapping) return false;
  temporal::TemporalDrawParameters parameters{};
  parameters.input_extent = {float(target.width), float(target.height)};
  if (target.temporal_jitter) parameters.jitter_clip = {
      2.0f * temporal_metadata_.jitter[0] / target.width,
      -2.0f * temporal_metadata_.jitter[1] / target.height};
  parameters.reactive = target.temporal_reactive;
  NativeUploadAllocation prior_vertex = vertex, prior_shared = shared, parameters_upload{};
  if (target.temporal_motion) {
    const auto& state = *command.pipeline_state;
    const auto* instance = command.temporal_instance.get();
    uint64_t generation = 0;
    temporal::GeometryKey key{};
    key.instance = instance && instance->instance ? instance->instance : XXH3_64bits(vertex.mapping, 64);
    key.drawable = instance ? instance->drawable : 0;
    key.pose = instance ? instance->pose : 0;
    key.vertex_shader = state.vertex_shader_resource->hash;
    key.pixel_shader = state.pixel_shader_resource ? state.pixel_shader_resource->hash : 0;
    key.layout = state.vertex_declaration_resource->content_hash;
    bool captured = command.type != CommandType::kDrawPrimitiveUp;
    for (uint32_t i = 0; i < kVertexStreamCount; ++i) {
      if (!command.vertex_buffers[i]) continue;
      key.streams[i] = {command.vertex_buffers[i]->generation,
                        state.vertex_streams[i].offset, state.vertex_streams[i].stride};
      captured &= key.streams[i].generation != 0;
      generation = XXH3_64bits_withSeed(&key.streams[i], sizeof(key.streams[i]), generation);
    }
    if (command.type == CommandType::kDrawPrimitive) {
      DrawPrimitiveCommand draw{}; std::memcpy(&draw, command.bytes.data(), sizeof(draw));
      key.primitive = draw.primitive_type; key.range = uint64_t(draw.start_vertex) << 32 | draw.vertex_count;
    } else if (command.type == CommandType::kDrawIndexedPrimitive) {
      DrawIndexedPrimitiveCommand draw{}; std::memcpy(&draw, command.bytes.data(), sizeof(draw));
      key.indexed = true; key.primitive = draw.primitive_type; key.base_vertex = draw.base_vertex;
      key.range = uint64_t(draw.start_index) << 32 | draw.index_count;
      key.index_generation = command.index_buffer ? command.index_buffer->generation : 0;
      captured &= key.index_generation != 0;
      generation = XXH3_64bits_withSeed(&key.index_generation, sizeof(key.index_generation), generation);
    }
    key.geometry_generation = captured ? generation : 0;
    const auto history = temporal_geometry_history_.Record(key,
        {vertex.mapping, kVertexConstantsSize}, {shared.mapping, sizeof(NativeSharedConstants)},
        parameters.jitter_clip);
    if (history.previous && !history.ambiguous && !state.vertex_shader_resource->used_texture_mask &&
        !(temporal_metadata_.flags & kTemporalCameraCut)) {
      if (!AllocateUpload(history.previous->vertex.size(), 16, prior_vertex, NativeUploadKind::kVertexConstants) ||
          !AllocateUpload(history.previous->shared.size(), 16, prior_shared, NativeUploadKind::kSharedConstants)) return false;
      std::memcpy(prior_vertex.mapping, history.previous->vertex.data(), history.previous->vertex.size());
      std::memcpy(prior_shared.mapping, history.previous->shared.data(), history.previous->shared.size());
      parameters.valid_history = 1;
      ++temporal_history_draws_;
      parameters.previous_jitter_clip = history.previous->jitter_clip;
    }
    NativeSharedConstants constants{};
    std::memcpy(&constants, shared.mapping, sizeof(constants));
    temporal_half_pixel_ = {constants.half_pixel_offset_x, constants.half_pixel_offset_y};
    if (constants.modern_effects.depth_range[0] != 0.0f || constants.modern_effects.depth_range[1] != 1.0f) {
      if (temporal_motion_complete_) NOTE_FG_MOTION_LOST("depth range", command.pipeline_state);
      temporal_motion_complete_ = false;
    }
    temporal_depth_source_ = target.depth_surface;
    ++temporal_world_draws_;
  }
  if (!AllocateUpload(sizeof(parameters), 16, parameters_upload, NativeUploadKind::kSharedConstants)) return false;
  std::memcpy(parameters_upload.mapping, &parameters, sizeof(parameters));
  temporal::TemporalPushConstants push{vertex.device_address, pixel.device_address, shared.device_address,
      prior_vertex.device_address, prior_shared.device_address, parameters_upload.device_address};
  auto* provider = static_cast<ui::vulkan::VulkanProvider*>(provider_.get());
  provider->vulkan_device()->functions().vkCmdPushConstants(cb, layout,
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
  return true;
}

bool Gta4NativeGraphicsSystem::ResolveTemporalScene(VkCommandBuffer cb, const NativeCommand& command,
    const NativeRenderingTarget& target, NativeFrameResources& resources) {
  if (!command.temporal_scene_binding || temporal_resolved_) return true;
  const auto* instance = command.temporal_instance.get();
  if (!temporal_active_ || !instance || instance->time_ns != temporal_composite_scope_) return false;
  auto* source = GetOrCreateTextureImage(cb, command.textures[command.temporal_scene_stage]);
  if (!source) return false;
  temporal::Image color{source->resource.image, source->resource.view, source->format,
      {source->width, source->height}, source->layout, source->usage, source->aspect};
  auto* provider = static_cast<ui::vulkan::VulkanProvider*>(provider_.get());
  const auto* device = provider->vulkan_device();
  if (command.temporal_prefilter) {
    if (!ReplayTemporalDraw(cb, *command.temporal_prefilter, target, resources, temporal_scene_.filtered(), true))
      return false;
    temporal::VulkanScene::Barrier(device, cb, temporal_scene_.filtered(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    color = temporal_scene_.filtered();
  }
  const auto color_layout = color.layout;
  bool encoded = false;
  const float delta_ms = temporal_metadata_.time_ns > temporal_last_time_ns_ && temporal_last_time_ns_
      ? float(double(temporal_metadata_.time_ns - temporal_last_time_ns_) / 1000000.0) : 0.0f;
  const bool continuity = temporal_history_valid_ && temporal_last_epoch_ == temporal_metadata_.epoch &&
      temporal_last_sequence_ != UINT64_MAX && TemporalHistorySequence() == temporal_last_sequence_ + 1 &&
      temporal_metadata_.time_ns > temporal_last_time_ns_
#if REX_PLATFORM_PS5
      // (scenes counted one after another: a long gap between them, menus or
      // loading, breaks the history as a skipped number did)
      && delta_ms <= 250.0f
#endif
      ;
  if (temporal_motion_complete_ && temporal_world_draws_ && temporal_depth_source_ &&
      temporal_camera_adopted_ && delta_ms > 0.0f) {
    temporal::VulkanScene::Camera camera{};
    camera.jitter = temporal_metadata_.jitter; camera.half_pixel = temporal_half_pixel_;
    camera.reversed = temporal_metadata_.flags & kTemporalDepthReversed;
    if (continuity) {
      const auto matrix = gta4_metal::temporal::BackgroundReprojection(temporal_metadata_.projection,
          temporal_metadata_.view_inverse, temporal_previous_projection_, temporal_previous_inverse_);
      if (matrix) { camera.background_reprojection = *matrix; camera.background_history = true; }
    }
    const auto& depth = *temporal_depth_source_;
    const temporal::VulkanScene::DepthSource depth_source{{depth.resource.image, depth.sampled_view,
        depth.format, {depth.width, depth.height}, depth.layout, VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT}, depth.aspect};
    if (temporal_scene_.Prepare(cb, frame_descriptor_pool_, depth_source, camera)) {
      temporal::VulkanScene::Barrier(device, cb, color, VK_IMAGE_LAYOUT_GENERAL);
      temporal::FrameInput frame{};
      frame.color = color; frame.depth = temporal_scene_.depth(); frame.motion = temporal_scene_.motion();
      frame.reactive = temporal_scene_.reactive(); frame.output = temporal_scene_.output();
      frame.render_extent = temporal_configuration_.render_extent;
      frame.output_extent = temporal_configuration_.output_extent;
      frame.sequence = TemporalHistorySequence(); frame.epoch = temporal_metadata_.epoch;
      frame.time_ns = temporal_metadata_.time_ns; frame.frame_time_ms = delta_ms;
      frame.jitter_x = temporal_metadata_.jitter[0]; frame.jitter_y = temporal_metadata_.jitter[1];
      frame.camera_near = temporal_metadata_.near_plane; frame.camera_far = temporal_metadata_.far_plane;
      frame.vertical_fov_radians = temporal_metadata_.field_of_view * (std::numbers::pi_v<float> / 180.0f);
      frame.reset = !continuity || (temporal_metadata_.flags & kTemporalCameraCut);
#if REX_PLATFORM_PS5
      // [ps5 port] A hitch between the two frames (2.5 of the slowest
      // cadence's 33 ms): interpolating across it would smear.
      if (rex::frame_stats::g_frame_generation_active.load(std::memory_order_relaxed) && delta_ms > 85.0f)
        frame.reset = true;
#endif
      frame.motion_complete = frame.scene_without_ui = true;
      {
        std::lock_guard lock(temporal_provider_mutex_);
        encoded = temporal_provider_ == TemporalUpscalerProvider::kFsr3
            ? fsr_upscaler_.Encode(cb, frame) : dlss_upscaler_.Encode(cb, frame);
        if (!encoded) temporal_provider_recreate_ = true;
      }
      temporal::VulkanScene::Barrier(device, cb, color, color_layout);
      if (encoded) {
        temporal_scene_.OutputReadable(cb);
        temporal_encoded_frame_ = frame;
        temporal_encoded_frame_.color.layout = color_layout;
        temporal_encoded_ = true;
      }
    }
  }
  native_draw_state_cache_.Reset();
  if (!encoded)
    rex::frame_stats::NoteFgDrop(!temporal_motion_complete_ ? rex::frame_stats::FgDrop::kSpatialMotion
        : !temporal_world_draws_ ? rex::frame_stats::FgDrop::kSpatialWorld
        : !temporal_depth_source_ ? rex::frame_stats::FgDrop::kSpatialDepth
        : !temporal_camera_adopted_ ? rex::frame_stats::FgDrop::kSpatialCamera
        : delta_ms <= 0.0f ? rex::frame_stats::FgDrop::kSpatialTime
                           : rex::frame_stats::FgDrop::kSpatialEncode);
  if (!encoded && !temporal_scene_.SpatialFallback(cb, color)) return false;
  temporal_resolved_ = encoded;
  // A spatial frame is a valid composite input but cannot seed vendor history.
  // Prevent another composite from dispatching again into the same scene.
  temporal_active_ = false;
  return true;
}

bool Gta4NativeGraphicsSystem::ReplayTemporalDraw(VkCommandBuffer cb, const NativeCommand& command,
    const NativeRenderingTarget& original, NativeFrameResources& resources,
    temporal::Image& image, bool clear) {
  const auto& fixed = command.fixed_function_state;
  const bool finite_viewport = std::all_of(fixed.viewport_bits.begin(), fixed.viewport_bits.end(),
      [](uint32_t bits) { return std::isfinite(std::bit_cast<float>(bits)); });
  const bool raster_area = original.logical_width && original.logical_height &&
      std::bit_cast<float>(fixed.viewport_bits[2]) > 0 && std::bit_cast<float>(fixed.viewport_bits[3]) > 0 &&
      std::clamp(fixed.scissor[2], 0, int(original.logical_width)) > std::clamp(fixed.scissor[0], 0, int(original.logical_width)) &&
      std::clamp(fixed.scissor[3], 0, int(original.logical_height)) > std::clamp(fixed.scissor[1], 0, int(original.logical_height));
  if (command.fixed_function_state.depth_enable || command.fixed_function_state.stencil_enable ||
      !command.pipeline_state || !image || !finite_viewport || !raster_area) return false;
  auto* provider = static_cast<ui::vulkan::VulkanProvider*>(provider_.get());
  const auto* device = provider->vulkan_device(); const auto& fn = device->functions();
  temporal::VulkanScene::Barrier(device, cb, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  NativeRenderingTarget target{};
  target.temporal_shader = command.temporal_composite_mode != 0;
  target.color_views[0] = image.view; target.color_formats[0] = image.format;
  target.color_attachment_mask = 1; target.color_write_mask = original.color_write_mask & 0xFu;
  target.width = image.extent.width; target.height = image.extent.height;
  target.logical_width = original.logical_width; target.logical_height = original.logical_height;
  VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView = image.view; attachment.imageLayout = image.layout;
  attachment.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea.extent = {target.width, target.height}; rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1; rendering.pColorAttachments = &attachment;
  native_draw_state_cache_.Reset();
  fn.vkCmdBeginRendering(cb, &rendering);
  bool result = false;
  if (command.type == CommandType::kDrawPrimitive)
    result = RecordPrimitive(cb, command, target.width, target.height, target, resources);
  else if (command.type == CommandType::kDrawPrimitiveUp)
    result = RecordPrimitiveUp(cb, command, target.width, target.height, target, resources);
  else if (command.type == CommandType::kDrawIndexedPrimitive)
    result = RecordIndexedPrimitive(cb, command, target.width, target.height, target, resources);
  fn.vkCmdEndRendering(cb);
  native_draw_state_cache_.Reset();
  return result;
}

bool Gta4NativeGraphicsSystem::ReplayTemporalDisplayDraw(VkCommandBuffer cb,
    const NativeCommand& command, const NativeRenderingTarget& original, NativeFrameResources& resources) {
  if (!temporal_frame_prepared_ || !original.color_surfaces[0] ||
      !(original.color_write_mask & 0xFu)) return true;
  const bool composite = command.temporal_scene_binding != nullptr;
  if (!composite && (!temporal_display_ready_ ||
      original.color_surfaces[0] != temporal_composite_source_)) return true;
  // Read/modify/write fullscreen effects may sample their destination. Replaying
  // those with the guest-sized source would not preserve the current scene.
  for (const auto& texture : command.textures) {
    if (texture && !composite && texture->handle == original.color_surfaces[0]->descriptor.handle) {
      temporal_ui_exact_ = temporal_display_ready_ = false; return true;
    }
  }
#if REX_PLATFORM_PS5
  // [ps5 port] A HUD draw that blends with the alpha already on the screen
  // (the radar: its mask shapes write alpha, the map is drawn through it).
  // The replay's target starts and keeps its alpha differently from the
  // game's 8-bit backbuffer (float, cleared to 0, later clears not mirrored),
  // so such a frame is shown as the game drew it, without a generated twin:
  // frame generation broke the radar (1.0.1).
  if (!composite && REXCVAR_GET(ps5_fg_hud_alpha_guard)) {
    const uint32_t packed = command.fixed_function_state.blend_controls[0];
    if (IsNativeBlendControlEnabled(packed)) {
      const NativeBlendControlState blend = DecodeNativeBlendControl(packed);
      const auto reads_alpha = [](uint32_t f) { return f == 10 || f == 11 || f == 16; };
      const auto reads_any = [](uint32_t f) { return (f >= 8 && f <= 11) || f == 16; };
      if (reads_alpha(blend.source_color) || reads_alpha(blend.destination_color) ||
          reads_any(blend.source_alpha) || reads_any(blend.destination_alpha)) {
        static std::atomic<uint32_t> noted{0};
        if (noted.fetch_add(1, std::memory_order_relaxed) < 4)
          REXLOG_INFO("ps5 frame generation: a HUD draw blends with the screen's alpha (blend {:08X}): that "
                      "frame shown as drawn", packed);
        temporal_ui_exact_ = temporal_display_ready_ = false;
        return true;
      }
    }
  }
#endif
  if (!ReplayTemporalDraw(cb, command, original, resources, temporal_scene_.display(), composite)) {
    temporal_ui_exact_ = temporal_display_ready_ = false;
    return true;
  }
  temporal_display_ready_ = true;
  if (composite) {
    temporal_composite_source_ = original.color_surfaces[0];
    if (temporal_resolved_ && !EncodeTemporalGeneration(cb)) return false;
  } else if (temporal_generated_frame_.has_generated_frame) {
    if (!ReplayTemporalDraw(cb, command, original, resources, temporal_generated_frame_.generated, false) ||
        !ReplayTemporalDraw(cb, command, original, resources, temporal_generated_frame_.real, false)) {
      temporal_ui_exact_ = false;
      temporal_generated_frame_.has_generated_frame = false;
    }
  }
  return true;
}

void Gta4NativeGraphicsSystem::AcquireNativePresenterImage(VkCommandBuffer cb) {
  if (temporal_presenter_acquired_) return;
  auto* provider = static_cast<ui::vulkan::VulkanProvider*>(provider_.get());
  provider->vulkan_device()->functions().vkCmdPipelineBarrier(cb,
      temporal_presenter_acquire_stages_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      0, 0, nullptr, 0, nullptr, 1, &temporal_presenter_acquire_);
  temporal_presenter_acquired_ = true;
}

bool Gta4NativeGraphicsSystem::CompleteTemporalCoverage(VkCommandBuffer cb, bool& valid) {
  valid = false;
  if (temporal_presenter_acquired_ || cb != command_buffer_ ||
      !temporal_scene_.RecordAudit(cb, frame_descriptor_pool_)) return false;
  auto* provider = static_cast<ui::vulkan::VulkanProvider*>(provider_.get());
  const auto* device = provider->vulkan_device();
  const auto& fn = device->functions();
  const auto flush = [&](const NativeUploadBuffer& buffer) {
    if (buffer.write_offset) ui::vulkan::util::FlushMappedMemoryRange(device, buffer.memory,
        buffer.memory_type, 0, buffer.allocation_size, buffer.write_offset);
  };
  flush(upload_buffer_);
  for (const auto& buffer : overflow_upload_buffers_) flush(buffer);
  flush(frame_constant_arenas_[active_frame_slot_].storage);
  VkFence& fence = temporal_partial_fence_;
  VkFenceCreateInfo create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (fn.vkCreateFence(device->device(), &create, nullptr, &fence) != VK_SUCCESS) return false;
  const auto destroy_fence = MakeScopeExit([&] {
    if (!temporal_partial_pending_) {
      fn.vkDestroyFence(device->device(), fence, nullptr); fence = VK_NULL_HANDLE;
    }
  });
  if (fn.vkEndCommandBuffer(cb) != VK_SUCCESS) return false;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1; submit.pCommandBuffers = &cb;
  VkResult accepted;
  {
    auto queue = device->AcquireQueue(device->queue_family_native_offscreen(), 0);
    accepted = fn.vkQueueSubmit(queue.queue(), 1, &submit, fence);
  }
  if (accepted != VK_SUCCESS) return false;
  temporal_partial_pending_ = true;
  // Only native offscreen images have been touched. In particular this partial
  // submission neither waits on nor signals the mailbox image timeline.
  {
    std::lock_guard lock(temporal_provider_mutex_);
    if (temporal_encoded_) {
      if (temporal_provider_ == TemporalUpscalerProvider::kFsr3) fsr_upscaler_.OnSubmitted();
      else dlss_upscaler_.OnSubmitted(TemporalHistorySequence());
      temporal_encoded_ = false;
    }
  }
  // Uploads, descriptors, geometry and SDK resources stay owned by the active
  // frame throughout this wait. A later final-submit failure must never report
  // the accepted SR work as a discarded recording.
  const VkResult completed = fn.vkWaitForFences(device->device(), 1, &fence, VK_TRUE, UINT64_MAX);
  if (completed != VK_SUCCESS) {
    std::vector<ui::vulkan::VulkanDevice::Queue::Acquisition> queues;
    for (const auto& family : device->queue_families())
      for (const auto& queue : family.queues) queues.emplace_back(queue->Acquire());
    const VkResult idle = fn.vkDeviceWaitIdle(device->device());
    if (idle != VK_SUCCESS && idle != VK_ERROR_DEVICE_LOST) {
      rex::FatalError(fmt::format(
          "Temporal motion coverage GPU submission could not be retired (fence {}, idle {}).",
          int32_t(completed), int32_t(idle)));
    }
    temporal_partial_pending_ = false;
    native_renderer_recovery_failed_ = true;
    return false;
  }
  temporal_partial_pending_ = false;
  valid = temporal_scene_.ReadAuditAfterCompletion();
  if (fn.vkResetCommandPool(device->device(), command_pool_, 0) != VK_SUCCESS) return false;
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (fn.vkBeginCommandBuffer(cb, &begin) != VK_SUCCESS) return false;
  VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  memory.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  fn.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      0, 1, &memory, 0, nullptr, 0, nullptr);
  native_draw_state_cache_.Reset();
  return true;
}

bool Gta4NativeGraphicsSystem::EncodeTemporalGeneration(VkCommandBuffer cb) {
  if (temporal_generated_frame_.lease()) {
    // A second final composite supersedes the earlier image pair. Keep its
    // lease and submission notification, but do not publish stale scene color.
    temporal_generated_frame_.has_generated_frame = false;
    temporal_frame_generator_.Reset();
    rex::frame_stats::NoteFgDrop(rex::frame_stats::FgDrop::kSecondComposite);
    return true;
  }
  const auto selected = CachedFlagByName("gta4_native_frame_generation");
  if (selected != "fsr3" && selected != "dlss") return true;
  if (!temporal_encoded_ || !temporal_motion_complete_ || !temporal_camera_adopted_ ||
      temporal_presenter_acquired_) {
    rex::frame_stats::NoteFgDrop(rex::frame_stats::FgDrop::kEncodeGate);
    return true;
  }
  auto* presenter = static_cast<ui::vulkan::VulkanPresenter*>(presenter_.get());
  if (!presenter || !presenter->CanPresentGeneratedFrames()) {
    rex::frame_stats::NoteFgDrop(rex::frame_stats::FgDrop::kPresenter);
    return true;
  }
  auto* provider = static_cast<ui::vulkan::VulkanProvider*>(provider_.get());
  const auto* device = provider->vulkan_device();
  // [ps5 port] Off on the PS5: the audit submits half the frame and waits for
  // the GPU on the recorder, CPU and GPU in series, and the upscaler has just
  // used the same motion and depth unaudited.
  if (REXCVAR_GET(gta4_native_fg_motion_audit)) {
    bool complete_motion = false;
    if (!CompleteTemporalCoverage(cb, complete_motion)) return false;
    if (!complete_motion) {
      temporal_frame_generator_.Reset();
      rex::frame_stats::NoteFgDrop(rex::frame_stats::FgDrop::kAudit);
      return true;
    }
  }
  if (!temporal_frame_generator_.Initialize(*device) ||
      !temporal_frame_generator_.Configure(selected == "dlss" ? temporal::FrameGenerationProvider::kDlss :
          temporal::FrameGenerationProvider::kFsr, temporal_configuration_, false)) {
    rex::frame_stats::NoteFgDrop(rex::frame_stats::FgDrop::kConfigure);
    return true;
  }
  temporal::FrameGenerationCamera camera{};
  const auto inverse_projection = temporal::InverseCameraMatrix(temporal_metadata_.projection);
  const auto previous_view = temporal::InverseCameraMatrix(temporal_previous_inverse_);
  if (!inverse_projection || !previous_view) {
    rex::frame_stats::NoteFgDrop(rex::frame_stats::FgDrop::kCamera);
    return true;
  }
  camera.view_to_clip = temporal_metadata_.projection; camera.clip_to_view = *inverse_projection;
  camera.clip_to_previous = temporal::MultiplyCameraMatrices(temporal::MultiplyCameraMatrices(
      temporal::MultiplyCameraMatrices(*inverse_projection, temporal_metadata_.view_inverse),
      *previous_view), temporal_previous_projection_);
  const auto inverse_reprojection = temporal::InverseCameraMatrix(camera.clip_to_previous);
  if (!inverse_reprojection) {
    rex::frame_stats::NoteFgDrop(rex::frame_stats::FgDrop::kCamera);
    return true;
  }
  camera.previous_to_clip = *inverse_reprojection;
  for (size_t i = 0; i < 3; ++i) {
    camera.right[i] = temporal_metadata_.view_inverse[i];
    camera.up[i] = temporal_metadata_.view_inverse[4 + i];
    camera.forward[i] = temporal_metadata_.view_inverse[8 + i];
    camera.position[i] = temporal_metadata_.view_inverse[12 + i];
  }
  camera.projection_jitter = {2.0f * temporal_metadata_.jitter[0] / temporal_metadata_.width,
      -2.0f * temporal_metadata_.jitter[1] / temporal_metadata_.height};
  camera.aspect_ratio = temporal_metadata_.aspect;
  camera.maximum_luminance = 100.0f;
  camera.valid = true;
  auto frame = temporal_encoded_frame_;
  const auto color_layout = frame.color.layout;
  frame.output = temporal_scene_.output();
  const auto output_layout = frame.output.layout;
  temporal::VulkanScene::Barrier(device, cb, frame.color, VK_IMAGE_LAYOUT_GENERAL);
  temporal::VulkanScene::Barrier(device, cb, frame.output, VK_IMAGE_LAYOUT_GENERAL);
  temporal::VulkanScene::Barrier(device, cb, temporal_scene_.display(), VK_IMAGE_LAYOUT_GENERAL);
  const bool encoded = temporal_frame_generator_.Encode(cb, frame, temporal_scene_.display(),
      camera, *presenter, temporal_generated_frame_);
  temporal::VulkanScene::Barrier(device, cb, frame.color, color_layout);
  temporal::VulkanScene::Barrier(device, cb, frame.output, output_layout);
  if (temporal_generated_frame_.lease()) temporal_frame_leases_[active_frame_slot_].push_back(temporal_generated_frame_.lease());
  if (!encoded) {
    temporal_generated_frame_.has_generated_frame = false;
    rex::frame_stats::NoteFgDrop(rex::frame_stats::FgDrop::kEncodeRefused);
    static std::atomic<uint32_t> refusals_logged{0};
    if (rex::frame_stats::g_frame_generation_active.load(std::memory_order_relaxed) &&
        refusals_logged.fetch_add(1, std::memory_order_relaxed) < 8)
      REXLOG_INFO("ps5 frame generation: refused ({})", temporal_frame_generator_.status());
  } else if (!temporal_generated_frame_.has_generated_frame) {
    rex::frame_stats::NoteFgDrop(rex::frame_stats::FgDrop::kHistoryReset);
  }
  native_draw_state_cache_.Reset();
  return true;
}

void Gta4NativeGraphicsSystem::SubmitTemporalFrame() {
  if (!temporal_frame_prepared_) return;
  std::lock_guard lock(temporal_provider_mutex_);
  if (temporal_encoded_) {
    if (temporal_provider_ == TemporalUpscalerProvider::kFsr3) fsr_upscaler_.OnSubmitted();
    else dlss_upscaler_.OnSubmitted(TemporalHistorySequence());
  }
  if (temporal_generated_frame_.lease()) temporal_frame_generator_.OnSubmitted(TemporalHistorySequence());
  temporal_history_valid_ = temporal_camera_adopted_ && temporal_motion_complete_;
  temporal_last_time_ns_ = temporal_metadata_.time_ns;
  temporal_last_sequence_ = TemporalHistorySequence();
  temporal_last_epoch_ = temporal_metadata_.epoch;
  temporal_previous_projection_ = temporal_metadata_.projection;
  temporal_previous_inverse_ = temporal_metadata_.view_inverse;
  temporal_encoded_ = false;
}
void Gta4NativeGraphicsSystem::DiscardTemporalFrame() {
  std::lock_guard lock(temporal_provider_mutex_);
  if (temporal_encoded_) {
    if (temporal_provider_ == TemporalUpscalerProvider::kFsr3) fsr_upscaler_.OnDiscarded();
    else dlss_upscaler_.OnDiscarded();
  }
  temporal_encoded_ = temporal_history_valid_ = false;
  temporal_provider_recreate_ = true;
  if (temporal_generated_frame_.lease()) temporal_frame_generator_.OnDiscarded();
  temporal_geometry_history_.Reset();
}
void Gta4NativeGraphicsSystem::DestroyTemporalResources() {
  std::lock_guard lock(temporal_provider_mutex_);
  auto* provider = static_cast<ui::vulkan::VulkanProvider*>(provider_.get());
  const auto* device = provider ? provider->vulkan_device() : nullptr;
  if (!device) return;
  fsr_upscaler_.Shutdown(); dlss_upscaler_.Shutdown();
  temporal_frame_generator_.Shutdown();
  temporal_generated_frame_ = {}; for (auto& leases : temporal_frame_leases_) leases.clear();
  for (const auto& [key, module] : temporal_shader_modules_)
    device->functions().vkDestroyShaderModule(device->device(), module, nullptr);
  temporal_shader_modules_.clear(); temporal_output_binding_.reset();
  temporal_scene_.Shutdown(); temporal_geometry_history_.Reset();
  temporal_assets_checked_ = temporal_assets_ready_ = false;
  temporal_capabilities_checked_ = false; temporal_capabilities_ = 0;
  temporal_encoded_ = temporal_history_valid_ = temporal_frame_prepared_ = false;
}
