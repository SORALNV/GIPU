#pragma once
#include "gipu/io.hpp"

namespace gipu {
struct Options {
  std::string backend = "gpu";
  std::string gpu_mode = "auto";
  int gpu = 0;
  uint64_t vram_limit = 4ULL << 30;
  uint64_t max_output = 1ULL << 40;
  bool durable = false;
};
struct Stats {
  uint64_t files = 0, bytes = 0, batches = 0, streams = 0, workspace = 0;
};
Stats run_cpu(const Archive&, OutputRoot*, const Options&);
Stats run_gpu(const Archive&, OutputRoot*, const Options&);
std::string gpu_info(int device);
}
