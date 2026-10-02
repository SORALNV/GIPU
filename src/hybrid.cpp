#include "gipu/backend.hpp"
#include <algorithm>
#include <future>
#include <mutex>

namespace gipu {
namespace {
Stats cpu_run(const Archive& archive, OutputRoot* root, Options opts, EntrySelection entries) {
  if (libdeflate_available()) return run_libdeflate(archive, root, opts, entries);
  opts.backend = isal_available() ? "isal" : "cpu";
  return run_cpu(archive, root, opts, entries);
}
bool prefer_cpu(const Entry& e) {
  // 多数の短い起動、全量バッファの巨大化、ほぼ非圧縮データのPCIe往復を避ける。
  return e.directory || e.method == 0 || e.uncompressed < (32ULL << 10) || e.uncompressed > (64ULL << 20) ||
         e.compressed >= e.uncompressed - e.uncompressed / 50;
}
}
Stats run_hybrid(const Archive& archive, OutputRoot* root, const Options& opts) {
  auto all = all_entries(archive);
  uint64_t cpu_bytes = 0;
  for (const auto* e : all) if (prefer_cpu(*e)) cpu_bytes += e->uncompressed;
  const auto total = archive.total_size();
  const uint64_t target = (total / 100) * opts.cpu_percent + (total % 100) * opts.cpu_percent / 100;
  std::vector<const Entry*> cpu, gpu;
  for (const auto* e : all) {
    if (prefer_cpu(*e)) cpu.push_back(e);
    else if (cpu_bytes < target) { cpu.push_back(e); cpu_bytes += e->uncompressed; }
    else gpu.push_back(e);
  }
  auto only_cpu = [&](const char* reason) {
    auto stats = cpu_run(archive, root, opts, all);
    stats.selected_backend = libdeflate_available() ? "libdeflate" : isal_available() ? "isal" : "cpu";
    stats.selection_reason = reason; return stats;
  };
  if (gpu.empty()) return only_cpu("cpu_suitable_entries");
  if (opts.host_limit < (64ULL << 20) || opts.vram_limit < (32ULL << 20)) return only_cpu("small_memory_budget");
  auto free = gpu_free_memory(opts.gpu);
  if (free < (64ULL << 20)) return only_cpu("gpu_unavailable");
  Options cpu_opts = opts, gpu_opts = opts;
  cpu_opts.host_limit = cpu.empty() ? 0 : opts.host_limit / 4;
  gpu_opts.host_limit = opts.host_limit - cpu_opts.host_limit;
  gpu_opts.vram_limit = std::min(opts.vram_limit, free - free / 4);
  gpu_opts.pipeline = opts.gpu_mode != "stream";
  // 最大input/outputが別バッチになる最悪ケースでも、二組のホストバッファを予算化。
  if (gpu_opts.pipeline) gpu_opts.vram_limit = std::min<uint64_t>(gpu_opts.vram_limit, (gpu_opts.host_limit - (12ULL << 20)) / 4);
  if (gpu_opts.vram_limit < (16ULL << 20)) return only_cpu("small_memory_budget");
  std::mutex lock;
  std::exception_ptr first_error;
  auto guarded = [&](auto work) {
    try { return work(); }
    catch (...) {
      { std::lock_guard guard(lock); if (!first_error) first_error = std::current_exception(); }
      request_cancel(); throw;
    }
  };
  std::future<Stats> cpu_job;
  if (!cpu.empty()) cpu_job = std::async(std::launch::async, [&] {
    return guarded([&] { return cpu_run(archive, root, cpu_opts, cpu); });
  });
  Stats result;
  try { result = guarded([&] { return run_gpu(archive, root, gpu_opts, gpu); }); }
  catch (...) {} // 両経路をjoinし、最初の根本エラーを再送出する。
  if (cpu_job.valid()) {
    try { add_stats(result, cpu_job.get(), true); }
    catch (...) {}
  }
  if (first_error) std::rethrow_exception(first_error);
  result.selected_backend = cpu.empty() ? "gpu" : "hybrid";
  result.selection_reason = "entry_partition";
  return result;
}
}
