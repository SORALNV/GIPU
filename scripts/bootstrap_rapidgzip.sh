#!/usr/bin/env bash
# 実験用のヘッダ依存を特定コミットへ固定する。既存チェックアウトは上書きしない。
set -euo pipefail
gipu_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$gipu_root"
if [[ "$#" -gt 1 || ( "$#" == 1 && "${1:-}" != "--with-isal" ) ]]; then
  printf '%s\n' '使い方: bash scripts/bootstrap_rapidgzip.sh [--with-isal]' >&2
  exit 1
fi
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
gipu_isal_option=""
if [[ "${1:-}" == "--with-isal" ]]; then
  if [[ ! -x .deps/env/bin/nasm ]] && ! command -v nasm >/dev/null 2>&1; then
    if [[ -x .deps/tools/bin/micromamba && -d .deps/env ]]; then
      .deps/tools/bin/micromamba install -y --no-rc --root-prefix "$gipu_root/.deps/mamba" \
        -p "$gipu_root/.deps/env" -c conda-forge 'nasm=2.16.03'
    else
      printf '%s\n' '修正版ISA-LにはNASMが必要です。先にbootstrap.shを実行するか、NASMを準備してください。' >&2
      exit 1
    fi
  fi
  git -C "$gipu_rapid_source/librapidarchive" submodule update --init src/external/isa-l
  gipu_isal_option=" -DGIPU_RAPIDGZIP_ISAL=ON"
fi
printf '%s\n' 'Rapidgzip 0.16.0のヘッダを準備しました。再現ビルド:'
printf '%s\n' "bash scripts/build.sh -DGIPU_ENABLE_RAPIDGZIP=ON -DRAPIDGZIP_ROOT=$gipu_root/$gipu_rapid_source/librapidarchive$gipu_isal_option"
