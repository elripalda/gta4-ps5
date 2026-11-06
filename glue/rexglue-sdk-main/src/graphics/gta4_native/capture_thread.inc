// [ps5 port] Capture on its own core (included by graphics_system.cpp).
//
// The game's render thread used to run the whole capture of every command it
// submitted: about 7 ms of each frame in busy streets, on the thread that
// limits the frame rate there. Now it only copies the command into a ring,
// with what the capture will read of the guest device: the 64-byte lines of
// the device's state that changed since it last sent them (texture fetches,
// render and fixed state, the bound resources), and the shader constant
// groups the draw's own dirty masks name (the same ones the capture copies).
// The capture thread keeps a copy of each device up to date from those lines
// and runs the same capture as before, reading that copy instead of the
// device. Guest objects (buffers, textures, surfaces) are still read where
// they are: the game reuses one only after the frame it was drawn in is
// complete, which happens after the capture (gta4_native_frame_complete_signal).
//
// Commands that need an answer now (texture locks) and the trace envelopes
// wait for everything already in the ring to be captured, then run here.

namespace {

struct CaptureRecordHeader {
  uint32_t record_size = 0;  // never NativeCaptureRing::kWrapMark
  uint32_t command_size = 0;
  uint32_t device = 0;       // the device the lines belong to (0: none)
  uint16_t line_count = 0;
  uint16_t flags = 0;
  uint32_t up_size = 0;      // DrawPrimitiveUp vertex bytes after the lines
  uint32_t object_count = 0; // guest objects (kCaptureObjectEntry each) after the payload
  uint32_t reserved[2] = {};
};
static_assert(sizeof(CaptureRecordHeader) == 32);
constexpr uint16_t kCaptureRecordPresent = 1;

constexpr uint32_t kCaptureLineBytes = 64;
constexpr uint32_t kCaptureDeviceLines = kGuestDeviceSize / kCaptureLineBytes;
static_assert(kGuestDeviceSize % kCaptureLineBytes == 0);
constexpr uint32_t kCaptureVertexConstantLine = kVertexConstantsOffset / kCaptureLineBytes;
constexpr uint32_t kCapturePixelConstantLine = kPixelConstantsOffset / kCaptureLineBytes;
constexpr uint32_t kCaptureConstantLines = (kVertexConstantsSize + kPixelConstantsSize) / kCaptureLineBytes;
static_assert(kVertexConstantsOffset % kCaptureLineBytes == 0 && kPixelConstantsOffset % kCaptureLineBytes == 0);
static_assert(kNativeConstantGroupBytes == kCaptureLineBytes);
static_assert(kVertexConstantsOffset + kVertexConstantsSize == kPixelConstantsOffset);

// The lines compared on every draw: what the capture reads outside the
// constants (texture fetches; the boolean constants; clip planes through the
// viewport, with the stencil, blend and depth words, the bound surfaces,
// streams, index buffer and texture handles in between).
struct CaptureLineSpan {
  uint32_t first, end;  // device offsets, line aligned
};
constexpr std::array<CaptureLineSpan, 3> kCaptureComparedSpans{{
    {0x480, 0x700},     // texture fetch constants (0x480 + 26 * 0x18 = 0x6F0)
    {0x2780, 0x27C0},   // vertex and pixel boolean constants (0x2780, 0x2790)
    {0x2800, 0x3180},   // clip planes (0x2820) ... viewport (to 12664)
}};
constexpr bool CaptureLineCompared(uint32_t offset, uint32_t bytes = 4) {
  for (const auto& span : kCaptureComparedSpans)
    if (offset >= span.first && offset + bytes <= span.end) return true;
  return false;
}
static_assert(CaptureLineCompared(kTextureFetchBase, kTextureStageCount * kTextureFetchSize));
static_assert(CaptureLineCompared(0x2780) && CaptureLineCompared(0x2790));
static_assert(CaptureLineCompared(kClipPlane0Offset, 16) && CaptureLineCompared(kPackedWindowOffset) &&
              CaptureLineCompared(kPackedScissorTopLeftOffset) && CaptureLineCompared(kPackedScissorBottomRightOffset) &&
              CaptureLineCompared(kPackedColorWriteMaskOffset) && CaptureLineCompared(kBlendConstantsOffset, 16) &&
              CaptureLineCompared(kAlphaReferenceOffset) && CaptureLineCompared(kDepthStencilPackedOffset) &&
              CaptureLineCompared(kAlphaTestFunctionPackedOffset) && CaptureLineCompared(kClipControlPackedOffset) &&
              CaptureLineCompared(kCullModePackedOffset) && CaptureLineCompared(kSampleMaskOffset) &&
              CaptureLineCompared(kDepthBiasOffset) && CaptureLineCompared(kSlopeScaledDepthBiasOffset) &&
              CaptureLineCompared(kBackDepthBiasOffset) && CaptureLineCompared(kBackSlopeScaledDepthBiasOffset) &&
              CaptureLineCompared(kBlendControlOffset) && CaptureLineCompared(kScissorEnableOffset) &&
              CaptureLineCompared(kDepthEnableOffset) && CaptureLineCompared(kStencilEnableOffset) &&
              CaptureLineCompared(kStencilReferenceOffset, 1) && CaptureLineCompared(kStencilMaskOffset, 1) &&
              CaptureLineCompared(kStencilWriteMaskOffset, 1) && CaptureLineCompared(kBackStencilReferenceOffset, 1) &&
              CaptureLineCompared(kBackStencilMaskOffset, 1) && CaptureLineCompared(kBackStencilWriteMaskOffset, 1));
static_assert(CaptureLineCompared(kIndexBufferOffset) && CaptureLineCompared(kRenderTargetOffset, 16) &&
              CaptureLineCompared(kDepthStencilOffset) &&
              CaptureLineCompared(kStreamBufferBase, kVertexStreamCount * 4) &&
              CaptureLineCompared(kTextureHandleBase, kTextureStageCount * 4) &&
              CaptureLineCompared(kViewportXOffset, 24));
static_assert([] {
  for (uint32_t offset : kHardwareBlendPackedOffsets)
    if (!CaptureLineCompared(offset)) return false;
  return true;
}());

inline bool CaptureLinesEqual(const uint8_t* a, const uint8_t* b) {
#if defined(__AVX2__)
  const __m256i a0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a));
  const __m256i a1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + 32));
  const __m256i b0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b));
  const __m256i b1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + 32));
  const __m256i same = _mm256_and_si256(_mm256_cmpeq_epi8(a0, b0), _mm256_cmpeq_epi8(a1, b1));
  return _mm256_movemask_epi8(same) == -1;
