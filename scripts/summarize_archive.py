"""大容量ZIPの測定JSONを、出力ファイル名を含まない公開用集計へ変換する。"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import statistics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("既存の集計は上書きしません")
    source = args.report.read_bytes()
    report = json.loads(source)
    fields = ("archive_bytes", "raw_bytes", "files", "binary_sha256", "time_utc", "mode", "durable", "cache", "cache_note")
    result = {"source_log": args.report.name, "source_sha256": hashlib.sha256(source).hexdigest(),
              "metadata": {key: report[key] for key in fields if key in report}, "groups": []}
    groups = defaultdict(list)
    for row in report["runs"]:
        groups[row["case"]].append(row)
    for case, rows in groups.items():
        times = [row["wall_seconds"] for row in rows]
        record = {"case": case, "runs": len(rows), "wall_seconds": times,
                  "median_seconds": statistics.median(times),
                  "sha256_samples": [row["sha256_samples"] for row in rows],
                  "crc_verified_files": [row["crc_verified_files"] for row in rows],
                  "median_stats": {}}
        for key, value in rows[0]["stats"].items():
            if isinstance(value, (int, float)):
                record["median_stats"][key] = statistics.median(row["stats"][key] for row in rows)
        result["groups"].append(record)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8") as output:
        json.dump(result, output, ensure_ascii=False, indent=2)
        output.write("\n")
    print(json.dumps({"groups": len(groups), "source_sha256": result["source_sha256"]}))


if __name__ == "__main__":
    main()
