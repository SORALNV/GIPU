#pragma once
#include "gipu/zip.hpp"
#include <array>
#include <functional>
#include <memory>
#include <ostream>
#include <streambuf>

namespace gipu {
class OutputRoot {
 public:
  explicit OutputRoot(const std::filesystem::path& path);
  ~OutputRoot();
  OutputRoot(const OutputRoot&) = delete;
  OutputRoot& operator=(const OutputRoot&) = delete;
  int parent(const std::string& name) const;
  void directory(const std::string& name) const;
 private:
  int fd_ = -1;
};
// 同一ディレクトリ内に一時ファイルを作り、検証後だけ確定する。
class OutputFile {
 public:
  OutputFile(); // testコマンド用の、名前を持たない一時出力。
  OutputFile(const OutputRoot& root, const Entry& entry, bool durable);
  ~OutputFile();
  OutputFile(const OutputFile&) = delete;
  OutputFile& operator=(const OutputFile&) = delete;
  void write(std::span<const char> bytes);
  void read_all(const std::function<void(std::span<const char>)>& consume) const;
  void commit();
 private:
  int parent_ = -1, fd_ = -1;
  std::string target_, temporary_;
  bool durable_ = false;
};
class Sink : public std::streambuf {
 public:
  Sink(uint64_t expected, OutputFile* file, std::function<void(std::span<const char>)> checksum);
  uint64_t bytes() const { return written_; }
  void finish() const;
 protected:
  std::streamsize xsputn(const char* data, std::streamsize size) override;
  int_type overflow(int_type c) override;
 private:
  uint64_t expected_, written_ = 0;
  OutputFile* file_;
  std::function<void(std::span<const char>)> checksum_;
};
class VirtualGzip : public std::streambuf {
 public:
  VirtualGzip(const Archive& archive, const Entry& entry);
 protected:
  int_type underflow() override;
 private:
  const Archive& archive_;
  const Entry& entry_;
  std::vector<char> buffer_;
  std::array<char, 18> wrapper_{};
  uint64_t position_ = 0;
};
}
