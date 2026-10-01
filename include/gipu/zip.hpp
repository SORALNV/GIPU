#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace gipu {
struct Entry {
  std::string name;
  std::string raw_name;
  uint16_t method = 0;
  uint16_t flags = 0;
  uint32_t crc = 0;
  uint64_t compressed = 0;
  uint64_t uncompressed = 0;
  uint64_t local_offset = 0;
  uint64_t data_offset = 0;
  bool directory = false;
};
class Archive {
 public:
  explicit Archive(const std::filesystem::path& path);
  ~Archive();
  Archive(const Archive&) = delete;
  Archive& operator=(const Archive&) = delete;
  void read(uint64_t offset, std::span<char> buffer) const;
  const std::vector<Entry>& entries() const { return entries_; }
  uint64_t total_size() const;
  uint64_t file_size() const { return size_; }
 private:
  int fd_ = -1;
  uint64_t size_ = 0;
  std::vector<Entry> entries_;
  void parse();
};
void check_cancelled();
void install_signal_handlers();
}
