#include "gipu/backend.hpp"
#include "gipu/checksum.hpp"
#include "gipu/stream_worker.hpp"
#include <cuda_runtime_api.h>
#include <nvcomp/crc32.h>
#include <nvcomp/deflate.h>
#include <nvcomp/gzip.h>
#include <nvcomp/native/streaming_gzip.hpp>
#include <nvcomp/version.h>
#include <zlib.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace gipu {
namespace {
void cuda_check(cudaError_t status) {
  if (status != cudaSuccess) throw std::runtime_error(std::string("CUDA: ") + cudaGetErrorString(status));
}
void nv_check(nvcompStatus_t status) {
  if (status != nvcompSuccess) throw std::runtime_error("nvCOMPエラー: " + std::to_string(static_cast<int>(status)));
}
class Stream {
 public:
  Stream() { cuda_check(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking)); }
  ~Stream() { cudaStreamDestroy(stream_); }
  operator cudaStream_t() const { return stream_; }
 private:
  cudaStream_t stream_{};
};
class DeviceBuffer {
 public:
  explicit DeviceBuffer(size_t size = 0) { if (size) reserve(size); }
  ~DeviceBuffer() { cudaFree(ptr_); }
  DeviceBuffer(const DeviceBuffer&) = delete;
  DeviceBuffer& operator=(const DeviceBuffer&) = delete;
  void* data() const { return ptr_; }
  template<class T> T* as() const { return static_cast<T*>(ptr_); }
  void reserve(size_t size) {
    if (size <= capacity_) return;
    cuda_check(cudaFree(ptr_)); ptr_ = nullptr; capacity_ = 0;
    cuda_check(cudaMalloc(&ptr_, size)); capacity_ = size;
  }
  void release() { cuda_check(cudaFree(ptr_)); ptr_ = nullptr; capacity_ = 0; }
  size_t capacity() const { return capacity_; }
 private:
  void* ptr_ = nullptr;
  size_t capacity_ = 0;
};
class PinnedBuffer {
 public:
  explicit PinnedBuffer(size_t size = 0) { if (size) reserve(size); }
  ~PinnedBuffer() { cudaFreeHost(ptr_); }
  char* data() const { return static_cast<char*>(ptr_); }
  size_t capacity() const { return capacity_; }
  void release() { cuda_check(cudaFreeHost(ptr_)); ptr_ = nullptr; capacity_ = 0; }
  void reserve(size_t size) {
    if (size <= capacity_) return;
    cuda_check(cudaFreeHost(ptr_)); ptr_ = nullptr; capacity_ = 0;
    cuda_check(cudaMallocHost(&ptr_, size)); capacity_ = size;
  }
 private:
  void* ptr_ = nullptr;
  size_t capacity_ = 0;
};
class Events {
 public:
  Events() { for (auto& e : events_) cuda_check(cudaEventCreate(&e)); }
  ~Events() { for (auto e : events_) cudaEventDestroy(e); }
  void mark(size_t index, cudaStream_t stream) { cuda_check(cudaEventRecord(events_[index], stream)); }
  double elapsed(size_t first, size_t last) const {
    float ms = 0; cuda_check(cudaEventElapsedTime(&ms, events_[first], events_[last])); return ms / 1000.0;
  }
 private:
  cudaEvent_t events_[5]{};
};
void upload(void* dst, const void* src, size_t bytes, cudaStream_t stream) {
  cuda_check(cudaMemcpyAsync(dst, src, bytes, cudaMemcpyHostToDevice, stream));
}
void download(void* dst, const void* src, size_t bytes, cudaStream_t stream) {
  cuda_check(cudaMemcpyAsync(dst, src, bytes, cudaMemcpyDeviceToHost, stream));
}
size_t checked_size(uint64_t n) {
  if (n > std::numeric_limits<size_t>::max()) throw std::runtime_error("GPUバッファサイズが表現範囲を超えています");
  return static_cast<size_t>(n);
}
size_t aligned(size_t n, size_t alignment) {
  if (alignment == 0 || (alignment & (alignment - 1)) != 0 || n > std::numeric_limits<size_t>::max() - alignment)
    throw std::runtime_error("GPUバッファのアラインメントが不正です");
  return (n + alignment - 1) & ~(alignment - 1);
}
nvcompBatchedCRC32Opts_t crc_options(size_t count, size_t max_bytes, cudaStream_t stream) {
  nvcompBatchedCRC32Opts_t opts{};
  opts.spec = nvcompCRC32;
  nv_check(nvcompBatchedCRC32GetHeuristicConf(nullptr, count, &opts.kernel_conf, std::max<size_t>(max_bytes, 1), stream));
  return opts;
}
// 公開Streaming APIのpersistentカーネルとCRCカーネルの同時起動は
// 大量出力で進行が止まるため、解凍完了後に一定量ずつGPUへ戻して検証する。
class StreamingCrc {
 public:
  static constexpr size_t capacity = 4 << 20;
  static constexpr size_t memory = capacity + sizeof(void*) + sizeof(size_t) + sizeof(uint32_t) + sizeof(nvcompStatus_t);
  StreamingCrc() : bytes_(capacity), ptr_(sizeof(void*)), length_(sizeof(size_t)), crc_(sizeof(uint32_t)), status_(sizeof(nvcompStatus_t)), host_(capacity) {
    const void* data = bytes_.data(); upload(ptr_.data(), &data, sizeof(data), stream_);
    opts_ = crc_options(1, capacity, stream_);
    cuda_check(cudaStreamSynchronize(stream_));
  }
  void reset() { started_ = false; }
  void update(std::span<const char> data) {
    while (!data.empty()) {
      check_cancelled();
      size_t size = std::min(data.size(), capacity);
      std::copy_n(data.data(), size, host_.data());
      upload(bytes_.data(), host_.data(), size, stream_);
      upload(length_.data(), &size, sizeof(size), stream_);
      nv_check(nvcompBatchedCRC32Async(ptr_.as<const void*>(), length_.as<size_t>(), 1, crc_.as<uint32_t>(), opts_,
          started_ ? nvcompCRC32MidSegment : nvcompCRC32FirstSegment, status_.as<nvcompStatus_t>(), stream_));
      nvcompStatus_t status{}; download(&status, status_.data(), sizeof(status), stream_);
      cuda_check(cudaStreamSynchronize(stream_)); nv_check(status);
      started_ = true; data = data.subspan(size);
    }
  }
  uint32_t finish() {
    if (!started_) return 0;
    nv_check(nvcompBatchedCRC32Async(nullptr, nullptr, 1, crc_.as<uint32_t>(), opts_, nvcompCRC32LastSegment,
                                   status_.as<nvcompStatus_t>(), stream_));
    uint32_t crc = 0; nvcompStatus_t status{};
    download(&crc, crc_.data(), sizeof(crc), stream_);
    download(&status, status_.data(), sizeof(status), stream_);
    cuda_check(cudaStreamSynchronize(stream_)); nv_check(status); return crc;
  }
 private:
  Stream stream_;
  DeviceBuffer bytes_, ptr_, length_, crc_, status_;
  PinnedBuffer host_;
  nvcompBatchedCRC32Opts_t opts_{};
  bool started_ = false;
};
struct Batch {
  std::vector<const Entry*> entries;
  std::vector<size_t> input_offsets, output_offsets;
  size_t input = 0, output = 0, max_output = 0, total_output = 0, scratch = 0, memory = 0;
  size_t crc_chunk = 0, crc_count = 0;
  bool gzip = false;
  bool stream_output = false;
};
struct Codec {
  bool gzip;
  nvcompBatchedDeflateDecompressOpts_t deflate = nvcompBatchedDeflateDecompressDefaultOpts;
  nvcompBatchedGzipDecompressOpts_t lookahead = nvcompBatchedGzipDecompressDefaultOpts;
  explicit Codec(bool use_gzip) : gzip(use_gzip) {
    deflate.backend = NVCOMP_DECOMPRESS_BACKEND_CUDA;
    lookahead.backend = NVCOMP_DECOMPRESS_BACKEND_CUDA;
    lookahead.algorithm = NVCOMP_GZIP_DECOMPRESS_ALGORITHM_LOOKAHEAD;
  }
  bool supported(const Entry& e) const {
    auto limit = gzip ? nvcompGzipLookaheadDecompressionMaxAllowedChunkSize : nvcompDeflateDecompressionMaxAllowedChunkSize;
    return e.uncompressed <= limit && e.compressed <= limit - (gzip ? 18 : 0);
  }
  size_t input_size(const Entry& e) const { return checked_size(e.compressed) + (gzip ? 18 : 0); }
  nvcompAlignmentRequirements_t alignments() const {
    nvcompAlignmentRequirements_t requirements{};
    nv_check(gzip ? nvcompBatchedGzipDecompressGetRequiredAlignments(lookahead, &requirements) :
                    nvcompBatchedDeflateDecompressGetRequiredAlignments(deflate, &requirements));
    return requirements;
  }
  size_t scratch_size(size_t count, size_t maximum, size_t total) const {
    size_t bytes = 0;
    nv_check(gzip ? nvcompBatchedGzipDecompressGetTempSizeAsync(count, maximum, lookahead, &bytes, total) :
                    nvcompBatchedDeflateDecompressGetTempSizeAsync(count, maximum, deflate, &bytes, total));
    return bytes;
  }
  void decode(const void* const* input, const size_t* sizes, const size_t* capacities, size_t* actual,
              size_t count, void* scratch, size_t scratch_bytes, void* const* output,
              nvcompStatus_t* status, cudaStream_t stream) const {
    nv_check(gzip ? nvcompBatchedGzipDecompressAsync(input, sizes, capacities, actual, count, scratch, scratch_bytes,
                                                    output, lookahead, status, stream) :
                    nvcompBatchedDeflateDecompressAsync(input, sizes, capacities, actual, count, scratch, scratch_bytes,
                                                       output, deflate, status, stream));
  }
};
Batch plan(const std::vector<const Entry*>& entries, const Codec& codec, uint64_t crc_chunk) {
  Batch b; b.entries = entries; b.gzip = codec.gzip; b.crc_chunk = checked_size(crc_chunk);
  const auto requirements = codec.alignments();
  for (const auto* e : entries) {
    b.input_offsets.push_back(b.input); b.output_offsets.push_back(b.output);
    b.input += aligned(std::max<size_t>(codec.input_size(*e), 1), std::max<size_t>(requirements.input, 8));
    b.output += aligned(std::max<size_t>(checked_size(e->uncompressed), 1), std::max<size_t>(requirements.output, 8));
    b.max_output = std::max(b.max_output, checked_size(e->uncompressed));
    b.total_output += checked_size(e->uncompressed);
    b.crc_count += b.crc_chunk ? (checked_size(e->uncompressed) - 1) / b.crc_chunk + 1 : 1;
  }
  b.scratch = codec.scratch_size(entries.size(), b.max_output, b.total_output);
  // 一つの再利用arenaに置く各領域を256byte境界に揃える。
  b.memory = aligned(b.input, 256) + aligned(b.output, 256) + aligned(std::max<size_t>(b.scratch, 1), 256)
      + 5 * aligned(entries.size() * sizeof(size_t), 256)
      + aligned(entries.size() * sizeof(nvcompStatus_t), 256)
      + 2 * aligned(b.crc_count * sizeof(size_t), 256)
      + aligned(b.crc_count * sizeof(nvcompStatus_t), 256) + aligned(b.crc_count * sizeof(uint32_t), 256);
  return b;
}
struct BatchHost {
  // 入力は先頭、出力は末尾。異なるバッチの入力最大＋出力最大を足す必要がない。
  PinnedBuffer storage;
  char* input() const { return storage.data(); }
  char* output(const Batch& b) const { return storage.data() + storage.capacity() - b.output; }
};
struct BatchBuffers {
  DeviceBuffer arena;
  Events events;
};
// GPU CRC入力、Streaming入力、比較用spool読み戻しの固定バッファ分。
constexpr uint64_t host_fixed_buffers = 12ULL << 20;
constexpr size_t output_window = 1ULL << 20;
bool streaming_output(const Batch& b, const Options& opts, bool extracting) {
  return extracting && !opts.pipeline && b.stream_output;
}
uint64_t output_reservation(const Batch& b, const Options& opts, bool extracting) {
  return streaming_output(b, opts, extracting) ? std::min(opts.write_threads, b.entries.size()) * output_window : 0;
}
bool fits_batch(const Batch& b, const Options& opts, bool extracting) {
  auto fixed = host_fixed_buffers + output_reservation(b, opts, extracting);
  if (opts.vram_limit < StreamingCrc::memory || opts.host_limit < fixed) return false;
  auto host_budget = (opts.host_limit - fixed) / (opts.pipeline ? 2 : 1);
  auto output = extracting && !streaming_output(b, opts, extracting) ? b.output : 0;
  return b.memory <= opts.vram_limit - StreamingCrc::memory && b.input <= host_budget && output <= host_budget - b.input;
}
void prepare_host(const Batch& b, BatchHost& host, const Options& opts, bool extracting) {
  auto fixed = host_fixed_buffers + output_reservation(b, opts, extracting);
  auto required = b.input + (extracting && !streaming_output(b, opts, extracting) ? b.output : 0);
  if (opts.host_limit < fixed || required > opts.host_limit - fixed) throw std::runtime_error("固定化バッファの予算が不足しています");
  if (host.storage.capacity() > opts.host_limit - fixed) host.storage.release();
  host.storage.reserve(required);
}
double read_batch(const Archive& archive, const Batch& b, BatchHost& host) {
  auto start = std::chrono::steady_clock::now();
  host.storage.reserve(b.input);
  for (size_t i = 0; i < b.entries.size(); ++i) {
    const auto& e = *b.entries[i];
    char* target = host.input() + b.input_offsets[i];
    if (b.gzip) {
      constexpr unsigned char header[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255};
      std::copy_n(header, 10, target);
      for (size_t byte = 0; byte < 4; ++byte) {
        target[10 + checked_size(e.compressed) + byte] = static_cast<char>((e.crc >> (8 * byte)) & 255);
        target[14 + checked_size(e.compressed) + byte] = static_cast<char>((e.uncompressed >> (8 * byte)) & 255);
      }
      target += 10;
    }
    archive.read(e.data_offset, std::span<char>(target, checked_size(e.compressed)));
  }
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
double write_batch(const Batch& b, const BatchHost& host, OutputRoot* root, const Options& opts) {
  auto start = std::chrono::steady_clock::now();
  auto write_one = [&](size_t i) {
    check_cancelled();
    OutputFile file(*root, *b.entries[i], opts.durable);
    file.write(std::span<const char>(host.output(b) + b.output_offsets[i], checked_size(b.entries[i]->uncompressed)));
    file.commit();
  };
  if (root) {
    size_t count = std::min(opts.write_threads, b.entries.size());
    if (count == 1) {
      for (size_t i = 0; i < b.entries.size(); ++i) write_one(i);
    } else {
      std::atomic<size_t> next{0};
      std::atomic<bool> stop{false};
      std::mutex lock;
      std::exception_ptr error;
      std::vector<std::jthread> workers;
      for (size_t worker = 0; worker < count; ++worker) workers.emplace_back([&] {
        try {
          while (!stop.load()) {
            auto i = next.fetch_add(1);
            if (i >= b.entries.size()) break;
            write_one(i);
          }
        } catch (...) {
          stop.store(true);
          std::lock_guard guard(lock);
          if (!error) error = std::current_exception();
        }
      });
      workers.clear();
      if (error) std::rethrow_exception(error);
    }
  }
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
void write_device_batch(const Batch& b, const void* device_output, OutputRoot& root, const Options& opts, Stats& stats) {
  auto started = std::chrono::steady_clock::now();
  struct Slot { PinnedBuffer buffer{output_window}; Stream stream; Events events; double copies = 0; };
  const size_t count = std::min(opts.write_threads, b.entries.size());
  std::vector<std::unique_ptr<Slot>> slots;
  for (size_t i = 0; i < count; ++i) slots.push_back(std::make_unique<Slot>());
  stats.allocation_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::atomic<size_t> next{0};
  std::atomic<bool> stop{false};
  std::mutex lock;
  std::exception_ptr error;
  std::vector<std::jthread> workers;
  for (size_t worker = 0; worker < count; ++worker) workers.emplace_back([&, worker] {
    try {
      cuda_check(cudaSetDevice(opts.gpu));
      auto& slot = *slots[worker];
      while (!stop.load(std::memory_order_relaxed)) {
        auto i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= b.entries.size()) break;
        check_cancelled();
        const auto& e = *b.entries[i];
        OutputFile file(root, e, opts.durable);
        for (uint64_t at = 0; at < e.uncompressed;) {
          check_cancelled();
          auto bytes = static_cast<size_t>(std::min<uint64_t>(output_window, e.uncompressed - at));
          slot.events.mark(0, slot.stream);
          download(slot.buffer.data(), static_cast<const char*>(device_output) + b.output_offsets[i] + at, bytes, slot.stream);
          slot.events.mark(1, slot.stream);
          cuda_check(cudaStreamSynchronize(slot.stream));
          slot.copies += slot.events.elapsed(0, 1);
          file.write(std::span<const char>(slot.buffer.data(), bytes));
          at += bytes;
        }
        file.commit();
      }
    } catch (...) {
      stop.store(true, std::memory_order_relaxed);
      std::lock_guard guard(lock);
      if (!error) error = std::current_exception();
      request_cancel();
    }
  });
  workers.clear(); // 全D2H・出力が終わるまで元のGPU arenaを再利用しない。
  if (error) std::rethrow_exception(error);
  for (const auto& slot : slots) stats.transfer_seconds += slot->copies;
  stats.write_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  stats.gpu_streamed_output_bytes += b.total_output;
}
void batch_decode(const Archive& archive, const Batch& b, OutputRoot* root, const Options& opts, cudaStream_t stream,
                  const Codec& codec, BatchBuffers& buffers, BatchHost& host, Stats& stats,
                  bool preloaded = false, bool defer_write = false) {
  size_t n = b.entries.size();
  auto allocate_start = std::chrono::steady_clock::now();
  buffers.arena.reserve(b.memory);
  if (!preloaded) prepare_host(b, host, opts, root != nullptr);
  const bool stream_output = streaming_output(b, opts, root != nullptr);
  stats.host_buffer_bytes = std::max<uint64_t>(stats.host_buffer_bytes,
      host.storage.capacity() + host_fixed_buffers + output_reservation(b, opts, root != nullptr));
  stats.allocation_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - allocate_start).count();
  if (!preloaded) stats.read_seconds += read_batch(archive, b, host);
  size_t offset = 0;
  auto take = [&](size_t bytes) {
    void* ptr = static_cast<char*>(buffers.arena.data()) + offset;
    offset += aligned(std::max<size_t>(bytes, 1), 256); return ptr;
  };
  auto input = take(b.input), output = take(b.output), scratch = take(b.scratch);
  auto input_ptrs = static_cast<const void**>(take(n * sizeof(void*)));
  auto output_ptrs = static_cast<void**>(take(n * sizeof(void*)));
  auto compressed_sizes = static_cast<size_t*>(take(n * sizeof(size_t)));
  auto capacities = static_cast<size_t*>(take(n * sizeof(size_t)));
  auto actual_sizes = static_cast<size_t*>(take(n * sizeof(size_t)));
  auto statuses = static_cast<nvcompStatus_t*>(take(n * sizeof(nvcompStatus_t)));
  auto crc_ptrs = static_cast<const void**>(take(b.crc_count * sizeof(void*)));
  auto crc_sizes = static_cast<size_t*>(take(b.crc_count * sizeof(size_t)));
  auto crc_statuses = static_cast<nvcompStatus_t*>(take(b.crc_count * sizeof(nvcompStatus_t)));
  auto checksums = static_cast<uint32_t*>(take(b.crc_count * sizeof(uint32_t)));
  if (offset != b.memory) throw std::runtime_error("GPU arenaの見積もりが一致しません");
  std::vector<const void*> in_ptrs(n);
  std::vector<void*> out_ptrs(n);
  std::vector<size_t> sizes(n), limits(n), actual(n);
  std::vector<nvcompStatus_t> status(n), crc_status(b.crc_count);
  std::vector<uint32_t> crc(b.crc_count);
  std::vector<const void*> checksum_ptrs;
  std::vector<size_t> checksum_sizes;
  checksum_ptrs.reserve(b.crc_count); checksum_sizes.reserve(b.crc_count);
  for (size_t i = 0; i < n; ++i) {
    const auto& e = *b.entries[i];
    in_ptrs[i] = static_cast<char*>(input) + b.input_offsets[i];
    out_ptrs[i] = static_cast<char*>(output) + b.output_offsets[i];
    sizes[i] = codec.input_size(e); limits[i] = checked_size(e.uncompressed);
    for (size_t at = 0; at < limits[i];) {
      size_t length = b.crc_chunk ? std::min(b.crc_chunk, limits[i] - at) : limits[i];
      checksum_ptrs.push_back(static_cast<char*>(out_ptrs[i]) + at);
      checksum_sizes.push_back(length);
      at += length;
    }
  }
  if (checksum_ptrs.size() != b.crc_count) throw std::runtime_error("GPU CRC区間の見積もりが一致しません");
  auto& events = buffers.events;
  events.mark(0, stream);
  upload(input, host.input(), b.input, stream);
  upload(input_ptrs, in_ptrs.data(), n * sizeof(void*), stream);
  upload(output_ptrs, out_ptrs.data(), n * sizeof(void*), stream);
  upload(compressed_sizes, sizes.data(), n * sizeof(size_t), stream);
  upload(capacities, limits.data(), n * sizeof(size_t), stream);
  upload(crc_ptrs, checksum_ptrs.data(), b.crc_count * sizeof(void*), stream);
  upload(crc_sizes, checksum_sizes.data(), b.crc_count * sizeof(size_t), stream);
  events.mark(1, stream);
  codec.decode(input_ptrs, compressed_sizes, capacities, actual_sizes, n, scratch, b.scratch, output_ptrs, statuses, stream);
  events.mark(2, stream);
  auto crc_opts = crc_options(b.crc_count, b.crc_chunk ? std::min(b.crc_chunk, b.max_output) : b.max_output, stream);
  nv_check(nvcompBatchedCRC32Async(crc_ptrs, crc_sizes, b.crc_count, checksums, crc_opts,
                                 nvcompCRC32OnlySegment, crc_statuses, stream));
  events.mark(3, stream);
  download(status.data(), statuses, n * sizeof(nvcompStatus_t), stream);
  download(actual.data(), actual_sizes, n * sizeof(size_t), stream);
  download(crc.data(), checksums, b.crc_count * sizeof(uint32_t), stream);
  download(crc_status.data(), crc_statuses, b.crc_count * sizeof(nvcompStatus_t), stream);
  // testでは検証結果だけ戻す。展開データ全量のD2H転送は不要。
  if (root && !stream_output) download(host.output(b), output, b.output, stream);
  events.mark(4, stream);
  cuda_check(cudaStreamSynchronize(stream));
  stats.transfer_seconds += events.elapsed(0, 1) + events.elapsed(3, 4);
  stats.decode_seconds += events.elapsed(1, 2); stats.crc_seconds += events.elapsed(2, 3);
  // バッチ全体のCRCを検証してから各ファイルを確定する。
  // 区間CRCはGPUで計算し、CPUでは短いチェックサム値だけを順に結合する。
  auto combine_start = std::chrono::steady_clock::now();
  size_t chunk = 0;
  for (size_t i = 0; i < n; ++i) {
    nv_check(status[i]);
    if (actual[i] != limits[i]) throw std::runtime_error("GPU展開サイズが一致しません: " + b.entries[i]->name);
    uint32_t combined = 0;
    for (size_t at = 0; at < limits[i];) {
      nv_check(crc_status[chunk]);
      auto length = checksum_sizes[chunk];
      combined = at == 0 ? crc[chunk] : static_cast<uint32_t>(::crc32_combine(combined, crc[chunk], static_cast<z_off_t>(length)));
      at += length; ++chunk;
    }
    if (combined != b.entries[i]->crc) throw std::runtime_error("GPU CRC32が一致しません: " + b.entries[i]->name);
  }
  stats.crc_combine_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - combine_start).count();
  stats.gpu_crc_chunks += b.crc_count;
  if (!defer_write) {
    if (stream_output) write_device_batch(b, output, *root, opts, stats);
    else stats.write_seconds += write_batch(b, host, root, opts);
  }
  for (size_t i = 0; i < n; ++i) {
    check_cancelled();
    const auto& e = *b.entries[i];
    ++stats.files; stats.bytes += e.uncompressed; stats.gpu_crc_bytes += e.uncompressed;
  }
  stats.workspace = std::max<uint64_t>(stats.workspace, buffers.arena.capacity() + StreamingCrc::memory);
  ++stats.batches;
  if (codec.gzip) ++stats.lookahead_batches;
}
Batch choose_batch(EntrySelection entries, size_t i, const Options& opts,
                   const Codec& codec, bool extracting) {
  std::vector<const Entry*> pending;
  uint64_t payload = 0;
  const auto remaining = opts.vram_limit - StreamingCrc::memory;
  for (size_t j = i; j < entries.size() && pending.size() < opts.batch_entries; ++j) {
    const auto& e = *entries[j];
    if (e.directory || e.method != 8 || e.uncompressed == 0 ||
        !codec.supported(e)) break;
    if (e.compressed > remaining || e.uncompressed > remaining || e.compressed > remaining - e.uncompressed ||
        payload > remaining - e.compressed - e.uncompressed) break;
    pending.push_back(&e); payload += e.compressed + e.uncompressed;
  }
  if (pending.empty()) return {};
  auto chosen = plan(pending, codec, opts.gpu_crc_chunk);
  // 二分探索の途中で出力方式を変えると、host予算に収まる条件が非単調になる。
  // 候補全体で一度決め、縮小したprefixにも同じ方式を適用する。
  const bool stream_output = !opts.pipeline && opts.gpu_output != "buffered" &&
      (opts.gpu_output == "stream" || (chosen.max_output >= (64ULL << 20) && chosen.output >= (128ULL << 20)));
  chosen.stream_output = stream_output;
  if (fits_batch(chosen, opts, extracting)) return chosen;
  size_t lo = 0, hi = pending.size();
  chosen = Batch{};
  while (lo + 1 < hi) {
    auto mid = lo + (hi - lo) / 2;
    std::vector<const Entry*> prefix(pending.begin(), pending.begin() + static_cast<ptrdiff_t>(mid));
    auto attempt = plan(prefix, codec, opts.gpu_crc_chunk);
    attempt.stream_output = stream_output;
    if (fits_batch(attempt, opts, extracting)) { lo = mid; chosen = std::move(attempt); }
    else hi = mid;
  }
  return chosen;
}
Stats pipeline_decode(const Archive& archive, OutputRoot* root, const Options& opts, cudaStream_t stream,
                      const Codec& codec, BatchBuffers& buffers, EntrySelection entries) {
  BatchHost hosts[2];
  std::vector<Batch> batches;
  size_t max_host = 0, max_memory = 0;
  for (size_t i = 0; i < entries.size();) {
    check_cancelled();
    if (entries[i]->directory) { ++i; continue; }
    auto b = choose_batch(entries, i, opts, codec, root != nullptr);
    if (b.entries.empty()) throw std::runtime_error("pipelineは全ファイルが予算内の非空Deflateバッチに収まるZIP専用です");
    max_host = std::max(max_host, b.input + (root ? b.output : 0));
    max_memory = std::max(max_memory, b.memory);
    i += b.entries.size(); batches.push_back(std::move(b));
  }
  if (root) for (const auto* e : entries) if (e->directory) root->directory(e->name);
  Stats stats; stats.workspace = StreamingCrc::memory;
  if (batches.empty()) return stats;
  // 全計画の最大容量を先に確保。先読み中のcudaMallocHostがGPU/DMAを同期するのを避ける。
  auto allocate_start = std::chrono::steady_clock::now();
  buffers.arena.reserve(max_memory);
  for (auto& host : hosts) host.storage.reserve(max_host);
  stats.host_buffer_bytes = 2 * max_host + host_fixed_buffers;
  stats.allocation_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - allocate_start).count();
  std::future<double> reader, writers[2]; // futureはhost/batchより先に破棄・joinされる。
  stats.read_seconds += read_batch(archive, batches[0], hosts[0]);
  for (size_t i = 0; i < batches.size(); ++i) {
    check_cancelled();
    size_t slot = i % 2;
    if (reader.valid()) stats.read_seconds += reader.get();
    if (writers[slot].valid()) stats.write_seconds += writers[slot].get();
    if (i + 1 < batches.size()) {
      size_t next_slot = (i + 1) % 2;
      // 次入力が同じslotの前出力へ食い込む場合だけ、出力完了を待つ。
      // output(b)は容量末尾基準で固定し、先読みから使用中の領域を上書きしない。
      if (root && i > 0 && writers[next_slot].valid() &&
          batches[i + 1].input > hosts[next_slot].storage.capacity() - batches[i - 1].output) {
        stats.write_seconds += writers[next_slot].get();
        ++stats.pipeline_overlap_waits;
      }
      reader = std::async(std::launch::async, [&, next = i + 1] {
        cuda_check(cudaSetDevice(opts.gpu));
        return read_batch(archive, batches[next], hosts[next % 2]);
      });
    }
    // GPU arenaは一つだけ。ホスト側を二重化しVRAM予算は変えない。
    batch_decode(archive, batches[i], root, opts, stream, codec, buffers, hosts[slot], stats, true, true);
    if (root) writers[slot] = std::async(std::launch::async, [&, i, slot] {
      return write_batch(batches[i], hosts[slot], root, opts);
    });
  }
  for (auto& writer : writers) if (writer.valid()) stats.write_seconds += writer.get();
  return stats;
}
void stream_decode(const Archive& archive, const Entry& e, OutputRoot* root, const Options& opts, StreamingCrc* crc,
                   std::unique_ptr<GpuStreamWorker>& worker, Stats& stats) {
  if (crc) crc->reset();
  const bool cpu_checksum = opts.stream_crc == "cpu";
  if (!cpu_checksum && !crc) throw std::runtime_error("GPU CRC領域がありません");
  uint32_t checksum = 0;
  std::unique_ptr<OutputFile> file;
  if (root) file = std::make_unique<OutputFile>(*root, e, opts.durable);
  else if (e.method == 8 && !cpu_checksum) file = std::make_unique<OutputFile>();
  // persistent GPUカーネルの出力コールバックでは別のCUDA処理を同期しない。
  // CPU CRCなら定量メモリのまま計算でき、検証専用時の全量spoolも不要。
  Sink sink(e.uncompressed, file.get(), [&](auto bytes) {
    auto start = std::chrono::steady_clock::now();
    if (cpu_checksum) checksum = cpu_crc32(checksum, bytes.data(), bytes.size());
    else if (e.method == 0) crc->update(bytes);
    stats.crc_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  });
  std::ostream output(&sink); output.exceptions(std::ios::badbit | std::ios::failbit);
  if (e.method == 0) {
    std::vector<char> buffer(StreamingCrc::capacity);
    for (uint64_t offset = 0; offset < e.compressed;) {
      const size_t n = static_cast<size_t>(std::min<uint64_t>(buffer.size(), e.compressed - offset));
      archive.read(e.data_offset + offset, std::span<char>(buffer).first(n));
      output.write(buffer.data(), static_cast<std::streamsize>(n)); offset += n;
    }
  } else {
    auto decode_start = std::chrono::steady_clock::now();
    if (!worker) {
      Options child = opts;
      if (crc) child.vram_limit -= StreamingCrc::memory;
      worker = std::make_unique<GpuStreamWorker>(archive, child); ++stats.gpu_stream_workers;
    }
    const uint64_t bytes = worker->decode(e, output);
    check_cancelled();
    // Streaming API内の読み出し・CRCコールバック・出力を含む時間。
    stats.decode_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - decode_start).count();
    stats.workspace = std::max<uint64_t>(stats.workspace, bytes + (crc ? StreamingCrc::memory : 0));
    ++stats.streams;
  }
  sink.finish();
  if (!cpu_checksum) {
    auto start = std::chrono::steady_clock::now();
    if (e.method == 8) file->read_all([&](auto bytes) { crc->update(bytes); });
    checksum = crc->finish();
    stats.crc_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  }
  if (checksum != e.crc) throw std::runtime_error("CRC32が一致しません: " + e.name);
  if (cpu_checksum) stats.cpu_crc_bytes += e.uncompressed;
  else stats.gpu_crc_bytes += e.uncompressed;
  if (root) file->commit();
  ++stats.files; stats.bytes += e.uncompressed;
}
}
int gpu_stream_worker_main(int argc, char** argv) {
  if (argc != 5) throw std::runtime_error("内部GPU worker引数が不正です");
  auto device = std::stoi(argv[2]);
  auto parent = std::stol(argv[3]);
  auto vram = std::stoull(argv[4]);
  if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() != parent) return 1;
  std::signal(SIGINT, SIG_DFL); std::signal(SIGTERM, SIG_DFL); std::signal(SIGPIPE, SIG_DFL);
  struct stat metadata{};
  if (::fstat(3, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size < 0) return 1;
  cuda_check(cudaSetDevice(device));
  int concurrent = 0;
  cuda_check(cudaDeviceGetAttribute(&concurrent, cudaDevAttrConcurrentManagedAccess, device));
  if (!concurrent) throw std::runtime_error("GPUストリーミングにはconcurrentManagedAccessが必要です");
  Stream stream;
  size_t bytes = 0;
  nv_check(nvcompGzipStreamingDecompressGetTempSize(&bytes));
  if (bytes > vram) return 1;
  DeviceBuffer scratch(bytes);
  auto abort_io = [] {
    constexpr char message[] = "gipu worker: Streamingの入出力エラーです\n";
    (void)::write(STDERR_FILENO, message, sizeof(message) - 1);
    // 所有するのはread-only入力とpipeだけ。nvCOMPのjoinを通らず、この子だけを終了する。
    std::_Exit(1);
  };
  class PipeOutput final : public std::streambuf {
   public:
    explicit PipeOutput(uint64_t expected) : remaining_(expected) {}
    uint64_t remaining() const { return remaining_; }
   protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override {
      if (count < 0 || static_cast<uint64_t>(count) > remaining_) std::_Exit(1);
      size_t offset = 0, size = static_cast<size_t>(count);
      while (offset < size) {
        auto n = ::write(STDOUT_FILENO, data + offset, size - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) std::_Exit(1);
        offset += static_cast<size_t>(n);
      }
      remaining_ -= size;
      return count;
    }
    int_type overflow(int_type c) override {
      if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
      char byte = traits_type::to_char_type(c); xsputn(&byte, 1); return c;
    }
   private:
    uint64_t remaining_;
  };
  for (;;) {
    StreamRequest request{};
    for (size_t offset = 0; offset < sizeof(request);) {
      auto n = ::read(STDIN_FILENO, reinterpret_cast<char*>(request.data()) + offset, sizeof(request) - offset);
      if (n < 0 && errno == EINTR) continue;
      if (!n && !offset) return 0;
      if (n <= 0) abort_io();
      offset += static_cast<size_t>(n);
    }
    uint64_t archive_size = static_cast<uint64_t>(metadata.st_size);
    if (request[0] != stream_request_magic || request[1] > archive_size || request[2] > archive_size - request[1] ||
        request[4] > std::numeric_limits<uint32_t>::max()) return 1;
    Entry e; e.data_offset = request[1]; e.compressed = request[2]; e.uncompressed = request[3];
    e.crc = static_cast<uint32_t>(request[4]); e.method = 8;
    VirtualGzip wrapper(e, [&](uint64_t offset, std::span<char> target) {
      size_t done = 0;
      while (done < target.size()) {
        auto n = ::pread(3, target.data() + done, target.size() - done, static_cast<off_t>(offset + done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) abort_io();
        done += static_cast<size_t>(n);
      }
    });
    std::istream input(&wrapper);
    PipeOutput sink(e.uncompressed); std::ostream output(&sink);
    auto status = nvcompGzipStreamingDecompress(input, output, bytes, scratch.data(), stream);
    if (status != nvcompSuccess || cudaStreamSynchronize(stream) != cudaSuccess || input.bad() || output.bad() || sink.remaining())
      std::_Exit(1);
    std::array<uint64_t, 2> response{stream_response_magic, bytes};
    if (::write(STDOUT_FILENO, response.data(), sizeof(response)) != static_cast<ssize_t>(sizeof(response))) abort_io();
  }
}
std::string gpu_info(int device) {
  cuda_check(cudaSetDevice(device));
  cudaDeviceProp prop{}; cuda_check(cudaGetDeviceProperties(&prop, device));
  size_t free = 0, total = 0, scratch = 0;
  cuda_check(cudaMemGetInfo(&free, &total));
  nv_check(nvcompGzipStreamingDecompressGetTempSize(&scratch));
  std::ostringstream out;
  out << "GPU: " << prop.name << "\nCUDA compute capability: " << prop.major << '.' << prop.minor
      << "\nnvCOMP: " << NVCOMP_VER_MAJOR << '.' << NVCOMP_VER_MINOR << '.' << NVCOMP_VER_PATCH
      << "\nVRAM: " << free << " / " << total << " bytes空き"
      << "\nconcurrentManagedAccess: " << prop.concurrentManagedAccess
      << "\nStreaming Gzip作業領域: " << scratch << " bytes"
      << "\n増分GPU CRC32作業領域: " << StreamingCrc::memory << " bytes";
  return out.str();
}
uint64_t gpu_free_memory(int device) {
  int count = 0;
  if (cudaGetDeviceCount(&count) != cudaSuccess || device < 0 || device >= count) return 0;
  if (cudaSetDevice(device) != cudaSuccess) return 0;
  size_t free = 0, total = 0;
  if (cudaMemGetInfo(&free, &total) != cudaSuccess) return 0;
  return free;
}
Stats run_gpu(const Archive& archive, OutputRoot* root, const Options& opts, EntrySelection entries) {
  if (opts.vram_limit < StreamingCrc::memory) throw std::runtime_error("--vram-limitがGPU CRC32の作業領域より小さいです");
  if (opts.host_limit < host_fixed_buffers) throw std::runtime_error("GPU経路には--host-limit 12M以上が必要です");
  Stats stats;
  Codec codec(opts.gpu_algorithm == "lookahead");
  std::vector<const Entry*> gpu_entries, copy_entries;
  if (!opts.pipeline && opts.gpu_mode != "stream") {
    // Stored／空エントリでGPUバッチを分断しない。Deflateの本体は引き続きGPUで処理する。
    for (const auto* e : entries)
      ((e->directory || e->method == 0 || e->uncompressed == 0) ? copy_entries : gpu_entries).push_back(e);
    if (!copy_entries.empty()) {
      if (libdeflate_available()) stats = run_libdeflate(archive, root, opts, copy_entries);
      else {
        Options cpu = opts; cpu.backend = isal_available() ? "isal" : "cpu";
        stats = run_cpu(archive, root, cpu, copy_entries);
      }
    }
    if (gpu_entries.empty()) {
      stats.selected_backend = libdeflate_available() ? "libdeflate" : isal_available() ? "isal" : "cpu";
      stats.selection_reason = "no_nonempty_deflate";
      return stats;
    }
    if (opts.gpu_order == "auto") {
      const size_t large = static_cast<size_t>(std::count_if(gpu_entries.begin(), gpu_entries.end(),
          [](const Entry* e) { return e->uncompressed >= (64ULL << 20); }));
      const size_t small = static_cast<size_t>(std::count_if(gpu_entries.begin(), gpu_entries.end(),
          [](const Entry* e) { return e->uncompressed <= (1ULL << 20); }));
      // ごく少数の大ファイルを各バッチへ散らすと、毎回同じ遅い1本に律速される。
      // サイズが近いZIPやpipelineの物理順読み取りは変えず、外れ値がある場合だけ集める。
      if (large >= 2 && large <= 64 && small >= 64) {
        std::stable_partition(gpu_entries.begin(), gpu_entries.end(),
            [](const Entry* e) { return e->uncompressed >= (64ULL << 20); });
        ++stats.gpu_size_reorders;
      }
    }
    entries = gpu_entries;
  }
  // Streaming＋CPU CRCだけなら親はCUDAへ触らず、コンテキストとCRC用領域を作らない。
  const bool streaming_only = opts.gpu_mode == "stream" || std::none_of(entries.begin(), entries.end(),
      [&](const Entry* e) { return !e->directory && e->method == 8 && e->uncompressed && codec.supported(*e); });
  if (!opts.pipeline && opts.gpu_mode != "batch" && opts.stream_crc == "cpu" && streaming_only) {
    std::unique_ptr<GpuStreamWorker> worker;
    stats.host_buffer_bytes = std::max<uint64_t>(stats.host_buffer_bytes, host_fixed_buffers);
    for (const auto* e : entries) {
      check_cancelled();
      if (e->directory) { if (root) root->directory(e->name); continue; }
      if (e->method == 8 && e->uncompressed == 0) add_stats(stats, run_cpu_entry(archive, *e, root, opts, true));
      else stream_decode(archive, *e, root, opts, nullptr, worker, stats);
    }
    return stats;
  }
  cuda_check(cudaSetDevice(opts.gpu));
  Stream stream;
  StreamingCrc crc;
  BatchBuffers buffers;
  BatchHost host;
  std::unique_ptr<GpuStreamWorker> worker;
  stats.workspace = StreamingCrc::memory;
  stats.host_buffer_bytes = std::max<uint64_t>(stats.host_buffer_bytes, host_fixed_buffers);
  if (opts.pipeline) return pipeline_decode(archive, root, opts, stream, codec, buffers, entries);
  for (size_t i = 0; i < entries.size();) {
    check_cancelled();
    const auto& e = *entries[i];
    if (e.directory) { if (root) root->directory(e.name); ++i; continue; }
    if (e.method == 8 && e.uncompressed == 0) {
      add_stats(stats, run_cpu_entry(archive, e, root, opts, true)); ++i; continue;
    }
    if (e.method == 0 || opts.gpu_mode == "stream" || !codec.supported(e) || e.uncompressed == 0) {
      if (opts.gpu_mode == "batch" && e.method == 8 && e.uncompressed != 0) throw std::runtime_error("エントリがバッチAPIのサイズ上限を超えています");
      buffers.arena.release(); // Streamingのscratchとarenaを同時に保持しない。
      stream_decode(archive, e, root, opts, &crc, worker, stats); ++i; continue;
    }
    auto chosen = choose_batch(entries, i, opts, codec, root != nullptr);
    if (chosen.entries.empty()) {
      if (opts.gpu_mode == "batch") throw std::runtime_error("エントリが--vram-limit内のバッチに収まりません");
      buffers.arena.release();
      stream_decode(archive, e, root, opts, &crc, worker, stats); ++i;
    } else {
      worker.reset(); // worker scratchとバッチarenaを同時に持たない。
      batch_decode(archive, chosen, root, opts, stream, codec, buffers, host, stats); i += chosen.entries.size();
    }
  }
  return stats;
}
}
