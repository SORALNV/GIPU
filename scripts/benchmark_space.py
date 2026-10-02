"""大量の小ファイル向けに、論理サイズだけでなく割当量とinodeも事前確認する。"""
import os
import shutil


def plan_output_space(root, entries, reserve_bytes):
    block = max(os.statvfs(root).f_frsize, 512)
    directories = set()
    allocated = files = 0
    for name, size in entries:
        directory = name.endswith("/")
        if not directory:
            files += 1
            allocated += ((size + block - 1) // block) * block
        name = name.rstrip("/")
        end = len(name) if directory else name.rfind("/")
        while end > 0:
            parent = name[:end]
            if parent in directories:
                break
            directories.add(parent)
            end = name.rfind("/", 0, end)
    # filesystemごとの実割当を保証する値ではない。データのblock切上げに加え、
    # ディレクトリ・inodeの目安と従来の余裕を残す。生成物は通常の非sparse出力。
    inodes = files + len(directories) + 1
    return {"required_bytes": allocated + len(directories) * block + inodes * 512 + reserve_bytes,
            "required_inodes": inodes, "block_size": block}


def check_output_space(root, plan):
    if shutil.disk_usage(root).free < plan["required_bytes"]:
        raise RuntimeError("小ファイルの割当量・メタデータ・余裕を含む展開先容量が不足しています")
    status = os.statvfs(root)
    if status.f_files > 0 and status.f_favail < plan["required_inodes"]:
        raise RuntimeError("展開先の空きinodeが不足しています")
