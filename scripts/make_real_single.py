"""ローカル実データの先頭部分から、単一大ファイルの標準ZIP64を作る。元データは変更しない。"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import time
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--gib", type=int, default=8)
    parser.add_argument("--level", type=int, choices=range(10), default=6)
    args = parser.parse_args()
    if args.gib <= 0 or not args.source.is_file():
        parser.error("正のサイズと通常ファイルのsourceを指定してください")
    count = min(args.gib << 30, args.source.stat().st_size)
    archive_path = args.output
    partial = archive_path.with_suffix(".zip.part")
    manifest = archive_path.with_suffix(".json")
    if archive_path.suffix != ".zip" or any(p.exists() for p in (archive_path, partial, manifest)):
        parser.error("未作成の.zip出力を指定してください（上書きしません）")
    archive_path.parent.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(archive_path.parent).free < count + (2 << 30):
        raise RuntimeError("出力の空き容量が不足しています")
    name = "large-real-data.bin"  # 元データのローカルパスをZIPへ含めない。
    info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info._compresslevel = args.level
    info.external_attr = 0o100644 << 16
    digest = hashlib.sha256()
    started = time.perf_counter()
    with args.source.open("rb") as source, zipfile.ZipFile(partial, "x") as archive:
        with archive.open(info, "w", force_zip64=True) as output:
            left = count
            while left:
                block = source.read(min(4 << 20, left))
                if not block:
                    raise RuntimeError("元データが短くなりました")
                output.write(block)
                digest.update(block)
                left -= len(block)
    archive_path.hardlink_to(partial)
    partial.unlink()
    report = {"case": archive_path.stem, "seed": None, "compression_level": args.level,
              "archive_bytes": archive_path.stat().st_size, "uncompressed_bytes": count,
              "files": 1, "creation_seconds": time.perf_counter() - started,
              "source_kind": "ユーザー所有のKaggle NPY先頭部分。元データは変更しない。",
              "entries": [{"name": name, "bytes": count, "sha256": digest.hexdigest()}]}
    with manifest.open("x", encoding="utf-8") as out:
        json.dump(report, out, ensure_ascii=False, indent=2)
        out.write("\n")
    print(json.dumps({k: v for k, v in report.items() if k != "entries"}, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
