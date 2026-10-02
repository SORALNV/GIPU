#include "gipu/backend.hpp"
#include <stdexcept>
namespace gipu {
Stats run_gpu(const Archive&, OutputRoot*, const Options&, EntrySelection) {
  throw std::runtime_error("GPUバックエンドが未ビルドです。GIPU_ENABLE_GPU=ONでビルドしてください");
}
std::string gpu_info(int) { return "GPUバックエンド: 未ビルド"; }
uint64_t gpu_free_memory(int) { return 0; }
}
