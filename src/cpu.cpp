#include "gipu/backend.hpp"
#include "gipu/checksum.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <zlib.h>
#ifdef GIPU_HAVE_ISAL
#include <igzip_lib.h>
#endif

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
    }
#ifdef GIPU_HAVE_ISAL
    else if (e.method == 8 && fast_checksum) {
      auto state = std::make_unique<inflate_state>();
      isal_inflate_init(state.get());
      uint64_t read_bytes = 0;
      while (state->block_state != ISAL_BLOCK_FINISH) {
        check_cancelled();
        if (state->avail_in == 0 && read_bytes < e.compressed) {
          auto n = static_cast<size_t>(std::min<uint64_t>(in.size(), e.compressed - read_bytes));
          read(e.data_offset + read_bytes, std::span<char>(in).first(n));
          read_bytes += n;
          state->next_in = reinterpret_cast<uint8_t*>(in.data()); state->avail_in = static_cast<uint32_t>(n);
        }
        state->next_out = reinterpret_cast<uint8_t*>(out.data()); state->avail_out = static_cast<uint32_t>(out.size());
        auto before_in = state->avail_in;
        auto before_state = state->block_state;
        auto before_bits = state->read_in_length;
        auto start = Clock::now();
        int result = isal_inflate(state.get());
        stats.decode_seconds += elapsed(start);
        if (result != ISAL_DECOMP_OK) throw std::runtime_error("ISA-L: Deflateデータが破損しています");
        auto n = out.size() - state->avail_out;
        emit(out.data(), n);
        if (state->block_state != ISAL_BLOCK_FINISH && !n && before_in == state->avail_in &&
            before_state == state->block_state && before_bits == state->read_in_length)
          throw std::runtime_error("Deflateストリームが途中で終了しています");
      }
      // ISA-Lは先読みした末尾バイトをbit reservoirに残す。丸々未使用のバイトを除外する。
      uint64_t unused = state->avail_in + static_cast<uint64_t>(std::max(state->read_in_length, 0) / 8);
      if (unused > read_bytes || read_bytes - unused != e.compressed)
        throw std::runtime_error("Deflateストリームの後に余分なデータがあります");
      ++stats.isal_files;
    }
#endif
    else if (e.method == 8) {
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
Stats run_cpu(const Archive& archive, OutputRoot* root, const Options& opts, EntrySelection entries) {
  if (opts.backend == "isal" && !isal_available()) throw std::runtime_error("ISA-Lを有効にしたビルドが必要です");
  return run_cpu_selected(archive, entries, root, opts, opts.backend == "isal");
}
Stats run_cpu_entry(const Archive& archive, const Entry& entry, OutputRoot* root, const Options& opts, bool fast_checksum) {
  const Entry* entries[] = {&entry};
  return run_cpu_selected(archive, entries, root, opts, fast_checksum);
}
bool isal_available() {
#ifdef GIPU_HAVE_ISAL
  return true;
#else
  return false;
#endif
}
}
