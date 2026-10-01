#include "gipu/backend.hpp"
#include <array>
#include <stdexcept>
#include <zlib.h>

namespace gipu {
Stats run_cpu(const Archive& archive, OutputRoot* root, const Options& opts) {
  Stats stats;
  std::vector<char> in(1 << 20), out(1 << 20);
  for (const auto& e : archive.entries()) {
    check_cancelled();
    if (e.directory) { if (root) root->directory(e.name); continue; }
    std::unique_ptr<OutputFile> file;
    if (root) file = std::make_unique<OutputFile>(*root, e, opts.durable);
    uint32_t crc = 0;
    Sink sink(e.uncompressed, file.get(), [&](std::span<const char> b) {
      crc = static_cast<uint32_t>(crc32_z(crc, reinterpret_cast<const Bytef*>(b.data()), b.size()));
    });
    std::ostream output(&sink); output.exceptions(std::ios::badbit | std::ios::failbit);
    if (e.method == 0) {
      for (uint64_t offset = 0; offset < e.compressed;) {
        auto n = static_cast<size_t>(std::min<uint64_t>(in.size(), e.compressed - offset));
        archive.read(e.data_offset + offset, std::span<char>(in).first(n));
        output.write(in.data(), static_cast<std::streamsize>(n)); offset += n;
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
            archive.read(e.data_offset + read_bytes, std::span<char>(in).first(n));
            read_bytes += n; stream.next_in = reinterpret_cast<Bytef*>(in.data()); stream.avail_in = static_cast<uInt>(n);
          }
          stream.next_out = reinterpret_cast<Bytef*>(out.data()); stream.avail_out = static_cast<uInt>(out.size());
          const auto before = stream.avail_in;
          result = inflate(&stream, Z_NO_FLUSH);
          auto n = out.size() - stream.avail_out;
          if (result != Z_OK && result != Z_STREAM_END) throw std::runtime_error("Deflateデータが破損しています");
          output.write(out.data(), static_cast<std::streamsize>(n));
          if (result != Z_STREAM_END && n == 0 && before == stream.avail_in) throw std::runtime_error("Deflateストリームが途中で終了しています");
        }
        if (read_bytes - stream.avail_in != e.compressed) throw std::runtime_error("Deflateストリームの後に余分なデータがあります");
      } catch (...) { inflateEnd(&stream); throw; }
      inflateEnd(&stream);
    } else throw std::runtime_error("未対応の圧縮方式です: " + std::to_string(e.method));
    sink.finish();
    if (crc != e.crc) throw std::runtime_error("CRC32が一致しません: " + e.name);
    if (file) file->commit();
    ++stats.files; stats.bytes += e.uncompressed;
  }
  return stats;
}
}
