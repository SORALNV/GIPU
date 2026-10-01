#!/usr/bin/env bash
# 管理者権限を使わず、プロジェクト内に開発環境を準備する。
set -euo pipefail
gipu_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$gipu_root"
mkdir -p .deps/tools .deps/nvcomp
if [[ ! -x .deps/tools/bin/micromamba ]]; then
  curl -fLsS https://micro.mamba.pm/api/micromamba/linux-64/latest |
    python3 -c 'import sys,tarfile; tarfile.open(fileobj=sys.stdin.buffer,mode="r|bz2").extractall(".deps/tools",filter="data")'
fi
if [[ ! -x .deps/env/bin/cmake ]]; then
  .deps/tools/bin/micromamba create -y --no-rc --root-prefix "$gipu_root/.deps/mamba" \
    -p "$gipu_root/.deps/env" -c conda-forge -c nvidia \
    'gxx_linux-64=14' cmake ninja zlib 'cuda-cudart-dev=13.0.*' \
    'cuda-crt-dev_linux-64=13.0.*' cuda-version=13.0
fi
if [[ ! -f .deps/nvcomp/include/nvcomp/native/streaming_gzip.hpp ]]; then
  curl -fLsS https://developer.download.nvidia.com/compute/nvcomp/redist/nvcomp/linux-x86_64/nvcomp-linux-x86_64-5.3.0.16_cuda13-archive.tar.xz |
    tar -xJ -C .deps/nvcomp --strip-components=1
fi
printf '%s\n' '開発環境の準備が完了しました。bash scripts/build.sh を実行してください。'
