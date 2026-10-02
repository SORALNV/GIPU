"""公式7-Zipを外部比較対象として計測する。試験が所有する出力だけを削除する。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import signal
import statistics
import subprocess
import tempfile
import time
import zipfile
from benchmark_space import check_output_space, plan_output_space


def digest(path):
    with path.open("rb") as data:
        return hashlib.file_digest(data, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--threads", type=int, default=16)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if min(args.threads, args.repeats, args.timeout) < 1 or args.report.exists():
        parser.error("正の個数・期限と未使用のreportが必要です")
    if args.source and not args.output_root:
        parser.error("sourceは実展開の検証用です")
    binary = args.binary.resolve(strict=True)
    archive = args.archive.resolve(strict=True)
    version = subprocess.run([str(binary), "-h"], capture_output=True, text=True, check=True).stdout.splitlines()
    with zipfile.ZipFile(archive) as data:
        members = data.infolist()
        entries = [e for e in members if not e.is_dir()]
        raw_bytes = sum(e.file_size for e in entries)
        # この測定器は既知の正しいベンチマークZIP専用。危険な出力パスを受け入れない。
        for entry in members:
            parts = entry.filename.rstrip("/").split("/")
            if any(p in ("", ".", "..") for p in parts) or "\\" in entry.filename or ":" in entry.filename:
                parser.error("相対の通常ファイルだけを含む比較用ZIPが必要です")
            if ((entry.external_attr >> 16) & 0o170000) not in (0, 0o040000 if entry.is_dir() else 0o100000):
                parser.error("特殊ファイルを含むZIPは計測しません")
            if entry.flag_bits & 1 or entry.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                parser.error("暗号化なしのStored／Deflate比較用ZIPが必要です")
    if args.output_root:
        args.output_root.mkdir(parents=True, exist_ok=True)
        space_plan = plan_output_space(args.output_root,
                                       ((e.filename, e.file_size) for e in members), 20_000_000_000)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    rows = []
    metadata = {"kind": "metadata", "tool_version": next(line for line in version if line.startswith("7-Zip")),
                "binary_sha256": digest(binary), "archive_bytes": archive.stat().st_size,
                "raw_bytes": raw_bytes, "files": len(entries), "threads_option": args.threads,
                "mode": "extract" if args.output_root else "test", "durable": False,
                "cache": "drop-advised", "time_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "note": "DONTNEEDは対象ZIPへの助言でcoldを保証しない。-mmtは要求値で実際のCPU並列数ではない。"}
    with args.report.open("x", encoding="utf-8", buffering=1) as report:
        report.write(json.dumps(metadata, ensure_ascii=False) + "\n")
        for repeat in range(args.repeats):
            if args.output_root:
                check_output_space(args.output_root, space_plan)
            with tempfile.TemporaryDirectory(prefix="gipu-7zip-", dir=args.output_root) as temp:
                destination = Path(temp) / "out"
                usage = Path(temp) / "usage.txt"
                with archive.open("rb") as data:
                    os.posix_fadvise(data.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)
                command = [str(binary), "x" if args.output_root else "t", str(archive),
                           f"-mmt={args.threads}", "-bd", "-bso0", "-bsp0"]
                if args.output_root:
                    command += [f"-o{destination}", "-y"]
                command = ["/usr/bin/time", "-f", "%M %U %S", "-o", str(usage), "--", *command]
                print(json.dumps({"event": "start", "repeat": repeat + 1}), flush=True)
                started = time.perf_counter()
                process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                           text=True, start_new_session=True)
                try:
                    stdout, stderr = process.communicate(timeout=args.timeout)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.communicate()
                    raise RuntimeError("7-Zip計測の期限を超えました")
                row = {"kind": "run", "repeat": repeat + 1, "wall_seconds": time.perf_counter() - started,
                       "returncode": process.returncode}
                if process.returncode:
                    row["error"] = stderr[-1000:]
                    report.write(json.dumps(row, ensure_ascii=False) + "\n")
                    raise RuntimeError("7-Zipが正常終了しませんでした")
                rss, user, system = usage.read_text().splitlines()[-1].split()
                row.update(peak_rss_kib=int(rss), user_seconds=float(user), system_seconds=float(system))
                row["crc_verified_files"] = len(entries)
                row["sha256_samples"] = 0
                if args.output_root:
                    for entry in entries:
                        target = destination / entry.filename
                        if not target.is_file() or target.stat().st_size != entry.file_size:
                            raise RuntimeError("7-Zipの出力サイズが一致しません")
                    if args.source:
                        samples = random.Random(3090).sample(entries, min(128, len(entries)))
                        for entry in samples:
                            if digest(destination / entry.filename) != digest(args.source / entry.filename):
                                raise RuntimeError("7-Zipの出力SHA256が元データと一致しません")
                        row["sha256_samples"] = len(samples)
                rows.append(row)
                report.write(json.dumps(row, ensure_ascii=False) + "\n")
                print(json.dumps(row, ensure_ascii=False), flush=True)
        summary = {"kind": "summary", "median_seconds": statistics.median(r["wall_seconds"] for r in rows)}
        report.write(json.dumps(summary) + "\n")
        print(json.dumps(summary), flush=True)


if __name__ == "__main__":
    main()
