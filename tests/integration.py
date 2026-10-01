"""標準ライブラリだけでZIP互換性と出力保護を検証する。"""
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
import zlib

BINARY = str(Path(sys.argv.pop(1)).resolve())
BACKEND = os.environ.get("GIPU_TEST_BACKEND", "cpu")
MODE = os.environ.get("GIPU_TEST_MODE", "auto")


class Unseekable(io.BytesIO):
    def seekable(self):
        return False

    def seek(self, *args):
        raise io.UnsupportedOperation("seek")


def zip_bytes(entries, *, method=zipfile.ZIP_DEFLATED, descriptor=False, force_zip64=False):
    data = Unseekable() if descriptor else io.BytesIO()
    with zipfile.ZipFile(data, "w", compression=method) as archive:
        for name, payload in entries:
            if force_zip64:
                info = zipfile.ZipInfo(name)
                info.compress_type = method
                with archive.open(info, "w", force_zip64=True) as out:
                    out.write(payload)
            else:
                archive.writestr(name, payload)
    return data.getvalue()


def full_zip64(data):
    end = data.rfind(b"PK\x05\x06")
    fields = struct.unpack_from("<4s4H2IH", data, end)
    count, cd_size, cd_offset = fields[4], fields[5], fields[6]
    record = struct.pack("<4sQ2H2I4Q", b"PK\x06\x06", 44, 45, 45, 0, 0, count, count, cd_size, cd_offset)
    locator = struct.pack("<4sIQI", b"PK\x06\x07", 0, end, 1)
    eocd = struct.pack("<4s4H2IH", b"PK\x05\x06", 0, 0, 65535, 65535, 0xffffffff, 0xffffffff, 0)
    return data[:end] + record + locator + eocd


