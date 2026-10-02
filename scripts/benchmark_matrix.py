"""同一コーパス・媒体・順序制御でGIPUと独立CPUツールを比較する。

既知の安全なStored／Deflate ZIP専用。検証・試験所有の出力削除は計測外。
異常終了・タイムアウトも記録し、成功した行だけで最速と判断しない。
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import signal
import statistics
import subprocess
import sys
import tempfile
import time
import zipfile
from benchmark_space import check_output_space, plan_output_space


VARIANTS = ("auto", "auto-experimental", "gpu", "gpu-pipeline", "hybrid", "cpu", "libdeflate1",
            "7zip", "unzip", "python-parallel")


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(4 << 20):
            result.update(block)
    return result.hexdigest()


def validate_archive(archive, manifest):
    expected = {e["name"]: e["bytes"] for e in manifest["entries"]}
    if len(expected) != len(manifest["entries"]):
        raise RuntimeError("manifestの出力パスが重複しています")
    if len(expected) != manifest["files"] or sum(expected.values()) != manifest["uncompressed_bytes"]:
        raise RuntimeError("manifestの件数・合計サイズが不正です")
    with zipfile.ZipFile(archive) as source:
        members = source.infolist()
    paths = set()
    for member in members:
        name = member.filename.rstrip("/")
        if (not name or any(p in ("", ".", "..") for p in name.split("/"))
                or "\\" in name or ":" in name or any(ord(c) < 32 or ord(c) == 127 for c in name) or name in paths):
            raise RuntimeError("危険なパスまたは重複を含む入力は外部ツールへ送りません")
        paths.add(name)
        file_type = (member.external_attr >> 16) & 0o170000
        if file_type not in (0, 0o040000 if member.is_dir() else 0o100000):
            raise RuntimeError("特殊ファイルを含む入力は測定しません")
        if member.flag_bits & 1 or member.compress_type not in (0, 8):
            raise RuntimeError("暗号化なしのStored／Deflateだけを測定します")
        if not member.is_dir() and (member.filename not in expected or member.file_size != expected[member.filename]):
            raise RuntimeError("ZIPとmanifestのファイル・サイズが一致しません")
    files = [m for m in members if not m.is_dir()]
    if len(files) != manifest["files"] or sum(m.file_size for m in files) != manifest["uncompressed_bytes"]:
        raise RuntimeError("ZIPとmanifestの件数・合計サイズが一致しません")
    for name in paths:
        for end in (i for i, c in enumerate(name) if c == "/"):
            if name[:end] in expected:
                raise RuntimeError("ファイルとディレクトリが衝突しています")
    return members


def verify(output, manifest, samples):
    entries = manifest["entries"]
    expected = {e["name"] for e in entries}
    actual = {p.relative_to(output).as_posix() for p in output.rglob("*") if p.is_file()}
    if expected != actual or any(p.is_symlink() for p in output.rglob("*")):
        raise RuntimeError("出力集合またはファイル種別が一致しません")
    for entry in entries:
        if (output / entry["name"]).stat().st_size != entry["bytes"]:
            raise RuntimeError("出力サイズが一致しません")
    for name in manifest.get("directories", []):
        if not (output / name).is_dir():
            raise RuntimeError("明示された空ディレクトリがありません")
    chosen = entries if samples == 0 else random.Random(3090).sample(entries, min(samples, len(entries)))
    for entry in chosen:
        if digest(output / entry["name"]) != entry["sha256"]:
            raise RuntimeError("出力SHA-256が一致しません")
    return len(chosen)


def command_for(args, variant, mode, archive, output):
    if variant == "7zip":
        command = [str(args.sevenzip.resolve()), "t" if mode == "test" else "x", str(archive),
                   f"-mmt={args.threads}", "-bd", "-bso0", "-bsp0"]
        return command + ([f"-o{output}", "-y"] if mode == "extract" else [])
    if variant == "unzip":
        command = [str(args.unzip.resolve()), "-tqq" if mode == "test" else "-qq", str(archive)]
        return command + (["-d", str(output)] if mode == "extract" else [])
    if variant == "python-parallel":
        command = [sys.executable, str(Path(__file__).with_name("reference_zip.py").resolve()),
                   mode, str(archive), "--threads", str(args.threads)]
        return command + (["--output", str(output)] if mode == "extract" else [])
    backend = {"auto-experimental": "auto", "gpu-pipeline": "gpu"}.get(variant, variant)
    if variant == "libdeflate1":
        backend = "libdeflate"
    command = [str(args.binary.resolve()), mode, str(archive), "--backend", backend,
               "--threads", str(1 if variant == "libdeflate1" else args.threads), "--json"]
    if variant == "auto-experimental":
        command += ["--auto-gpu", "--auto-parallel"]
    if variant == "gpu-pipeline":
        command += ["--pipeline"]
    if args.path_mode != "auto":
        command += ["--path-mode", args.path_mode]
    if args.host_limit:
        command += ["--host-limit", args.host_limit]
    return command + (["--output", str(output)] if mode == "extract" else [])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--cases", nargs="+", required=True)
    parser.add_argument("--variants", nargs="+", choices=VARIANTS,
                        default=["auto", "gpu", "7zip", "unzip", "python-parallel"])
    parser.add_argument("--binary", type=Path, default=Path("build/gipu"))
    parser.add_argument("--sevenzip", type=Path, default=Path(".deps/7zip-26.03/7zz"))
    parser.add_argument("--unzip", type=Path, default=Path("/usr/bin/unzip"))
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--threads", type=int, default=16)
    parser.add_argument("--cpu-count", type=int, help="同じ実機でaffinityを制限する。別CPU機種の試験ではない")
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--samples", type=int, default=0, help="0は全出力SHA256、正数は固定seedサンプル")
    parser.add_argument("--modes", nargs="+", choices=("test", "extract"), default=["test", "extract"])
    parser.add_argument("--cache", choices=("warm", "drop-advised"), default="drop-advised")
    parser.add_argument("--path-mode", choices=("auto", "portable"), default="auto")
    parser.add_argument("--host-limit")
    args = parser.parse_args()
    if min(args.repeats, args.threads, args.timeout) < 1 or args.threads > 32 or args.samples < 0:
        parser.error("反復数・期限・worker数が不正です")
    if args.report.exists():
        parser.error("既存の測定結果を上書きしません")
    if args.cpu_count is not None:
        allowed = sorted(os.sched_getaffinity(0))
        if not 1 <= args.cpu_count <= len(allowed):
            parser.error("利用可能CPU数を超えています")
        os.sched_setaffinity(0, allowed[:args.cpu_count])
    args.output_root.mkdir(parents=True, exist_ok=True)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    tools = {"gipu": args.binary, "7zip": args.sevenzip, "unzip": args.unzip}
    metadata = {"kind": "metadata", "time_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "platform": platform.platform(), "python": sys.version, "cpu_affinity": sorted(os.sched_getaffinity(0)),
                "threads_requested": args.threads, "output_root": str(args.output_root.resolve()),
                "output_device": args.output_root.stat().st_dev, "cache": args.cache, "durable": False,
                "path_mode": args.path_mode, "host_limit": args.host_limit, "samples": args.samples,
                "tool_sha256": {name: digest(path) for name, path in tools.items()},
                "reference_script_sha256": digest(Path(__file__).with_name("reference_zip.py")),
                "note": "同じ機械でのaffinity制限。cold cache保証なし。通常write終了まで、fsyncなし。"
                        "属性復元・出力確定方式はツールで異なる。Python並列は比較用で最速CPU保証ではない。"}
    metadata["7zip_version"] = subprocess.run([str(args.sevenzip.resolve()), "i"], capture_output=True,
                                             text=True, check=True).stdout.splitlines()[1]
    metadata["unzip_version"] = subprocess.run([str(args.unzip.resolve()), "-v"], capture_output=True,
                                             text=True, check=True).stdout.splitlines()[0]
    rows = []
    with args.report.open("x", encoding="utf-8", buffering=1) as report:
        report.write(json.dumps(metadata, ensure_ascii=False) + "\n")
        for case in args.cases:
            manifest = json.loads((args.corpus / f"{case}.json").read_text())
            archive = (args.corpus / f"{case}.zip").resolve(strict=True)
            members = validate_archive(archive, manifest)
            archive_sha256 = digest(archive)
            plan = plan_output_space(args.output_root, ((m.filename, m.file_size) for m in members), 2 << 30)
            for mode in args.modes:
                # 固定seedで初期順を決め、反復ごとに巡回する。測定の同時実行はしない。
                order = list(args.variants)
                random.Random(3090).shuffle(order)
                for repeat in range(args.repeats):
                    for variant in order[repeat % len(order):] + order[:repeat % len(order)]:
                        if mode == "extract":
                            check_output_space(args.output_root, plan)
                        with tempfile.TemporaryDirectory(prefix="gipu-matrix-", dir=args.output_root) as temporary:
                            output, usage = Path(temporary) / "out", Path(temporary) / "usage.txt"
                            command = command_for(args, variant, mode, archive, output)
                            timed = ["/usr/bin/time", "-f", "%M %U %S", "-o", str(usage), "--", *command]
                            if args.cache == "drop-advised":
                                with archive.open("rb") as source:
                                    os.posix_fadvise(source.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)
                            print(json.dumps({"event": "start", "case": case, "variant": variant,
                                              "mode": mode, "repeat": repeat + 1}), flush=True)
                            row = {"kind": "run", "case": case, "variant": variant, "mode": mode,
                                   "repeat": repeat + 1, "archive_bytes": archive.stat().st_size,
                                   "raw_bytes": manifest["uncompressed_bytes"], "files": manifest["files"],
                                   "archive_sha256": archive_sha256, "verified": False}
                            started = time.perf_counter()
                            process = subprocess.Popen(timed, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                                       text=True, start_new_session=True)
                            try:
                                stdout, stderr = process.communicate(timeout=args.timeout)
                            except subprocess.TimeoutExpired:
                                os.killpg(process.pid, signal.SIGKILL)
                                stdout, stderr = process.communicate()
                                row["timeout"] = True
                            row.update(returncode=process.returncode, wall_seconds=time.perf_counter() - started)
                            if usage.exists():
                                try:
                                    rss, user, system = usage.read_text().strip().splitlines()[-1].split()
                                    row.update(peak_rss_kib=int(rss), user_seconds=float(user), system_seconds=float(system))
                                except (ValueError, IndexError):
                                    pass
                            if process.returncode == 0:
                                try:
                                    if variant not in ("7zip", "unzip"):
                                        stats = json.loads(stdout)
                                        if (stats["files"], stats["bytes"]) != (manifest["files"], manifest["uncompressed_bytes"]):
                                            raise RuntimeError("CLI件数・展開量が一致しません")
                                        row["stats"] = stats
                                    row["crc_verified_files"] = manifest["files"]
                                    row["sha256_verified_files"] = verify(output, manifest, args.samples) if mode == "extract" else 0
                                    row["verified"] = True
                                except Exception as error:
                                    row["verification_error"] = str(error)
                            else:
                                row["stderr"] = stderr[-2000:]
                            rows.append(row)
                            report.write(json.dumps(row, ensure_ascii=False) + "\n")
                            print(json.dumps({k: v for k, v in row.items() if k not in ("stats", "archive_sha256")}), flush=True)
        groups = sorted({(r["case"], r["mode"], r["variant"]) for r in rows})
        summaries = []
        for case, mode, variant in groups:
            runs = [r for r in rows if (r["case"], r["mode"], r["variant"]) == (case, mode, variant)]
            summary = {"case": case, "mode": mode, "variant": variant, "runs": len(runs),
                       "failures": sum(not r["verified"] for r in runs)}
            if not summary["failures"]:
                values = [r["wall_seconds"] for r in runs]
                summary.update(median_seconds=statistics.median(values), min_seconds=min(values), max_seconds=max(values))
            summaries.append(summary)
        report.write(json.dumps({"kind": "summary", "groups": summaries}, ensure_ascii=False) + "\n")
    if any(not r["verified"] for r in rows):
        raise SystemExit("失敗を測定結果へ記録しました。成功した行だけによる倍率の訴求は禁止します。")


if __name__ == "__main__":
    main()
