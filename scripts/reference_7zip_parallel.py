"""既知の安全なZIPを、独立した複数の公式7-Zipプロセスで分担する。

7-Zip本体のZIP並列対応を主張するものではない。workerごとに全メタデータを
再解析するコスト、フィルタ一覧作成、親ディレクトリ作成もCLI時間に含む。
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile


def result_counts(stdout):
    sizes = re.findall(r"^Size:[ \t]+(\d+)[ \t]*$", stdout, re.MULTILINE)
    counts = re.findall(r"^Files:[ \t]+(\d+)[ \t]*$", stdout, re.MULTILINE)
    if len(sizes) != 1 or len(counts) > 1 or "Everything is Ok" not in stdout:
        raise RuntimeError("7-Zipの全件CRC検証結果を確認できません")
    return int(counts[0]) if counts else 1, int(sizes[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("test", "extract"))
    parser.add_argument("archive", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--threads", type=int, default=16)
    parser.add_argument("--sevenzip", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.threads <= 32 or (args.mode == "extract" and args.output is None):
        parser.error("1〜32 workerと実展開時のoutputが必要です")
    with zipfile.ZipFile(args.archive) as archive:
        members = archive.infolist()
    names = set()
    for member in members:
        name = member.filename.rstrip("/")
        if (not name or any(p in ("", ".", "..") for p in name.split("/"))
                or "\\" in name or ":" in name or name in names
                or any(ord(c) < 32 or ord(c) == 127 for c in name)):
            parser.error("安全な相対パス・重複なしの比較用ZIPだけを使用してください")
        names.add(name)
        file_type = (member.external_attr >> 16) & 0o170000
        if file_type not in (0, 0o040000 if member.is_dir() else 0o100000):
            parser.error("特殊ファイルを含む入力は測定しません")
        if member.flag_bits & 1 or member.compress_type not in (0, 8):
            parser.error("暗号化なしのStored／Deflateだけに対応します")
    files = [m for m in members if not m.is_dir()]
    file_names = {m.filename for m in files}
    for name in names:
        if any(name[:end] in file_names for end, c in enumerate(name) if c == "/"):
            parser.error("ファイルとディレクトリの衝突を含む入力は測定しません")
    if args.mode == "extract":
        args.output.mkdir(parents=True, exist_ok=False)
        parents = {args.output / m.filename.rstrip("/") for m in members if m.is_dir()}
        parents.update((args.output / m.filename).parent for m in files)
        for parent in sorted(parents, key=lambda p: len(p.parts)):
            parent.mkdir(parents=True, exist_ok=True)
    # 固定順のround-robin。自分のworker以外のファイルは選択しない。
    batches = [files[i::args.threads] for i in range(min(args.threads, len(files)))]
    with tempfile.TemporaryDirectory(prefix="gipu-7zip-reference-") as temporary:
        def worker(item):
            index, batch = item
            listing = Path(temporary) / f"worker-{index}.txt"
            listing.write_text("\n".join(m.filename for m in batch) + "\n", encoding="utf-8")
            command = [str(args.sevenzip.resolve()), "t" if args.mode == "test" else "x",
                       str(args.archive.resolve()), "-mmt=1", "-bd", "-bb0", "-bsp0",
                       "-scsUTF-8", "-ssc", "-spd", f"-i@{listing}"]
            if args.mode == "extract":
                command += [f"-o{args.output.resolve()}", "-y"]
            # matrixが所有するprocess groupを継承。中断時に子だけ残さない。
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, env=dict(os.environ, LC_ALL="C"))
            if result.returncode:
                raise RuntimeError(f"7-Zip worker {index} が失敗しました: {result.returncode}")
            actual = result_counts(result.stdout)
            expected = len(batch), sum(m.file_size for m in batch)
            if actual != expected:
                raise RuntimeError("7-Zipの選択件数・全件CRC検証量が一致しません")
            return actual
        with ThreadPoolExecutor(max_workers=args.threads) as pool:
            results = list(pool.map(worker, enumerate(batches)))
    print(json.dumps({"files": sum(r[0] for r in results), "bytes": sum(r[1] for r in results),
                      "crc_verified_files": len(files), "threads_requested": args.threads,
                      "sevenzip_processes": len(batches), "sevenzip_threads_each": 1}))


if __name__ == "__main__":
    main()
