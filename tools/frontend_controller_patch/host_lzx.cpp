// Host build of the rex LZX persistent decoder.
//
// This mirrors glue/rexglue-sdk-main/src/system/lzx.cpp, which depends on the
// full runtime (logging, math, memory and XEX types). The controller
// transformer only needs the persistent XMem decoder, so the host tool links
// this small implementation against the repository's patched libmspack.
#include <algorithm>
#include <bit>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

#include <rex/system/lzx.h>

extern "C" {
#include <lzx.h>
#include <mspack.h>
}

namespace rex::lzx {
namespace {

constexpr uint32_t kMinimumWindowBits = 15;
constexpr uint32_t kMaximumWindowBits = 21;
constexpr size_t kInputBufferSize = 0x8000;
constexpr uint64_t kFrameSize = 0x8000;

bool GetWindowBits(uint32_t window_size, uint32_t* window_bits) {
  if (!window_size || (window_size & (window_size - 1)) != 0) return false;
  const uint32_t bits = static_cast<uint32_t>(std::countr_zero(window_size));
  if (bits < kMinimumWindowBits || bits > kMaximumWindowBits) return false;
  *window_bits = bits;
  return true;
}

struct RebindableMemoryFile {
  const uint8_t* input = nullptr;
  uint8_t* output = nullptr;
  size_t size = 0;
  size_t offset = 0;
  size_t source_bytes_read = 0;
  bool reached_eof = false;
  bool writable = false;

  void BindInput(std::span<const uint8_t> bytes) {
    input = bytes.data();
    output = nullptr;
    size = bytes.size();
    offset = 0;
    source_bytes_read = 0;
    reached_eof = false;
    writable = false;
  }

  void BindOutput(std::span<uint8_t> bytes) {
    input = nullptr;
    output = bytes.data();
    size = bytes.size();
    offset = 0;
    source_bytes_read = 0;
    reached_eof = false;
    writable = true;
  }
};

int PersistentRead(mspack_file* file, void* destination, int requested) {
  auto* memory = reinterpret_cast<RebindableMemoryFile*>(file);
  if (!memory || memory->writable || requested < 0 || (!destination && requested != 0)) return -1;
  if (requested == 0) return 0;
  auto* bytes = static_cast<uint8_t*>(destination);
  const size_t request_size = static_cast<size_t>(requested);
  const size_t source_remaining = memory->size - memory->offset;
  const size_t source_count = std::min(request_size, source_remaining);
  if (source_count != 0) {
    std::memcpy(bytes, memory->input + memory->offset, source_count);
    memory->offset += source_count;
    memory->source_bytes_read += source_count;
  } else {
    memory->reached_eof = true;
  }
  return static_cast<int>(source_count);
}

int PersistentWrite(mspack_file* file, void* source, int requested) {
  auto* memory = reinterpret_cast<RebindableMemoryFile*>(file);
  if (!memory || !memory->writable || requested < 0 || (!source && requested != 0)) return -1;
  const size_t request_size = static_cast<size_t>(requested);
  const size_t remaining = memory->size - memory->offset;
  const size_t count = std::min(request_size, remaining);
  if (count != 0) {
    std::memcpy(memory->output + memory->offset, source, count);
    memory->offset += count;
  }
  return static_cast<int>(count);
}

void* PersistentAlloc(mspack_system*, size_t size) { return std::calloc(size, 1); }
void PersistentFree(void* pointer) { std::free(pointer); }
void PersistentCopy(void* source, void* destination, size_t size) {
  std::memcpy(destination, source, size);
}

}  // namespace

struct PersistentDecoder::Impl {
  explicit Impl(uint32_t requested_window_size) : window_size(requested_window_size) {
    system.read = PersistentRead;
    system.write = PersistentWrite;
    system.alloc = PersistentAlloc;
    system.free = PersistentFree;
    system.copy = PersistentCopy;
  }
  ~Impl() {
    if (stream) lzxd_free(stream);
  }

  bool Initialize() {
    uint32_t window_bits = 0;
    if (!GetWindowBits(window_size, &window_bits)) return false;
    input.BindInput({});
    output.BindOutput({});
    stream = lzxd_init(&system, reinterpret_cast<mspack_file*>(&input),
                       reinterpret_cast<mspack_file*>(&output), static_cast<int>(window_bits), 0,
                       static_cast<int>(kInputBufferSize), 0, 0);
    if (stream) stream->uncompressed_block_padding = 0;
    poisoned = stream == nullptr;
    total_output = 0;
    return stream != nullptr;
  }

