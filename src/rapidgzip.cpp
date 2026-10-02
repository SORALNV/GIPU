#include "gipu/backend.hpp"
#include "gipu/checksum.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>
#ifdef GIPU_HAVE_RAPIDGZIP
#include <rapidgzip/ParallelGzipReader.hpp>
#endif

namespace gipu {
bool rapidgzip_available() {
#ifdef GIPU_HAVE_RAPIDGZIP
  return true;
#else
  return false;
#endif
}
#ifdef GIPU_HAVE_RAPIDGZIP
namespace {
// 標準ZIPのraw Deflateを、seek可能な仮想Gzipとして提供する。
// compressed dataは元ZIPからpreadし、別Gzipや展開spoolを作らない。
class ZipGzipReader final : public rapidgzip::FileReader {
 public:
  ZipGzipReader(const Archive& archive, const Entry& entry) : archive_(archive), entry_(entry) {
    if (entry.compressed > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - 18)
      throw std::runtime_error("仮想Gzipのサイズが範囲外です");
    wrapper_[0] = '\x1f'; wrapper_[1] = '\x8b'; wrapper_[2] = 8; wrapper_[9] = static_cast<char>(255);
    for (unsigned i = 0; i < 4; ++i) {
      wrapper_[10 + i] = static_cast<char>((entry.crc >> (8 * i)) & 255);
      wrapper_[14 + i] = static_cast<char>((entry.uncompressed >> (8 * i)) & 255);
    }
  }
  void close() override { closed_ = true; }
  bool closed() const override { return closed_; }
  bool eof() const override { return position_ == *size(); }
  bool fail() const override { return false; }
  int fileno() const override { return -1; }
  bool seekable() const override { return true; }
  size_t read(char* buffer, size_t count) override {
    if (closed_) throw std::runtime_error("閉じた仮想Gzipを読み出せません");
    check_cancelled();
    count = std::min(count, *size() - position_);
    if (!buffer) { position_ += count; return count; }
    size_t done = 0;
    while (done < count) {
      size_t n = 0;
      if (position_ < 10) {
        n = std::min(count - done, 10 - position_);
        std::copy_n(wrapper_.data() + position_, n, buffer + done);
      } else if (position_ - 10 < entry_.compressed) {
        n = static_cast<size_t>(std::min<uint64_t>(count - done, entry_.compressed - (position_ - 10)));
        archive_.read(entry_.data_offset + position_ - 10, std::span<char>(buffer + done, n));
      } else {
        size_t offset = static_cast<size_t>(position_ - 10 - entry_.compressed);
        n = std::min(count - done, 8 - offset);
        std::copy_n(wrapper_.data() + 10 + offset, n, buffer + done);
      }
      done += n; position_ += n;
    }
    return done;
  }
  size_t seek(long long offset, int origin = SEEK_SET) override {
    if (closed_) throw std::runtime_error("閉じた仮想Gzipをseekできません");
    position_ = effectiveOffset(offset, origin); return position_;
  }
  std::optional<size_t> size() const override { return static_cast<size_t>(entry_.compressed) + 18; }
  size_t tell() const override { return position_; }
  void clearerr() override {}
 protected:
  rapidgzip::UniqueFileReader cloneRaw() const override {
    auto result = std::make_unique<ZipGzipReader>(archive_, entry_);
    if (closed_) result->close();
    return result;
  }
 private:
  const Archive& archive_;
  const Entry& entry_;
  std::array<char, 18> wrapper_{};
  size_t position_ = 0;
  bool closed_ = false;
};
}
#endif

Stats run_rapidgzip(const Archive& archive, OutputRoot* root, const Options& opts, EntrySelection entries) {
#ifndef GIPU_HAVE_RAPIDGZIP
  (void)archive; (void)root; (void)opts; (void)entries;
  throw std::runtime_error("Rapidgzipを有効にしたビルドが必要です");
#else
  using Clock = std::chrono::steady_clock;
  Stats stats;
  for (const auto* entry : entries) {
    const auto& e = *entry;
    check_cancelled();
    if (e.directory) { if (root) root->directory(e.name); continue; }
    if (e.method == 0 || e.uncompressed == 0 || opts.host_limit < (64ULL << 20)) {
      auto one = run_cpu_entry(archive, e, root, opts, true);
      add_stats(stats, one);
      continue;
    }
    size_t parallel = std::min<size_t>(opts.threads, static_cast<size_t>(opts.host_limit / (64ULL << 20)));
    uint64_t decoded_chunk = std::clamp<uint64_t>(opts.host_limit / (parallel * 8), 4ULL << 20, 64ULL << 20);
    rapidgzip::ParallelGzipReader<> reader(std::make_unique<ZipGzipReader>(archive, e), parallel, 4ULL << 20);
    reader.setKeepIndex(false);
    reader.setMaxDecompressedChunkSize(decoded_chunk);
    // 出力順で全バイトのCRCを独立に計算する。null出力によるデコード省略は行わない。
    reader.setCRC32Enabled(false);
    std::unique_ptr<OutputFile> file;
    if (root) file = std::make_unique<OutputFile>(*root, e, opts.durable);
    uint32_t crc = 0;
    Sink sink(e.uncompressed, file.get(), [&](auto bytes) {
      auto start = Clock::now(); crc = cpu_crc32(crc, bytes.data(), bytes.size());
      stats.crc_seconds += std::chrono::duration<double>(Clock::now() - start).count();
    });
    std::ostream output(&sink); output.exceptions(std::ios::badbit | std::ios::failbit);
    std::vector<char> buffer(4 << 20);
    stats.host_buffer_bytes = std::max<uint64_t>(stats.host_buffer_bytes, buffer.size());
    for (;;) {
      check_cancelled();
      auto start = Clock::now();
      size_t size = reader.read(buffer.data(), buffer.size());
      stats.decode_seconds += std::chrono::duration<double>(Clock::now() - start).count();
      if (!size) break;
      start = Clock::now(); double before_crc = stats.crc_seconds;
      output.write(buffer.data(), static_cast<std::streamsize>(size));
      stats.write_seconds += std::max(0.0, std::chrono::duration<double>(Clock::now() - start).count() -
                                         (stats.crc_seconds - before_crc));
    }
    sink.finish();
    if (crc != e.crc) throw std::runtime_error("CRC32が一致しません: " + e.name);
    if (file) file->commit();
    ++stats.files; ++stats.cpu_parallel_files; stats.bytes += e.uncompressed; stats.cpu_crc_bytes += e.uncompressed;
  }
  return stats;
#endif
}
}