#else
  return std::memcmp(a, b, kCaptureLineBytes) == 0;
#endif
}

// Set on the capture thread: its waits for the renderer are not the game's.
thread_local bool t_capture_thread = false;
// Bigger DrawPrimitiveUp payloads (up to 4 MB) are captured here, after the ring.
constexpr uint32_t kCaptureRingUpLimit = 256u << 10;

}  // namespace

Gta4NativeGraphicsSystem::CaptureDeviceCopy& Gta4NativeGraphicsSystem::CaptureMirror(uint32_t device) {
  if (capture_last_mirror_ && capture_last_mirror_->device == device) return *capture_last_mirror_;
  for (auto& mirror : capture_mirrors_)
    if (mirror->device == device) return *(capture_last_mirror_ = mirror.get());
  auto& added = capture_mirrors_.emplace_back(std::make_unique<CaptureDeviceCopy>());
  added->device = device;
  return *(capture_last_mirror_ = added.get());
}

Gta4NativeGraphicsSystem::CaptureDeviceCopy& Gta4NativeGraphicsSystem::CaptureShadow(uint32_t device) {
  if (capture_last_shadow_ && capture_last_shadow_->device == device) return *capture_last_shadow_;
  for (auto& shadow : capture_shadows_)
    if (shadow->device == device) return *(capture_last_shadow_ = shadow.get());
  auto& added = capture_shadows_.emplace_back(std::make_unique<CaptureDeviceCopy>());
  added->device = device;
  return *(capture_last_shadow_ = added.get());
}

