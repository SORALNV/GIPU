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
};
Stats run_cpu(const Archive&, OutputRoot*, const Options&);
Stats run_cpu_entry(const Archive&, const Entry&, OutputRoot*, const Options&, bool fast_checksum);
bool isal_available();
Stats run_libdeflate(const Archive&, OutputRoot*, const Options&);
Stats run_rapidgzip(const Archive&, OutputRoot*, const Options&);
Stats run_gpu(const Archive&, OutputRoot*, const Options&);
std::string gpu_info(int device);
}
