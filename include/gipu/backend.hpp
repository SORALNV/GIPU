#pragma once
#include "gipu/io.hpp"

namespace gipu {
struct Options {
  std::string backend = "gpu";
  std::string gpu_mode = "auto";
  std::string gpu_algorithm = "deflate";
  std::string stream_crc = "cpu";
  int gpu = 0;
  size_t threads = 1;
  size_t write_threads = 8;
  size_t batch_entries = 4096;
  unsigned cpu_percent = 50;
  uint64_t vram_limit = 4ULL << 30;
  uint64_t host_limit = 8ULL << 30;
  uint64_t max_output = 1ULL << 40;
  uint64_t metadata_limit = 256ULL << 20;
  bool durable = false;
  bool pipeline = false;
};
struct Stats {
  uint64_t files = 0, bytes = 0, batches = 0, streams = 0, workspace = 0;
  uint64_t cpu_crc_bytes = 0, gpu_crc_bytes = 0;
  uint64_t lookahead_batches = 0;
  uint64_t host_buffer_bytes = 0, cpu_buffered_files = 0, cpu_stream_files = 0;
  uint64_t cpu_parallel_files = 0;
  uint64_t isal_files = 0;
  double read_seconds = 0, write_seconds = 0, decode_seconds = 0, crc_seconds = 0, transfer_seconds = 0;
  double allocation_seconds = 0;
  std::string selected_backend, selection_reason;
};
using EntrySelection = std::span<const Entry* const>;
inline std::vector<const Entry*> all_entries(const Archive& archive) {
  std::vector<const Entry*> entries;
  entries.reserve(archive.entries().size());
  for (const auto& e : archive.entries()) entries.push_back(&e);
  return entries;
}
// 同じ検証済みArchiveからCPU/GPUへ別々のエントリ集合を渡せる。
Stats run_cpu(const Archive&, OutputRoot*, const Options&, EntrySelection);
Stats run_cpu_entry(const Archive&, const Entry&, OutputRoot*, const Options&, bool fast_checksum);
bool isal_available();
bool libdeflate_available();
Stats run_libdeflate(const Archive&, OutputRoot*, const Options&, EntrySelection);
Stats run_rapidgzip(const Archive&, OutputRoot*, const Options&, EntrySelection);
Stats run_gpu(const Archive&, OutputRoot*, const Options&, EntrySelection);
inline Stats run_cpu(const Archive& a, OutputRoot* r, const Options& o) {
  auto entries = all_entries(a); return run_cpu(a, r, o, entries);
}
inline Stats run_libdeflate(const Archive& a, OutputRoot* r, const Options& o) {
  auto entries = all_entries(a); return run_libdeflate(a, r, o, entries);
}
inline Stats run_rapidgzip(const Archive& a, OutputRoot* r, const Options& o) {
  auto entries = all_entries(a); return run_rapidgzip(a, r, o, entries);
}
inline Stats run_gpu(const Archive& a, OutputRoot* r, const Options& o) {
  auto entries = all_entries(a); return run_gpu(a, r, o, entries);
}
void add_stats(Stats& total, const Stats& other, bool concurrent = false);
Stats run_hybrid(const Archive&, OutputRoot*, const Options&);
uint64_t gpu_free_memory(int device);
std::string gpu_info(int device);
}
