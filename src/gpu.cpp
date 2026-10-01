#include "gipu/backend.hpp"
#include <cuda_runtime_api.h>
#include <nvcomp/crc32.h>
#include <nvcomp/deflate.h>
#include <nvcomp/native/streaming_gzip.hpp>
#include <nvcomp/version.h>
#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>

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
  explicit DeviceBuffer(size_t size) { cuda_check(cudaMalloc(&ptr_, std::max<size_t>(size, 1))); }
  ~DeviceBuffer() { cudaFree(ptr_); }
  DeviceBuffer(const DeviceBuffer&) = delete;
  DeviceBuffer& operator=(const DeviceBuffer&) = delete;
  void* data() const { return ptr_; }
  template<class T> T* as() const { return static_cast<T*>(ptr_); }
 private:
  void* ptr_ = nullptr;
};
class PinnedBuffer {
 public:
  explicit PinnedBuffer(size_t size) { cuda_check(cudaMallocHost(&ptr_, std::max<size_t>(size, 1))); }
  ~PinnedBuffer() { cudaFreeHost(ptr_); }
  char* data() const { return static_cast<char*>(ptr_); }
 private:
  void* ptr_ = nullptr;
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
};
constexpr size_t metadata_per_entry = 2 * sizeof(void*) + 3 * sizeof(size_t) + 2 * sizeof(nvcompStatus_t) + sizeof(uint32_t);
Batch plan(const std::vector<const Entry*>& entries, nvcompBatchedDeflateDecompressOpts_t opts) {
  Batch b; b.entries = entries;
  nvcompAlignmentRequirements_t requirements{};
  nv_check(nvcompBatchedDeflateDecompressGetRequiredAlignments(opts, &requirements));
  for (const auto* e : entries) {
    b.input_offsets.push_back(b.input); b.output_offsets.push_back(b.output);
    b.input += aligned(std::max<size_t>(checked_size(e->compressed), 1), std::max<size_t>(requirements.input, 8));
    b.output += aligned(std::max<size_t>(checked_size(e->uncompressed), 1), std::max<size_t>(requirements.output, 8));
    b.max_output = std::max(b.max_output, checked_size(e->uncompressed));
    b.total_output += checked_size(e->uncompressed);
  }
  nv_check(nvcompBatchedDeflateDecompressGetTempSizeAsync(entries.size(), b.max_output, opts, &b.scratch, b.total_output));
  b.memory = b.input + b.output + std::max<size_t>(b.scratch, 1) + entries.size() * metadata_per_entry;
  return b;
}
void batch_decode(const Archive& archive, const Batch& b, OutputRoot* root, const Options& opts, cudaStream_t stream,
                  nvcompBatchedDeflateDecompressOpts_t decompress_opts, Stats& stats) {
  size_t n = b.entries.size();
  DeviceBuffer input(b.input), output(b.output), scratch(b.scratch), input_ptrs(n * sizeof(void*)), output_ptrs(n * sizeof(void*));
  DeviceBuffer compressed_sizes(n * sizeof(size_t)), capacities(n * sizeof(size_t)), actual_sizes(n * sizeof(size_t));
  DeviceBuffer statuses(n * sizeof(nvcompStatus_t)), crc_statuses(n * sizeof(nvcompStatus_t)), checksums(n * sizeof(uint32_t));
  PinnedBuffer host_input(b.input), host_output(b.output);
  std::vector<const void*> in_ptrs(n);
  std::vector<void*> out_ptrs(n);
  std::vector<size_t> sizes(n), limits(n), actual(n);
  std::vector<nvcompStatus_t> status(n), crc_status(n);
  std::vector<uint32_t> crc(n);
  for (size_t i = 0; i < n; ++i) {
    const auto& e = *b.entries[i];
    archive.read(e.data_offset, std::span<char>(host_input.data() + b.input_offsets[i], checked_size(e.compressed)));
    in_ptrs[i] = static_cast<char*>(input.data()) + b.input_offsets[i];
    out_ptrs[i] = static_cast<char*>(output.data()) + b.output_offsets[i];
    sizes[i] = checked_size(e.compressed); limits[i] = checked_size(e.uncompressed);
  }
  upload(input.data(), host_input.data(), b.input, stream);
  upload(input_ptrs.data(), in_ptrs.data(), n * sizeof(void*), stream);
  upload(output_ptrs.data(), out_ptrs.data(), n * sizeof(void*), stream);
  upload(compressed_sizes.data(), sizes.data(), n * sizeof(size_t), stream);
  upload(capacities.data(), limits.data(), n * sizeof(size_t), stream);
  nv_check(nvcompBatchedDeflateDecompressAsync(input_ptrs.as<const void*>(), compressed_sizes.as<size_t>(), capacities.as<size_t>(),
      actual_sizes.as<size_t>(), n, scratch.data(), b.scratch, output_ptrs.as<void*>(), decompress_opts, statuses.as<nvcompStatus_t>(), stream));
  download(status.data(), statuses.data(), n * sizeof(nvcompStatus_t), stream);
  download(actual.data(), actual_sizes.data(), n * sizeof(size_t), stream);
  cuda_check(cudaStreamSynchronize(stream));
  for (size_t i = 0; i < n; ++i) {
    nv_check(status[i]);
    if (actual[i] != limits[i]) throw std::runtime_error("GPU展開サイズが一致しません: " + b.entries[i]->name);
  }
  auto crc_opts = crc_options(n, b.max_output, stream);
  nv_check(nvcompBatchedCRC32Async(output_ptrs.as<const void*>(), capacities.as<size_t>(), n, checksums.as<uint32_t>(), crc_opts,
                                 nvcompCRC32OnlySegment, crc_statuses.as<nvcompStatus_t>(), stream));
  download(crc.data(), checksums.data(), n * sizeof(uint32_t), stream);
  download(crc_status.data(), crc_statuses.data(), n * sizeof(nvcompStatus_t), stream);
  download(host_output.data(), output.data(), b.output, stream);
  cuda_check(cudaStreamSynchronize(stream));
  // バッチ全体のCRCを検証してから各ファイルを確定する。
  for (size_t i = 0; i < n; ++i) {
    nv_check(crc_status[i]);
    if (crc[i] != b.entries[i]->crc) throw std::runtime_error("GPU CRC32が一致しません: " + b.entries[i]->name);
  }
  for (size_t i = 0; i < n; ++i) {
    check_cancelled();
    const auto& e = *b.entries[i];
    if (root) {
      OutputFile file(*root, e, opts.durable);
      file.write(std::span<const char>(host_output.data() + b.output_offsets[i], checked_size(e.uncompressed)));
      file.commit();
    }
    ++stats.files; stats.bytes += e.uncompressed;
  }
  stats.workspace = std::max<uint64_t>(stats.workspace, b.memory + StreamingCrc::memory);
  ++stats.batches;
}
void stream_decode(const Archive& archive, const Entry& e, OutputRoot* root, const Options& opts, StreamingCrc& crc,
                   cudaStream_t stream, Stats& stats) {
  crc.reset();
  std::unique_ptr<OutputFile> file;
  if (root) file = std::make_unique<OutputFile>(*root, e, opts.durable);
  else if (e.method == 8) file = std::make_unique<OutputFile>();
  Sink sink(e.uncompressed, file.get(), [&](auto bytes) { if (e.method == 0) crc.update(bytes); });
  std::ostream output(&sink); output.exceptions(std::ios::badbit | std::ios::failbit);
  if (e.method == 0) {
    std::vector<char> buffer(StreamingCrc::capacity);
    for (uint64_t offset = 0; offset < e.compressed;) {
      const size_t n = static_cast<size_t>(std::min<uint64_t>(buffer.size(), e.compressed - offset));
      archive.read(e.data_offset + offset, std::span<char>(buffer).first(n));
      output.write(buffer.data(), static_cast<std::streamsize>(n)); offset += n;
    }
  } else {
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
    stats.workspace = std::max<uint64_t>(stats.workspace, bytes + StreamingCrc::memory);
    ++stats.streams;
  }
  sink.finish();
  if (e.method == 8) file->read_all([&](auto bytes) { crc.update(bytes); });
  if (crc.finish() != e.crc) throw std::runtime_error("GPU CRC32が一致しません: " + e.name);
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
  Stats stats; stats.workspace = StreamingCrc::memory;
  auto decompress_opts = nvcompBatchedDeflateDecompressDefaultOpts;
  decompress_opts.backend = NVCOMP_DECOMPRESS_BACKEND_CUDA;
  const auto& entries = archive.entries();
  for (size_t i = 0; i < entries.size();) {
    check_cancelled();
    const auto& e = entries[i];
    if (e.directory) { if (root) root->directory(e.name); ++i; continue; }
    if (e.method == 0 || opts.gpu_mode == "stream" || e.uncompressed > nvcompDeflateDecompressionMaxAllowedChunkSize ||
        e.compressed > nvcompDeflateDecompressionMaxAllowedChunkSize || e.uncompressed == 0) {
      if (opts.gpu_mode == "batch" && e.method == 8 && e.uncompressed != 0) throw std::runtime_error("エントリがバッチAPIのサイズ上限を超えています");
      stream_decode(archive, e, root, opts, crc, stream, stats); ++i; continue;
    }
    Batch chosen;
    std::vector<const Entry*> pending;
    size_t j = i;
    // バッチを最大256エントリとし、GPU入力・出力・scratch・metadataを含めて予算化する。
    while (j < entries.size() && pending.size() < 256) {
      const auto& candidate = entries[j];
      if (candidate.directory || candidate.method != 8 || candidate.uncompressed == 0 ||
          candidate.uncompressed > nvcompDeflateDecompressionMaxAllowedChunkSize || candidate.compressed > nvcompDeflateDecompressionMaxAllowedChunkSize) break;
      // 見積もりの加算前に上限をチェックし、巨大な宣言値による整数オーバーフローを防ぐ。
      const auto remaining = opts.vram_limit - StreamingCrc::memory;
      if (candidate.compressed > remaining || candidate.uncompressed > remaining ||
          candidate.compressed > remaining - candidate.uncompressed) break;
      pending.push_back(&candidate);
      auto b = plan(pending, decompress_opts);
      if (b.memory > remaining) { pending.pop_back(); break; }
      chosen = std::move(b); ++j;
    }
    if (chosen.entries.empty()) {
      if (opts.gpu_mode == "batch") throw std::runtime_error("エントリが--vram-limit内のバッチに収まりません");
      stream_decode(archive, e, root, opts, crc, stream, stats); ++i;
    } else {
      batch_decode(archive, chosen, root, opts, stream, decompress_opts, stats); i = j;
    }
  }
  return stats;
}
}
