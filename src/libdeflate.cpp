#include "gipu/backend.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#ifdef GIPU_HAVE_LIBDEFLATE
#include <libdeflate.h>
#endif

namespace gipu {
bool libdeflate_available() {
#ifdef GIPU_HAVE_LIBDEFLATE
  return true;
#else
  return false;
#endif
}
Stats run_libdeflate(const Archive& archive, OutputRoot* root, const Options& opts, EntrySelection entries) {
#ifndef GIPU_HAVE_LIBDEFLATE
  (void)archive; (void)root; (void)opts; (void)entries;
  throw std::runtime_error("libdeflateを有効にしたビルドが必要です");
#else
  // workerごとに予算を分割。巨大エントリだけは定量メモリのStreamingへ回す。
  if (opts.host_limit < (2ULL << 20)) throw std::runtime_error("CPU経路には--host-limit 2M以上が必要です");
  size_t files = 0;
  for (const auto* entry : entries) {
    const auto& e = *entry;
    if (e.directory && root) root->directory(e.name);
    if (!e.directory) ++files;
  }
  if (!files) return {};
  auto count = std::min({opts.threads, files, static_cast<size_t>(opts.host_limit / (2ULL << 20))});
  const uint64_t worker_budget = opts.host_limit / count;
  const uint64_t buffer_budget = std::min(worker_budget, opts.cpu_buffer_limit);
  std::atomic<size_t> next{0};
  std::atomic<bool> stop{false};
  std::mutex lock;
  std::exception_ptr error;
  Stats result;
  std::vector<std::jthread> workers;
  for (size_t worker = 0; worker < count; ++worker) workers.emplace_back([&] {
    Stats local;
    std::unique_ptr<libdeflate_decompressor, decltype(&libdeflate_free_decompressor)> decoder(
        libdeflate_alloc_decompressor(), libdeflate_free_decompressor);
    try {
      if (!decoder) throw std::runtime_error("libdeflateを初期化できません");
      std::unique_ptr<char[]> buffer;
      size_t capacity = 0;
      while (!stop.load()) {
        auto index = next.fetch_add(1);
        if (index >= entries.size()) break;
        const auto& e = *entries[index];
        if (e.directory) continue;
        check_cancelled();
        uint64_t output_bytes = e.method == 8 ? std::max<uint64_t>(e.uncompressed, 1) : 0;
        if (e.compressed > buffer_budget || output_bytes > buffer_budget - e.compressed) {
          // 残っている全量バッファを解放してからStreaming用2MiBを確保する。
          buffer.reset(); capacity = 0;
          Options streaming = opts; streaming.host_limit = worker_budget;
          auto stats = run_cpu_entry(archive, e, root, streaming, true);
          add_stats(local, stats);
          continue;
        }
        auto required = static_cast<size_t>(std::max<uint64_t>(e.compressed + output_bytes, 1));
        if (required > capacity) {
          auto allocate_start = std::chrono::steady_clock::now();
          buffer.reset(); capacity = 0;
          buffer = std::make_unique_for_overwrite<char[]>(required); capacity = required;
          local.allocation_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - allocate_start).count();
        }
        local.host_buffer_bytes = std::max<uint64_t>(local.host_buffer_bytes, capacity);
        auto input = std::span<char>(buffer.get(), static_cast<size_t>(e.compressed));
        auto output = std::span<char>(e.method == 8 ? buffer.get() + e.compressed : buffer.get(),
                                      static_cast<size_t>(e.uncompressed));
        auto start = std::chrono::steady_clock::now();
        archive.read(e.data_offset, input);
        auto read = std::chrono::steady_clock::now();
        if (e.method == 8) {
          size_t consumed = 0, produced = 0;
          auto status = libdeflate_deflate_decompress_ex(decoder.get(), input.data(), input.size(), output.data(),
              static_cast<size_t>(e.uncompressed), &consumed, &produced);
          if (status != LIBDEFLATE_SUCCESS || consumed != e.compressed || produced != e.uncompressed)
            throw std::runtime_error("Deflateデータまたは展開サイズが不正です: " + e.name);
        } else if (e.method != 0) throw std::runtime_error("未対応の圧縮方式です");
        auto decoded = std::chrono::steady_clock::now();
        if (libdeflate_crc32(0, output.data(), static_cast<size_t>(e.uncompressed)) != e.crc)
          throw std::runtime_error("CRC32が一致しません: " + e.name);
        auto verified = std::chrono::steady_clock::now();
        if (root) {
          OutputFile file(*root, e, opts.durable);
          file.write(std::span<const char>(output.data(), static_cast<size_t>(e.uncompressed)));
          file.commit();
        }
        auto end = std::chrono::steady_clock::now();
        local.read_seconds += std::chrono::duration<double>(read - start).count();
        local.decode_seconds += std::chrono::duration<double>(decoded - read).count();
        local.crc_seconds += std::chrono::duration<double>(verified - decoded).count();
        local.write_seconds += std::chrono::duration<double>(end - verified).count();
        ++local.files; local.bytes += e.uncompressed; local.cpu_crc_bytes += e.uncompressed;
        ++local.cpu_buffered_files;
      }
    } catch (...) {
      stop.store(true);
      std::lock_guard guard(lock);
      if (!error) error = std::current_exception();
    }
    std::lock_guard guard(lock);
    // 複数workerの時間は加算値であり、実経過時間ではない。
    add_stats(result, local, true);
  });
  workers.clear();
  if (error) std::rethrow_exception(error);
  return result;
#endif
}
}
