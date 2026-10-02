"""VRAMを超える単一ZIP64エントリを作り、全体出力を確保せずGPUで検証する。"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build/gipu")
    parser.add_argument("--gib", type=int, default=26)
    parser.add_argument("--vram-limit", default="64M")
    parser.add_argument("--stream-crc", choices=("cpu", "gpu"), default="cpu")
    parser.add_argument("--save", type=Path)
    args = parser.parse_args()
    if args.gib <= 0:
        parser.error("gibは正の整数です")
    if args.save and args.save.exists():
        parser.error("既存の結果ファイルは上書きしません")
    binary = str(Path(args.binary).resolve())
    with tempfile.TemporaryDirectory(prefix="gipu-large-") as temporary:
        archive = Path(temporary) / "single-zip64.zip"
        block = b"\0" * (1 << 20)
        generated = time.perf_counter()
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=1) as output:
            with output.open("larger-than-vram.bin", "w", force_zip64=True) as member:
                for i in range(args.gib * 1024):
                    member.write(block)
                    if (i + 1) % 1024 == 0:
                        print(f"ZIP生成: {(i + 1) // 1024}/{args.gib} GiB", flush=True)
        samples = []
        parent_samples, child_samples, worker_samples = [], [], []
        rss_samples = []
        monitor_errors = []
        stop = threading.Event()
        def monitor():
            while not stop.is_set():
                pids = {process.pid}
                pending = [process.pid]
                while pending:
                    pid = pending.pop()
                    try:
                        children = Path(f"/proc/{pid}/task/{pid}/children").read_text().split()
                    except FileNotFoundError:
                        continue
                    for child in map(int, children):
                        if child not in pids:
                            pids.add(child)
                            pending.append(child)
                rss = 0
                for pid in pids:
                    try:
                        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
                            if line.startswith("VmRSS:"):
                                rss += int(line.split()[1])
                    except FileNotFoundError:
                        pass
                rss_samples.append(rss)
                try:
                    result = subprocess.run(["nvidia-smi", "--query-compute-apps=pid,used_gpu_memory", "--format=csv,noheader,nounits"],
                                            capture_output=True, text=True, timeout=10, check=True)
                except (OSError, subprocess.SubprocessError) as error:
                    monitor_errors.append(type(error).__name__)
                    return
                parent_memory, child_memory = 0, 0
                workers = 0
                for line in result.stdout.splitlines():
                    values = line.split(",")
                    if len(values) == 2 and values[0].strip() in {str(pid) for pid in pids}:
                        try:
                            memory = int(values[1].strip())
                            if int(values[0]) == process.pid:
                                parent_memory += memory
                            else:
                                child_memory += memory
                                workers += 1
                        except ValueError:
                            pass
                if parent_memory or child_memory:
                    samples.append(parent_memory + child_memory)
                    parent_samples.append(parent_memory)
                    child_samples.append(child_memory)
                    worker_samples.append(workers)
                stop.wait(0.2)
        command = [binary, "test", str(archive), "--backend", "gpu", "--gpu-mode", "stream",
                   "--vram-limit", args.vram_limit, "--stream-crc", args.stream_crc, "--json"]
        started = time.perf_counter()
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        thread = threading.Thread(target=monitor, daemon=True)
        thread.start()
        try:
            stdout, stderr = process.communicate(timeout=600)
            finished = time.perf_counter()
        except BaseException:
            process.kill()
            process.communicate()
            raise
        finally:
            stop.set()
            thread.join(timeout=12)
        if process.returncode:
            raise RuntimeError(stderr)
        stats = json.loads(stdout)
        if stats["bytes"] != args.gib * (1 << 30) or stats["gpu_streams"] != 1:
            raise RuntimeError("GPU展開量またはストリーム数が一致しません")
        report = {"output_gib": args.gib, "archive_bytes": archive.stat().st_size, "vram_limit": args.vram_limit,
                  "peak_process_tree_gpu_mib_sampled": max(samples, default=None), "memory_samples": len(samples),
                  "peak_parent_gpu_mib_sampled": max(parent_samples, default=None),
                  "peak_children_gpu_mib_sampled": max(child_samples, default=None),
                  "peak_gpu_workers_sampled": max(worker_samples, default=None), "monitor_errors": monitor_errors,
                  "peak_process_tree_rss_kib_sampled": max(rss_samples, default=None),
                  "memory_note": "親子のGPU使用量・RSSを合算してサンプリング。RSSの共有ページは重複計上する。",
                  "wall_seconds": finished - started, "generation_seconds": started - generated,
                  "output_written": "なし" if args.stream_crc == "cpu" else "名前なし一時ファイル（照合後に削除）",
                  "verified": f"宣言サイズとGIPUの{args.stream_crc.upper()} CRC32照合", "stats": stats}
        print(json.dumps(report, ensure_ascii=False, indent=2))
        if args.save:
            args.save.parent.mkdir(parents=True, exist_ok=True)
            with args.save.open("x", encoding="utf-8") as output:
                json.dump(report, output, ensure_ascii=False, indent=2)
                output.write("\n")


if __name__ == "__main__":
    main()
