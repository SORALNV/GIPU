#include "gipu/stream_worker.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

extern char** environ;
namespace gipu {
namespace {
struct Pipe {
  int fds[2]{-1, -1};
  Pipe() { if (::pipe2(fds, O_CLOEXEC) != 0) throw std::runtime_error("GPU worker用pipeを作成できません"); }
  ~Pipe() { for (int fd : fds) if (fd >= 0) ::close(fd); }
};
struct Duplicate {
  int fd;
  explicit Duplicate(int source) : fd(::fcntl(source, F_DUPFD_CLOEXEC, 10)) {
    if (fd < 0) throw std::runtime_error("GPU worker用FDを複製できません");
  }
  ~Duplicate() { ::close(fd); }
};
void spawn_check(int code) {
  if (code) throw std::runtime_error(std::string("GPU workerを起動できません: ") + std::strerror(code));
}
struct Actions {
  posix_spawn_file_actions_t value;
  Actions() { spawn_check(::posix_spawn_file_actions_init(&value)); }
  ~Actions() { ::posix_spawn_file_actions_destroy(&value); }
};
}
GpuStreamWorker::GpuStreamWorker(const Archive& archive, const Options& opts)
    : timeout_(opts.stream_timeout), vram_limit_(opts.vram_limit) {
  Pipe commands, output;
  // dup先0/1/3と元FDが衝突しないよう、子へ渡す元FDだけ10以上へ複製する。
  Duplicate input(archive.native_handle()), request(commands.fds[0]), response(output.fds[1]);
  Actions actions;
  spawn_check(::posix_spawn_file_actions_adddup2(&actions.value, input.fd, 3));
  spawn_check(::posix_spawn_file_actions_adddup2(&actions.value, request.fd, STDIN_FILENO));
  spawn_check(::posix_spawn_file_actions_adddup2(&actions.value, response.fd, STDOUT_FILENO));
  std::string executable = "/proc/self/exe", command = "__gpu-stream-worker";
  std::string gpu = std::to_string(opts.gpu), parent = std::to_string(::getpid()), vram = std::to_string(opts.vram_limit);
  char* argv[] = {executable.data(), command.data(), gpu.data(), parent.data(), vram.data(), nullptr};
  pid_t child = -1;
  spawn_check(::posix_spawn(&child, executable.c_str(), &actions.value, nullptr, argv, environ));
  pid_ = child;
  commands_ = std::exchange(commands.fds[1], -1);
  output_ = std::exchange(output.fds[0], -1);
}
GpuStreamWorker::~GpuStreamWorker() {
  // workerはアーカイブのread-only FDとpipeだけを持ち、出力パスには触れない。
  // 未回収の自分の子PIDだけを止める。CUDA内部のjoinやリセットには依存しない。
  if (pid_ > 0) {
    ::kill(pid_, SIGKILL);
    while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
  }
  if (commands_ >= 0) ::close(commands_);
  if (output_ >= 0) ::close(output_);
}
uint64_t GpuStreamWorker::decode(const Entry& e, std::ostream& destination) {
  check_cancelled();
  StreamRequest request{stream_request_magic, e.data_offset, e.compressed, e.uncompressed, e.crc};
  // レコードはPIPE_BUFより小さい。SIGPIPEは親CLI側で無視し、EPIPEを通常エラーにする。
  ssize_t written;
  do { written = ::write(commands_, request.data(), sizeof(request)); } while (written < 0 && errno == EINTR);
  if (written != static_cast<ssize_t>(sizeof(request))) throw std::runtime_error("GPU workerへ要求を送れません");
  std::vector<char> buffer(1 << 20);
  using Clock = std::chrono::steady_clock;
  auto deadline = Clock::now() + std::chrono::seconds(timeout_);
  auto receive = [&](char* data, size_t size) {
    for (;;) {
      check_cancelled();
      if (Clock::now() >= deadline) throw std::runtime_error("GPU Streamingの無進捗タイムアウトです");
      pollfd fd{output_, POLLIN, 0};
      int ready = ::poll(&fd, 1, 100);
      if (ready < 0 && errno == EINTR) continue;
      if (ready < 0) throw std::runtime_error("GPU workerの出力を監視できません");
      if (!ready) continue;
      auto n = ::read(output_, data, size);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) throw std::runtime_error("GPU workerが完了前に終了しました（解凍またはI/Oエラー）");
      deadline = Clock::now() + std::chrono::seconds(timeout_);
      return static_cast<size_t>(n);
    }
  };
  uint64_t left = e.uncompressed;
  while (left) {
    size_t n = receive(buffer.data(), static_cast<size_t>(std::min<uint64_t>(left, buffer.size())));
    destination.write(buffer.data(), static_cast<std::streamsize>(n));
    // 親のCRC／filesystem待ちをGPUの無進捗時間へ含めない。
    deadline = Clock::now() + std::chrono::seconds(timeout_);
    left -= n;
  }
  std::array<uint64_t, 2> response{};
  for (size_t offset = 0; offset < sizeof(response);)
    offset += receive(reinterpret_cast<char*>(response.data()) + offset, sizeof(response) - offset);
  if (response[0] != stream_response_magic || response[1] > vram_limit_)
    throw std::runtime_error("GPU workerの完了レコードが不正です");
  return response[1];
}
}
