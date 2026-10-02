"""固定seedの標準ZIP比較コーパスを作る。元データ不要、既存ファイルは上書きしない。"""
import argparse
import hashlib
import json
from pathlib import Path
import random
import shutil
import time
import zipfile

MIB = 1 << 20
CASES = {
    "tiny": (32768, 128, "text"),
    "small": (16384, 4096, "half"),
    "medium": (4096, 256 << 10, "half"),
    "single-high": (1, 1 << 30, "text"),
    "single-medium": (1, 512 * MIB, "half"),
    "single-random": (1, 512 * MIB, "random"),
    "single-large": (1, 8 << 30, "text"),
    "mixed": (8192, 4096, "mixed"),
    "many-tiny": (262144, 128, "text"),
    "many-small": (131072, 8192, "half"),
    "few-large": (32, 64 * MIB, "half"),
    "mixed-large": (32768, 4096, "mixed-large"),
    "flat-tiny": (131072, 128, "text"),
    "flat-small": (65536, 4096, "half"),
    "deep-tiny": (32768, 128, "text"),
    "wide-tiny": (32768, 128, "text"),
}


def payload(rng, size, pattern):
    if pattern == "random":
        return rng.randbytes(size)
    if pattern == "text":
        line = b"GIPU ZIP corpus / Huffman + LZ77 / 0123456789abcdef\n"
        return (line * ((size + len(line) - 1) // len(line)))[:size]
    noise = rng.randbytes((size + 1) // 2)
    return noise + b"\0" * (size - len(noise))


def create(root, name, level):
    count, size, pattern = CASES[name]
    archive_path = root / f"{name}.zip"
    partial = root / f"{name}.zip.part"
    manifest_path = root / f"{name}.json"
    if any(p.exists() for p in (archive_path, partial, manifest_path)):
        raise FileExistsError(f"既存コーパスを保護します: {name}")
    raw_size = count * size
    if pattern == "mixed":
        raw_size += ((count + 31) // 32) * MIB
    elif pattern == "mixed-large":
        raw_size += ((count + 8191) // 8192) * (512 * MIB)
    if shutil.disk_usage(root).free < raw_size + (2 << 30):
        raise RuntimeError("コーパスの作成に必要な空き容量がありません")
    started = time.perf_counter()
    rng = random.Random(3090)
    manifest = []
    with zipfile.ZipFile(partial, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=level) as archive:
        for i in range(count):
            entry_size, entry_pattern = size, pattern
            method = zipfile.ZIP_DEFLATED
            if pattern == "mixed":
                entry_pattern = "half"
                if i % 32 == 0:
                    entry_size = MIB
                elif i % 8 == 0:
                    entry_size = 0
                elif i % 4 == 0:
                    method = zipfile.ZIP_STORED
            elif pattern == "mixed-large":
                entry_pattern = "half"
                if i % 8192 == 0:
                    entry_size = 512 * MIB
                elif i % 8 == 0:
                    entry_size = 0
                elif i % 4 == 0:
                    method = zipfile.ZIP_STORED
            filename = f"group-{i % 64:02d}/entry-{i:05d}.bin"
            if name.startswith("flat-"):
                filename = f"entry-{i:06d}.bin"
            elif name == "deep-tiny":
                filename = f"group-{i % 64:02d}/" + "/".join(f"level-{depth}" for depth in range(8)) + f"/entry-{i:05d}.bin"
            elif name == "wide-tiny":
                filename = f"group-{i % 64:02d}/folder-{i:05d}/entry.bin"
            info = zipfile.ZipInfo(filename, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = method
            info._compresslevel = level
            info.external_attr = 0o100644 << 16
            digest = hashlib.sha256()
            with archive.open(info, "w", force_zip64=True) as out:
                left = entry_size
                while left:
                    block = payload(rng, min(left, MIB), entry_pattern)
                    out.write(block)
                    digest.update(block)
                    left -= len(block)
            manifest.append({"name": info.filename, "bytes": entry_size, "sha256": digest.hexdigest()})
    # 別プロセスが同名ZIPを作った場合も置換しない。
    archive_path.hardlink_to(partial)
    partial.unlink()
    report = {"case": name, "seed": 3090, "compression_level": level, "archive_bytes": archive_path.stat().st_size,
              "uncompressed_bytes": sum(e["bytes"] for e in manifest), "files": count,
              "creation_seconds": time.perf_counter() - started, "entries": manifest}
    with manifest_path.open("x", encoding="utf-8") as out:
        json.dump(report, out, ensure_ascii=False, indent=2)
        out.write("\n")
    print(json.dumps({k: v for k, v in report.items() if k != "entries"}), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cases", nargs="+", choices=CASES, default=list(CASES))
    parser.add_argument("--level", type=int, choices=range(10), default=6)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for name in args.cases:
        create(args.output, name, args.level)


if __name__ == "__main__":
    main()
