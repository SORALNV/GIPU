"""外部ツールが作る暗号化ZIP／Deflate64／分割・非ZIPの拒否を検証する。"""
import argparse
import gzip
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import time
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--sevenzip", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists():
        parser.error("既存結果を上書きしません")
    binary, sevenzip = args.binary.resolve(strict=True), args.sevenzip.resolve(strict=True)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    rows = []
    with tempfile.TemporaryDirectory(prefix="gipu-formats-") as temporary:
        root = Path(temporary)
        payload = bytes(range(256)) * 8192
        (root / "data.bin").write_bytes(payload)
        specifications = [
            ("deflate", "deflate.zip", ["-tzip", "-mm=Deflate"], True),
            ("stored", "stored.zip", ["-tzip", "-mx=0"], True),
            ("deflate64", "deflate64.zip", ["-tzip", "-mm=Deflate64"], False),
            ("bzip2", "bzip2.zip", ["-tzip", "-mm=BZip2"], False),
            ("lzma", "lzma.zip", ["-tzip", "-mm=LZMA"], False),
            ("aes256", "aes.zip", ["-tzip", "-mem=AES256", "-ppublic-test-password"], False),
            ("zipcrypto", "zipcrypto.zip", ["-tzip", "-mem=ZipCrypto", "-ppublic-test-password"], False),
            ("7z", "archive.7z", ["-t7z"], False),
            ("multipart", "multipart.zip", ["-tzip", "-mx=0", "-v256k"], False),
        ]
        for name, filename, options, supported in specifications:
            archive = root / filename
            subprocess.run([str(sevenzip), "a", *options, str(archive), "data.bin"], cwd=root,
                           capture_output=True, text=True, check=True, timeout=60)
            if name == "multipart":
                archive = root / (filename + ".001")
            command = [str(sevenzip), "t", str(archive), "-ppublic-test-password", "-bso0", "-bsp0"]
            subprocess.run(command, capture_output=True, text=True, check=True, timeout=60)
            output = root / f"output-{name}"
            started = time.perf_counter()
            result = subprocess.run([str(binary), "extract", str(archive), "--backend", "cpu", "--output", str(output)],
                                    capture_output=True, text=True, timeout=60)
            correct = ((result.returncode == 0 and (output / "data.bin").read_bytes() == payload) if supported else
                       (result.returncode != 0 and not output.exists()))
            rows.append({"format": name, "expected": "supported" if supported else "reject_before_output",
                         "verified": correct, "reference_archive_valid": True, "returncode": result.returncode,
                         "seconds": time.perf_counter() - started, "stderr": result.stderr.strip()[:300]})
        for name in ("gzip", "tar-gzip"):
            archive = root / f"{name}.bin"
            if name == "gzip":
                archive.write_bytes(gzip.compress(payload))
            else:
                with tarfile.open(archive, "w:gz") as tar:
                    info = tarfile.TarInfo("data.bin")
                    info.size = len(payload)
                    tar.addfile(info, io.BytesIO(payload))
            output = root / f"output-{name}"
            result = subprocess.run([str(binary), "extract", str(archive), "--backend", "cpu", "--output", str(output)],
                                    capture_output=True, text=True, timeout=60)
            rows.append({"format": name, "expected": "reject_before_output", "verified": result.returncode != 0 and not output.exists(),
                         "returncode": result.returncode, "stderr": result.stderr.strip()[:300]})
    report = {"time_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "runs": rows,
              "all_passed": all(r["verified"] for r in rows),
              "note": "拒否は対応・速度の証明ではない。GPUへ未対応・不正な圧縮本体を送らない。"}
    with args.report.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    print(json.dumps(report, ensure_ascii=False))
    if not report["all_passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