class Integration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gipu-test-")
        self.root = Path(self.temp.name)
        self.archive = self.root / "input.zip"
        self.out = self.root / "out"

    def tearDown(self):
        self.temp.cleanup()

    def run_cli(self, *args, ok=True):
        result = subprocess.run([BINARY, *map(str, args)], capture_output=True, text=True, timeout=120)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def process(self, data, *, ok=True, extract=True, extra=()):
        self.archive.write_bytes(data)
        args = ["extract" if extract else "test", self.archive, "--backend", BACKEND, "--gpu-mode", MODE,
                "--threads", os.environ.get("GIPU_TEST_THREADS", "1")]
        if extract:
            args += ["--output", self.out]
        return self.run_cli(*args, *extra, ok=ok)

    def test_mixed_archive(self):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as archive:
            archive.writestr("folder/", b"")
            archive.writestr("folder/日本語.txt", "こんにちは\n".encode(), compress_type=zipfile.ZIP_DEFLATED)
            archive.writestr("raw.bin", bytes(range(256)) * 1024, compress_type=zipfile.ZIP_STORED)
            archive.writestr("empty.txt", b"", compress_type=zipfile.ZIP_DEFLATED)
            archive.comment = b"comment PK\x05\x06 end"
        result = self.process(buffer.getvalue(), extra=("--json", "--sync"))
        self.assertEqual(json.loads(result.stdout)["files"], 3)
        self.assertEqual((self.out / "folder/日本語.txt").read_text(), "こんにちは\n")
        self.assertEqual((self.out / "raw.bin").read_bytes(), bytes(range(256)) * 1024)
        self.assertEqual((self.out / "empty.txt").stat().st_size, 0)

    def test_empty_archive(self):
        self.process(zip_bytes([]))

    def test_streaming_and_stored_blocks(self):
        payload = os.urandom(2 * 1024 * 1024 + 71)
        self.process(zip_bytes([("large.bin", payload)]))
        self.assertEqual((self.out / "large.bin").read_bytes(), payload)

    def test_zip64_and_descriptor(self):
        for descriptor in (False, True):
            with self.subTest(descriptor=descriptor):
                data = full_zip64(zip_bytes([("zip64.bin", b"zip64" * 100000)], descriptor=descriptor, force_zip64=True))
                self.process(data, extract=False)

    def test_descriptor(self):
        self.process(zip_bytes([("data.bin", b"descriptor" * 20000)], descriptor=True))
        self.assertEqual((self.out / "data.bin").read_bytes(), b"descriptor" * 20000)

    def test_unsigned_descriptor(self):
        data = bytearray(zip_bytes([("data.bin", b"descriptor" * 20000)], descriptor=True))
        descriptor = data.index(b"PK\x07\x08")
        del data[descriptor:descriptor + 4]
        end = data.rfind(b"PK\x05\x06")
        offset = struct.unpack_from("<I", data, end + 16)[0]
        struct.pack_into("<I", data, end + 16, offset - 4)
        self.process(data, extract=False)

    def test_zip64_central_sizes_and_offset(self):
        data = bytearray(zip_bytes([("wide.bin", b"zip64" * 1000)], force_zip64=True))
        cd = data.index(b"PK\x01\x02")
        compressed, uncompressed = struct.unpack_from("<II", data, cd + 20)
        local_offset = struct.unpack_from("<I", data, cd + 42)[0]
        name_length = struct.unpack_from("<H", data, cd + 28)[0]
        extra = struct.pack("<HHQQQ", 1, 24, uncompressed, compressed, local_offset)
        struct.pack_into("<II", data, cd + 20, 0xffffffff, 0xffffffff)
        struct.pack_into("<I", data, cd + 42, 0xffffffff)
        struct.pack_into("<H", data, cd + 30, len(extra))
        position = cd + 46 + name_length
        data[position:position] = extra
        end = data.rfind(b"PK\x05\x06")
        size = struct.unpack_from("<I", data, end + 12)[0]
        struct.pack_into("<I", data, end + 12, size + len(extra))
        self.process(full_zip64(data), extract=False)

    @unittest.skipUnless(BACKEND == "gpu" and MODE == "auto", "GPU自動スケジューラのテスト")
    def test_gpu_memory_budget(self):
        entries = [(f"{i}.bin", b"a" * (1 << 20)) for i in range(20)]
        result = self.process(zip_bytes(entries), extract=False, extra=("--vram-limit", "12M", "--json"))
        stats = json.loads(result.stdout)
        self.assertGreater(stats["gpu_batches"], 1)
        self.assertLessEqual(stats["workspace_bytes"], 12 << 20)
        result = self.process(zip_bytes([("big.bin", b"a" * (32 << 20))]), extract=False,
                              extra=("--vram-limit", "16M", "--json"))
        stats = json.loads(result.stdout)
        self.assertEqual(stats["gpu_streams"], 1)
        self.assertEqual(stats["gpu_batches"], 0)
        self.assertLessEqual(stats["workspace_bytes"], 16 << 20)
        # バッチの後にStreamingを選んでもarenaを解放して予算を守る。
        result = self.process(zip_bytes([("small", b"a" * (8 << 20)), ("large", b"b" * (32 << 20))]), extract=False,
                              extra=("--vram-limit", "16M", "--json"))
        stats = json.loads(result.stdout)
        self.assertEqual(stats["gpu_batches"], 1)
        self.assertEqual(stats["gpu_streams"], 1)
        self.assertLessEqual(stats["workspace_bytes"], 16 << 20)

    @unittest.skipUnless(BACKEND == "gpu", "GPU予算のテスト")
    def test_insufficient_gpu_budget(self):
        self.process(zip_bytes([("file", b"data")]), extract=False, ok=False, extra=("--vram-limit", "4M"))

    def test_cp437(self):
        data = zip_bytes([("x.txt", b"cp437")]).replace(b"x.txt", b"\x82.txt")
        self.process(data)
        self.assertEqual((self.out / "é.txt").read_bytes(), b"cp437")

    def test_many_entries(self):
        entries = [(f"entries/{i}.txt", (f"value={i}\n" * 1000).encode()) for i in range(48)]
        self.process(zip_bytes(entries))
        for name, payload in entries:
            self.assertEqual((self.out / name).read_bytes(), payload)

    @unittest.skipUnless(BACKEND == "gpu" and MODE != "stream", "GPUパイプラインのテスト")
    def test_pipeline(self):
        entries = [(f"pipeline/{i}.bin", bytes(range(256)) * (32 + i)) for i in range(20)]
        result = self.process(zip_bytes(entries), extra=("--pipeline", "--batch-entries", "3", "--json", "--sync"))
        self.assertGreater(json.loads(result.stdout)["gpu_batches"], 1)
        for name, payload in entries:
            self.assertEqual((self.out / name).read_bytes(), payload)
        self.process(zip_bytes([("stored", b"x")], method=zipfile.ZIP_STORED), extract=False, ok=False, extra=("--pipeline",))
        self.process(zip_bytes([("empty", b"")]), extract=False, ok=False, extra=("--pipeline",))
        self.process(zip_bytes([("large", b"a" * (32 << 20))]), extract=False, ok=False,
                     extra=("--pipeline", "--vram-limit", "16M"))

    def test_invalid_counts(self):
        for flag, value in (("--threads", "0"), ("--threads", "33"), ("--threads", "-1"),
                            ("--batch-entries", "0"), ("--batch-entries", "65537"), ("--batch-entries", "3x")):
            with self.subTest(flag=flag, value=value):
                self.process(zip_bytes([("file", b"data")]), ok=False, extra=(flag, value))

    def test_unsafe_paths(self):
        for name in ("../escape", "/absolute", "a/../../escape", "C:/drive", "a\\..\\escape", "a//b", "./dot"):
            with self.subTest(name=name):
                self.process(zip_bytes([(name, b"evil")]), ok=False)
        self.assertFalse((self.root / "escape").exists())

    def test_duplicate_and_conflicting_paths(self):
        for names in (("same", "same"), ("file", "file/child"), ("dir/", "dir")):
            with self.subTest(names=names):
                self.process(zip_bytes([(n, b"" if n.endswith("/") else b"data") for n in names]), ok=False)

    def test_archive_symlink(self):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as archive:
            info = zipfile.ZipInfo("link")
            info.create_system = 3
            info.external_attr = 0o120777 << 16
            archive.writestr(info, "../target")
        self.process(buffer.getvalue(), ok=False)

    def test_output_symlink(self):
        outside = self.root / "outside"
        outside.mkdir()
        self.out.mkdir()
        (self.out / "linked").symlink_to(outside, target_is_directory=True)
        self.process(zip_bytes([("linked/escape", b"data")]), ok=False)
        self.assertFalse((outside / "escape").exists())

    def test_no_overwrite(self):
        self.out.mkdir()
        target = self.out / "existing"
        target.write_bytes(b"original")
        self.process(zip_bytes([("existing", b"new")]), ok=False)
        self.assertEqual(target.read_bytes(), b"original")

    def test_crc_and_size_failure_cleanup(self):
        data = bytearray(zip_bytes([("bad.bin", b"data" * 4000)], method=zipfile.ZIP_STORED))
        cd = data.index(b"PK\x01\x02")
        crc = struct.unpack_from("<I", data, cd + 16)[0] ^ 1
        struct.pack_into("<I", data, 14, crc)
        struct.pack_into("<I", data, cd + 16, crc)
        self.process(data, ok=False)
        self.assertFalse((self.out / "bad.bin").exists())
        self.assertEqual(list(self.out.glob("*.part")), [])

    def test_limits(self):
        self.process(zip_bytes([("big", b"x" * 5000)]), ok=False, extra=("--max-output", "1K"))
        self.assertFalse(self.out.exists())

    def test_bad_headers(self):
        original = zip_bytes([("data", b"value")])
        cd = original.index(b"PK\x01\x02")
        for offset, replacement in ((cd + 42, 0xffffff00), (cd + 20, 0xffffff00)):
            data = bytearray(original)
            struct.pack_into("<I", data, offset, replacement)
            self.process(data, ok=False)
        self.process(original[:-7], ok=False)
        self.process(b"not a zip", ok=False)

    def test_local_mismatch_and_encryption(self):
        original = zip_bytes([("file", b"data")])
        data = bytearray(original)
        data[30] = ord("X")
        self.process(data, ok=False)
        data = bytearray(original)
        cd = data.index(b"PK\x01\x02")
        struct.pack_into("<H", data, 6, 1)
        struct.pack_into("<H", data, cd + 8, 1)
        self.process(data, ok=False)

    def test_unsupported_codec(self):
        self.process(zip_bytes([("file", b"data")], method=zipfile.ZIP_BZIP2), ok=False)
        self.assertFalse(self.out.exists())

    def test_test_writes_nothing(self):
        self.process(zip_bytes([("file", b"data")]), extract=False)
        self.assertFalse(self.out.exists())


if __name__ == "__main__":
    unittest.main()
