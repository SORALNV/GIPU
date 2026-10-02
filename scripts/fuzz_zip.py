"""小さな合成ZIPを変異させCPU経路を差分検証する。GPUへ不正圧縮データを送らない。"""
import argparse
import io
import json
import os
from pathlib import Path
import random
import subprocess
import tempfile
import time
import zipfile


def fixtures():
    result = []
    rng = random.Random(3090)
    for method in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
        for payload in (b"", b"a", b"abc" * 10000, rng.randbytes(65537), b"x" * (1 << 20)):
            out = io.BytesIO()
            with zipfile.ZipFile(out, "w", compression=method) as archive:
                archive.writestr("folder/日本語.bin", payload)
            result.append(out.getvalue())
    return result


def mutate(original, rng):
    data = bytearray(original)
    kind = rng.randrange(5)
    if kind == 0:
        for _ in range(rng.randint(1, 4)):
            data[rng.randrange(len(data))] ^= 1 << rng.randrange(8)
    elif kind == 1:
        del data[rng.randrange(len(data)):]
    elif kind == 2:
        at = rng.randrange(len(data))
        data[at:at] = rng.randbytes(rng.randint(1, 12))
    elif kind == 3:
        data += rng.randbytes(rng.randint(1, 16))
    else:
        cd = data.index(b"PK\x01\x02")
        # 名前・追加フィールドを避けた圧縮本体の変異を増やす。
        start = 30 + int.from_bytes(data[26:28], "little") + int.from_bytes(data[28:30], "little")
        if start < cd:
            for _ in range(rng.randint(1, 4)):
                data[rng.randrange(start, cd)] ^= rng.randrange(1, 256)
    return data


def reference(data):
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        files = [i for i in archive.infolist() if not i.is_dir()]
        count = 0
        for info in files:
            with archive.open(info) as member:
                while block := member.read(1 << 20):
                    count += len(block)
                    if count > 64 << 20:
                        raise RuntimeError("参照解凍の出力上限です")
        return len(files), count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/gipu"))
    parser.add_argument("--backend", choices=("cpu", "libdeflate", "isal", "rapidgzip"), required=True)
    parser.add_argument("--iterations", type=int, default=2000)
    parser.add_argument("--seed", type=int, default=3090)
    parser.add_argument("--timeout", type=float, default=5)
    parser.add_argument("--results", type=Path, required=True)
    args = parser.parse_args()
    if args.iterations <= 0 or args.timeout <= 0 or args.results.exists():
        parser.error("正の反復数／時間と未使用のresultsを指定してください")
    args.results.parent.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)
    bases = fixtures()
    counts = {"accepted": 0, "rejected": 0}
    start = time.perf_counter()
    environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gipu-fuzz-") as temp:
        path = Path(temp) / "input.zip"
        for i in range(args.iterations + len(bases)):
            data = bases[i] if i < len(bases) else mutate(rng.choice(bases), rng)
            path.write_bytes(data)
            command = [str(args.binary.resolve()), "test", str(path), "--backend", args.backend,
                       "--threads", "4", "--max-output", "64M", "--host-limit", "64M",
                       "--metadata-limit", "4M", "--json"]
            failure = None
            try:
                result = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout, env=environment)
                if result.returncode < 0 or "Sanitizer" in result.stderr or "runtime error:" in result.stderr:
                    failure = {"returncode": result.returncode, "stderr": result.stderr}
                elif result.returncode == 0:
                    try:
                        files, size = reference(data)
                        stats = json.loads(result.stdout)
                        if (stats["files"], stats["bytes"]) != (files, size):
                            raise RuntimeError("参照解凍との件数／サイズ不一致")
                        counts["accepted"] += 1
                    except Exception as error:
                        failure = {"accepted_but_reference_failed": str(error), "stdout": result.stdout}
                elif i < len(bases):
                    failure = {"valid_fixture_rejected": result.stderr}
                else:
                    counts["rejected"] += 1
            except subprocess.TimeoutExpired:
                failure = {"timeout": args.timeout}
            if failure is not None:
                repro = args.results.with_suffix(f".failure-{i}.zip")
                with repro.open("xb") as output:
                    output.write(data)
                report = {"backend": args.backend, "seed": args.seed, "iteration": i, "failure": failure,
                          "reproducer": str(repro), **counts}
                with args.results.open("x", encoding="utf-8") as output:
                    json.dump(report, output, ensure_ascii=False, indent=2)
                    output.write("\n")
                raise RuntimeError(json.dumps(report, ensure_ascii=False))
            if i and i % 500 == 0:
                print(json.dumps({"iteration": i, **counts}), flush=True)
    report = {"backend": args.backend, "seed": args.seed, "iterations": args.iterations,
              "valid_fixtures": len(bases), "seconds": time.perf_counter() - start, **counts, "failure": None}
    with args.results.open("x", encoding="utf-8") as output:
        json.dump(report, output, ensure_ascii=False, indent=2)
        output.write("\n")
    print(json.dumps(report, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
