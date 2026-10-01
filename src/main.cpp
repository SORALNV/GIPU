#include "gipu/backend.hpp"
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
uint64_t size_value(const std::string& text) {
  size_t used = 0;
  if (text.empty() || text.front() < '0' || text.front() > '9') throw std::runtime_error("サイズの指定が不正です");
  uint64_t n = std::stoull(text, &used), multiplier = 1;
  auto suffix = text.substr(used);
  if (suffix == "K" || suffix == "KiB") multiplier = 1ULL << 10;
  else if (suffix == "M" || suffix == "MiB") multiplier = 1ULL << 20;
  else if (suffix == "G" || suffix == "GiB") multiplier = 1ULL << 30;
  else if (suffix == "T" || suffix == "TiB") multiplier = 1ULL << 40;
  else if (!suffix.empty()) throw std::runtime_error("サイズの単位はK/M/G/TまたはKiB/MiB/GiB/TiBです");
  if (n == 0 || n > std::numeric_limits<uint64_t>::max() / multiplier) throw std::runtime_error("サイズの範囲が不正です");
  return n * multiplier;
}
void help() {
  std::cout << "GIPU 0.1.0 — Ubuntu向けGPU ZIP解凍ツール\n"
               "使い方:\n"
               "  gipu doctor [--gpu N]\n"
               "  gipu list ARCHIVE.zip\n"
               "  gipu extract ARCHIVE.zip --output DIR [オプション]\n"
               "  gipu test ARCHIVE.zip [オプション]\n"
               "オプション:\n"
               "  --backend gpu|cpu|libdeflate  既定gpu。CPUは比較・検証用\n"
               "  --threads N              libdeflateのCPU worker数（既定1、最大32）\n"
               "  --batch-entries N        GPUバッチの最大エントリ数（既定4096）\n"
               "  --pipeline               GPUバッチ専用、読み込み・GPU・書き込みを重畳\n"
               "  --gpu N                  CUDAデバイス番号（既定0）\n"
               "  --gpu-mode auto|stream|batch  GPU経路（既定auto）\n"
               "  --vram-limit 4G          GIPUが確保するGPU作業領域の上限\n"
               "  --max-output 1T          合計展開サイズの上限\n"
               "  --sync                   出力ファイルと親ディレクトリをfsync\n"
               "  --json                   結果をJSONで出力\n";
}
}
int main(int argc, char** argv) {
  try {
    gipu::install_signal_handlers();
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") { help(); return 0; }
    if (std::string(argv[1]) == "--version") { std::cout << "GIPU 0.1.0\n"; return 0; }
    const std::string command = argv[1];
    if (command != "doctor" && command != "list" && command != "extract" && command != "test") throw std::runtime_error("不明なコマンドです: " + command);
    gipu::Options opts;
    std::string archive_path, output_path;
    bool json = false;
    for (int i = 2; i < argc; ++i) {
      const std::string arg = argv[i];
      auto value = [&]() -> std::string { if (++i == argc) throw std::runtime_error(arg + "の値がありません"); return argv[i]; };
      if (arg == "--output" || arg == "-o") output_path = value();
      else if (arg == "--backend") opts.backend = value();
      else if (arg == "--gpu-mode") opts.gpu_mode = value();
      else if (arg == "--threads" || arg == "--batch-entries") {
        auto text = value(); size_t used = 0;
        if (text.empty() || text.front() < '0' || text.front() > '9') throw std::runtime_error("個数が不正です");
        auto count = std::stoull(text, &used);
        if (used != text.size() || count == 0 || count > (arg == "--threads" ? 32ULL : 65536ULL)) throw std::runtime_error("個数の範囲が不正です");
        if (arg == "--threads") opts.threads = static_cast<size_t>(count);
        else opts.batch_entries = static_cast<size_t>(count);
      }
      else if (arg == "--gpu") {
        auto text = value(); size_t used = 0; opts.gpu = std::stoi(text, &used);
        if (used != text.size() || opts.gpu < 0) throw std::runtime_error("GPU番号が不正です");
      } else if (arg == "--vram-limit") opts.vram_limit = size_value(value());
      else if (arg == "--max-output") opts.max_output = size_value(value());
      else if (arg == "--sync") opts.durable = true;
      else if (arg == "--pipeline") opts.pipeline = true;
      else if (arg == "--json") json = true;
      else if (!arg.empty() && arg.front() == '-') throw std::runtime_error("不明なオプションです: " + arg);
      else if (archive_path.empty()) archive_path = arg;
      else throw std::runtime_error("位置引数が多すぎます");
    }
    if (opts.backend != "gpu" && opts.backend != "cpu" && opts.backend != "libdeflate") throw std::runtime_error("backendはgpu/cpu/libdeflateです");
    if (opts.gpu_mode != "auto" && opts.gpu_mode != "stream" && opts.gpu_mode != "batch") throw std::runtime_error("gpu-modeはauto/stream/batchです");
    if (opts.pipeline && (opts.backend != "gpu" || opts.gpu_mode == "stream")) throw std::runtime_error("pipelineはGPU auto/batch専用です");
    if (command == "doctor") { std::cout << gipu::gpu_info(opts.gpu) << '\n'; return 0; }
    if (archive_path.empty()) throw std::runtime_error("ZIPファイルを指定してください");
    const auto start = std::chrono::steady_clock::now();
    gipu::Archive archive(archive_path);
    const auto parsed = std::chrono::steady_clock::now();
    if (command == "list") {
      for (const auto& e : archive.entries()) std::cout << e.uncompressed << '\t' << e.compressed << '\t' << e.method << '\t' << e.name << '\n';
      return 0;
    }
    if (archive.total_size() > opts.max_output) throw std::runtime_error("展開サイズが--max-outputを超えています");
    for (const auto& e : archive.entries()) if (e.method != 0 && e.method != 8) throw std::runtime_error("未対応の圧縮方式です: " + std::to_string(e.method));
    if (command == "extract" && output_path.empty()) throw std::runtime_error("--outputで展開先を指定してください");
    if (command == "test" && !output_path.empty()) throw std::runtime_error("testには--outputを指定できません");
    std::unique_ptr<gipu::OutputRoot> root;
    if (command == "extract") root = std::make_unique<gipu::OutputRoot>(output_path);
    auto stats = opts.backend == "cpu" ? gipu::run_cpu(archive, root.get(), opts) :
        opts.backend == "libdeflate" ? gipu::run_libdeflate(archive, root.get(), opts) : gipu::run_gpu(archive, root.get(), opts);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    double throughput = static_cast<double>(stats.bytes) / (1ULL << 30) / seconds;
    if (json) std::cout << "{\"backend\":\"" << opts.backend << "\",\"files\":" << stats.files << ",\"bytes\":" << stats.bytes
                        << ",\"seconds\":" << seconds << ",\"gib_per_second\":" << throughput << ",\"gpu_batches\":" << stats.batches
                        << ",\"gpu_streams\":" << stats.streams << ",\"workspace_bytes\":" << stats.workspace
                        << ",\"parse_seconds\":" << std::chrono::duration<double>(parsed - start).count()
                        << ",\"read_seconds\":" << stats.read_seconds << ",\"write_seconds\":" << stats.write_seconds
                        << ",\"decode_seconds\":" << stats.decode_seconds << ",\"crc_seconds\":" << stats.crc_seconds
                        << ",\"transfer_seconds\":" << stats.transfer_seconds << "}\n";
    else std::cout << "完了: " << stats.files << "ファイル / " << stats.bytes << " bytes / " << seconds << "秒 / " << throughput
                   << " GiB/s（" << opts.backend << ", batch=" << stats.batches << ", stream=" << stats.streams << "）\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "gipu: " << error.what() << '\n'; return 1; }
}
