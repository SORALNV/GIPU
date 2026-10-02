"""所有する解凍子プロセスだけへsignalを送り、未検証出力の確定と残骸を検査する。"""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import zipfile


def progress(process, destination):
    """自分の子プロセスが保持する出力FDを観察。名前なし一時出力も検査できる。"""
    directory = Path(f"/proc/{process.pid}/fd")
    try:
        for fd in directory.iterdir():
            try:
                if str(destination) in os.readlink(fd) and fd.stat().st_size >= (1 << 20):
                    return True
            except (FileNotFoundError, PermissionError):
                pass
    except FileNotFoundError:
        pass
    return False


def child_states(pid):
    result = {}
    try:
        children = Path(f"/proc/{pid}/task/{pid}/children").read_text().split()
    except FileNotFoundError:
        return result
    for child in children:
        try:
            fields = Path(f"/proc/{child}/stat").read_text().rsplit(")", 1)[1].split()
            result[int(child)] = (fields[0], fields[19])
        except FileNotFoundError:
            pass
    return result


def child_running(pid, start_time):
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        return fields[19] == start_time and fields[0] != "Z"
    except FileNotFoundError:
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--backend", choices=("cpu", "isal", "libdeflate", "rapidgzip", "gpu", "auto"), required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument("--worker-faults", action="store_true")
    parser.add_argument("--gpu-mode", choices=("auto", "stream", "batch"), default="stream")
    parser.add_argument("--gpu-output", choices=("auto", "buffered", "stream"), default="auto")
    args = parser.parse_args()
    if args.timeout <= 0 or args.report.exists():
        parser.error("正のtimeoutと未使用reportが必要です")
    if args.worker_faults and args.gpu_mode == "batch":
        parser.error("worker-faultsはGPU Streaming用です")
    with zipfile.ZipFile(args.archive) as archive:
        files = [i for i in archive.infolist() if not i.is_dir()]
        if len(files) != 1 or files[0].file_size < (256 << 20):
            parser.error("256MiB以上の単一ファイルZIPが必要です")
        member = files[0].filename
    args.output_root.mkdir(parents=True, exist_ok=True)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    failed = False
    with args.report.open("x", encoding="utf-8", buffering=1) as report:
        for mode in ("named", "auto"):
            for stop in (signal.SIGINT, signal.SIGTERM, signal.SIGKILL):
                with tempfile.TemporaryDirectory(prefix="gipu-cancel-", dir=args.output_root) as temp:
                    destination = Path(temp) / "out"
                    command = [str(args.binary.resolve()), "extract", str(args.archive.resolve()),
                               "--backend", args.backend, "--threads", "4", "--gpu-mode", args.gpu_mode,
                               "--gpu-output", args.gpu_output,
                               "--temp-mode", mode, "--output", str(destination), "--json"]
                    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    try:
                        deadline = time.monotonic() + args.timeout
                        while process.poll() is None and not progress(process, destination) and time.monotonic() < deadline:
                            time.sleep(0.005)
                        if process.poll() is not None or not progress(process, destination):
                            raise RuntimeError("signal前に出力中の状態を確認できませんでした")
                        started = time.monotonic()
                        children = child_states(process.pid)
                        process.send_signal(stop)
                        timeout = False
                        try:
                            stdout, stderr = process.communicate(timeout=args.timeout)
                        except subprocess.TimeoutExpired:
                            timeout = True
                            process.kill()
                            stdout, stderr = process.communicate()
                        parts = list(destination.rglob("*.part"))
                        committed = (destination / member).exists()
                        # SIGKILL＋namedでは残骸が残り得る。親のTemporaryDirectoryがこの試験分だけ回収する。
                        okay = process.returncode != 0 and not timeout and not committed
                        if stop != signal.SIGKILL:
                            okay = okay and not parts
                        deadline = time.monotonic() + 2
                        while any(child_running(pid, state[1]) for pid, state in children.items()) and time.monotonic() < deadline:
                            time.sleep(0.01)
                        live_children = sum(child_running(pid, state[1]) for pid, state in children.items())
                        okay = okay and not live_children
                        row = {"backend": args.backend, "mode": mode, "signal": stop.name,
                               "returncode": process.returncode, "seconds_after_signal": time.monotonic() - started,
                               "timeout": timeout, "committed": committed, "remaining_parts": len(parts),
                               "remaining_workers": live_children, "passed": okay, "stderr": stderr[-1000:]}
                        report.write(json.dumps(row, ensure_ascii=False) + "\n")
                        print(json.dumps(row, ensure_ascii=False), flush=True)
                        failed |= not okay
                    finally:
                        if process.poll() is None:
                            process.kill()
                        process.wait()
        if args.worker_faults and args.backend == "gpu":
            for fault in (signal.SIGSTOP, signal.SIGKILL):
                with tempfile.TemporaryDirectory(prefix="gipu-worker-fault-", dir=args.output_root) as temp:
                    destination = Path(temp) / "out"
                    process = subprocess.Popen([str(args.binary.resolve()), "extract", str(args.archive.resolve()),
                                                "--backend", "gpu", "--gpu-mode", "stream", "--stream-timeout", "1",
                                                "--temp-mode", "auto", "--output", str(destination)],
                                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    children = {}
                    try:
                        deadline = time.monotonic() + args.timeout
                        while process.poll() is None and not progress(process, destination) and time.monotonic() < deadline:
                            time.sleep(0.005)
                        children = child_states(process.pid)
                        if not children or not progress(process, destination):
                            raise RuntimeError("実行中workerを確認できませんでした")
                        for pid in children:
                            os.kill(pid, fault)
                        stdout, stderr = process.communicate(timeout=args.timeout)
                        okay = process.returncode != 0 and not (destination / member).exists()
                        okay = okay and not list(destination.rglob("*.part"))
                        row = {"backend": args.backend, "worker_fault": fault.name, "passed": okay, "stderr": stderr}
                        report.write(json.dumps(row, ensure_ascii=False) + "\n")
                        print(json.dumps(row, ensure_ascii=False), flush=True)
                        failed |= not okay
                    finally:
                        if process.poll() is None:
                            process.kill()
                        process.wait()
                        for pid, state in children.items():
                            if child_running(pid, state[1]):
                                os.kill(pid, signal.SIGKILL)
    if failed:
        raise SystemExit("キャンセル試験に失敗しました")


if __name__ == "__main__":
    main()
