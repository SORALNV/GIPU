#include "gipu/backend.hpp"
#include "gipu/stream_worker.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <sys/stat.h>

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
               "  --backend auto|gpu|cpu|libdeflate|isal|rapidgzip|hybrid  既定auto\n"
               "  --auto-gpu               autoで既知の正しい大規模入力にGPUを許可\n"
               "  --auto-parallel          autoで実験用の単一ファイル並列CPUを許可\n"
               "  --cpu-percent N          hybridのCPU担当バイト割合目安（0〜100、既定50）\n"
               "  --threads N              CPU worker数（auto最大16、hybrid最大8、他1、上限32）\n"
               "  --write-threads N        GPUの1バッチ出力worker数（既定8、最大32）\n"
               "  --batch-entries N        GPUバッチの最大エントリ数（既定4096）\n"
               "  --pipeline               GPUバッチ専用、読み込み・GPU・書き込みを重畳\n"
               "  --gpu N                  CUDAデバイス番号（既定0）\n"
               "  --gpu-mode auto|stream|batch  GPU経路（既定auto）\n"
               "  --gpu-algorithm deflate|lookahead  バッチ方式（既定deflate）\n"
               "  --gpu-order auto|archive 大小混在時のGPU処理順（既定auto）\n"
               "  --gpu-output auto|buffered|stream バッチ出力のホスト転送方式\n"
               "  --stream-crc cpu|gpu     StreamingのCRC（既定cpu、再読み込み不要）\n"
               "  --gpu-crc-chunk 1M|whole バッチCRCの区間サイズ（4K〜64M、既定1M）\n"
               "  --stream-timeout N       GPU Streamingの無進捗timeout秒（既定120）\n"
               "  --vram-limit 4G          GIPUが確保するGPU作業領域の上限\n"
               "  --host-limit 8G          CPU/GPUのホストデータバッファ合計予算\n"
               "  --cpu-buffer-limit 64M   CPU全量バッファの1worker上限（超過はStreaming）\n"
               "  --max-output 1T          合計展開サイズの上限\n"
               "  --metadata-limit 256M    中央ディレクトリのサイズ上限\n"
               "  --metadata-threads auto|N ローカルヘッダ検証worker数（既定auto、最大32）\n"
               "  --sync                   出力ファイルと親ディレクトリをfsync\n"
               "  --temp-mode auto|named   一時出力方式（既定auto、O_TMPFILEを試す）\n"
               "  --path-mode auto|portable 親ディレクトリの安全な探索（既定auto）\n"
               "  --json                   結果をJSONで出力\n";
}
}
int main(int argc, char** argv) {
  try {
    if (argc >= 2 && std::string(argv[1]) == "__gpu-stream-worker") return gipu::gpu_stream_worker_main(argc, argv);
    // ヘッダ検証・CPU/GPU workerの起動前にだけ取得する。途中で権限を緩めない。
    const mode_t process_mask = ::umask(0777);
    ::umask(process_mask);
    const mode_t file_mode = 0644 & ~process_mask;
    gipu::install_signal_handlers();
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") { help(); return 0; }
    if (std::string(argv[1]) == "--version") { std::cout << "GIPU 0.1.0\n"; return 0; }
    const std::string command = argv[1];
    if (command != "doctor" && command != "list" && command != "extract" && command != "test") throw std::runtime_error("不明なコマンドです: " + command);
    gipu::Options opts;
    std::string archive_path, output_path;
    bool json = false, threads_set = false;
    for (int i = 2; i < argc; ++i) {
      const std::string arg = argv[i];
      auto value = [&]() -> std::string { if (++i == argc) throw std::runtime_error(arg + "の値がありません"); return argv[i]; };
      if (arg == "--output" || arg == "-o") output_path = value();
      else if (arg == "--backend") opts.backend = value();
      else if (arg == "--gpu-mode") opts.gpu_mode = value();
      else if (arg == "--gpu-algorithm") opts.gpu_algorithm = value();
      else if (arg == "--gpu-order") opts.gpu_order = value();
      else if (arg == "--gpu-output") opts.gpu_output = value();
      else if (arg == "--cpu-percent") {
        auto text = value(); size_t used = 0;
        if (text.empty() || text.front() < '0' || text.front() > '9') throw std::runtime_error("CPU割合が不正です");
        auto percent = std::stoul(text, &used);
        if (used != text.size() || percent > 100) throw std::runtime_error("CPU割合は0〜100です");
        opts.cpu_percent = static_cast<unsigned>(percent);
      }
      else if (arg == "--stream-crc") opts.stream_crc = value();
      else if (arg == "--gpu-crc-chunk") {
        auto text = value();
        opts.gpu_crc_chunk = text == "whole" ? 0 : size_value(text);
        if (opts.gpu_crc_chunk && (opts.gpu_crc_chunk < 4096 || opts.gpu_crc_chunk > (64ULL << 20)))
          throw std::runtime_error("gpu-crc-chunkはwholeまたは4K〜64Mです");
      }
      else if (arg == "--stream-timeout") {
        auto text = value(); size_t used = 0;
        auto seconds = std::stoul(text, &used);
        if (used != text.size() || text.empty() || text.front() < '0' || text.front() > '9' || seconds == 0 || seconds > 86400)
          throw std::runtime_error("stream-timeoutは1〜86400秒です");
        opts.stream_timeout = static_cast<unsigned>(seconds);
      }
      else if (arg == "--temp-mode") opts.temp_mode = value();
      else if (arg == "--path-mode") opts.path_mode = value();
      else if (arg == "--metadata-threads") {
        auto text = value();
        if (text == "auto") opts.metadata_threads = 0;
        else {
          size_t used = 0;
          auto count = std::stoull(text, &used);
          if (used != text.size() || text.empty() || text.front() < '0' || text.front() > '9' || count == 0 || count > 32)
            throw std::runtime_error("metadata-threadsはautoまたは1〜32です");
          opts.metadata_threads = static_cast<size_t>(count);
        }
      }
      else if (arg == "--threads" || arg == "--write-threads" || arg == "--batch-entries") {
        auto text = value(); size_t used = 0;
        if (text.empty() || text.front() < '0' || text.front() > '9') throw std::runtime_error("個数が不正です");
        auto count = std::stoull(text, &used);
        if (used != text.size() || count == 0 || count > (arg == "--batch-entries" ? 65536ULL : 32ULL)) throw std::runtime_error("個数の範囲が不正です");
        if (arg == "--threads") { opts.threads = static_cast<size_t>(count); threads_set = true; }
        else if (arg == "--write-threads") opts.write_threads = static_cast<size_t>(count);
        else opts.batch_entries = static_cast<size_t>(count);
      }
      else if (arg == "--gpu") {
        auto text = value(); size_t used = 0; opts.gpu = std::stoi(text, &used);
        if (used != text.size() || opts.gpu < 0) throw std::runtime_error("GPU番号が不正です");
      } else if (arg == "--vram-limit") opts.vram_limit = size_value(value());
      else if (arg == "--host-limit") opts.host_limit = size_value(value());
      else if (arg == "--cpu-buffer-limit") opts.cpu_buffer_limit = size_value(value());
      else if (arg == "--max-output") opts.max_output = size_value(value());
      else if (arg == "--metadata-limit") opts.metadata_limit = size_value(value());
      else if (arg == "--sync") opts.durable = true;
      else if (arg == "--pipeline") opts.pipeline = true;
      else if (arg == "--auto-gpu") opts.auto_gpu = true;
      else if (arg == "--auto-parallel") opts.auto_parallel = true;
      else if (arg == "--json") json = true;
      else if (!arg.empty() && arg.front() == '-') throw std::runtime_error("不明なオプションです: " + arg);
      else if (archive_path.empty()) archive_path = arg;
      else throw std::runtime_error("位置引数が多すぎます");
    }
    if (opts.backend != "auto" && opts.backend != "gpu" && opts.backend != "cpu" && opts.backend != "libdeflate" && opts.backend != "isal" && opts.backend != "rapidgzip" && opts.backend != "hybrid") throw std::runtime_error("backendはauto/gpu/cpu/libdeflate/isal/rapidgzip/hybridです");
    // 従来の「--pipelineだけ」の呼び出しはGPUの明示指定として互換性を保つ。
    if (opts.pipeline && opts.backend == "auto") opts.backend = "gpu";
    if (!threads_set && (opts.backend == "auto" || opts.backend == "hybrid"))
      opts.threads = std::min<size_t>(opts.backend == "auto" ? 16 : 8, gipu::available_cpu_threads());
    if (opts.gpu_mode != "auto" && opts.gpu_mode != "stream" && opts.gpu_mode != "batch") throw std::runtime_error("gpu-modeはauto/stream/batchです");
    if (opts.gpu_algorithm != "deflate" && opts.gpu_algorithm != "lookahead") throw std::runtime_error("gpu-algorithmはdeflate/lookaheadです");
    if (opts.gpu_order != "auto" && opts.gpu_order != "archive") throw std::runtime_error("gpu-orderはauto/archiveです");
    if (opts.gpu_output != "auto" && opts.gpu_output != "buffered" && opts.gpu_output != "stream")
      throw std::runtime_error("gpu-outputはauto/buffered/streamです");
    if (opts.gpu_output == "stream" && (opts.pipeline || opts.backend == "hybrid" || opts.auto_gpu))
      throw std::runtime_error("gpu-output streamは通常GPUバッチ専用です。pipeline/hybrid/auto-gpuにはautoまたはbufferedを使ってください");
    if (opts.stream_crc != "cpu" && opts.stream_crc != "gpu") throw std::runtime_error("stream-crcはcpu/gpuです");
    if (opts.temp_mode != "named" && opts.temp_mode != "auto") throw std::runtime_error("temp-modeはnamed/autoです");
    if (opts.path_mode != "auto" && opts.path_mode != "portable") throw std::runtime_error("path-modeはauto/portableです");
    if (opts.pipeline && ((opts.backend != "gpu" && opts.backend != "hybrid") || opts.gpu_mode == "stream")) throw std::runtime_error("pipelineはGPU/hybrid auto/batch専用です");
    if (command == "doctor") { std::cout << gipu::gpu_info(opts.gpu) << '\n'; return 0; }
    if (archive_path.empty()) throw std::runtime_error("ZIPファイルを指定してください");
    const auto start = std::chrono::steady_clock::now();
    gipu::Archive archive(archive_path, opts.metadata_limit, opts.metadata_threads);
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
    if (command == "extract") root = std::make_unique<gipu::OutputRoot>(output_path, file_mode,
        opts.temp_mode == "auto", opts.path_mode == "auto", opts.durable);
    auto stats = opts.backend == "auto" ? gipu::run_auto(archive, root.get(), opts) :
        (opts.backend == "cpu" || opts.backend == "isal") ? gipu::run_cpu(archive, root.get(), opts) :
        opts.backend == "hybrid" ? gipu::run_hybrid(archive, root.get(), opts) :
        opts.backend == "rapidgzip" ? gipu::run_rapidgzip(archive, root.get(), opts) :
        opts.backend == "libdeflate" ? gipu::run_libdeflate(archive, root.get(), opts) : gipu::run_gpu(archive, root.get(), opts);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    double throughput = static_cast<double>(stats.bytes) / (1ULL << 30) / seconds;
    if (json) std::cout << "{\"backend\":\"" << opts.backend << "\",\"files\":" << stats.files << ",\"bytes\":" << stats.bytes
                        << ",\"seconds\":" << seconds << ",\"gib_per_second\":" << throughput << ",\"gpu_batches\":" << stats.batches
                        << ",\"gpu_streams\":" << stats.streams << ",\"workspace_bytes\":" << stats.workspace
                        << ",\"parse_seconds\":" << std::chrono::duration<double>(parsed - start).count()
                        << ",\"metadata_threads\":" << archive.metadata_threads()
                        << ",\"read_seconds\":" << stats.read_seconds << ",\"write_seconds\":" << stats.write_seconds
                        << ",\"decode_seconds\":" << stats.decode_seconds << ",\"crc_seconds\":" << stats.crc_seconds
                        << ",\"transfer_seconds\":" << stats.transfer_seconds
                        << ",\"cpu_crc_bytes\":" << stats.cpu_crc_bytes << ",\"gpu_crc_bytes\":" << stats.gpu_crc_bytes
                        << ",\"lookahead_batches\":" << stats.lookahead_batches
                        << ",\"pipeline_overlap_waits\":" << stats.pipeline_overlap_waits
                        << ",\"gpu_stream_workers\":" << stats.gpu_stream_workers
                        << ",\"gpu_crc_chunks\":" << stats.gpu_crc_chunks
                        << ",\"gpu_size_reorders\":" << stats.gpu_size_reorders
                        << ",\"gpu_streamed_output_bytes\":" << stats.gpu_streamed_output_bytes
                        << ",\"crc_combine_seconds\":" << stats.crc_combine_seconds
                        << ",\"host_buffer_bytes\":" << stats.host_buffer_bytes
                        << ",\"cpu_buffered_files\":" << stats.cpu_buffered_files << ",\"cpu_stream_files\":" << stats.cpu_stream_files
                        << ",\"cpu_parallel_files\":" << stats.cpu_parallel_files
                        << ",\"isal_files\":" << stats.isal_files
                        << ",\"selected_backend\":\"" << (stats.selected_backend.empty() ? opts.backend : stats.selected_backend)
                        << "\",\"selection_reason\":\"" << stats.selection_reason << "\""
                        << ",\"anonymous_output_files\":" << (root ? root->anonymous_files() : 0)
                        << ",\"named_output_files\":" << (root ? root->named_files() : 0)
                        << ",\"fast_parent_opens\":" << (root ? root->fast_parent_opens() : 0)
                        << ",\"portable_parent_walks\":" << (root ? root->portable_parent_walks() : 0)
                        << ",\"allocation_seconds\":" << stats.allocation_seconds << "}\n";
    else std::cout << "完了: " << stats.files << "ファイル / " << stats.bytes << " bytes / " << seconds << "秒 / " << throughput
                   << " GiB/s（" << (stats.selected_backend.empty() ? opts.backend : stats.selected_backend)
                   << ", batch=" << stats.batches << ", stream=" << stats.streams << "）\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "gipu: " << error.what() << '\n'; return 1; }
}
