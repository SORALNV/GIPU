#include "gipu/backend.hpp"
#include "gipu/checksum.hpp"
#include <cuda_runtime_api.h>
#include <nvcomp/crc32.h>
#include <nvcomp/deflate.h>
#include <nvcomp/gzip.h>
#include <nvcomp/native/streaming_gzip.hpp>
#include <nvcomp/version.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>

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
  bool gzip = false;
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
Batch plan(const std::vector<const Entry*>& entries, const Codec& codec) {
  Batch b; b.entries = entries; b.gzip = codec.gzip;
  const auto requirements = codec.alignments();
  for (const auto* e : entries) {
    b.input_offsets.push_back(b.input); b.output_offsets.push_back(b.output);
    b.input += aligned(std::max<size_t>(codec.input_size(*e), 1), std::max<size_t>(requirements.input, 8));
    b.output += aligned(std::max<size_t>(checked_size(e->uncompressed), 1), std::max<size_t>(requirements.output, 8));
    b.max_output = std::max(b.max_output, checked_size(e->uncompressed));
    b.total_output += checked_size(e->uncompressed);
  }
  b.scratch = codec.scratch_size(entries.size(), b.max_output, b.total_output);
  // 一つの再利用arenaに置く各領域を256byte境界に揃える。
  b.memory = aligned(b.input, 256) + aligned(b.output, 256) + aligned(std::max<size_t>(b.scratch, 1), 256)
      + 5 * aligned(entries.size() * sizeof(size_t), 256)
      + 2 * aligned(entries.size() * sizeof(nvcompStatus_t), 256) + aligned(entries.size() * sizeof(uint32_t), 256);
  return b;
}
struct BatchHost {
  PinnedBuffer input, output;
};
struct BatchBuffers {
  DeviceBuffer arena;
  Events events;
};
double read_batch(const Archive& archive, const Batch& b, BatchHost& host) {
  auto start = std::chrono::steady_clock::now();
  host.input.reserve(b.input);
  for (size_t i = 0; i < b.entries.size(); ++i) {
    const auto& e = *b.entries[i];
    char* target = host.input.data() + b.input_offsets[i];
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
    file.write(std::span<const char>(host.output.data() + b.output_offsets[i], checked_size(b.entries[i]->uncompressed)));
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
void batch_decode(const Archive& archive, const Batch& b, OutputRoot* root, const Options& opts, cudaStream_t stream,
                  const Codec& codec, BatchBuffers& buffers, BatchHost& host, Stats& stats,
                  bool preloaded = false, bool defer_write = false) {
  size_t n = b.entries.size();
  auto allocate_start = std::chrono::steady_clock::now();
  buffers.arena.reserve(b.memory);
  if (root) host.output.reserve(b.output);
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
  auto crc_statuses = static_cast<nvcompStatus_t*>(take(n * sizeof(nvcompStatus_t)));
  auto checksums = static_cast<uint32_t*>(take(n * sizeof(uint32_t)));
  if (offset != b.memory) throw std::runtime_error("GPU arenaの見積もりが一致しません");
  std::vector<const void*> in_ptrs(n);
  std::vector<void*> out_ptrs(n);
  std::vector<size_t> sizes(n), limits(n), actual(n);
  std::vector<nvcompStatus_t> status(n), crc_status(n);
  std::vector<uint32_t> crc(n);
  for (size_t i = 0; i < n; ++i) {
    const auto& e = *b.entries[i];
    in_ptrs[i] = static_cast<char*>(input) + b.input_offsets[i];
    out_ptrs[i] = static_cast<char*>(output) + b.output_offsets[i];
    sizes[i] = codec.input_size(e); limits[i] = checked_size(e.uncompressed);
  }
  auto& events = buffers.events;
  events.mark(0, stream);
  upload(input, host.input.data(), b.input, stream);
  upload(input_ptrs, in_ptrs.data(), n * sizeof(void*), stream);
  upload(output_ptrs, out_ptrs.data(), n * sizeof(void*), stream);
  upload(compressed_sizes, sizes.data(), n * sizeof(size_t), stream);
  upload(capacities, limits.data(), n * sizeof(size_t), stream);
  events.mark(1, stream);
  codec.decode(input_ptrs, compressed_sizes, capacities, actual_sizes, n, scratch, b.scratch, output_ptrs, statuses, stream);
  events.mark(2, stream);
  auto crc_opts = crc_options(n, b.max_output, stream);
  nv_check(nvcompBatchedCRC32Async(static_cast<const void**>(static_cast<void*>(output_ptrs)), capacities, n, checksums, crc_opts,
                                 nvcompCRC32OnlySegment, crc_statuses, stream));
  events.mark(3, stream);
  download(status.data(), statuses, n * sizeof(nvcompStatus_t), stream);
  download(actual.data(), actual_sizes, n * sizeof(size_t), stream);
  download(crc.data(), checksums, n * sizeof(uint32_t), stream);
  download(crc_status.data(), crc_statuses, n * sizeof(nvcompStatus_t), stream);
  // testでは検証結果だけ戻す。展開データ全量のD2H転送は不要。
  if (root) download(host.output.data(), output, b.output, stream);
  events.mark(4, stream);
  cuda_check(cudaStreamSynchronize(stream));
  stats.transfer_seconds += events.elapsed(0, 1) + events.elapsed(3, 4);
  stats.decode_seconds += events.elapsed(1, 2); stats.crc_seconds += events.elapsed(2, 3);
  // バッチ全体のCRCを検証してから各ファイルを確定する。
  for (size_t i = 0; i < n; ++i) {
    nv_check(status[i]);
    if (actual[i] != limits[i]) throw std::runtime_error("GPU展開サイズが一致しません: " + b.entries[i]->name);
    nv_check(crc_status[i]);
    if (crc[i] != b.entries[i]->crc) throw std::runtime_error("GPU CRC32が一致しません: " + b.entries[i]->name);
  }
  if (!defer_write) stats.write_seconds += write_batch(b, host, root, opts);
  for (size_t i = 0; i < n; ++i) {
    check_cancelled();
    const auto& e = *b.entries[i];
    ++stats.files; stats.bytes += e.uncompressed; stats.gpu_crc_bytes += e.uncompressed;
  }
  stats.workspace = std::max<uint64_t>(stats.workspace, buffers.arena.capacity() + StreamingCrc::memory);
  ++stats.batches;
  if (codec.gzip) ++stats.lookahead_batches;
}
Batch choose_batch(const std::vector<Entry>& entries, size_t i, const Options& opts,
                   const Codec& codec) {
  std::vector<const Entry*> pending;
  uint64_t payload = 0;
  const auto remaining = opts.vram_limit - StreamingCrc::memory;
  for (size_t j = i; j < entries.size() && pending.size() < opts.batch_entries; ++j) {
    const auto& e = entries[j];
    if (e.directory || e.method != 8 || e.uncompressed == 0 ||
        !codec.supported(e)) break;
    if (e.compressed > remaining || e.uncompressed > remaining || e.compressed > remaining - e.uncompressed ||
        payload > remaining - e.compressed - e.uncompressed) break;
    pending.push_back(&e); payload += e.compressed + e.uncompressed;
  }
  if (pending.empty()) return {};
  auto chosen = plan(pending, codec);
  if (chosen.memory <= remaining) return chosen;
  size_t lo = 0, hi = pending.size();
  chosen = Batch{};
  while (lo + 1 < hi) {
    auto mid = lo + (hi - lo) / 2;
    std::vector<const Entry*> prefix(pending.begin(), pending.begin() + static_cast<ptrdiff_t>(mid));
    auto attempt = plan(prefix, codec);
    if (attempt.memory <= remaining) { lo = mid; chosen = std::move(attempt); }
    else hi = mid;
  }
  return chosen;
}
Stats pipeline_decode(const Archive& archive, OutputRoot* root, const Options& opts, cudaStream_t stream,
                      const Codec& codec, BatchBuffers& buffers) {
  BatchHost hosts[2];
  std::vector<Batch> batches;
  const auto& entries = archive.entries();
  for (size_t i = 0; i < entries.size();) {
    if (entries[i].directory) { ++i; continue; }
    auto b = choose_batch(entries, i, opts, codec);
    if (b.entries.empty()) throw std::runtime_error("pipelineは全ファイルが予算内の非空Deflateバッチに収まるZIP専用です");
    i += b.entries.size(); batches.push_back(std::move(b));
  }
  if (root) for (const auto& e : entries) if (e.directory) root->directory(e.name);
  Stats stats; stats.workspace = StreamingCrc::memory;
  if (batches.empty()) return stats;
  // 全計画の最大容量を先に確保。先読み中のcudaMallocHostがGPU/DMAを同期するのを避ける。
  auto allocate_start = std::chrono::steady_clock::now();
  size_t max_input = 0, max_output = 0, max_memory = 0;
  for (const auto& b : batches) {
    max_input = std::max(max_input, b.input); max_output = std::max(max_output, b.output);
    max_memory = std::max(max_memory, b.memory);
  }
  buffers.arena.reserve(max_memory);
  for (auto& host : hosts) {
    host.input.reserve(max_input);
    if (root) host.output.reserve(max_output);
  }
  stats.allocation_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - allocate_start).count();
  std::future<double> reader, writers[2]; // futureはhost/batchより先に破棄・joinされる。
  stats.read_seconds += read_batch(archive, batches[0], hosts[0]);
  for (size_t i = 0; i < batches.size(); ++i) {
    check_cancelled();
    size_t slot = i % 2;
    if (reader.valid()) stats.read_seconds += reader.get();
    if (writers[slot].valid()) stats.write_seconds += writers[slot].get();
    if (i + 1 < batches.size()) reader = std::async(std::launch::async, [&, next = i + 1] {
      cuda_check(cudaSetDevice(opts.gpu));
      return read_batch(archive, batches[next], hosts[next % 2]);
    });
    // GPU arenaは一つだけ。ホスト側を二重化しVRAM予算は変えない。
    batch_decode(archive, batches[i], root, opts, stream, codec, buffers, hosts[slot], stats, true, true);
    if (root) writers[slot] = std::async(std::launch::async, [&, i, slot] {
      return write_batch(batches[i], hosts[slot], root, opts);
    });
  }
  for (auto& writer : writers) if (writer.valid()) stats.write_seconds += writer.get();
  return stats;
}
void stream_decode(const Archive& archive, const Entry& e, OutputRoot* root, const Options& opts, StreamingCrc& crc,
                   cudaStream_t stream, Stats& stats) {
  crc.reset();
  const bool cpu_checksum = opts.stream_crc == "cpu";
  uint32_t checksum = 0;
  std::unique_ptr<OutputFile> file;
  if (root) file = std::make_unique<OutputFile>(*root, e, opts.durable);
  else if (e.method == 8 && !cpu_checksum) file = std::make_unique<OutputFile>();
  // persistent GPUカーネルの出力コールバックでは別のCUDA処理を同期しない。
  // CPU CRCなら定量メモリのまま計算でき、検証専用時の全量spoolも不要。
  Sink sink(e.uncompressed, file.get(), [&](auto bytes) {
    auto start = std::chrono::steady_clock::now();
    if (cpu_checksum) checksum = cpu_crc32(checksum, bytes.data(), bytes.size());
    else if (e.method == 0) crc.update(bytes);
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
    int concurrent = 0;
    cuda_check(cudaDeviceGetAttribute(&concurrent, cudaDevAttrConcurrentManagedAccess, opts.gpu));
    if (!concurrent) throw std::runtime_error("GPUストリーミングにはconcurrentManagedAccessが必要です");
    size_t bytes = 0;
    nv_check(nvcompGzipStreamingDecompressGetTempSize(&bytes));
    if (bytes > opts.vram_limit - StreamingCrc::memory) throw std::runtime_error("ストリーミング作業領域が--vram-limitを超えています");
    DeviceBuffer scratch(bytes);
    VirtualGzip wrapper(archive, e);
    std::istream input(&wrapper); input.exceptions(std::ios::badbit);
    nv_check(nvcompGzipStreamingDecompress(input, output, bytes, scratch.data(), stream));
    cuda_check(cudaStreamSynchronize(stream));
    if (input.bad() || output.bad()) throw std::runtime_error("GPUストリームの入出力に失敗しました");
    // Streaming API内の読み出し・CRCコールバック・出力を含む時間。
    stats.decode_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - decode_start).count();
    stats.workspace = std::max<uint64_t>(stats.workspace, bytes + StreamingCrc::memory);
    ++stats.streams;
  }
  sink.finish();
  if (!cpu_checksum) {
    auto start = std::chrono::steady_clock::now();
    if (e.method == 8) file->read_all([&](auto bytes) { crc.update(bytes); });
    checksum = crc.finish();
    stats.crc_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  }
  if (checksum != e.crc) throw std::runtime_error("CRC32が一致しません: " + e.name);
  if (cpu_checksum) stats.cpu_crc_bytes += e.uncompressed;
  else stats.gpu_crc_bytes += e.uncompressed;
  if (root) file->commit();
  ++stats.files; stats.bytes += e.uncompressed;
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
Stats run_gpu(const Archive& archive, OutputRoot* root, const Options& opts) {
  cuda_check(cudaSetDevice(opts.gpu));
  if (opts.vram_limit < StreamingCrc::memory) throw std::runtime_error("--vram-limitがGPU CRC32の作業領域より小さいです");
  Stream stream;
  StreamingCrc crc;
  BatchBuffers buffers;
  BatchHost host;
  Stats stats; stats.workspace = StreamingCrc::memory;
  Codec codec(opts.gpu_algorithm == "lookahead");
  if (opts.pipeline) return pipeline_decode(archive, root, opts, stream, codec, buffers);
  const auto& entries = archive.entries();
  for (size_t i = 0; i < entries.size();) {
    check_cancelled();
    const auto& e = entries[i];
    if (e.directory) { if (root) root->directory(e.name); ++i; continue; }
    if (e.method == 0 || opts.gpu_mode == "stream" || !codec.supported(e) || e.uncompressed == 0) {
      if (opts.gpu_mode == "batch" && e.method == 8 && e.uncompressed != 0) throw std::runtime_error("エントリがバッチAPIのサイズ上限を超えています");
      buffers.arena.release(); // Streamingのscratchとarenaを同時に保持しない。
      stream_decode(archive, e, root, opts, crc, stream, stats); ++i; continue;
    }
    auto chosen = choose_batch(entries, i, opts, codec);
    if (chosen.entries.empty()) {
      if (opts.gpu_mode == "batch") throw std::runtime_error("エントリが--vram-limit内のバッチに収まりません");
      buffers.arena.release();
      stream_decode(archive, e, root, opts, crc, stream, stats); ++i;
    } else {
      batch_decode(archive, chosen, root, opts, stream, codec, buffers, host, stats); i += chosen.entries.size();
    }
  }
  return stats;
}
}
