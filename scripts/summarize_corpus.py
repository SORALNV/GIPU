"""ベンチマークJSONLから、個別ファイル名を含めない再検算可能な集計JSONを作る。"""
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
    groups = defaultdict(list)
    metadata = []
    for line in args.report.read_text().splitlines():
        row = json.loads(line)
        if row.get("kind") == "metadata":
            metadata.append(row)
        else:
            groups[(row["case"], row["mode"], row["variant"])].append(row)
    result = {"source_log": args.report.name,
              "source_sha256": hashlib.sha256(args.report.read_bytes()).hexdigest(),
              "metadata": metadata, "groups": []}
    for (case, mode, variant), rows in groups.items():
        okay = [r for r in rows if r["returncode"] == 0]
        record = {"case": case, "mode": mode, "variant": variant, "runs": len(rows),
                  "failures": len(rows) - len(okay), "raw_bytes": rows[0]["raw_bytes"],
                  "archive_bytes": rows[0]["archive_bytes"]}
        if okay:
            seconds = [r["wall_seconds"] for r in okay]
            record.update(wall_seconds=seconds, median_seconds=statistics.median(seconds),
                          min_seconds=min(seconds), max_seconds=max(seconds),
                          files=okay[0]["stats"]["files"],
                          sha256_samples=[r.get("verified_sha256_samples", 0) for r in okay],
                          selected_backends=sorted({r["stats"].get("selected_backend", "") for r in okay}))
            for field in ("peak_rss_kib", "user_seconds", "system_seconds"):
                values = [r[field] for r in okay if field in r]
                if values:
                    record[field] = values
            record["median_stats"] = {}
            for field in ("parse_seconds", "metadata_threads", "read_seconds", "write_seconds", "decode_seconds", "crc_seconds",
                          "allocation_seconds", "transfer_seconds", "crc_combine_seconds", "gpu_crc_chunks", "gpu_size_reorders", "gpu_batches", "gpu_streams",
                          "lookahead_batches", "pipeline_overlap_waits", "gpu_stream_workers", "cpu_buffered_files", "cpu_stream_files", "cpu_parallel_files",
                          "isal_files", "host_buffer_bytes", "cpu_crc_bytes", "gpu_crc_bytes",
                          "anonymous_output_files", "named_output_files", "fast_parent_opens", "portable_parent_walks"):
                values = [r["stats"][field] for r in okay if field in r["stats"]]
                if values:
                    record["median_stats"][field] = statistics.median(values)
        result["groups"].append(record)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8") as output:
        json.dump(result, output, ensure_ascii=False, indent=2)
        output.write("\n")
    print(json.dumps({"groups": len(groups), "source_sha256": result["source_sha256"]}))


if __name__ == "__main__":
    main()
