"""同じZIPをCPU/GPUで処理し、CLI全体の時間と内部統計を記録する。"""
import argparse
import hashlib
import json
from pathlib import Path
import random
import shutil
import statistics
import subprocess
import tempfile
import time
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build/gipu")
    parser.add_argument("--total-mib", type=int, default=256)
    parser.add_argument("--entries", type=int, default=16)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--pattern", choices=("repeated", "random"), default="repeated")
    parser.add_argument("--vram-limit", default="4G")
    parser.add_argument("--gpu-mode", choices=("auto", "stream", "batch"), default="auto")
    parser.add_argument("--extract", action="store_true", help="ファイル生成・書き込みも測定する")
    parser.add_argument("--sync", action="store_true", help="fsyncを含む（--extract時）")
    parser.add_argument("--save", type=Path, help="測定結果のJSONを保存する")
    args = parser.parse_args()
    if min(args.total_mib, args.entries, args.repeats) <= 0 or args.entries > args.total_mib * (1 << 20):
        parser.error("測定サイズ・エントリ数・反復数の範囲が不正です")
    binary = str(Path(args.binary).resolve())
    records = []
    expected = {}
    with tempfile.TemporaryDirectory(prefix="gipu-bench-") as temp:
        root = Path(temp)
        archive = root / "benchmark.zip"
        randomizer = random.Random(3090)
        repeated = (b"GIPU benchmark: standard ZIP Deflate / GPU CRC32\n" * 23000)[:1 << 20]
        per_entry = args.total_mib * (1 << 20) // args.entries
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zip_out:
            for i in range(args.entries):
                name = f"entry-{i:04d}.bin"
                size = per_entry if i < args.entries - 1 else args.total_mib * (1 << 20) - per_entry * i
                digest = hashlib.sha256()
                with zip_out.open(name, "w", force_zip64=True) as output:
                    remaining = size
                    while remaining:
                        n = min(1 << 20, remaining)
                        data = randomizer.randbytes(n) if args.pattern == "random" else repeated[:n]
                        output.write(data)
                        digest.update(data)
                        remaining -= n
                expected[name] = digest.hexdigest()
        # 先行実行によるOSキャッシュの影響を偏らせないよう交互に順番を変える。
        for repeat in range(args.repeats):
            order = ("cpu", "gpu") if repeat % 2 == 0 else ("gpu", "cpu")
            for backend in order:
                command = [binary, "extract" if args.extract else "test", str(archive), "--backend", backend,
                           "--gpu-mode", args.gpu_mode, "--vram-limit", args.vram_limit, "--json"]
                destination = root / f"{backend}-{repeat}"
                if args.extract:
                    command += ["--output", str(destination)]
                    if args.sync:
                        command += ["--sync"]
                started = time.perf_counter()
                result = subprocess.run(command, text=True, capture_output=True, timeout=600)
                wall = time.perf_counter() - started
                if result.returncode:
                    raise RuntimeError(result.stderr)
                row = json.loads(result.stdout)
                row.update(repeat=repeat, wall_seconds=wall)
                records.append(row)
                if args.extract:
                    for name, wanted in expected.items():
                        digest = hashlib.sha256()
                        with (destination / name).open("rb") as data:
                            while block := data.read(1 << 20):
                                digest.update(block)
                        if digest.hexdigest() != wanted:
                            raise RuntimeError(f"出力SHA256が不一致: {name}")
                    # このスクリプトが作った一時展開先だけを削除する。
                    shutil.rmtree(destination)
                print(json.dumps(row, ensure_ascii=False), flush=True)
        medians = {b: statistics.median(r["wall_seconds"] for r in records if r["backend"] == b) for b in ("cpu", "gpu")}
        report = {"cpu_reference": "zlib", "mode": "extract" if args.extract else "test", "durable": args.sync,
                  "pattern": args.pattern, "total_mib": args.total_mib, "entries": args.entries,
                  "archive_bytes": archive.stat().st_size, "cache": "OSキャッシュを排除せず交互に実行",
                  "median_wall_seconds": medians, "cpu_over_gpu": medians["cpu"] / medians["gpu"], "runs": records}
        print(json.dumps(report, ensure_ascii=False, indent=2))
        if args.save:
            args.save.parent.mkdir(parents=True, exist_ok=True)
            args.save.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