  mspack_system system = {};
  RebindableMemoryFile input;
  RebindableMemoryFile output;
  lzxd_stream* stream = nullptr;
  uint64_t total_output = 0;
  uint32_t window_size = 0;
  bool poisoned = false;
};

std::unique_ptr<PersistentDecoder> PersistentDecoder::Create(uint32_t window_size) {
  uint32_t window_bits = 0;
  if (!GetWindowBits(window_size, &window_bits)) return nullptr;
  auto impl = std::unique_ptr<Impl>(new (std::nothrow) Impl(window_size));
  if (!impl || !impl->Initialize()) return nullptr;
  return std::unique_ptr<PersistentDecoder>(new (std::nothrow) PersistentDecoder(std::move(impl)));
}

PersistentDecoder::PersistentDecoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PersistentDecoder::~PersistentDecoder() = default;
PersistentDecoder::PersistentDecoder(PersistentDecoder&&) noexcept = default;
PersistentDecoder& PersistentDecoder::operator=(PersistentDecoder&&) noexcept = default;

DecodeFrameResult PersistentDecoder::DecodeFrame(std::span<const uint8_t> input,
                                                 std::span<uint8_t> output) {
  if (!impl_ || impl_->poisoned || !impl_->stream) return {DecodeStatus::kPoisoned, 0, 0};
  if ((input.empty() && !output.empty()) || input.size() >= static_cast<size_t>(INT_MAX) ||
      output.size() >= static_cast<size_t>(INT_MAX) ||
      impl_->total_output > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
      output.size() > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) - impl_->total_output)
    return {DecodeStatus::kInvalidArgument, 0, 0};
  if (output.empty()) return {DecodeStatus::kSuccess, 0, 0};

  impl_->input.BindInput(input);
  impl_->output.BindOutput(output);
  impl_->stream->input = reinterpret_cast<mspack_file*>(&impl_->input);
  impl_->stream->output = reinterpret_cast<mspack_file*>(&impl_->output);
  impl_->stream->i_ptr = impl_->stream->inbuf;
  impl_->stream->i_end = impl_->stream->inbuf;
  impl_->stream->bit_buffer = 0;
  impl_->stream->bits_left = 0;
  impl_->stream->input_end = 0;

  const uint64_t next_total = impl_->total_output + output.size();
  lzxd_set_output_length(impl_->stream, static_cast<off_t>(next_total));
  int result = MSPACK_ERR_OK;
  if ((next_total % kFrameSize) == 0 && output.size() > 1) {
    const off_t first_part = static_cast<off_t>(output.size() - 1);
    result = lzxd_decompress(impl_->stream, first_part);
    if (result == MSPACK_ERR_OK) result = lzxd_decompress(impl_->stream, 1);
  } else {
    result = lzxd_decompress(impl_->stream, static_cast<off_t>(output.size()));
  }
  const size_t written = impl_->output.offset;
  const size_t unread_buffered = static_cast<size_t>(impl_->stream->i_end - impl_->stream->i_ptr);
  const size_t unread_source =
      impl_->input.reached_eof ? 0 : std::min(unread_buffered, impl_->input.source_bytes_read);
  const size_t consumed = impl_->input.source_bytes_read - unread_source;
  if (result != MSPACK_ERR_OK || written != output.size()) {
    impl_->poisoned = true;
    return {DecodeStatus::kDataError, written, consumed};
  }
  impl_->total_output = next_total;
  return {DecodeStatus::kSuccess, written, consumed};
}

bool PersistentDecoder::Reset() {
  if (!impl_) return false;
  if (impl_->stream) lzxd_free(impl_->stream);
  return impl_->Initialize();
}
bool PersistentDecoder::poisoned() const { return !impl_ || impl_->poisoned; }
uint64_t PersistentDecoder::total_output_bytes() const { return impl_ ? impl_->total_output : 0; }
uint32_t PersistentDecoder::window_size() const { return impl_ ? impl_->window_size : 0; }

}  // namespace rex::lzx
