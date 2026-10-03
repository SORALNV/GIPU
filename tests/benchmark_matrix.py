"""外部比較の入力制約と独立SHA256検証を試験する。"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import shutil
import subprocess
import tempfile
import unittest
import zipfile


SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location("matrix_impl", SCRIPTS / "benchmark_matrix.py")
matrix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(matrix)
from summarize_matrix import summarize
from reference_7zip_parallel import result_counts


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

    def test_missing_hash_requires_source(self):
        del self.manifest["entries"][0]["sha256"]
        output = self.output()
        with self.assertRaises(RuntimeError):
            matrix.verify(output, self.manifest, 0)
        source = self.root / "original"
        (source / "nested").mkdir(parents=True)
        (source / "nested/data").write_bytes(self.payload)
        self.assertEqual(matrix.verify(output, self.manifest, 0, source), 1)

    def test_unexpected_empty_directory_rejected(self):
        output = self.output()
        (output / "unexpected").mkdir()
        with self.assertRaises(RuntimeError):
            matrix.verify(output, self.manifest, 0)

    def summary_fixture(self, runs, finished=True):
        report = self.root / "result.jsonl"
        metadata = {"kind": "metadata", "cases_requested": ["small"],
                    "modes_requested": ["extract"], "variants_requested": ["auto", "gpu"],
                    "repeats_requested": 3}
        rows = [metadata, *runs]
        if finished:
            rows.append({"kind": "summary"})
        report.write_text("\n".join(json.dumps(row) for row in rows) + "\n")
        return summarize(report)

    def summary_runs(self):
        return [{"kind": "run", "case": "small", "mode": "extract", "variant": variant,
                 "repeat": repeat, "verified": True, "wall_seconds": seconds}
                for variant, seconds in (("auto", 1.0), ("gpu", 2.0)) for repeat in range(1, 4)]

    def test_summary_complete_ratio(self):
        result = self.summary_fixture(self.summary_runs())
        self.assertEqual(result["comparisons"][0]["ratio_to_auto"]["gpu"], 2)

    def test_summary_partial_no_ratio(self):
        result = self.summary_fixture(self.summary_runs(), finished=False)
        self.assertNotIn("ratio_to_auto", result["comparisons"][0])

    def test_summary_missing_variant_no_ratio(self):
        result = self.summary_fixture(self.summary_runs()[:3])
        self.assertEqual(result["missing_groups"], [("small", "extract", "gpu")])
        self.assertNotIn("ratio_to_auto", result["comparisons"][0])

    def test_summary_missing_repeat_no_ratio(self):
        result = self.summary_fixture(self.summary_runs()[:-1])
        self.assertNotIn("ratio_to_auto", result["comparisons"][0])

    def test_summary_duplicate_repeat_no_ratio(self):
        runs = self.summary_runs()
        runs[-1]["repeat"] = 2
        result = self.summary_fixture(runs)
        self.assertNotIn("ratio_to_auto", result["comparisons"][0])

    def test_summary_failed_run_no_ratio(self):
        runs = self.summary_runs()
        runs[-1]["verified"] = False
        result = self.summary_fixture(runs)
        self.assertEqual(result["failures"], 1)
        self.assertNotIn("ratio_to_auto", result["comparisons"][0])

    def test_summary_different_input_no_ratio(self):
        runs = self.summary_runs()
        for row in runs:
            row["archive_sha256"] = "archive-one"
        runs[-1]["archive_sha256"] = "archive-two"
        result = self.summary_fixture(runs)
        self.assertNotIn("ratio_to_auto", result["comparisons"][0])

    def test_7zip_single_file_counts(self):
        self.assertEqual(result_counts("Everything is Ok\n\nSize: 123\nCompressed: 456\n"), (1, 123))

    def test_7zip_many_file_counts(self):
        self.assertEqual(result_counts("Everything is Ok\n\nFiles: 32\nSize:       123\n"), (32, 123))

    def test_7zip_missing_crc_success_rejected(self):
        with self.assertRaises(RuntimeError):
            result_counts("Files: 32\nSize: 123\n")

    def reference_fixture(self):
        contents = {"nested/日本語.bin": self.payload, "nested/empty": b"",
                    "other/wild*[?].bin": b"literal wildcard", "other/data": bytes(range(256))}
        with zipfile.ZipFile(self.archive, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("empty-dir/nested/", b"")
            for name, payload in contents.items():
                archive.writestr(name, payload)
        return {"files": len(contents), "uncompressed_bytes": sum(map(len, contents.values())),
                "directories": ["empty-dir/nested/"],
                "entries": [{"name": name, "bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}
                            for name, payload in contents.items()]}

    def check_reference(self, script, extra):
        manifest = self.reference_fixture()
        for mode in ("test", "extract"):
            output = self.root / "reference-output"
            command = [sys.executable, str(SCRIPTS / script), mode, str(self.archive), "--threads", "3", *extra]
            if mode == "extract":
                command += ["--output", str(output)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            stats = json.loads(result.stdout)
            self.assertEqual((stats["files"], stats["bytes"]), (manifest["files"], manifest["uncompressed_bytes"]))
            self.assertEqual(stats["crc_verified_files"], manifest["files"])
            if mode == "extract":
                self.assertEqual(matrix.verify(output, manifest, 0), manifest["files"])

    def test_python_parallel_reference_end_to_end(self):
        self.check_reference("reference_zip.py", [])

    def test_7zip_parallel_reference_end_to_end(self):
        local = SCRIPTS.parent / ".deps/7zip-26.03/7zz"
        sevenzip = os.environ.get("GIPU_TEST_7ZIP") or (
            str(local) if local.is_file() else shutil.which("7zz") or shutil.which("7z"))
        if not sevenzip:
            self.skipTest("7-Zipがないため外部参照の実行は未検証")
        self.check_reference("reference_7zip_parallel.py", ["--sevenzip", sevenzip])

    def release_arguments(self):
        return [sys.executable, str(SCRIPTS / "run_release_validation.py"),
                "--corpus", str(self.root), "--archive", str(self.archive), "--source", str(self.root),
                "--ssd-output", str(self.root / "ssd"), "--optane-output", str(self.root / "optane"),
                "--tmpfs-output", str(self.root / "tmpfs"), "--report-prefix", str(self.root / "release")]

    def test_release_unknown_phase_rejected_before_start(self):
        result = subprocess.run([*self.release_arguments(), "--from-phase", "unknown"],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 2)
        self.assertFalse((self.root / "release-status.jsonl").exists())

    def test_release_conflicting_plans_rejected_before_start(self):
        result = subprocess.run([*self.release_arguments(), "--controlled-final", "--parallel-reference-only"],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 2)
        self.assertFalse((self.root / "release-status.jsonl").exists())


if __name__ == "__main__":
    unittest.main()
