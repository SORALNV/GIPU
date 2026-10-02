"""標準ライブラリだけでZIP互換性と出力保護を検証する。"""
import io
import json
import os
import resource
import signal
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
ALGORITHM = os.environ.get("GIPU_TEST_ALGORITHM", "deflate")


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


def replace_deflate(data, compressed):
    """単一エントリのZIP構造を整合させたまま圧縮本体だけを置き換える。"""
    old_cd = data.index(b"PK\x01\x02")
    name_size, extra_size = struct.unpack_from("<HH", data, 26)
    start = 30 + name_size + extra_size
    result = bytearray(data[:start] + compressed + data[old_cd:])
    cd = start + len(compressed)
    struct.pack_into("<I", result, 18, len(compressed))
    struct.pack_into("<I", result, cd + 20, len(compressed))
    end = result.rfind(b"PK\x05\x06")
    struct.pack_into("<I", result, end + 16, cd)
    return result


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
                "--gpu-algorithm", ALGORITHM,
                "--temp-mode", os.environ.get("GIPU_TEST_TEMP_MODE", "named"),
                "--path-mode", os.environ.get("GIPU_TEST_PATH_MODE", "auto"),
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

    def test_parallel_metadata_validation(self):
        original = zip_bytes([(f"meta/{i:04d}", b"x" * 64) for i in range(1024)], descriptor=True, force_zip64=True)
        self.archive.write_bytes(original)
        expected = self.run_cli("list", self.archive, "--metadata-threads", "1").stdout
        self.assertEqual(self.run_cli("list", self.archive, "--metadata-threads", "4").stdout, expected)
        stats = json.loads(self.run_cli("test", self.archive, "--backend", "cpu",
                                      "--metadata-threads", "4", "--json").stdout)
        self.assertEqual(stats["metadata_threads"], 4)
        self.assertEqual(stats["files"], 1024)
        with zipfile.ZipFile(io.BytesIO(original)) as archive:
            offset = archive.infolist()[700].header_offset
        damaged = bytearray(original)
        damaged[offset + 30] ^= 1
        self.archive.write_bytes(damaged)
        self.run_cli("extract", self.archive, "--backend", BACKEND, "--metadata-threads", "4",
                     "--output", self.out, ok=False)
        self.assertFalse(self.out.exists())
        for value in ("0", "33", "-1", "2x"):
            self.run_cli("list", self.archive, "--metadata-threads", value, ok=False)

    def test_anonymous_output(self):
        result = self.process(zip_bytes([(f"{i}.bin", b"value" * 1000) for i in range(10)]),
                              extra=("--temp-mode", "auto", "--json", "--sync"))
        stats = json.loads(result.stdout)
        self.assertEqual(stats["anonymous_output_files"] + stats["named_output_files"], 10)
        if hasattr(os, "O_TMPFILE") and Path("/proc/self/fd").is_dir():
            try:
                fd = os.open(self.out, os.O_TMPFILE | os.O_RDWR, 0o600)
            except OSError:
                pass
            else:
                os.close(fd)
                self.assertEqual(stats["anonymous_output_files"], 10)
        self.assertEqual(list(self.out.glob("*.part")), [])
        for i in range(10):
            self.assertEqual((self.out / f"{i}.bin").read_bytes(), b"value" * 1000)

    def test_metadata_read_windows(self):
        data = io.BytesIO()
        expected = {}
        with zipfile.ZipFile(data, "w") as archive:
            for i in range(8):
                info = zipfile.ZipInfo(f"long-metadata-{i}.txt")
                info.compress_type = zipfile.ZIP_DEFLATED
                info.extra = struct.pack("<HH", 0xcafe, 65516) + b"x" * 65516
                info.comment = b"c" * 65535
                expected[info.filename] = f"payload {i}".encode()
                archive.writestr(info, expected[info.filename])
        self.process(data.getvalue())
        for name, payload in expected.items():
            self.assertEqual((self.out / name).read_bytes(), payload)

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
        result = self.process(zip_bytes([("small", b"a" * (8 << 20)), ("large", b"b" * (64 << 20))]), extract=False,
                              extra=("--vram-limit", "32M", "--json"))
        stats = json.loads(result.stdout)
        self.assertEqual(stats["gpu_batches"], 1)
        self.assertEqual(stats["gpu_streams"], 1)
        self.assertLessEqual(stats["workspace_bytes"], 32 << 20)

    @unittest.skipUnless(BACKEND == "gpu" and MODE != "stream", "Stored／空でバッチを分断しない")
    def test_mixed_entries_coalesced(self):
        out = io.BytesIO()
        payload = b"coalesce" * 8192
        with zipfile.ZipFile(out, "w") as archive:
            for i in range(16):
                archive.writestr(f"raw-{i}", b"copy", compress_type=zipfile.ZIP_STORED)
                archive.writestr(f"empty-{i}", b"", compress_type=zipfile.ZIP_DEFLATED)
                archive.writestr(f"deflate-{i}", payload, compress_type=zipfile.ZIP_DEFLATED)
        result = self.process(out.getvalue(), extra=("--json",))
        stats = json.loads(result.stdout)
        self.assertEqual(stats["gpu_batches"], 1)
        self.assertEqual(stats["gpu_streams"], 0)
        self.assertEqual(stats["cpu_crc_bytes"], 16 * 4)
        for i in range(16):
            self.assertEqual((self.out / f"deflate-{i}").read_bytes(), payload)
            self.assertEqual((self.out / f"raw-{i}").read_bytes(), b"copy")
            self.assertEqual((self.out / f"empty-{i}").read_bytes(), b"")

    @unittest.skipUnless(BACKEND == "gpu", "GPU予算のテスト")
    def test_insufficient_gpu_budget(self):
        self.process(zip_bytes([("file", b"data")]), extract=False, ok=False, extra=("--vram-limit", "4M"))

    @unittest.skipUnless(BACKEND == "gpu" and MODE != "stream", "GPU CRCの区間結合")
    def test_gpu_crc_chunk_boundaries(self):
        sizes = [1, 4095, 4096, 4097, (1 << 20) - 1, 1 << 20, (1 << 20) + 1, (3 << 20) + 39]
        entries = [(f"crc/{i}", (bytes(range(251)) * (size // 251 + 1))[:size]) for i, size in enumerate(sizes)]
        data = zip_bytes(entries)
        for chunk, width in (("4K", 4096), ("1M", 1 << 20), ("64M", 64 << 20), ("whole", None)):
            result = self.process(data, extract=False, extra=("--gpu-crc-chunk", chunk, "--json"))
            stats = json.loads(result.stdout)
            expected = sum((size + width - 1) // width for size in sizes) if width else len(sizes)
            self.assertEqual(stats["gpu_crc_chunks"], expected)
            self.assertEqual(stats["gpu_crc_bytes"], sum(sizes))
        self.process(data, extra=("--gpu-crc-chunk", "1M"))
        for name, payload in entries:
            self.assertEqual((self.out / name).read_bytes(), payload)

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
        # デコードできてもCRCが異なるバッチは、出力workerへ渡さない。
        data = bytearray(zip_bytes([("bad-crc", b"valid deflate" * 1000)]))
        cd = data.index(b"PK\x01\x02")
        wrong_crc = struct.unpack_from("<I", data, cd + 16)[0] ^ 1
        struct.pack_into("<I", data, 14, wrong_crc)
        struct.pack_into("<I", data, cd + 16, wrong_crc)
        self.process(data, ok=False, extra=("--pipeline",))
        self.assertFalse((self.out / "bad-crc").exists())
        self.assertEqual(list(self.out.rglob("*.part")), [])

    def test_invalid_counts(self):
        for flag, value in (("--threads", "0"), ("--threads", "33"), ("--threads", "-1"),
                            ("--write-threads", "0"), ("--write-threads", "33"),
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

    @unittest.skipUnless(BACKEND not in ("gpu", "hybrid"), "不正DeflateはCPU経路でのみ検証する")
    def test_corrupt_deflate_body(self):
        original = zip_bytes([("bad.bin", b"a" * (4 << 20))])
        cd = original.index(b"PK\x01\x02")
        start = 30 + len("bad.bin")
        valid = original[start:cd]
        for compressed in (b"\x07", valid[:-1], valid + b"trailing", b""):
            with self.subTest(compressed_bytes=len(compressed)):
                self.process(replace_deflate(original, compressed), ok=False)
                self.assertFalse((self.out / "bad.bin").exists())
                self.assertEqual(list(self.out.glob("*.part")), [])
                if BACKEND == "libdeflate":
                    self.process(replace_deflate(original, compressed), ok=False,
                                 extra=("--host-limit", "2M"))

    def test_limits(self):
        self.process(zip_bytes([("big", b"x" * 5000)]), ok=False, extra=("--max-output", "1K"))
        self.assertFalse(self.out.exists())
        self.process(zip_bytes([(f"{i}.txt", b"a") for i in range(100)]), ok=False,
                     extra=("--metadata-limit", "1K"))
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

    @unittest.skipUnless(BACKEND == "hybrid", "CPU/GPU分担とGPU不在時の検証")
    def test_hybrid_partition_and_fallback(self):
        data = io.BytesIO()
        payload = b"hybrid" * 90000
        with zipfile.ZipFile(data, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("folder/", b"")
            archive.writestr("folder/stored", b"stored", compress_type=zipfile.ZIP_STORED)
            archive.writestr("folder/empty", b"")
            for i in range(64):
                archive.writestr(f"folder/{i}", payload)
        result = self.process(data.getvalue(), extra=("--host-limit", "128M", "--json"))
        stats = json.loads(result.stdout)
        self.assertEqual(stats["files"], 66)
        self.assertGreater(stats["cpu_buffered_files"] + stats["cpu_stream_files"], 0)
        self.assertEqual(stats["cpu_crc_bytes"] + stats["gpu_crc_bytes"], stats["bytes"])
        self.assertLessEqual(stats["host_buffer_bytes"], 128 << 20)
        if os.environ.get("GIPU_EXPECT_HYBRID_GPU") == "1":
            self.assertEqual(stats["selected_backend"], "hybrid")
            self.assertGreater(stats["gpu_batches"], 0)
        for i in range(64):
            self.assertEqual((self.out / f"folder/{i}").read_bytes(), payload)
        result = self.process(data.getvalue(), extract=False, extra=("--gpu", "99999", "--json"))
        fallback = json.loads(result.stdout)
        self.assertEqual(fallback["gpu_batches"], 0)
        self.assertEqual(fallback["cpu_crc_bytes"], fallback["bytes"])
        self.assertEqual(fallback["selection_reason"], "gpu_unavailable")

    @unittest.skipUnless(BACKEND == "hybrid", "両経路の例外伝播と部分出力の検証")
    def test_hybrid_cpu_error_is_not_hidden(self):
        data = bytearray(zip_bytes([(f"{i}", b"x" * (1 << 20)) for i in range(32)]))
        cd = data.index(b"PK\x01\x02")
        bad_crc = struct.unpack_from("<I", data, cd + 16)[0] ^ 1
        struct.pack_into("<I", data, cd + 16, bad_crc)
        struct.pack_into("<I", data, 14, bad_crc)
        result = self.process(data, ok=False)
        self.assertIn("CRC32", result.stderr)
        self.assertFalse((self.out / "0").exists())
        self.assertEqual(list(self.out.rglob("*.part")), [])

    @unittest.skipUnless(BACKEND == "auto", "自動選択の既定は安全なCPU経路")
    def test_auto_small_does_not_require_gpu(self):
        result = self.process(zip_bytes([("small", b"hello" * 1000)]),
                              extra=("--gpu", "9999", "--auto-gpu", "--auto-parallel", "--json"))
        stats = json.loads(result.stdout)
        self.assertEqual(stats["gpu_batches"], 0)
        self.assertEqual(stats["gpu_streams"], 0)
        self.assertIn(stats["selected_backend"], ("cpu", "isal", "libdeflate"))
        self.assertTrue(stats["selection_reason"].startswith("auto_"))
        self.assertEqual((self.out / "small").read_bytes(), b"hello" * 1000)

    @unittest.skipUnless(BACKEND == "auto", "既定のCPU選択")
    def test_default_backend(self):
        self.archive.write_bytes(zip_bytes([("x", b"default-auto")]))
        stats = json.loads(self.run_cli("test", self.archive, "--json").stdout)
        self.assertEqual(stats["backend"], "auto")
        self.assertEqual(stats["selection_reason"], "auto_cpu_test")

    def test_output_file_limit_cleanup(self):
        self.archive.write_bytes(zip_bytes([("limited", b"limits" * 32768)]))
        def limits():
            resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024))
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        for mode in ("named", "auto"):
            with self.subTest(mode=mode):
                destination = self.root / mode
                result = subprocess.run([BINARY, "extract", str(self.archive), "--backend", BACKEND,
                                         "--gpu-mode", MODE, "--gpu-algorithm", ALGORITHM,
                                         "--output", str(destination), "--temp-mode", mode],
                                        preexec_fn=limits, capture_output=True, text=True, timeout=120)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((destination / "limited").exists())
                self.assertEqual(list(destination.rglob("*.part")), [])

    def test_output_fd_limit_cleanup(self):
        self.archive.write_bytes(zip_bytes([("folder/limited", b"limits" * 1000)]))
        def limits():
            resource.setrlimit(resource.RLIMIT_NOFILE, (6, 6))
        result = subprocess.run([BINARY, "extract", str(self.archive), "--backend", BACKEND,
                                 "--output", str(self.out), "--temp-mode", "named"],
                                preexec_fn=limits, capture_output=True, text=True, timeout=120)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.out / "folder/limited").exists())
        self.assertEqual(list(self.out.rglob("*.part")), [])

    def test_safe_parent_path_modes(self):
        payload = b"path-selection"
        self.archive.write_bytes(zip_bytes([("folder/a", payload), ("folder/b", payload)]))
        for mode in ("auto", "portable"):
            with self.subTest(mode=mode):
                out = self.root / mode
                stats = json.loads(self.run_cli("extract", self.archive, "--backend", BACKEND,
                                               "--path-mode", mode, "--output", out, "--json").stdout)
                self.assertEqual((out / "folder/a").read_bytes(), payload)
                self.assertEqual((out / "folder/b").read_bytes(), payload)
                self.assertGreaterEqual(stats["fast_parent_opens"] + stats["portable_parent_walks"], 2)
                if mode == "portable":
                    self.assertEqual(stats["fast_parent_opens"], 0)
                protected = self.root / f"protected-{mode}"
                protected.mkdir()
                outside = self.root / f"outside-{mode}"
                outside.mkdir()
                (protected / "folder").symlink_to(outside, target_is_directory=True)
                self.run_cli("extract", self.archive, "--backend", BACKEND, "--path-mode", mode,
                             "--output", protected, ok=False)
                self.assertEqual(list(outside.iterdir()), [])

    def test_deep_path_beyond_path_max(self):
        components = ["p" * 80] * 55
        self.process(zip_bytes([("/".join(components + ["end"]), b"deep")]))
        fd = os.open(self.out, os.O_RDONLY | os.O_DIRECTORY)
        try:
            for component in components:
                next_fd = os.open(component, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
                os.close(fd)
                fd = next_fd
            data = os.open("end", os.O_RDONLY | os.O_NOFOLLOW, dir_fd=fd)
            try:
                self.assertEqual(os.read(data, 16), b"deep")
            finally:
                os.close(data)
        finally:
            os.close(fd)

    @unittest.skipUnless(BACKEND not in ("gpu", "hybrid"), "CPUのDeflateブロック構成")
    def test_deflate_strategies_and_flush_boundaries(self):
        payload = bytes(range(256)) * 130 + b"same" * 20000
        original = zip_bytes([("x", payload)])
        for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FIXED, zlib.Z_HUFFMAN_ONLY, zlib.Z_RLE):
            for flush in (zlib.Z_SYNC_FLUSH, zlib.Z_FULL_FLUSH):
                with self.subTest(strategy=strategy, flush=flush):
                    compressor = zlib.compressobj(6, zlib.DEFLATED, -15, 8, strategy)
                    compressed = bytearray()
                    for offset in range(0, len(payload), 4097):
                        compressed += compressor.compress(payload[offset:offset + 4097])
                        compressed += compressor.flush(flush)
                    compressed += compressor.flush(zlib.Z_FINISH)
                    result = self.process(replace_deflate(original, compressed), extract=False, extra=("--json",))
                    self.assertEqual(json.loads(result.stdout)["bytes"], len(payload))

    @unittest.skipUnless(BACKEND == "libdeflate", "CPUの共有メモリ予算")
    def test_cpu_memory_budget_and_streaming(self):
        small = os.urandom(1 << 20)
        large = b"large-file" * (2 << 20)
        result = self.process(zip_bytes([("small", small), ("large", large)]),
                              extra=("--host-limit", "4M", "--threads", "1", "--json"))
        stats = json.loads(result.stdout)
        self.assertEqual(stats["cpu_buffered_files"], 1)
        self.assertEqual(stats["cpu_stream_files"], 1)
        self.assertLessEqual(stats["host_buffer_bytes"], 4 << 20)
        self.assertEqual((self.out / "small").read_bytes(), small)
        self.assertEqual((self.out / "large").read_bytes(), large)
        result = self.process(zip_bytes([(f"{i}", large) for i in range(5)]), extract=False,
                              extra=("--host-limit", "8M", "--threads", "32", "--json"))
        stats = json.loads(result.stdout)
        self.assertEqual(stats["cpu_stream_files"], 5)
        self.assertLessEqual(stats["host_buffer_bytes"], 8 << 20)

    @unittest.skipUnless(BACKEND in ("cpu", "libdeflate", "isal"), "CPUの最小メモリ予算")
    def test_cpu_insufficient_memory_budget(self):
        self.process(zip_bytes([("x", b"x")]), extract=False, ok=False, extra=("--host-limit", "1M"))

    @unittest.skipUnless(BACKEND == "gpu" and MODE == "auto", "固定化ホストバッファの予算")
    def test_gpu_host_memory_budget(self):
        result = self.process(zip_bytes([(f"{i}", b"x" * (1 << 20)) for i in range(12)]),
                              extra=("--host-limit", "16M", "--json"))
        stats = json.loads(result.stdout)
        self.assertGreater(stats["gpu_batches"], 1)
        self.assertLessEqual(stats["host_buffer_bytes"], 16 << 20)
        result = self.process(zip_bytes([("large", b"x" * (32 << 20))]), extract=False,
                              extra=("--host-limit", "12M", "--json"))
        self.assertEqual(json.loads(result.stdout)["gpu_streams"], 1)

    @unittest.skipUnless(BACKEND == "gpu" and MODE == "auto", "pipelineの入力／出力最大値の予算")
    def test_pipeline_packed_host_budget(self):
        entries = [(f"zeros-first-{i}", b"\0" * (2 << 20)) for i in range(4)]
        entries += [(f"random-{i}", os.urandom(1 << 20)) for i in range(8)]
        entries += [(f"zeros-last-{i}", b"\0" * (2 << 20)) for i in range(4)]
        result = self.process(zip_bytes(entries), extra=("--pipeline", "--host-limit", "30M", "--json"))
        stats = json.loads(result.stdout)
        self.assertLessEqual(stats["host_buffer_bytes"], 30 << 20)
        self.assertGreater(stats["gpu_batches"], 2)
        self.assertGreater(stats["pipeline_overlap_waits"], 0)
        for name, payload in entries:
            self.assertEqual((self.out / name).read_bytes(), payload)

    @unittest.skipUnless(BACKEND == "gpu" and ALGORITHM == "lookahead", "LOOKAHEADの選択確認")
    def test_lookahead_selected(self):
        result = self.process(zip_bytes([("data", b"lookahead" * (1 << 20))]), extract=False, extra=("--json",))
        stats = json.loads(result.stdout)
        self.assertEqual(stats["lookahead_batches"], 1)
        self.assertEqual(stats["gpu_crc_bytes"], 9 << 20)

    @unittest.skipUnless(BACKEND == "gpu" and MODE == "stream", "Streaming CRCの併用検証")
    def test_stream_cpu_crc_without_spool(self):
        payload = b"stream-crc" * (4 << 20)
        self.archive.write_bytes(zip_bytes([("large.bin", payload)]))
        # 使用できないTMPDIRでもCPU CRCのtestは一時展開を作らない。
        env = dict(os.environ, TMPDIR=str(self.root / "missing-temp-directory"))
        result = subprocess.run([BINARY, "test", str(self.archive), "--backend", "gpu",
                                 "--gpu-mode", "stream", "--stream-crc", "cpu", "--json"],
                                capture_output=True, text=True, timeout=120, env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        stats = json.loads(result.stdout)
        self.assertEqual(stats["cpu_crc_bytes"], len(payload))
        self.assertEqual(stats["gpu_crc_bytes"], 0)
        self.assertEqual(list(self.root.iterdir()), [self.archive])
        gpu = self.run_cli("test", self.archive, "--backend", "gpu", "--gpu-mode", "stream",
                           "--stream-crc", "gpu", "--json")
        self.assertEqual(json.loads(gpu.stdout)["gpu_crc_bytes"], len(payload))

    @unittest.skipUnless(BACKEND == "gpu" and MODE == "stream", "Streaming CRCの破損検出")
    def test_stream_cpu_crc_mismatch(self):
        data = bytearray(zip_bytes([("bad.bin", b"crc" * (4 << 20))]))
        cd = data.index(b"PK\x01\x02")
        crc = struct.unpack_from("<I", data, cd + 16)[0] ^ 1
        struct.pack_into("<I", data, 14, crc)
        struct.pack_into("<I", data, cd + 16, crc)
        self.process(data, ok=False, extra=("--stream-crc", "cpu"))
        self.assertFalse((self.out / "bad.bin").exists())
        self.assertEqual(list(self.out.glob("*.part")), [])

    @unittest.skipUnless(BACKEND == "gpu" and MODE == "stream", "隔離workerの再利用")
    def test_stream_worker_reuse(self):
        entries = [(f"{i}", b"worker" * (1 << 20)) for i in range(3)]
        result = self.process(zip_bytes(entries), extra=("--json",))
        stats = json.loads(result.stdout)
        self.assertEqual(stats["gpu_streams"], 3)
        self.assertEqual(stats["gpu_stream_workers"], 1)
        for name, payload in entries:
            self.assertEqual((self.out / name).read_bytes(), payload)


if __name__ == "__main__":
    unittest.main()
