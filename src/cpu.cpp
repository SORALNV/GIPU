#include "gipu/backend.hpp"
#include "gipu/checksum.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <zlib.h>

namespace gipu {
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }
Stats run_cpu_selected(const Archive& archive, std::span<const Entry* const> entries, OutputRoot* root,
                       const Options& opts, bool fast_checksum) {
  if (opts.host_limit < (2ULL << 20)) throw std::runtime_error("CPU Streamingには--host-limit 2M以上が必要です");
  Stats stats;
  std::vector<char> in(1 << 20), out(1 << 20);
  stats.host_buffer_bytes = in.size() + out.size();
  for (const auto* entry : entries) {
    const auto& e = *entry;
    check_cancelled();
    if (e.directory) { if (root) root->directory(e.name); continue; }
    std::unique_ptr<OutputFile> file;
    auto file_start = Clock::now();
    if (root) file = std::make_unique<OutputFile>(*root, e, opts.durable);
    stats.write_seconds += elapsed(file_start);
    uint32_t crc = 0;
    Sink sink(e.uncompressed, file.get(), [&](std::span<const char> b) {
      auto start = Clock::now();
      crc = fast_checksum ? cpu_crc32(crc, b.data(), b.size()) :
          static_cast<uint32_t>(crc32_z(crc, reinterpret_cast<const Bytef*>(b.data()), b.size()));
      stats.crc_seconds += elapsed(start);
    });
    std::ostream output(&sink); output.exceptions(std::ios::badbit | std::ios::failbit);
    auto read = [&](uint64_t offset, std::span<char> buffer) {
      auto start = Clock::now(); archive.read(offset, buffer); stats.read_seconds += elapsed(start);
    };
    auto emit = [&](const char* bytes, size_t size) {
      auto start = Clock::now(); double before_crc = stats.crc_seconds;
      output.write(bytes, static_cast<std::streamsize>(size));
      stats.write_seconds += std::max(0.0, elapsed(start) - (stats.crc_seconds - before_crc));
    };
    if (e.method == 0) {
      for (uint64_t offset = 0; offset < e.compressed;) {
        auto n = static_cast<size_t>(std::min<uint64_t>(in.size(), e.compressed - offset));
        read(e.data_offset + offset, std::span<char>(in).first(n));
        emit(in.data(), n); offset += n;
      }
    } else if (e.method == 8) {
      z_stream stream{};
      if (inflateInit2(&stream, -15) != Z_OK) throw std::runtime_error("zlibを初期化できません");
      try {
        uint64_t read_bytes = 0;
        int result = Z_OK;
        while (result != Z_STREAM_END) {
          check_cancelled();
          if (stream.avail_in == 0 && read_bytes < e.compressed) {
            auto n = static_cast<size_t>(std::min<uint64_t>(in.size(), e.compressed - read_bytes));
            read(e.data_offset + read_bytes, std::span<char>(in).first(n));
            read_bytes += n; stream.next_in = reinterpret_cast<Bytef*>(in.data()); stream.avail_in = static_cast<uInt>(n);
          }
          stream.next_out = reinterpret_cast<Bytef*>(out.data()); stream.avail_out = static_cast<uInt>(out.size());
          const auto before = stream.avail_in;
          auto decode_start = Clock::now();
          result = inflate(&stream, Z_NO_FLUSH);
          stats.decode_seconds += elapsed(decode_start);
          auto n = out.size() - stream.avail_out;
          if (result != Z_OK && result != Z_STREAM_END) throw std::runtime_error("Deflateデータが破損しています");
          emit(out.data(), n);
          if (result != Z_STREAM_END && n == 0 && before == stream.avail_in) throw std::runtime_error("Deflateストリームが途中で終了しています");
        }
        if (read_bytes - stream.avail_in != e.compressed) throw std::runtime_error("Deflateストリームの後に余分なデータがあります");
      } catch (...) { inflateEnd(&stream); throw; }
      inflateEnd(&stream);
    } else throw std::runtime_error("未対応の圧縮方式です: " + std::to_string(e.method));
    sink.finish();
    if (crc != e.crc) throw std::runtime_error("CRC32が一致しません: " + e.name);
    file_start = Clock::now();
    if (file) file->commit();
    stats.write_seconds += elapsed(file_start);
    ++stats.files; stats.bytes += e.uncompressed; stats.cpu_crc_bytes += e.uncompressed;
    ++stats.cpu_stream_files;
  }
  return stats;
}
}
Stats run_cpu(const Archive& archive, OutputRoot* root, const Options& opts) {
  std::vector<const Entry*> entries;
  entries.reserve(archive.entries().size());
  for (const auto& e : archive.entries()) entries.push_back(&e);
  return run_cpu_selected(archive, entries, root, opts, false);
}
Stats run_cpu_entry(const Archive& archive, const Entry& entry, OutputRoot* root, const Options& opts, bool fast_checksum) {
  const Entry* entries[] = {&entry};
  return run_cpu_selected(archive, entries, root, opts, fast_checksum);
}
}
