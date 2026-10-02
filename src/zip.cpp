#include "gipu/zip.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iconv.h>
#include <limits>
#include <mutex>
#include <thread>
#include <string_view>
#include <unordered_set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace gipu {
namespace {
std::atomic<bool> stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);
void on_signal(int) { stop_requested.store(true, std::memory_order_relaxed); }
uint16_t u16(const char* p) {
  return static_cast<uint16_t>(static_cast<unsigned char>(p[0]) | (static_cast<unsigned char>(p[1]) << 8));
}
uint32_t u32(const char* p) { return uint32_t(u16(p)) | (uint32_t(u16(p + 2)) << 16); }
uint64_t u64(const char* p) { return uint64_t(u32(p)) | (uint64_t(u32(p + 4)) << 32); }
void require(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}
void range(uint64_t start, uint64_t bytes, uint64_t end) {
  require(start <= end && bytes <= end - start, "ZIPのデータ範囲が不正です");
}
void supported_version(uint16_t version) {
  // 低位byteが必要なZIP仕様版。方式・フラグの対応可否は別途厳密に確認する。
  require((version & 255) <= 63, "未対応のZIP必要バージョンです");
}
std::string filename(const std::string& raw, bool utf8) {
  require(!raw.empty() && raw.find('\0') == std::string::npos, "空またはNULを含むファイル名です");
  std::string out;
  if (std::all_of(raw.begin(), raw.end(), [](unsigned char c) { return c < 128; })) {
    out = raw; // ASCIIはUTF-8/CP437で同一。iconvの初期化・確保を省く。
  } else {
    auto cd = iconv_open("UTF-8", utf8 ? "UTF-8" : "CP437");
    require(cd != reinterpret_cast<iconv_t>(-1), "ファイル名変換を初期化できません");
    out.resize(raw.size() * 4);
    char* in = const_cast<char*>(raw.data());
    char* dest = out.data();
    size_t inleft = raw.size(), outleft = out.size();
    const auto result = iconv(cd, &in, &inleft, &dest, &outleft);
    iconv_close(cd);
    require(result != size_t(-1) && inleft == 0, "ファイル名の文字コードが不正です");
    out.resize(out.size() - outleft);
  }
  if (out.front() == '/' || out.find('\\') != std::string::npos || out.find(':') != std::string::npos)
    throw std::runtime_error("危険なファイルパスです: " + out);
  for (unsigned char c : out) require(c >= 32 && c != 127, "制御文字を含むファイル名です");
  size_t pos = 0;
  while (pos < out.size()) {
    auto end = out.find('/', pos);
    if (end == std::string::npos) end = out.size();
    auto component = std::string_view(out).substr(pos, end - pos);
    if (component.empty() || component == "." || component == "..") throw std::runtime_error("危険なファイルパスです: " + out);
    pos = end + 1;
  }
  return out;
}
// 中央ディレクトリは順次先読み、散在するローカルヘッダは小さな窓で読む。
// 圧縮本体を大幅に先読みせず、多数の短いpreadをまとめる。
class MetadataReader {
 public:
  MetadataReader(const Archive& archive, size_t capacity) : archive_(archive), buffer_(capacity) {}
  void read(uint64_t offset, std::span<char> output) {
    range(offset, output.size(), archive_.file_size());
    if (output.empty()) return;
    if (output.size() > buffer_.size()) { archive_.read(offset, output); return; }
    if (offset < start_ || offset - start_ > valid_ || output.size() > valid_ - static_cast<size_t>(offset - start_)) {
      start_ = offset;
      valid_ = static_cast<size_t>(std::min<uint64_t>(buffer_.size(), archive_.file_size() - offset));
      archive_.read(start_, std::span<char>(buffer_).first(valid_));
    }
    range(offset - start_, output.size(), valid_);
    std::copy_n(buffer_.data() + (offset - start_), output.size(), output.data());
  }
 private:
  const Archive& archive_;
  std::vector<char> buffer_;
  uint64_t start_ = 0;
  size_t valid_ = 0;
};
bool zip64_extra(std::span<const char> extra, uint64_t& size, uint64_t& compressed,
                 uint64_t& offset, uint32_t& disk) {
  bool found = false;
  size_t pos = 0;
  while (pos < extra.size()) {
    require(extra.size() - pos >= 4, "ZIP追加フィールドが切れています");
    const auto id = u16(extra.data() + pos), len = u16(extra.data() + pos + 2);
    pos += 4;
    require(len <= extra.size() - pos, "ZIP追加フィールドの長さが不正です");
    if (id == 1) {
      require(!found, "ZIP64追加フィールドが重複しています");
      found = true;
      size_t consumed = 0;
      auto next64 = [&]() {
        require(len - consumed >= 8, "ZIP64追加フィールドが不足しています");
        auto value = u64(extra.data() + pos + consumed); consumed += 8; return value;
      };
      if (size == 0xffffffffULL) size = next64();
      if (compressed == 0xffffffffULL) compressed = next64();
      if (offset == 0xffffffffULL) offset = next64();
      if (disk == 0xffff) {
        require(len - consumed >= 4, "ZIP64ディスク番号が不足しています");
        disk = u32(extra.data() + pos + consumed);
      }
    }
    pos += len;
  }
  require((size != 0xffffffffULL && compressed != 0xffffffffULL && offset != 0xffffffffULL && disk != 0xffff) || found,
          "ZIP64追加フィールドがありません");
  return found;
}
uint64_t validate_local(Entry& e, uint64_t cd_offset,
                        MetadataReader& locals, std::vector<char>& local_variable) {
  range(e.local_offset, 30, cd_offset);
  std::array<char, 30> local{}; locals.read(e.local_offset, local); auto l = local.data();
  require(u32(l) == 0x04034b50 && u16(l + 6) == e.flags && u16(l + 8) == e.method, "ローカルヘッダが中央ディレクトリと一致しません");
  supported_version(u16(l + 4));
  uint64_t local_len = uint64_t(u16(l + 26)) + u16(l + 28);
  range(e.local_offset + 30, local_len, cd_offset);
  local_variable.resize(static_cast<size_t>(local_len)); locals.read(e.local_offset + 30, local_variable);
  require(std::string(local_variable.data(), u16(l + 26)) == e.raw_name, "ローカルファイル名が一致しません");
  uint64_t local_size = u32(l + 22), local_compressed = u32(l + 18), ignored_offset = 0;
  uint32_t ignored_disk = 0;
  const bool local_zip64 = zip64_extra(std::span<const char>(local_variable).subspan(u16(l + 26)), local_size, local_compressed, ignored_offset, ignored_disk);
  if (!(e.flags & 8)) require(u32(l + 14) == e.crc && local_size == e.uncompressed && local_compressed == e.compressed, "ローカルサイズまたはCRCが一致しません");
  e.data_offset = e.local_offset + 30 + local_len;
  range(e.data_offset, e.compressed, cd_offset);
  auto data_end = e.data_offset + e.compressed;
  if (e.flags & 8) {
    // Descriptorの64bitサイズはローカルZIP64フィールドの存在で決まる。
    bool wide = local_zip64 || e.compressed > 0xffffffffULL || e.uncompressed > 0xffffffffULL;
    std::array<char, 4> signature{}; range(data_end, 4, cd_offset); locals.read(data_end, signature);
    const size_t fields = wide ? 20 : 12;
    auto matches = [&](size_t skip) {
      if (fields + skip > cd_offset - data_end) return false;
      std::array<char, 24> descriptor{};
      locals.read(data_end, std::span<char>(descriptor).first(fields + skip));
      const auto d = descriptor.data() + skip;
      return u32(d) == e.crc && (wide ? u64(d + 4) : u32(d + 4)) == e.compressed &&
             (wide ? u64(d + 12) : u32(d + 8)) == e.uncompressed;
    };
    // CRC自体が署名値と同じ場合もある。先頭4byteだけで形式を決めず、
    // 中央ディレクトリのCRC・両サイズに一致する署名付き／無しを選ぶ。
    if (u32(signature.data()) == 0x08074b50 && matches(4)) data_end += fields + 4;
    else {
      require(matches(0), "Data Descriptorが一致しません");
      data_end += fields;
    }
  }
  return data_end;
}
}
void install_signal_handlers() {
  std::signal(SIGINT, on_signal); std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);
}
void request_cancel() { stop_requested.store(true, std::memory_order_relaxed); }
void check_cancelled() { if (stop_requested.load(std::memory_order_relaxed)) throw std::runtime_error("処理をキャンセルしました"); }
Archive::Archive(const std::filesystem::path& path, uint64_t metadata_limit, size_t metadata_threads, uint64_t max_entries)
    : metadata_limit_(metadata_limit), metadata_threads_(metadata_threads), max_entries_(max_entries) {
  require(metadata_threads <= 32, "metadata worker数が上限を超えています");
  require(max_entries > 0, "max-entriesは1以上です");
  // FIFOを指定された場合もopenで待たず、直後のfstatで通常ファイル以外を拒否する。
  // O_NONBLOCKは通常ファイルのpread動作を変更しない。
  fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd_ < 0) throw std::runtime_error("ZIPを開けません: " + std::string(std::strerror(errno)));
  try {
    struct stat st{};
    require(::fstat(fd_, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0, "通常のZIPファイルを指定してください");
    size_ = static_cast<uint64_t>(st.st_size);
    parse();
  } catch (...) { ::close(fd_); fd_ = -1; throw; }
}
Archive::~Archive() { if (fd_ >= 0) ::close(fd_); }
void Archive::read(uint64_t offset, std::span<char> buffer) const {
  check_cancelled(); range(offset, buffer.size(), size_);
  size_t done = 0;
  while (done < buffer.size()) {
    check_cancelled();
    auto n = ::pread(fd_, buffer.data() + done, buffer.size() - done, static_cast<off_t>(offset + done));
    if (n < 0 && errno == EINTR) continue;
    require(n > 0, "ZIP読み込みに失敗しました（切り詰めまたはI/Oエラー）");
    done += static_cast<size_t>(n);
  }
}
uint64_t Archive::total_size() const {
  uint64_t total = 0;
  for (const auto& e : entries_) {
    require(e.uncompressed <= std::numeric_limits<uint64_t>::max() - total, "展開サイズの合計がオーバーフローします");
    total += e.uncompressed;
  }
  return total;
}
void Archive::parse() {
  require(size_ >= 22, "ZIP終端レコードがありません");
  std::vector<char> tail(static_cast<size_t>(std::min<uint64_t>(size_, 65557)));
  const auto tail_start = size_ - tail.size();
  read(tail_start, tail);
  size_t end = tail.size();
  for (size_t i = tail.size() - 22;; --i) {
    if (u32(tail.data() + i) == 0x06054b50 && i + 22 + u16(tail.data() + i + 20) == tail.size()) { end = i; break; }
    if (i == 0) break;
  }
  require(end != tail.size(), "有効なZIP終端レコードがありません");
  const char* p = tail.data() + end;
  require(u16(p + 4) == 0 && u16(p + 6) == 0, "分割ZIPには対応していません");
  uint64_t count = u16(p + 10), cd_size = u32(p + 12), cd_offset = u32(p + 16);
  require(u16(p + 8) == count, "ZIPエントリ数が一致しません");
  uint64_t metadata_start = tail_start + end;
  if (count == 0xffff || cd_size == 0xffffffffULL || cd_offset == 0xffffffffULL) {
    require(metadata_start >= 20, "ZIP64ロケータがありません");
    std::array<char, 20> locator{}; read(metadata_start - 20, locator);
    require(u32(locator.data()) == 0x07064b50 && u32(locator.data() + 4) == 0 && u32(locator.data() + 16) == 1,
            "ZIP64ロケータが不正または分割ZIPです");
    uint64_t zip64_offset = u64(locator.data() + 8);
    range(zip64_offset, 56, metadata_start - 20);
    std::array<char, 56> record{}; read(zip64_offset, record); p = record.data();
    require(u32(p) == 0x06064b50 && u64(p + 4) >= 44, "ZIP64終端レコードが不正です");
    supported_version(u16(p + 14));
    range(zip64_offset + 12, u64(p + 4), metadata_start - 20);
    require(zip64_offset + 12 + u64(p + 4) == metadata_start - 20, "ZIP64終端の位置が不正です");
    require(u32(p + 16) == 0 && u32(p + 20) == 0 && u64(p + 24) == u64(p + 32), "分割ZIPには対応していません");
    count = u64(p + 32); cd_size = u64(p + 40); cd_offset = u64(p + 48);
    metadata_start = zip64_offset;
  }
  range(cd_offset, cd_size, metadata_start);
  require(cd_offset + cd_size == metadata_start, "中央ディレクトリの終端が不正です");
  require(cd_size <= metadata_limit_, "中央ディレクトリが--metadata-limitを超えています");
  require(count <= max_entries_, "ZIPエントリ数が--max-entriesを超えています");
  require(count <= cd_size / 46 && count <= std::numeric_limits<size_t>::max(), "ZIPエントリ数が不正です");
  uint64_t pos = cd_offset;
  std::unordered_set<std::string> paths, files;
  paths.reserve(static_cast<size_t>(count)); files.reserve(static_cast<size_t>(count));
  entries_.reserve(static_cast<size_t>(count));
  std::vector<std::pair<uint64_t, uint64_t>> occupied;
  occupied.resize(static_cast<size_t>(count));
  MetadataReader central(*this, 256 << 10);
  std::vector<char> variable;
  for (uint64_t index = 0; index < count; ++index) {
    range(pos, 46, cd_offset + cd_size);
    std::array<char, 46> h{}; central.read(pos, h); p = h.data();
    require(u32(p) == 0x02014b50, "中央ディレクトリの署名が不正です");
    supported_version(u16(p + 6));
    const auto name_len = u16(p + 28), extra_len = u16(p + 30), comment_len = u16(p + 32);
    require(name_len > 0, "空のファイル名です");
    const auto variable_size = uint64_t(name_len) + extra_len + comment_len;
    range(pos + 46, variable_size, cd_offset + cd_size);
    variable.resize(static_cast<size_t>(variable_size)); central.read(pos + 46, variable);
    Entry e;
    e.raw_name.assign(variable.data(), name_len);
    e.flags = u16(p + 8); e.method = u16(p + 10); e.crc = u32(p + 16);
    e.compressed = u32(p + 20); e.uncompressed = u32(p + 24); e.local_offset = u32(p + 42);
    uint32_t disk = u16(p + 34);
    zip64_extra(std::span<const char>(variable).subspan(name_len, extra_len), e.uncompressed, e.compressed, e.local_offset, disk);
    require(disk == 0, "分割ZIPには対応していません");
    require((e.flags & ~(uint16_t(0x080e))) == 0, "暗号化または未対応のZIPフラグです");
    e.name = filename(e.raw_name, (e.flags & 0x800) != 0);
    e.directory = e.name.back() == '/';
    auto key = e.directory ? e.name.substr(0, e.name.size() - 1) : e.name;
    if (!paths.insert(key).second) throw std::runtime_error("重複する出力パスです: " + key);
    if (!e.directory) files.insert(key);
    const auto host = static_cast<unsigned char>(p[5]);
    const auto type = (u32(p + 38) >> 16) & 0170000;
    if (host == 3 && type != 0) {
      if (type != 0100000 && type != 0040000) throw std::runtime_error("シンボリックリンク・特殊ファイルには対応していません: " + e.name);
      require((type == 0040000) == e.directory, "ディレクトリ属性と名前が一致しません");
    }
    if (e.directory) require(e.uncompressed == 0 && e.crc == 0, "ディレクトリにデータがあります");
    if (e.method == 0) require(e.compressed == e.uncompressed, "Storedのサイズが一致しません");
    entries_.push_back(std::move(e)); pos += 46 + variable_size;
  }
  require(pos == cd_offset + cd_size, "中央ディレクトリに余分なデータがあります");
  // 小規模は4KiB窓で直列、密集した多数ヘッダは4、大きく散在するヘッダは8 worker。
  // 中央・名前の検証を終え、全ローカル検証と重複検査が成功するまで出力は開かない。
  if (metadata_threads_ == 0)
    metadata_threads_ = std::min<size_t>(available_cpu_threads(),
        count >= 4096 && cd_offset / count >= 4096 ? 8 : count >= 16384 ? 4 : 1);
  metadata_threads_ = std::min<size_t>(metadata_threads_, std::max<size_t>(1, (entries_.size() + 255) / 256));
  auto validate_range = [&](size_t first, size_t last, MetadataReader& locals, std::vector<char>& extra) {
    for (size_t i = first; i < last; ++i) {
      auto& e = entries_[i];
      occupied[i] = {e.local_offset, validate_local(e, cd_offset, locals, extra)};
    }
  };
  if (metadata_threads_ == 1) {
    MetadataReader locals(*this, 4096);
    std::vector<char> extra;
    validate_range(0, entries_.size(), locals, extra);
  } else {
    std::atomic<size_t> next{0};
    std::atomic<bool> stop{false};
    std::mutex error_lock;
    std::exception_ptr error;
    std::vector<std::jthread> workers;
    for (size_t i = 0; i < metadata_threads_; ++i) workers.emplace_back([&] {
      try {
        MetadataReader locals(*this, 4096);
        std::vector<char> extra;
        while (!stop.load(std::memory_order_relaxed)) {
          const size_t first = next.fetch_add(256, std::memory_order_relaxed);
          if (first >= entries_.size()) break;
          validate_range(first, std::min(entries_.size(), first + 256), locals, extra);
        }
      } catch (...) {
        stop.store(true, std::memory_order_relaxed);
        std::lock_guard guard(error_lock);
        if (!error) error = std::current_exception();
      }
    });
    workers.clear();
    if (error) std::rethrow_exception(error);
  }
  std::sort(occupied.begin(), occupied.end());
  for (size_t i = 1; i < occupied.size(); ++i) require(occupied[i - 1].second <= occupied[i].first, "ZIPエントリのデータが重複しています");
  for (const auto& path : paths) {
    for (auto at = path.find('/'); at != std::string::npos; at = path.find('/', at + 1))
      require(!files.contains(path.substr(0, at)), "出力先のファイルとディレクトリが衝突しています");
  }
  (void)total_size();
}
}
