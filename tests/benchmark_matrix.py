"""外部比較の入力制約と独立SHA256検証を試験する。"""
import hashlib
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile


SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location("matrix_impl", SCRIPTS / "benchmark_matrix.py")
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)


class Matrix(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gipu-matrix-test-")
        self.root = Path(self.temporary.name)
        self.archive = self.root / "input.zip"
        self.payload = "検証用データ".encode()
        self.manifest = {"files": 1, "uncompressed_bytes": len(self.payload),
                         "entries": [{"name": "nested/data", "bytes": len(self.payload),
                                      "sha256": hashlib.sha256(self.payload).hexdigest()}]}
        with zipfile.ZipFile(self.archive, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("nested/data", self.payload)

    def tearDown(self):
        self.temporary.cleanup()

    def test_valid_manifest(self):
        self.assertEqual(len(matrix.validate_archive(self.archive, self.manifest)), 1)

    def test_manifest_count_rejected(self):
        self.manifest["files"] = 2
        with self.assertRaises(RuntimeError):
            matrix.validate_archive(self.archive, self.manifest)

    def test_unsupported_codec_rejected(self):
        with zipfile.ZipFile(self.archive, "w", compression=zipfile.ZIP_BZIP2) as archive:
            archive.writestr("nested/data", self.payload)
        with self.assertRaises(RuntimeError):
            matrix.validate_archive(self.archive, self.manifest)

    def test_traversal_rejected(self):
        with zipfile.ZipFile(self.archive, "w") as archive:
            archive.writestr("../escape", self.payload)
        with self.assertRaises(RuntimeError):
            matrix.validate_archive(self.archive, self.manifest)

    def output(self):
        output = self.root / "out"
        (output / "nested").mkdir(parents=True)
        (output / "nested/data").write_bytes(self.payload)
        return output

    def test_full_sha256(self):
        self.assertEqual(matrix.verify(self.output(), self.manifest, 0), 1)

    def test_same_size_wrong_data_rejected(self):
        output = self.output()
        (output / "nested/data").write_bytes(bytes(len(self.payload)))
        with self.assertRaises(RuntimeError):
            matrix.verify(output, self.manifest, 0)

    def test_extra_output_rejected(self):
        output = self.output()
        (output / "extra").touch()
        with self.assertRaises(RuntimeError):
            matrix.verify(output, self.manifest, 0)

    def test_symlink_rejected(self):
        output = self.output()
        (output / "link").symlink_to(output / "nested/data")
        with self.assertRaises(RuntimeError):
            matrix.verify(output, self.manifest, 0)

    def test_missing_empty_directory_rejected(self):
        self.manifest["directories"] = ["empty/"]
        output = self.output()
        with self.assertRaises(RuntimeError):
            matrix.verify(output, self.manifest, 0)
        (output / "empty").mkdir()
        self.assertEqual(matrix.verify(output, self.manifest, 0), 1)


if __name__ == "__main__":
    unittest.main()
