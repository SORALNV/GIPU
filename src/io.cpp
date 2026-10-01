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
OutputRoot::OutputRoot(const std::filesystem::path& path) {
  std::filesystem::create_directories(path);
  fd_ = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd_ < 0) fail("出力先を開けません");
}
OutputRoot::~OutputRoot() { if (fd_ >= 0) ::close(fd_); }
int OutputRoot::parent(const std::string& name) const {
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
OutputFile::OutputFile(const OutputRoot& root, const Entry& entry, bool durable) : durable_(durable) {
  parent_ = root.parent(entry.name);
  try {
    target_ = entry.name.substr(entry.name.find_last_of('/') + 1);
    struct stat st{};
    if (::fstatat(parent_, target_.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0)
      throw std::runtime_error("既存ファイルは上書きしません: " + entry.name);
    if (errno != ENOENT) fail("出力先を検査できません");
    uint64_t random = 0;
    if (::getrandom(&random, sizeof(random), 0) != sizeof(random)) fail("一時名を生成できません");
    temporary_ = ".gipu-" + std::to_string(random) + ".part";
    fd_ = ::openat(parent_, temporary_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd_ < 0) fail("一時ファイルを作成できません");
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
void OutputFile::commit() {
  if (::fchmod(fd_, 0644) != 0) fail("ファイル権限を設定できません");
  if (durable_ && ::fsync(fd_) != 0) fail("ファイルを同期できません");
  // linkatは既存名を置き換えない。検査から確定までの競合も防ぐ。
  if (::linkat(parent_, temporary_.c_str(), parent_, target_.c_str(), 0) != 0) fail("検証済みファイルを確定できません");
  if (::unlinkat(parent_, temporary_.c_str(), 0) != 0) fail("一時ファイル名を削除できません");
  temporary_.clear();
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
VirtualGzip::VirtualGzip(const Archive& archive, const Entry& entry) : archive_(archive), entry_(entry), buffer_(1 << 20) {
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
    archive_.read(entry_.data_offset + consumed, std::span<char>(buffer_).first(bytes));
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
