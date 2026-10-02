"""比較用の独立した並列CPU参照実装。GIPUやlibdeflateには依存しない。

既知の安全な試験ZIP専用。各workerが独立したZipFileを開き、zlibで解凍する。
汎用の安全な解凍製品として配布するものではない。
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("test", "extract"))
    parser.add_argument("archive", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--threads", type=int, default=16)
    args = parser.parse_args()
    if not 1 <= args.threads <= 32 or (args.mode == "extract" and args.output is None):
        parser.error("1〜32 workerと実展開時のoutputが必要です")
    with zipfile.ZipFile(args.archive) as archive:
        members = archive.infolist()
    names = set()
    for member in members:
        name = member.filename.rstrip("/")
        if (not name or any(p in ("", ".", "..") for p in name.split("/"))
                or "\\" in name or ":" in name or "\0" in name or name in names):
            parser.error("安全な相対パス・重複なしの比較用ZIPだけを使用してください")
        names.add(name)
        file_type = (member.external_attr >> 16) & 0o170000
        if file_type not in (0, 0o040000 if member.is_dir() else 0o100000):
            parser.error("特殊ファイルを含む入力は測定しません")
        if member.flag_bits & 1 or member.compress_type not in (0, 8):
            parser.error("暗号化なしのStored／Deflateだけに対応します")
    files = [m for m in members if not m.is_dir()]
    if args.mode == "extract":
        args.output.mkdir(parents=True, exist_ok=False)
        parents = {args.output / m.filename.rstrip("/") for m in members if m.is_dir()}
        parents.update((args.output / m.filename).parent for m in files)
        for parent in sorted(parents, key=lambda p: len(p.parts)):
            parent.mkdir(parents=True, exist_ok=True)

    def worker(batch):
        count = size = 0
        with zipfile.ZipFile(args.archive) as archive:
            for member in batch:
                # EOFまで読み、ZipExtFileのサイズ・全件CRC検証を完了させる。
                with archive.open(member) as source:
                    target = (args.output / member.filename).open("xb") if args.mode == "extract" else None
                    try:
                        while block := source.read(1 << 20):
                            size += len(block)
                            if target is not None:
                                target.write(block)
                    finally:
                        if target is not None:
                            target.close()
                count += 1
        return count, size

    batches = [files[i::args.threads] for i in range(min(args.threads, max(1, len(files))))]
    with ThreadPoolExecutor(max_workers=args.threads) as pool:
        results = list(pool.map(worker, batches))
    print(json.dumps({"files": sum(r[0] for r in results), "bytes": sum(r[1] for r in results),
                      "crc_verified_files": len(files), "threads_requested": args.threads}))


if __name__ == "__main__":
    main()
