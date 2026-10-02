#!/usr/bin/env bash
# 実験用のヘッダ依存を特定コミットへ固定する。既存チェックアウトは上書きしない。
set -euo pipefail
gipu_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$gipu_root"
gipu_rapid_revision=d2350e9c9ba54398cd64e45bfc8c631beec017f0
gipu_rapid_source=.deps/rapidgzip
if [[ ! -e "$gipu_rapid_source" ]]; then
  git clone --no-checkout https://github.com/mxmlnkn/rapidgzip.git "$gipu_rapid_source"
  git -C "$gipu_rapid_source" checkout --detach "$gipu_rapid_revision"
elif [[ "$(git -C "$gipu_rapid_source" rev-parse HEAD)" != "$gipu_rapid_revision" ]]; then
  printf '%s\n' '既存Rapidgzipの版が異なります。退避またはRAPIDGZIP_ROOTを個別指定してください。' >&2
  exit 1
fi
git -C "$gipu_rapid_source" submodule update --init librapidarchive
printf '%s\n' 'Rapidgzip 0.16.0のヘッダを準備しました。再現ビルド:'
printf '%s\n' "bash scripts/build.sh -DGIPU_ENABLE_RAPIDGZIP=ON -DRAPIDGZIP_ROOT=$gipu_root/$gipu_rapid_source/librapidarchive"
