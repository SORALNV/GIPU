#include "gipu/backend.hpp"
#include <algorithm>
#include <sched.h>
#include <thread>

namespace gipu {
size_t available_cpu_threads() {
  cpu_set_t affinity;
  CPU_ZERO(&affinity);
  if (::sched_getaffinity(0, sizeof(affinity), &affinity) == 0)
    return std::max<size_t>(1, static_cast<size_t>(CPU_COUNT(&affinity)));
  return std::max(1U, std::thread::hardware_concurrency());
}
namespace {
Stats adaptive_cpu(const Archive& archive, OutputRoot* root, Options opts, EntrySelection entries) {
  Stats result;
  if (libdeflate_available()) {
    result = run_libdeflate(archive, root, opts, entries);
    result.selected_backend = "libdeflate";
  } else {
    opts.backend = isal_available() ? "isal" : "cpu";
    result = run_cpu(archive, root, opts, entries);
    result.selected_backend = opts.backend;
  }
  return result;
}
bool gpu_candidate(const Entry& e) {
  return !e.directory && e.method == 8 && e.uncompressed >= (32ULL << 10) &&
         e.uncompressed <= (64ULL << 20) && e.compressed < e.uncompressed - e.uncompressed / 50;
}
bool parallel_candidate(const Entry& e) {
  // 高圧縮率は探索と巨大な履歴が不利、ほぼ非圧縮はStreamingの方が安い。
  return !e.directory && e.method == 8 && e.uncompressed >= (256ULL << 20) &&
         e.compressed >= e.uncompressed / 8 && e.compressed < e.uncompressed - e.uncompressed / 50;
}
}
Stats run_auto(const Archive& archive, OutputRoot* root, const Options& opts) {
  auto all = all_entries(archive);
  size_t candidates = 0;
  uint64_t gpu_bytes = 0;
  for (const auto* e : all) if (gpu_candidate(*e)) { ++candidates; gpu_bytes += e->uncompressed; }
  // GPU初期化・固定化メモリ確保を小入力では一切行わない。
  // 明示許可した既知の正しい入力だけ。testでは並列CPUの方が速かったため選ばない。
  if (opts.auto_gpu && root && candidates >= 4096 && gpu_bytes >= (16ULL << 30) &&
      gpu_bytes >= archive.total_size() - archive.total_size() / 5) {
    Options mixed = opts;
    mixed.cpu_percent = 0;
    auto result = run_hybrid(archive, root, mixed);
    result.selection_reason = "auto_large_extract_" + result.selection_reason;
    return result;
  }
  std::vector<const Entry*> ordinary, parallel;
  if (opts.auto_parallel && rapidgzip_available() && opts.threads > 1 && opts.host_limit >= (1ULL << 30)) {
    for (const auto* e : all) (parallel_candidate(*e) ? parallel : ordinary).push_back(e);
    // 多数の独立ファイルは通常のエントリ並列の方が効率的。内部並列と二重起動しない。
    if (parallel.size() >= opts.threads) parallel.clear();
  }
  if (!parallel.empty()) {
    Stats result = run_rapidgzip(archive, root, opts, parallel);
    if (!ordinary.empty()) add_stats(result, adaptive_cpu(archive, root, opts, ordinary));
    result.selected_backend = ordinary.empty() ? "rapidgzip" : "cpu-adaptive";
    result.selection_reason = "auto_parallel_large";
    return result;
  }
  auto result = adaptive_cpu(archive, root, opts, all);
  result.selection_reason = opts.auto_parallel && !rapidgzip_available() ? "auto_parallel_unavailable" :
                            root ? "auto_cpu_extract" : "auto_cpu_test";
  return result;
}
}
