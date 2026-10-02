#include "gipu/io.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/syscall.h>
#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#endif

namespace gipu {
namespace {
void fail(const std::string& message) { throw std::runtime_error(message + ": " + std::strerror(errno)); }
int descend(int fd, const std::string& component) {
  if (::mkdirat(fd, component.c_str(), 0755) != 0 && errno != EEXIST) fail("ディレクトリを作成できません");
  int next = ::openat(fd, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (next < 0) fail("安全に出力ディレクトリを開けません");
  return next;
}
}
OutputRoot::OutputRoot(const std::filesystem::path& path, bool anonymous, bool fast_paths) : fast_paths_(fast_paths) {
  std::filesystem::create_directories(path);
  fd_ = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd_ < 0) fail("出力先を開けません");
  // /procがない環境では従来の名前付き一時ファイルへ戻る。
  if (anonymous) proc_fds_ = ::open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}
OutputRoot::~OutputRoot() { if (proc_fds_ >= 0) ::close(proc_fds_); if (fd_ >= 0) ::close(fd_); }
int OutputRoot::parent(const std::string& name) const {
#if defined(SYS_openat2) && defined(RESOLVE_BENEATH) && defined(RESOLVE_NO_SYMLINKS)
  auto separator = name.find_last_of('/');
  if (separator != std::string::npos && fast_paths_.load(std::memory_order_relaxed)) {
    auto parent = name.substr(0, separator);
    open_how how{};
    how.flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS;
    int result = static_cast<int>(::syscall(SYS_openat2, fd_, parent.c_str(), &how, sizeof(how)));
    if (result >= 0) {
      fast_parent_opens_.fetch_add(1, std::memory_order_relaxed);
      return result;
    }
    if (errno == ENOSYS || errno == EINVAL) fast_paths_.store(false, std::memory_order_relaxed);
    else if (errno != ENOENT && errno != ENAMETOOLONG) fail("安全に出力ディレクトリを開けません");
    // 未作成／PATH_MAX超の階層だけ、成分ごとのmkdirat・O_NOFOLLOWへ戻す。
  }
#endif
  portable_parent_walks_.fetch_add(1, std::memory_order_relaxed);
  int current = ::fcntl(fd_, F_DUPFD_CLOEXEC, 0);
  if (current < 0) fail("出力先を複製できません");
  try {
    size_t start = 0;
    for (size_t end = name.find('/'); end != std::string::npos; end = name.find('/', start)) {
      int next = descend(current, name.substr(start, end - start));
      ::close(current); current = next; start = end + 1;
    }
    return current;
  } catch (...) { ::close(current); throw; }
}
void OutputRoot::directory(const std::string& name) const {
  int fd = parent(name); ::close(fd);
}
OutputFile::OutputFile() {
  auto temporary = (std::filesystem::temp_directory_path() / "gipu-spool-XXXXXX").string();
  fd_ = ::mkstemp(temporary.data());
  if (fd_ < 0) fail("ストリーミング検証用の一時出力を作成できません");
  if (::unlink(temporary.c_str()) != 0) {
    const int saved = errno; ::close(fd_); fd_ = -1; errno = saved;
    fail("ストリーミング検証用の一時名を削除できません");
  }
  ::fcntl(fd_, F_SETFD, FD_CLOEXEC);
}
OutputFile::OutputFile(const OutputRoot& root, const Entry& entry, bool durable) : durable_(durable) {
  parent_ = root.parent(entry.name);
  try {
    target_ = entry.name.substr(entry.name.find_last_of('/') + 1);
    struct stat st{};
    if (::fstatat(parent_, target_.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0)
      throw std::runtime_error("既存ファイルは上書きしません: " + entry.name);
    if (errno != ENOENT) fail("出力先を検査できません");
    if (root.proc_fds() >= 0) {
      fd_ = ::openat(parent_, ".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
      if (fd_ >= 0) {
        anonymous_ = true; proc_fds_ = root.proc_fds(); root.record_temporary(true); return;
      }
      // サポートしないfilesystem／古いkernelだけfallbackする。容量・FD不足は隠さない。
      if (errno != EOPNOTSUPP && errno != EINVAL && errno != EISDIR && errno != ENOENT && errno != ENOSYS)
        fail("名前なし一時ファイルを作成できません");
    }
    uint64_t random = 0;
    if (::getrandom(&random, sizeof(random), 0) != sizeof(random)) fail("一時名を生成できません");
    temporary_ = ".gipu-" + std::to_string(random) + ".part";
    fd_ = ::openat(parent_, temporary_.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd_ < 0) fail("一時ファイルを作成できません");
    root.record_temporary(false);
  } catch (...) { ::close(parent_); parent_ = -1; throw; }
}
OutputFile::~OutputFile() {
  if (fd_ >= 0) ::close(fd_);
  if (parent_ >= 0) {
    if (!temporary_.empty()) ::unlinkat(parent_, temporary_.c_str(), 0);
    ::close(parent_);
  }
}
void OutputFile::write(std::span<const char> bytes) {
  size_t offset = 0;
  while (offset < bytes.size()) {
    check_cancelled();
    auto n = ::write(fd_, bytes.data() + offset, bytes.size() - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) fail("展開データを書き込めません");
    offset += static_cast<size_t>(n);
  }
}
void OutputFile::read_all(const std::function<void(std::span<const char>)>& consume) const {
  std::vector<char> buffer(4 << 20);
  off_t offset = 0;
  for (;;) {
    check_cancelled();
    auto n = ::pread(fd_, buffer.data(), buffer.size(), offset);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) fail("GPU CRC用の出力再読み込みに失敗しました");
    if (n == 0) break;
    consume(std::span<const char>(buffer.data(), static_cast<size_t>(n)));
    offset += n;
  }
}
void OutputFile::commit() {
  if (::fchmod(fd_, 0644) != 0) fail("ファイル権限を設定できません");
  if (durable_ && ::fsync(fd_) != 0) fail("ファイルを同期できません");
  // linkatは既存名を置き換えない。検査から確定までの競合も防ぐ。
  if (anonymous_) {
    // AT_EMPTY_PATHの権限を要求せず、自プロセスの保持中FDだけを参照する。
    auto source = std::to_string(fd_);
    if (::linkat(proc_fds_, source.c_str(), parent_, target_.c_str(), AT_SYMLINK_FOLLOW) != 0)
      fail("名前なし一時ファイルを確定できません");
  } else {
    if (::linkat(parent_, temporary_.c_str(), parent_, target_.c_str(), 0) != 0) fail("検証済みファイルを確定できません");
    if (::unlinkat(parent_, temporary_.c_str(), 0) != 0) fail("一時ファイル名を削除できません");
    temporary_.clear();
  }
  if (durable_ && ::fsync(parent_) != 0) fail("ディレクトリを同期できません");
}
Sink::Sink(uint64_t expected, OutputFile* file, std::function<void(std::span<const char>)> checksum)
    : expected_(expected), file_(file), checksum_(std::move(checksum)) {}
std::streamsize Sink::xsputn(const char* data, std::streamsize size) {
  check_cancelled();
  if (size < 0 || static_cast<uint64_t>(size) > expected_ - written_) throw std::runtime_error("宣言された展開サイズを超えました");
  auto bytes = std::span<const char>(data, static_cast<size_t>(size));
  checksum_(bytes);
  if (file_) file_->write(bytes);
  written_ += static_cast<uint64_t>(size); return size;
}
Sink::int_type Sink::overflow(int_type c) {
  if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
  char byte = traits_type::to_char_type(c); xsputn(&byte, 1); return c;
}
void Sink::finish() const { if (written_ != expected_) throw std::runtime_error("展開サイズが一致しません"); }
VirtualGzip::VirtualGzip(const Archive& archive, const Entry& entry)
    : VirtualGzip(entry, [&archive](uint64_t offset, std::span<char> bytes) { archive.read(offset, bytes); }) {}
VirtualGzip::VirtualGzip(const Entry& entry, std::function<void(uint64_t, std::span<char>)> read)
    : entry_(entry), read_(std::move(read)), buffer_(1 << 20) {
  wrapper_[0] = '\x1f'; wrapper_[1] = '\x8b'; wrapper_[2] = 8; wrapper_[9] = static_cast<char>(255);
  for (unsigned i = 0; i < 4; ++i) {
    wrapper_[10 + i] = static_cast<char>((entry.crc >> (8 * i)) & 255);
    wrapper_[14 + i] = static_cast<char>((entry.uncompressed >> (8 * i)) & 255);
  }
}
VirtualGzip::int_type VirtualGzip::underflow() {
  check_cancelled();
  if (gptr() && gptr() < egptr()) return traits_type::to_int_type(*gptr());
  size_t bytes = 0;
  if (position_ < 10) {
    bytes = static_cast<size_t>(10 - position_);
    std::copy_n(wrapper_.data() + position_, bytes, buffer_.data());
  } else if (position_ - 10 < entry_.compressed) {
    const auto consumed = position_ - 10;
    bytes = static_cast<size_t>(std::min<uint64_t>(buffer_.size(), entry_.compressed - consumed));
    read_(entry_.data_offset + consumed, std::span<char>(buffer_).first(bytes));
  } else {
    const auto consumed = position_ - 10 - entry_.compressed;
    if (consumed >= 8) return traits_type::eof();
    bytes = static_cast<size_t>(8 - consumed);
    std::copy_n(wrapper_.data() + 10 + consumed, bytes, buffer_.data());
  }
  position_ += bytes; setg(buffer_.data(), buffer_.data(), buffer_.data() + bytes);
  return traits_type::to_int_type(*gptr());
}
}