bool Gta4NativeGraphicsSystem::SubmitTitleCommand(uint32_t title_id, uint32_t abi_version,
                                                  const void* command, size_t command_size) {
  if (title_id != kTitleId || !command || !memory_) return false;
  if (abi_version == kTitleCommandAbi &&
      (capture_on_core_flag_.load(std::memory_order_relaxed) ||
       rex::frame_stats::g_capture_on_core.load(std::memory_order_relaxed))) {
    const int route = SubmitTitleCommandOnCore(command, command_size);
    if (route >= 0) return route != 0;
  } else if (capture_on_core_flag_.load(std::memory_order_relaxed)) {
    // An envelope (traces, pass markers): after everything in the ring, here.
    std::lock_guard push_lock(capture_push_mutex_);
    if (capture_on_core_) {
      capture_ring_->WaitProcessed(capture_ring_->head());
      // (it may take a draw's dirty state with it: every device sent afresh)
      for (auto& mirror : capture_mirrors_) mirror->primed = false;
    }
    return SubmitTitleCommandHere(title_id, abi_version, command, command_size);
  }
  return SubmitTitleCommandHere(title_id, abi_version, command, command_size);
}

void Gta4NativeGraphicsSystem::FinishCaptureOnCore() {
  // (capture_push_mutex_ held)
  if (capture_on_core_ && capture_ring_) capture_ring_->WaitProcessed(capture_ring_->head());
}

