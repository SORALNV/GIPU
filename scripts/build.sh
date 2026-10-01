#!/usr/bin/env bash
set -euo pipefail
gipu_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$gipu_root"
if [[ ! -x .deps/env/bin/cmake ]]; then
  printf '%s\n' '先に bash scripts/bootstrap.sh を実行してください。' >&2
  exit 1
fi
export PATH="$gipu_root/.deps/env/bin:$PATH"
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="$gipu_root/.deps/env/bin/x86_64-conda-linux-gnu-g++" \
  -DCMAKE_PREFIX_PATH="$gipu_root/.deps/env" \
  -DNVCOMP_ROOT="$gipu_root/.deps/nvcomp" -DCUDA_ROOT="$gipu_root/.deps/env" \
  "${@}"
cmake --build build -j 4
ctest --test-dir build --output-on-failure
