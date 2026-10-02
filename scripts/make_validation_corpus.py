"""OSS検証用の追加コーパス。固定seedで生成し、私有データを使わない。"""
import argparse
import hashlib
import json
from pathlib import Path
import random
import shutil
import time
import zipfile


PROFILES = {
    "stored-small": (16384, 4096, "random", 0, 0),
    "stored-medium": (1024, 256 << 10, "random", 0, 0),
    "stored-large": (4, 128 << 20, "random", 0, 0),
    "deflate-level0": (32, 16 << 20, "random", 8, 0),
    "incompressible": (32, 8 << 20, "random", 8, 6),
    "zeros-medium": (4096, 64 << 10, "zeros", 8, 6),
    "documents-level1": (8192, 8192, "json", 8, 1),
    "documents-level9": (8192, 8192, "json", 8, 9),
    "unicode-paths": (2048, 4096, "json", 8, 6),
    "empty-directories": (512, 128, "json", 8, 6),
    "many-empty": (32768, 0, "zeros", 8, 6),
    "skewed": (8192, 4096, "skewed", 8, 6),
}


def create(root, profile):
    count, size, pattern, method, level = PROFILES[profile]
    archive_path, manifest_path = root / f"{profile}.zip", root / f"{profile}.json"
    partial = root / f"{profile}.zip.part"
    if any(p.exists() for p in (archive_path, manifest_path, partial)):
        raise FileExistsError(f"既存ファイルを保護します: {profile}")
    expected = count * size + ((512 << 20) if pattern == "skewed" else 0)
    if shutil.disk_usage(root).free < expected + (2 << 30):
        raise RuntimeError("コーパス作成の空き容量不足です")
    rng = random.Random(79453090)
    entries, directories = [], []
    start = time.perf_counter()
    with zipfile.ZipFile(partial, "x") as archive:
        if profile == "empty-directories":
            directories = [f"empty-{i:05d}/nested/" for i in range(8192)]
            for name in directories:
                info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
                info.external_attr = (0o040755 << 16) | 0x10
                archive.writestr(info, b"")
        for index in range(count):
            name = f"group-{index % 64:02d}/entry-{index:06d}.bin"
            if profile == "unicode-paths":
                name = f"日本語-{index % 64:02d}/café/模型🧪-{index:06d}.json"
            entry_size = (512 << 20) if pattern == "skewed" and index == 0 else size
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.external_attr = 0o100644 << 16
            info.compress_type, info._compresslevel = method, level
            digest = hashlib.sha256()
            with archive.open(info, "w", force_zip64=True) as output:
                left = entry_size
                while left:
                    length = min(left, 1 << 20)
                    if pattern == "random" or (pattern == "skewed" and index == 0):
                        block = rng.randbytes(length)
                    elif pattern == "zeros":
                        block = bytes(length)
                    else:
                        record = json.dumps({"id": index, "text": "学習用テキスト / reproducible corpus",
                                             "value": rng.randrange(1000000)}, ensure_ascii=False).encode() + b"\n"
                        block = (record * ((length + len(record) - 1) // len(record)))[:length]
                    output.write(block)
                    digest.update(block)
                    left -= length
            entries.append({"name": name, "bytes": entry_size, "sha256": digest.hexdigest()})
    archive_path.hardlink_to(partial)
    partial.unlink()
    manifest = {"case": profile, "seed": 79453090, "files": len(entries), "directories": directories,
                "archive_bytes": archive_path.stat().st_size, "uncompressed_bytes": sum(e["bytes"] for e in entries),
                "method": method, "compression_level": level, "creation_seconds": time.perf_counter() - start,
                "entries": entries}
    with manifest_path.open("x", encoding="utf-8") as output:
        json.dump(manifest, output, ensure_ascii=False, indent=2)
        output.write("\n")
    print(json.dumps({k: v for k, v in manifest.items() if k not in ("entries", "directories")}), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cases", nargs="+", choices=PROFILES, default=list(PROFILES))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for profile in args.cases:
        create(args.output, profile)


if __name__ == "__main__":
    main()
