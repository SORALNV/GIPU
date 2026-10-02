"""固定コーパスを複数経路で比較する。検証と一時出力削除は計測の外で行う。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
import tempfile
import time

VARIANTS = {
    "cpu": ["--backend", "cpu"],
    "isal": ["--backend", "isal"],
    "hybrid": ["--backend", "hybrid"],
    "hybrid25": ["--backend", "hybrid", "--cpu-percent", "25"],
    "hybrid75": ["--backend", "hybrid", "--cpu-percent", "75"],
    "libdeflate1": ["--backend", "libdeflate", "--threads", "1"],
    "libdeflate4": ["--backend", "libdeflate", "--threads", "4"],
    "libdeflate16": ["--backend", "libdeflate", "--threads", "16"],
    "libdeflate-full": ["--backend", "libdeflate", "--threads", "16", "--cpu-buffer-limit", "8G"],
    "libdeflate16-anonymous": ["--backend", "libdeflate", "--threads", "16", "--temp-mode", "auto"],
    "libdeflate32": ["--backend", "libdeflate", "--threads", "32"],
    "libdeflate-stream": ["--backend", "libdeflate", "--threads", "16", "--host-limit", "32M"],
    "rapidgzip1": ["--backend", "rapidgzip", "--threads", "1"],
    "rapidgzip4": ["--backend", "rapidgzip", "--threads", "4"],
    "rapidgzip16": ["--backend", "rapidgzip", "--threads", "16"],
    "gpu": ["--backend", "gpu"],
    "gpu-lookahead": ["--backend", "gpu", "--gpu-algorithm", "lookahead"],
    "gpu-lookahead16g": ["--backend", "gpu", "--gpu-algorithm", "lookahead", "--vram-limit", "16G", "--host-limit", "24G"],
    "gpu-pipeline": ["--backend", "gpu", "--pipeline"],
    "gpu-stream-cpu": ["--backend", "gpu", "--gpu-mode", "stream", "--stream-crc", "cpu"],
    "gpu-stream-gpu": ["--backend", "gpu", "--gpu-mode", "stream", "--stream-crc", "gpu"],
    "baseline-gpu": ["--backend", "gpu"],
    "baseline-cpu": ["--backend", "cpu"],
    "baseline-libdeflate16": ["--backend", "libdeflate", "--threads", "16"],
}


def sha256(path):
    with path.open("rb") as data:
        return hashlib.file_digest(data, "sha256").hexdigest()


def verify(destination, manifest, samples):
    entries = manifest["entries"]
    for entry in entries:
        path = destination / entry["name"]
        if not path.is_file() or path.stat().st_size != entry["bytes"]:
            raise RuntimeError(f"出力ファイル/サイズ不一致: {entry['name']}")
    expected = {e["name"] for e in entries}
    actual = {p.relative_to(destination).as_posix() for p in destination.rglob("*") if p.is_file()}
    if expected != actual:
        raise RuntimeError("出力集合に過不足があります")
    chosen = random.Random(3090).sample(entries, min(samples, len(entries)))
    for entry in chosen:
        if sha256(destination / entry["name"]) != entry["sha256"]:
            raise RuntimeError(f"出力SHA256不一致: {entry['name']}")
    return len(chosen)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--cases", nargs="+", required=True)
    parser.add_argument("--variants", nargs="+", choices=VARIANTS, default=["cpu", "libdeflate16", "gpu"])
    parser.add_argument("--binary", type=Path, default=Path("build/gipu"))
    parser.add_argument("--baseline", type=Path, default=Path(".deps/gipu-baseline-20261002"))
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--modes", nargs="+", choices=("test", "extract"), default=["test", "extract"])
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--samples", type=int, default=128)
    parser.add_argument("--sync", action="store_true")
    parser.add_argument("--cache", choices=("warm", "drop-advised"), default="warm")
    args = parser.parse_args()
    if min(args.repeats, args.timeout, args.samples) <= 0:
        parser.error("反復数・タイムアウト・検証数は正の値が必要です")
    args.output_root.mkdir(parents=True, exist_ok=True)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    metadata = {"kind": "metadata", "time_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "binary_sha256": sha256(args.binary), "cache": args.cache, "durable": args.sync,
                "note": "warmはOSキャッシュを排除しない。drop-advisedもcoldを保証しない。異常終了は行に記録する。"}
    if any(v.startswith("baseline-") for v in args.variants):
        metadata["baseline_sha256"] = sha256(args.baseline)
    with args.report.open("x", encoding="utf-8", buffering=1) as report:
        report.write(json.dumps(metadata, ensure_ascii=False) + "\n")
        for case in args.cases:
            manifest = json.loads((args.corpus / f"{case}.json").read_text())
            archive = args.corpus / f"{case}.zip"
            for mode in args.modes:
                for repeat in range(args.repeats):
                    variants = args.variants if repeat % 2 == 0 else list(reversed(args.variants))
                    for variant in variants:
                        if mode == "extract" and shutil.disk_usage(args.output_root).free < manifest["uncompressed_bytes"] + (2 << 30):
                            raise RuntimeError("展開先の空き容量が不足しています")
                        binary = args.baseline if variant.startswith("baseline-") else args.binary
                        with tempfile.TemporaryDirectory(prefix="gipu-corpus-", dir=args.output_root) as temp:
                            destination = Path(temp) / "out"
                            usage = Path(temp) / "usage.txt"
                            command = [str(binary.resolve()), mode, str(archive.resolve()), *VARIANTS[variant], "--json"]
                            if mode == "extract":
                                command += ["--output", str(destination)]
                            if args.sync:
                                command += ["--sync"]
                            if args.cache == "drop-advised":
                                with archive.open("rb") as fd:
                                    os.posix_fadvise(fd.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)
                            if Path("/usr/bin/time").exists():
                                command = ["/usr/bin/time", "-f", "%M %U %S", "-o", str(usage), "--", *command]
                            row = {"case": case, "variant": variant, "mode": mode, "repeat": repeat,
                                   "raw_bytes": manifest["uncompressed_bytes"], "archive_bytes": manifest["archive_bytes"]}
                            start = time.perf_counter()
                            # timeは単一CLIの親だけ。期限時はプロセスグループごと停止する。
                            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                                       text=True, start_new_session=True)
                            try:
                                stdout, stderr = process.communicate(timeout=args.timeout)
                                row["returncode"] = process.returncode
                            except subprocess.TimeoutExpired:
                                import signal
                                os.killpg(process.pid, signal.SIGKILL)
                                stdout, stderr = process.communicate()
                                row["returncode"] = -9
                                row["timeout"] = True
                            row["wall_seconds"] = time.perf_counter() - start
                            if usage.exists():
                                try:
                                    rss, user, system = usage.read_text().strip().splitlines()[-1].split()
                                    row.update(peak_rss_kib=int(rss), user_seconds=float(user), system_seconds=float(system))
                                except (ValueError, IndexError):
                                    pass
                            if row["returncode"] == 0:
                                stats = json.loads(stdout)
                                if stats["files"] != manifest["files"] or stats["bytes"] != manifest["uncompressed_bytes"]:
                                    raise RuntimeError("CLIのファイル数/展開サイズが期待値と一致しません")
                                row["stats"] = stats
                                if mode == "extract":
                                    row["verified_sha256_samples"] = verify(destination, manifest, args.samples)
                            else:
                                row["stderr"] = stderr.strip()[:2000]
                            report.write(json.dumps(row, ensure_ascii=False) + "\n")
                            print(json.dumps(row, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
