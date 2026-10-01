"""Kaggleの既存ファイルを読み取り、指定した圧縮容量の標準ZIP64を作る。"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from collections import deque
import json
import math
import os
from pathlib import Path
import shutil
import struct
import time
import zlib


def paths(root):
    for current, dirs, files in os.walk(root, followlinks=False):
        dirs.sort()
        for name in sorted(files):
            p = Path(current) / name
            if p.suffix == ".dcm" and not p.is_symlink():
                yield p


def compress(path):
    with path.open("rb") as source:
        data = source.read((16 << 20) + 1)
    if len(data) > 16 << 20:
        raise RuntimeError("16MiBを超えるDICOMは初期生成器の対象外です")
    encoder = zlib.compressobj(6, zlib.DEFLATED, -15)
    return path, len(data), zlib.crc32(data), encoder.compress(data) + encoder.flush()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--target-gb", type=float, default=50, help="圧縮ZIP容量、10進GB")
    parser.add_argument("--workers", type=int, default=12)
    args = parser.parse_args()
    if not math.isfinite(args.target_gb) or args.target_gb <= 0 or not 1 <= args.workers <= 32:
        parser.error("容量またはworker数が不正です")
    if not args.source.is_dir():
        parser.error("データソースがありません")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    target = int(args.target_gb * 1_000_000_000)
    reserve = 15_000_000_000
    if shutil.disk_usage(args.output.parent).free < target + reserve:
        parser.error("ZIP容量に加えて15GBの空きを確保できません")
    part = args.output.with_suffix(args.output.suffix + ".part")
    central_path = args.output.with_suffix(args.output.suffix + ".central.part")
    manifest_path = args.output.with_suffix(args.output.suffix + ".manifest.jsonl")
    summary_path = args.output.with_suffix(args.output.suffix + ".summary.json")
    if any(p.exists() for p in (args.output, part, central_path, manifest_path, summary_path)):
        parser.error("出力または作業ファイルが既に存在します。上書きしません")
    started = time.perf_counter()
    count = total_raw = 0
    progress_at = 0
    with part.open("xb") as out, central_path.open("xb+") as central, manifest_path.open("x", encoding="utf-8") as manifest:
        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            source = iter(paths(args.source))
            pending = deque()
            def submit():
                try:
                    path = next(source)
                except StopIteration:
                    return False
                # 既存DICOMをそのまま使い、巨大な補助配列は対象にしない。
                if path.stat().st_size > 16 << 20:
                    raise RuntimeError("16MiBを超えるDICOMは初期生成器の対象外です")
                pending.append(pool.submit(compress, path))
                return True
            for _ in range(args.workers * 3):
                if not submit():
                    break
            while pending:
                path, size, crc, compressed = pending.popleft().result()
                name = path.relative_to(args.source).as_posix().encode("utf-8")
                if len(name) > 65535:
                    raise RuntimeError("ZIPファイル名が長すぎます")
                offset = out.tell()
                local = struct.pack("<I5H3I2H", 0x04034b50, 20, 0x800, 8, 0, 33, crc, len(compressed), size, len(name), 0)
                out.write(local)
                out.write(name)
                out.write(compressed)
                extra = struct.pack("<HHQ", 1, 8, offset) if offset >= 0xffffffff else b""
                header = struct.pack("<I6H3I5H2I", 0x02014b50, 0x0314, 45 if extra else 20, 0x800, 8, 0, 33,
                                     crc, len(compressed), size, len(name), len(extra), 0, 0, 0,
                                     0o100644 << 16, 0xffffffff if extra else offset)
                central.write(header + name + extra)
                manifest.write(json.dumps({"name": name.decode(), "bytes": size, "crc32": crc}, ensure_ascii=False) + "\n")
                count += 1
                total_raw += size
                # 中央ディレクトリを含む見込み容量で停止する。最後の1ファイル分だけ超過する。
                position = out.tell() + central.tell() + 98
                if position - progress_at >= 1_000_000_000:
                    print(json.dumps({"files": count, "zip_gb": round(position / 1e9, 3), "raw_gb": round(total_raw / 1e9, 3),
                                      "seconds": round(time.perf_counter() - started, 1)}), flush=True)
                    progress_at = position
                if position >= target:
                    for future in pending:
                        future.cancel()
                    break
                if count >= 950000:
                    raise RuntimeError("ZIPエントリ数が95万を超えました")
                submit()
        if out.tell() + central.tell() < target:
            raise RuntimeError("対象DICOMだけでは指定した圧縮容量に届きません")
        cd_offset, cd_size = out.tell(), central.tell()
        central.seek(0)
        shutil.copyfileobj(central, out, length=8 << 20)
        zip64_offset = out.tell()
        out.write(struct.pack("<IQ2H2I4Q", 0x06064b50, 44, 45, 45, 0, 0, count, count, cd_size, cd_offset))
        out.write(struct.pack("<IIQI", 0x07064b50, 0, zip64_offset, 1))
        out.write(struct.pack("<I4H2IH", 0x06054b50, 0, 0, 65535, 65535, 0xffffffff, 0xffffffff, 0))
        out.flush()
        os.fsync(out.fileno())
    # hard linkで公開し、既存の最終ファイル名を置き換えない。
    os.link(part, args.output)
    part.unlink()
    central_path.unlink()
    report = {"source": str(args.source), "archive": str(args.output), "archive_bytes": args.output.stat().st_size,
              "raw_bytes": total_raw, "files": count, "method": "Deflate level 6 / ZIP64", "seconds": time.perf_counter() - started}
    with summary_path.open("x", encoding="utf-8") as summary:
        summary.write(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(report, ensure_ascii=False, indent=2), flush=True)


if __name__ == "__main__":
    main()