// 1: queued, 0: rejected, -1: not for the ring (captured here by the caller,
// with nothing in the ring left before it).
int Gta4NativeGraphicsSystem::SubmitTitleCommandOnCore(const void* command, size_t command_size) {
  static_assert(kResourceSizeOffset + 4 <= kCaptureObjectBytes &&
                40 + 4 <= kCaptureObjectBytes);  // (the object words the capture reads)
  if (command_size < sizeof(CommandHeader)) return 0;
  CommandHeader header;
  std::memcpy(&header, command, sizeof(header));
  const size_t expected_size = CommandSize(header.type);
  if (!expected_size || command_size != expected_size || header.size != expected_size) {
    // The capture's own rejection (and its log line), in order.
    std::lock_guard push_lock(capture_push_mutex_);
    FinishCaptureOnCore();
    return SubmitTitleCommandHere(kTitleId, kTitleCommandAbi, command, command_size) ? 1 : 0;
  }
  const CommandType type = header.type;
  const bool draw = type == CommandType::kDrawPrimitive || type == CommandType::kDrawPrimitiveUp ||
                    type == CommandType::kDrawIndexedPrimitive;
  const bool device_state = draw || type == CommandType::kClear;
  const uint32_t device = device_state ? CommandDevice(type, command) : 0;
  const uint8_t* device_memory = device ? memory_->TranslateVirtual<const uint8_t*>(device) : nullptr;
  const uint8_t* up_data = nullptr;
  uint32_t up_size = 0;
  if (type == CommandType::kDrawPrimitiveUp) {
    const auto& up = *static_cast<const DrawPrimitiveUpCommand*>(command);
    up_size = up.vertex_data_size;
    if (up_size) up_data = up.vertex_data ? memory_->TranslateVirtual<const uint8_t*>(up.vertex_data) : nullptr;
  }
  // The profiler's transport timing, a device or payload that can't be read,
  // a payload too big for the ring, or diagnostics that read the whole device
  // (full snapshots, the bulb trace): here, after the ring.
  const bool here =
      (device_state && !device_memory) || (up_size && (!up_data || up_size > kCaptureRingUpLimit)) ||
      (CachedDiagnosticsEnabled(rex::diagnostics::Category::kNativeProfiler) &&
       (g_native_profile_transport_active.load(std::memory_order_relaxed) ||
        g_native_profile_capture_requested.load(std::memory_order_relaxed))) ||
      (device_state &&
       (CachedDiagnosticsEnabled(rex::diagnostics::Category::kNativeTrace) ||
        CachedDiagnosticsEnabled(rex::diagnostics::Category::kNativeProbes) ||
        CachedDiagnosticsEnabled(rex::diagnostics::Category::kNativeTranslucency) ||
        CachedDiagnosticsEnabled(rex::diagnostics::Category::kTransition) ||
        REXCVAR_GET(gta4_validate_native_dirty_snapshots) || EmissionTraceConfig().pipeline ||
        std::string_view(REXCVAR_GET(gta4_native_state_transport)) == "snapshot" ||
        std::string_view(REXCVAR_GET(gta4_native_state_transport)) == "compare"));

  std::lock_guard push_lock(capture_push_mutex_);
  if (capture_stopped_) return 0;  // shut down
  const bool wanted = rex::frame_stats::g_capture_on_core.load(std::memory_order_relaxed);
  if (!capture_on_core_) {
    if (!wanted) return -1;
    // Switching on: the thread (once), and every device's lines sent afresh.
    if (!capture_ring_) {
      auto ring = std::make_unique<NativeCaptureRing>();
      if (!ring->valid()) {
        rex::frame_stats::g_capture_on_core.store(false, std::memory_order_relaxed);
        REXLOG_ERROR("ps5 capture: no memory for the capture ring; capturing on the game's thread");
        return -1;
      }
      capture_ring_ = std::move(ring);
      capture_ring_published_.store(capture_ring_.get(), std::memory_order_release);
    }
    if (!capture_thread_running_.exchange(true)) {
      capture_thread_ = std::thread([this]() { CaptureThreadMain(); });
    }
    for (auto& mirror : capture_mirrors_) mirror->primed = false;
    capture_on_core_ = true;
    capture_on_core_flag_.store(true, std::memory_order_relaxed);
  }
  const bool present = type == CommandType::kPresent;
  if (here) {
    FinishCaptureOnCore();
    // Captured here, it takes the draw's dirty constants (or starts the
    // device afresh): that device's lines go out in full next time.
    if (device_state && device) CaptureMirror(device).primed = false;
    if (type == CommandType::kDeviceCreated || type == CommandType::kDeviceDestroyed)
      CaptureMirror(CommandDevice(type, command)).primed = false;
    const bool accepted = SubmitTitleCommandHere(kTitleId, kTitleCommandAbi, command, command_size);
    if (present && !wanted) {
      capture_on_core_ = false;
      capture_on_core_flag_.store(false, std::memory_order_relaxed);
    }
    return accepted ? 1 : 0;
  }

  // The lines to send.
  std::array<uint16_t, kCaptureDeviceLines> lines;
  uint32_t line_count = 0;
  CaptureDeviceCopy* mirror = nullptr;
  if (device_state) {
    mirror = &CaptureMirror(device);
    if (!mirror->primed) {
      // A new copy: all of it, once.
      for (uint32_t line = 0; line < kCaptureDeviceLines; ++line) lines[line_count++] = uint16_t(line);
      std::memcpy(mirror->bytes.data(), device_memory, kGuestDeviceSize);
      mirror->primed = true;
    } else {
      for (const auto& span : kCaptureComparedSpans) {
        for (uint32_t offset = span.first; offset < span.end; offset += kCaptureLineBytes) {
          uint8_t* kept = mirror->bytes.data() + offset;
          if (CaptureLinesEqual(kept, device_memory + offset)) continue;
          std::memcpy(kept, device_memory + offset, kCaptureLineBytes);
          lines[line_count++] = uint16_t(offset / kCaptureLineBytes);
        }
      }
      const NativeDirtyState* dirty = CommandDirtyState(type, command);
      const bool full_constants =
          draw && RequiresFullLightingConstants(
                      type == CommandType::kDrawPrimitive
                          ? static_cast<const DrawPrimitiveCommand*>(command)->lighting
                      : type == CommandType::kDrawPrimitiveUp
                          ? static_cast<const DrawPrimitiveUpCommand*>(command)->lighting
                          : static_cast<const DrawIndexedPrimitiveCommand*>(command)->lighting);
      if (full_constants) {
        for (uint32_t line = 0; line < kCaptureConstantLines; ++line)
          lines[line_count++] = uint16_t(kCaptureVertexConstantLine + line);
      } else if (dirty) {
        // Xenos masks: group g is bit 63 - g (vertex: all 64; pixel: 56, bits 63..8).
        for (uint64_t bits = dirty->words[0]; bits; bits &= bits - 1) {
          const uint32_t group = 63u - uint32_t(std::countr_zero(bits));
          lines[line_count++] = uint16_t(kCaptureVertexConstantLine + group);
        }
        for (uint64_t bits = dirty->words[1] & ~uint64_t(0xFF); bits; bits &= bits - 1) {
          const uint32_t group = 63u - uint32_t(std::countr_zero(bits));
          lines[line_count++] = uint16_t(kCapturePixelConstantLine + group);
        }
      }
    }
  } else if (type == CommandType::kDeviceCreated || type == CommandType::kDeviceDestroyed) {
    // The capture starts that device's constants afresh: so do its lines.
    CaptureMirror(CommandDevice(type, command)).primed = false;
  }

  // The bound surfaces and buffers, as their objects are now.
  std::array<uint32_t, kRenderTargetCount + 1 + kVertexStreamCount + 1> object_handles;
  uint32_t object_count = 0;
  if (device_state) {
    const auto add = [&](uint32_t offset) {
      uint32_t handle;
      std::memcpy(&handle, device_memory + offset, sizeof(handle));
      handle = __builtin_bswap32(handle);
      if (!handle || !memory_->TranslateVirtual<const uint8_t*>(handle)) return;
      for (uint32_t i = 0; i < object_count; ++i)
        if (object_handles[i] == handle) return;
      object_handles[object_count++] = handle;
    };
    for (uint32_t index = 0; index < kRenderTargetCount; ++index)
      add(kRenderTargetOffset + index * sizeof(uint32_t));
    add(kDepthStencilOffset);
    if (type == CommandType::kDrawPrimitive || type == CommandType::kDrawIndexedPrimitive) {
      for (uint32_t stream = 0; stream < kVertexStreamCount; ++stream)
        add(kStreamBufferBase + stream * sizeof(uint32_t));
      if (type == CommandType::kDrawIndexedPrimitive) add(kIndexBufferOffset);
    }
  }
  const size_t command_bytes = NativeCaptureRing::Align(command_size);
  const size_t index_bytes = NativeCaptureRing::Align(line_count * sizeof(uint16_t));
  const size_t record_size = sizeof(CaptureRecordHeader) + command_bytes + index_bytes +
                             size_t(line_count) * kCaptureLineBytes + NativeCaptureRing::Align(up_size) +
                             size_t(object_count) * kCaptureObjectEntry;
  uint64_t waited_us = 0;
  if (present && capture_ring_->presents_queued()) {
    // At most one frame in the ring: the game waits for the capture thread to
    // finish the last one (as it waited for the renderer before).
    const auto begin = std::chrono::steady_clock::now();
    capture_ring_->WaitFor([&] { return capture_ring_->presents_queued() == 0; });
    waited_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - begin).count());
  }
  uint8_t* out = capture_ring_->Reserve(record_size, &waited_us);
  CaptureRecordHeader record{};
  record.record_size = uint32_t(record_size);
  record.command_size = uint32_t(command_size);
  record.device = device_state ? device : 0;
  record.line_count = uint16_t(line_count);
  record.flags = present ? kCaptureRecordPresent : 0;
  record.up_size = up_size;
  record.object_count = object_count;
  std::memcpy(out, &record, sizeof(record));
  uint8_t* cursor = out + sizeof(record);
  std::memcpy(cursor, command, command_size);
  cursor += command_bytes;
  if (line_count) {
    std::memcpy(cursor, lines.data(), line_count * sizeof(uint16_t));
    cursor += index_bytes;
    // From the device itself: the compared lines were just copied from it.
    for (uint32_t i = 0; i < line_count; ++i, cursor += kCaptureLineBytes)
      std::memcpy(cursor, device_memory + size_t(lines[i]) * kCaptureLineBytes, kCaptureLineBytes);
  }
  if (up_size) std::memcpy(cursor, up_data, up_size);
  cursor += NativeCaptureRing::Align(up_size);
  for (uint32_t i = 0; i < object_count; ++i, cursor += kCaptureObjectEntry) {
    std::memcpy(cursor, &object_handles[i], sizeof(uint32_t));
    std::memcpy(cursor + 4, memory_->TranslateVirtual<const uint8_t*>(object_handles[i]), kCaptureObjectBytes);
  }
  capture_ring_->Commit(record_size, present);
  if (waited_us) {
    rex::frame_stats::NoteTitleSlotWait(waited_us);
    rex::frame_stats::g_capture_ring_wait_us.fetch_add(waited_us, std::memory_order_relaxed);
  }
  if (present && !wanted) {
    // Switching off, at the end of a frame: once it's captured, back here.
    FinishCaptureOnCore();
    capture_on_core_ = false;
    capture_on_core_flag_.store(false, std::memory_order_relaxed);
  }
  return 1;
}

