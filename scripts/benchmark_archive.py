"""既存の大容量ZIPで、同じ入力・同じ出力媒体の比較を行う。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import statistics
import subprocess
import tempfile
import time
import zipfile


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", type=Path, default=Path("build/gipu"))
    p.add_argument("--archive", type=Path, required=True)
    p.add_argument("--results", type=Path, required=True)
    p.add_argument("--cases", nargs="+", default=["cpu", "libdeflate1", "libdeflate16", "gpu256", "gpu4096"])
    p.add_argument("--repeats", type=int, default=1)
    p.add_argument("--extract-root", type=Path, help="指定時は実ファイル展開。試験出力だけ終了後に削除")
    p.add_argument("--source", type=Path, help="展開後のサンプルSHA256比較用")
    p.add_argument("--sync", action="store_true", help="各出力ファイルのfsyncを時間に含める")
    p.add_argument("--cache", choices=["warm", "drop-advised"], default="drop-advised")
    args = p.parse_args()
    binary = str(args.binary.resolve())
    archive = args.archive.resolve(strict=True)
    if args.repeats < 1 or (args.source and not args.extract_root):
        p.error("繰り返し回数またはsource指定が不正です")
    cases = {
        "auto": ["--backend", "auto"],
        "auto-experimental": ["--backend", "auto", "--auto-gpu", "--auto-parallel"],
        "cpu": ["--backend", "cpu"],
        "libdeflate1": ["--backend", "libdeflate", "--threads", "1"],
        "libdeflate8": ["--backend", "libdeflate", "--threads", "8"],
        "libdeflate16": ["--backend", "libdeflate", "--threads", "16"],
        "libdeflate16-anonymous": ["--backend", "libdeflate", "--threads", "16", "--temp-mode", "auto"],
        "libdeflate16-named": ["--backend", "libdeflate", "--threads", "16", "--temp-mode", "named"],
        "libdeflate32": ["--backend", "libdeflate", "--threads", "32"],
        "hybrid": ["--backend", "hybrid"],
        "hybrid25": ["--backend", "hybrid", "--cpu-percent", "25"],
        "hybrid75": ["--backend", "hybrid", "--cpu-percent", "75"],
        "hybrid16": ["--backend", "hybrid", "--threads", "16"],
        "hybrid-anonymous": ["--backend", "hybrid", "--temp-mode", "auto"],
        "gpu256": ["--backend", "gpu", "--batch-entries", "256"],
        "gpu1024": ["--backend", "gpu", "--batch-entries", "1024"],
        "gpu4096": ["--backend", "gpu", "--batch-entries", "4096"],
        "gpupipeline": ["--backend", "gpu", "--pipeline", "--batch-entries", "4096"],
        "gpupipeline-anonymous": ["--backend", "gpu", "--pipeline", "--batch-entries", "4096", "--temp-mode", "auto"],
        "gpupipeline8g": ["--backend", "gpu", "--pipeline", "--batch-entries", "8192", "--vram-limit", "8G", "--host-limit", "24G"],
        "gpupipeline16g": ["--backend", "gpu", "--pipeline", "--batch-entries", "16384", "--vram-limit", "16G", "--host-limit", "40G"],
    }
    if any(case not in cases for case in args.cases):
        p.error("不明な測定caseです")
    with zipfile.ZipFile(archive) as z:
        entries = [info for info in z.infolist() if not info.is_dir()]
        raw_bytes = sum(info.file_size for info in entries)
    if args.extract_root:
        args.extract_root.mkdir(parents=True, exist_ok=True)
        if shutil.disk_usage(args.extract_root).free < raw_bytes + 20_000_000_000:
            p.error("展開量に加えて20GBの空きが必要です")
    report = {"archive_bytes": archive.stat().st_size, "raw_bytes": raw_bytes, "files": len(entries),
              "mode": "extract" if args.extract_root else "test", "durable": args.sync,
              "cache": args.cache, "cache_note": "DONTNEEDは対象ZIPだけへの助言。完全なcold cacheは保証しない。",
              "runs": [], "median_wall_seconds": {}}
    args.results.parent.mkdir(parents=True, exist_ok=True)
    # 途中結果も残すJSONL。元データのファイル名・画像は結果に含めない。
    log_path = args.results.with_suffix(args.results.suffix + ".jsonl")
    if args.results.exists() or log_path.exists():
        p.error("結果ファイルは上書きしません")
    with log_path.open("x", encoding="utf-8") as log:
        log.write(json.dumps({key: value for key, value in report.items() if key != "runs"}) + "\n")
        log.flush()
        for repeat in range(args.repeats):
            # 2回目以降は順序を交互にし、一方向の温度・キャッシュ影響を減らす。
            order = args.cases if repeat % 2 == 0 else list(reversed(args.cases))
            for case in order:
                with tempfile.TemporaryDirectory(prefix="gipu-benchmark-", dir=args.extract_root) as temporary:
                    if args.cache == "drop-advised":
                        with archive.open("rb") as input_file:
                            os.posix_fadvise(input_file.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)
                    command = [binary, "extract" if args.extract_root else "test", str(archive), *cases[case], "--json"]
                    if args.extract_root:
                        command += ["--output", temporary]
                    if args.sync:
                        command += ["--sync"]
                    print(json.dumps({"event": "start", "case": case, "repeat": repeat + 1}), flush=True)
                    started = time.perf_counter()
                    run = subprocess.run(command, capture_output=True, text=True, timeout=7200)
                    wall = time.perf_counter() - started
                    if run.returncode:
                        raise RuntimeError(f"{case}: {run.stderr}")
                    stats = json.loads(run.stdout)
                    if stats["files"] != len(entries) or stats["bytes"] != raw_bytes:
                        raise RuntimeError("処理したファイル数または展開量が一致しません")
                    verified_samples = 0
                    if args.extract_root:
                        for info in entries:
                            path = Path(temporary) / info.filename
                            if path.stat().st_size != info.file_size:
                                raise RuntimeError("展開ファイルのサイズが一致しません")
                        if args.source:
                            samples = random.Random(3090).sample(entries, min(128, len(entries)))
                            for info in samples:
                                source = (args.source / info.filename).read_bytes()
                                output = (Path(temporary) / info.filename).read_bytes()
                                if hashlib.sha256(source).digest() != hashlib.sha256(output).digest():
                                    raise RuntimeError("元データとのSHA256が一致しません")
                            verified_samples = len(samples)
                    record = {"case": case, "repeat": repeat + 1, "wall_seconds": wall, "stats": stats,
                              "sha256_samples": verified_samples, "crc_verified_files": stats["files"]}
                    report["runs"].append(record)
                    log.write(json.dumps(record) + "\n"); log.flush()
                    print(json.dumps({"event": "complete", **record}), flush=True)
                # TemporaryDirectoryでこの試験が作った展開物だけを削除する。
                if args.extract_root:
                    print(json.dumps({"event": "removed_generated_output", "case": case}), flush=True)
    for case in args.cases:
        report["median_wall_seconds"][case] = statistics.median(r["wall_seconds"] for r in report["runs"] if r["case"] == case)
    with args.results.open("x", encoding="utf-8") as output:
        json.dump(report, output, indent=2); output.write("\n")
    print(json.dumps(report["median_wall_seconds"]), flush=True)


if __name__ == "__main__":
    main()
