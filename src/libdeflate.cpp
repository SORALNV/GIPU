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
  uint64_t largest = 0;
  for (const auto* entry : entries) {
    const auto& e = *entry;
    if (e.directory && root) root->directory(e.name);
    if (!e.directory) { ++files; largest = std::max(largest, e.uncompressed); }
  }
  if (!files) return {};
  auto count = std::min({opts.threads, files, static_cast<size_t>(opts.host_limit / (2ULL << 20))});
  const uint64_t worker_budget = opts.host_limit / count;
  const uint64_t buffer_budget = std::min(worker_budget, opts.cpu_buffer_limit);
  // 大きさが揃った小ファイルだけ仕事の取得をまとめ、共有atomicの競合を抑える。
  // 大きい外れ値を含む場合は1件ずつ分配し、特定workerへの偏りを避ける。
  const size_t grain = largest <= (64ULL << 10) && files >= count * 64 ? 16 : 1;
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
      size_t index = 0, task_end = 0;
      while (!stop.load()) {
        if (index == task_end) {
          index = next.fetch_add(grain, std::memory_order_relaxed);
          if (index >= entries.size()) break;
          task_end = std::min(entries.size(), index + grain);
        }
        const auto& e = *entries[index++];
        if (e.directory) continue;
        check_cancelled();
        uint64_t output_bytes = e.method == 8 ? std::max<uint64_t>(e.uncompressed, 1) : 0;
        // ほぼ非圧縮の大きなDeflateでは全量の確保・コピーが不利だった。
        // 実測したISA-Lがある場合だけ小窓へ回す。Storedと小入力は従来どおり。
        const bool near_stored = isal_available() && e.method == 8 && e.uncompressed >= (8ULL << 20) &&
                                 e.compressed >= e.uncompressed - e.uncompressed / 50;
        if (near_stored || e.compressed > buffer_budget || output_bytes > buffer_budget - e.compressed) {
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
      {
        std::lock_guard guard(lock);
        if (!error) error = std::current_exception();
      }
      // 別workerが大きなStreaming処理中でも次のI/O境界で止める。
      request_cancel();
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
