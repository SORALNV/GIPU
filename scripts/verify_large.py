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
    parser.add_argument("--save", type=Path)
    args = parser.parse_args()
    if args.gib <= 0:
        parser.error("gibは正の整数です")
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
        stop = threading.Event()
        def monitor():
            while not stop.is_set():
                result = subprocess.run(["nvidia-smi", "--query-compute-apps=pid,used_gpu_memory", "--format=csv,noheader,nounits"],
                                        capture_output=True, text=True, timeout=10)
                for line in result.stdout.splitlines():
                    values = line.split(",")
                    if len(values) == 2 and values[0].strip() == str(process.pid):
                        try:
                            samples.append(int(values[1].strip()))
                        except ValueError:
                            pass
                stop.wait(0.1)
        command = [binary, "test", str(archive), "--gpu-mode", "stream", "--vram-limit", args.vram_limit, "--json"]
        started = time.perf_counter()
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        thread = threading.Thread(target=monitor, daemon=True)
        thread.start()
        try:
            stdout, stderr = process.communicate(timeout=600)
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
                  "peak_process_gpu_mib_sampled": max(samples, default=None), "memory_samples": len(samples),
                  "wall_seconds": time.perf_counter() - started, "generation_seconds": started - generated,
                  "output_written": "名前なし一時ファイル（GPU CRC照合後に削除）", "verified": "宣言サイズとGIPUのGPU CRC32照合", "stats": stats}
        print(json.dumps(report, ensure_ascii=False, indent=2))
        if args.save:
            args.save.parent.mkdir(parents=True, exist_ok=True)
            args.save.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
