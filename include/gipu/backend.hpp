#pragma once
#include "gipu/io.hpp"

namespace gipu {
struct Options {
  std::string backend = "gpu";
  std::string gpu_mode = "auto";
  int gpu = 0;
  size_t threads = 1;
  size_t batch_entries = 4096;
  uint64_t vram_limit = 4ULL << 30;
  uint64_t max_output = 1ULL << 40;
  bool durable = false;
};
struct Stats {
  uint64_t files = 0, bytes = 0, batches = 0, streams = 0, workspace = 0;
  double read_seconds = 0, write_seconds = 0, decode_seconds = 0, crc_seconds = 0, transfer_seconds = 0;
};
Stats run_cpu(const Archive&, OutputRoot*, const Options&);
Stats run_libdeflate(const Archive&, OutputRoot*, const Options&);
Stats run_gpu(const Archive&, OutputRoot*, const Options&);
std::string gpu_info(int device);
}
