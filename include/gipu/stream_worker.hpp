#pragma once
#include "gipu/backend.hpp"
#include <array>

namespace gipu {
inline constexpr uint64_t stream_request_magic = 0x4750495553545231ULL;
inline constexpr uint64_t stream_response_magic = 0x475049555354524fULL;
using StreamRequest = std::array<uint64_t, 5>;
// nvCOMPのpersistent Streaming APIを隔離。出力ファイルを所有するのは親だけ。
class GpuStreamWorker {
 public:
  GpuStreamWorker(const Archive&, const Options&);
  ~GpuStreamWorker();
  GpuStreamWorker(const GpuStreamWorker&) = delete;
  GpuStreamWorker& operator=(const GpuStreamWorker&) = delete;
  void decode(const Entry&, std::ostream&);
 private:
  int pid_ = -1, commands_ = -1, output_ = -1;
  unsigned timeout_ = 120;
};
int gpu_stream_worker_main(int argc, char** argv);
}
