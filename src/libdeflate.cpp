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
Stats run_libdeflate(const Archive& archive, OutputRoot* root, const Options& opts) {
#ifndef GIPU_HAVE_LIBDEFLATE
  (void)archive; (void)root; (void)opts;
  throw std::runtime_error("libdeflateを有効にしたビルドが必要です");
#else
  // 比較用の全バッファCPU実装。workerごとの上限を固定し巨大エントリを拒否する。
  for (const auto& e : archive.entries()) {
    if (e.compressed > (256ULL << 20) || e.uncompressed > (256ULL << 20))
      throw std::runtime_error("libdeflate比較経路は1エントリ256MiBまでです。大きなファイルにはcpu/gpuを使ってください");
    if (e.directory && root) root->directory(e.name);
  }
  std::atomic<size_t> next{0};
  std::atomic<bool> stop{false};
  std::mutex lock;
  std::exception_ptr error;
  Stats result;
  std::vector<std::jthread> workers;
  for (size_t worker = 0; worker < opts.threads; ++worker) workers.emplace_back([&] {
    Stats local;
    std::unique_ptr<libdeflate_decompressor, decltype(&libdeflate_free_decompressor)> decoder(
        libdeflate_alloc_decompressor(), libdeflate_free_decompressor);
    try {
      if (!decoder) throw std::runtime_error("libdeflateを初期化できません");
      std::vector<char> input, output;
      while (!stop.load()) {
        auto index = next.fetch_add(1);
        if (index >= archive.entries().size()) break;
        const auto& e = archive.entries()[index];
        if (e.directory) continue;
        check_cancelled();
        input.resize(static_cast<size_t>(e.compressed));
        output.resize(std::max<size_t>(static_cast<size_t>(e.uncompressed), 1));
        auto start = std::chrono::steady_clock::now();
        archive.read(e.data_offset, input);
        auto read = std::chrono::steady_clock::now();
        if (e.method == 8) {
          size_t consumed = 0, produced = 0;
          auto status = libdeflate_deflate_decompress_ex(decoder.get(), input.data(), input.size(), output.data(),
              static_cast<size_t>(e.uncompressed), &consumed, &produced);
          if (status != LIBDEFLATE_SUCCESS || consumed != e.compressed || produced != e.uncompressed)
            throw std::runtime_error("Deflateデータまたは展開サイズが不正です: " + e.name);
        } else if (e.method == 0) {
          std::copy(input.begin(), input.end(), output.begin());
        } else throw std::runtime_error("未対応の圧縮方式です");
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
      }
    } catch (...) {
      stop.store(true);
      std::lock_guard guard(lock);
      if (!error) error = std::current_exception();
    }
    std::lock_guard guard(lock);
    result.files += local.files; result.bytes += local.bytes;
    result.cpu_crc_bytes += local.cpu_crc_bytes;
    // 複数workerの時間は加算値であり、実経過時間ではない。
    result.read_seconds += local.read_seconds; result.write_seconds += local.write_seconds;
    result.decode_seconds += local.decode_seconds; result.crc_seconds += local.crc_seconds;
  });
  workers.clear();
  if (error) std::rethrow_exception(error);
  return result;
#endif
}
}
