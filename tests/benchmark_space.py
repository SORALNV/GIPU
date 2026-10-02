"""小ファイルのblock切上げ、親階層の共有、inode不足を検証する。"""
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from benchmark_space import check_output_space, plan_output_space


class SpacePlan(unittest.TestCase):
    @patch("benchmark_space.os.statvfs", return_value=SimpleNamespace(f_frsize=4096))
    def test_rounding_and_shared_directories(self, statvfs):
        result = plan_output_space("unused", [("a/b/empty", 0), ("a/b/tiny", 1), ("other/block", 4097)], 100)
        self.assertEqual(result["required_inodes"], 7)
        self.assertEqual(result["required_bytes"], 3 * 4096 + 3 * 4096 + 7 * 512 + 100)

    @patch("benchmark_space.os.statvfs", return_value=SimpleNamespace(f_frsize=4096))
    def test_explicit_empty_directories(self, statvfs):
        result = plan_output_space("unused", [("a/", 0), ("a/b/", 0), ("a/b/empty", 0)], 0)
        self.assertEqual(result["required_inodes"], 4)
        self.assertEqual(result["required_bytes"], 2 * 4096 + 4 * 512)

    @patch("benchmark_space.shutil.disk_usage", return_value=SimpleNamespace(free=10))
    def test_insufficient_bytes(self, usage):
        with self.assertRaisesRegex(RuntimeError, "容量"):
            check_output_space("unused", {"required_bytes": 11, "required_inodes": 1})

    @patch("benchmark_space.os.statvfs", return_value=SimpleNamespace(f_files=100, f_favail=2))
    @patch("benchmark_space.shutil.disk_usage", return_value=SimpleNamespace(free=100))
    def test_insufficient_inodes(self, usage, statvfs):
        with self.assertRaisesRegex(RuntimeError, "inode"):
            check_output_space("unused", {"required_bytes": 10, "required_inodes": 3})

    @patch("benchmark_space.os.statvfs", return_value=SimpleNamespace(f_files=0, f_favail=0))
    @patch("benchmark_space.shutil.disk_usage", return_value=SimpleNamespace(free=100))
    def test_unknown_inode_capacity(self, usage, statvfs):
        check_output_space("unused", {"required_bytes": 10, "required_inodes": 3})


if __name__ == "__main__":
    unittest.main()