void Gta4NativeGraphicsSystem::CaptureThreadMain() {
  rex::ps5diag::RegisterThread("capture");
  t_capture_thread = true;
  rex::frame_stats::FollowRendererCpuMask(capture_cpu_mask_applied_, rex::frame_stats::RendererRole::kCapture);
  NativeCaptureRing& ring = *capture_ring_;
  while (ring.WaitForRecords(capture_thread_running_, 300)) {
    while (const uint8_t* at = ring.Peek()) {
      CaptureRecordHeader record;
      std::memcpy(&record, at, sizeof(record));
      const uint8_t* command = at + sizeof(record);
      const uint8_t* cursor = command + NativeCaptureRing::Align(record.command_size);
      const uint8_t* shadow_bytes = nullptr;
      if (record.device) {
        CaptureDeviceCopy& shadow = CaptureShadow(record.device);
        if (record.line_count) {
          uint16_t lines[kCaptureDeviceLines];
          std::memcpy(lines, cursor, record.line_count * sizeof(uint16_t));
          cursor += NativeCaptureRing::Align(record.line_count * sizeof(uint16_t));
          for (uint32_t i = 0; i < record.line_count; ++i, cursor += kCaptureLineBytes)
            std::memcpy(shadow.bytes.data() + size_t(lines[i]) * kCaptureLineBytes, cursor, kCaptureLineBytes);
        }
        shadow_bytes = shadow.bytes.data();
      }
      const uint8_t* up_payload = record.up_size ? cursor : nullptr;
      cursor += NativeCaptureRing::Align(record.up_size);
      SubmitTitleCommandHere(kTitleId, kTitleCommandAbi, command, record.command_size, record.device,
                             shadow_bytes, up_payload, record.object_count ? cursor : nullptr,
                             record.object_count);
      const bool present = record.flags & kCaptureRecordPresent;
      ring.Pop(record.record_size, present);
      if (present) {
        rex::frame_stats::NoteCpu(rex::frame_stats::g_cpus_capture);
        rex::frame_stats::FollowRendererCpuMask(capture_cpu_mask_applied_,
                                                rex::frame_stats::RendererRole::kCapture);
      }
    }
  }
}

// At shutdown, after the render worker has stopped (a capture waiting for room
// in its queue gives up then): whatever is still in the ring is dropped.
void Gta4NativeGraphicsSystem::StopCaptureThread() {
  rex::frame_stats::g_capture_on_core.store(false, std::memory_order_relaxed);
  rex::frame_stats::g_capture_core_compare.store(false, std::memory_order_relaxed);
  // First every wait on the ring (a submitter may hold the lock in one), then
  // the lock: from here on submissions are dropped and no thread starts.
  NativeCaptureRing* ring = capture_ring_published_.load(std::memory_order_acquire);
  if (ring) ring->Stop();
  std::thread thread;
  {
    std::lock_guard push_lock(capture_push_mutex_);
    capture_stopped_ = true;
    capture_on_core_ = false;
    capture_on_core_flag_.store(false, std::memory_order_relaxed);
    capture_thread_running_.store(false);
    thread = std::move(capture_thread_);
  }
  if (ring) ring->WakeConsumer();
  if (thread.joinable()) thread.join();
}
