# GIPU

Ubuntu＋NVIDIA GPUで、通常のZIPを展開するCLIを開発しています。実験用GPUはRTX 3090です。

目標は、Deflate解凍とCRC32計算をGPUで行い、VRAMより大きいアーカイブや単一ファイルをストリーミング展開することです。CPUはZIP構造の解析とファイル操作を担当します。CPUより高速かどうかは実測で判断します。

## 実装状況

- ZIP32／ZIP64、Stored／Deflate、Data Descriptor、UTF-8／CP437ファイル名の解析。
- 絶対パス、`..`、重複パス、シンボリックリンク、データ範囲の重複を拒否。
- 展開サイズとCRCを確認してから、同じディレクトリ内の一時ファイルを確定。既存ファイルは上書きしません。
- nvCOMP 5.3.0によるGPUバッチDeflate／Streaming Gzip。予算に応じて自動選択します。
- GPU CRC32（バッチ／増分計算）。CPU参照経路（zlib）は明示したときだけ使います。
- JSONの処理時間・展開量・バッチ数・ストリーム数・作業領域の出力。

## ビルド

Ubuntu x86_64で、CUDA 13系に対応するNVIDIAドライバが必要です。管理者権限なしで開発環境を用意できます。ダウンロードした依存は`.deps/`に保存し、Gitには含めません。nvCOMPはNVIDIAのライセンスに従います。

```bash
bash scripts/bootstrap.sh
bash scripts/build.sh
build/gipu doctor
```

既存の開発環境を使う場合は、C++20コンパイラ、CMake 3.24以上、zlib開発パッケージ、CUDAのランタイムとヘッダ、nvCOMP 5.3.0を用意します。CUDAカーネルを自作していないため、ビルド自体にnvccは不要です。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DGIPU_ENABLE_GPU=ON \
  -DNVCOMP_ROOT=/path/to/nvcomp -DCUDA_ROOT=/path/to/cuda
cmake --build build -j
ctest --test-dir build --output-on-failure
```

GPUなしでZIP解析・CPU参照経路だけをビルドする場合:

```bash
cmake -S . -B build-cpu -DCMAKE_BUILD_TYPE=Release -DGIPU_ENABLE_GPU=OFF
cmake --build build-cpu -j
ctest --test-dir build-cpu --output-on-failure
```

## 使い方

```bash
build/gipu list archive.zip
build/gipu extract archive.zip --output ./out --vram-limit 4G
build/gipu test archive.zip --gpu-mode stream --json
build/gipu test archive.zip --backend cpu
```

`test`は展開先を作らずに解凍・サイズ・CRCを検証します。GPUストリーミングの`test`だけは、CRC再読み込み用に名前なし一時ファイルを作るため、展開量と同じ一時ディスク容量が必要です（配置先は`TMPDIR`、未指定時は通常`/tmp`）。終了時に削除されます。CPU／GPUバッチの`test`は展開データをディスクへ書きません。`--max-output`の既定値は1TiBです。`--sync`を付けるとファイルと親ディレクトリをfsyncします。失敗時、処理中の一時ファイルは削除されます。既に検証・確定されたファイルは残ります。

`--gpu-mode auto`は予算内の最大256エントリをまとめ、収まらないエントリをストリーミングへ回します。`stream`は全DeflateをStreaming Gzipへ、`batch`はサイズ・予算内のDeflateだけをバッチへ送ります。Storedはコピー＋GPU CRCです。空のDeflateエントリはストリーミング経路で処理します。

`--vram-limit`は、GIPUが明示的に確保する入力・出力・nvCOMP作業領域・メタデータ・CRC領域の合計を制限します。CUDAコンテキストやライブラリ内部の割り当て、他プロセスの使用量は含まれません。総VRAMの厳密な上限を保証するオプションではありません。ホストRAMにはバッチ入出力と同程度の固定化メモリが必要です。Streaming経路はアーカイブ／展開量の全体バッファを確保しません。

公開Streaming APIの出力はホスト上です。解凍カーネルが稼働中にGPU CRCカーネルを同期実行すると、大量出力で処理が進まなくなることを3090で確認しました。初期版は出力を一時ファイルへ流し、解凍完了後に4MiB単位で再読み込み・GPU再転送し、増分CRCを計算します。追加のディスク読み込みとPCIe転送が発生しますが、RAM／VRAMを展開量に比例して確保しません。バッチ経路は展開済みVRAM上でCRCを計算します。いずれもCPU CRCへ暗黙に切り替えません。この二度読みを減らす組み込み方は今後の性能改善項目です。

## 検証と測定

```bash
ctest --test-dir build --output-on-failure
GIPU_TEST_BACKEND=gpu python3 tests/integration.py build/gipu
GIPU_TEST_BACKEND=gpu GIPU_TEST_MODE=stream python3 tests/integration.py build/gipu
python3 scripts/benchmark.py --binary build/gipu --total-mib 256 --entries 16 --repeats 3
python3 scripts/verify_large.py --binary build/gipu --gib 26 --vram-limit 64M
```

ベンチマークのCPU比較対象は現在zlibです。7-Zip／libdeflateとの比較や本格的なI/O重畳は次の段階です。`test`はデコード＋CRC（GPUストリーミング時は一時出力のI/Oも含む）の測定で、`benchmark.py --extract`はZIP解析・ファイル生成・書き込みも含む測定です。CUDA初期化を含むCLI全体時間を外部から測り、CLI内部時間も保存します。速度倍率を一般的なZIPや7-Zipへの倍率として解釈しないでください。

初期版のGPU経路は既知の正しいDeflateストリームで検証する実験実装です。nvCOMPは破損した圧縮入力に対する動作を保証しておらず、ZIPヘッダ検査と展開後CRCだけで解凍中の安全性は保証できません。不明な配布元や破損が疑われる入力は、まず`--backend cpu`で検証してください。圧縮ストリームの安全なGPU検証は未実装です。

## 開発方針

1. ZIP解析と安全な出力、CPU参照経路を検証する。
2. ZIPの圧縮本体にGzipヘッダとトレーラを仮想的に付け、nvCOMPのStreaming Gzipへ接続する。
3. メモリ予算内の複数エントリをnvCOMPバッチDeflateへまとめる。
4. GPU CRC32、実機での正確性・メモリ使用量・速度を検証する。
5. 7-Zip／libdeflateと比較し、測定で判明したボトルネックを改善する。

暗号化ZIP、Deflate64、BZip2、LZMA、分割ZIP、特殊ファイル、Windowsは初期版の対象外です。

設計の依存API: [nvCOMP Native API](https://docs.nvidia.com/cuda/nvcomp/native_api.html)、[C API](https://docs.nvidia.com/cuda/nvcomp/c_api.html)、[CRC32](https://docs.nvidia.com/cuda/nvcomp/crc32.html)。Native APIは実験的なため、SDKのバージョンを固定します。

## 2026年10月1日の実機検証

Ubuntu 26.04.1、RTX 3090（24GiB）、CUDAランタイム13.0、nvCOMP 5.3.0で確認しました。

- CPU／GPU auto／GPU stream／GPU batchの4統合テストスイートが成功。1スイート22テストで、経路に該当しないテストはskipしています。
- CPU経路のAddressSanitizer／UndefinedBehaviorSanitizer検証が成功。
- **26GiBの単一ZIP64エントリ**で展開サイズとGPU CRCが一致。プロセスVRAMのサンプリング最大値は**270MiB**、アプリが明示的に確保する作業領域は9,633,904 bytes。高圧縮率のゼロデータでの成立確認であり、26GiBの圧縮入力自体を処理した検証ではありません。
- 256MiB／16エントリの合成ZIPを展開・fsync・SHA256確認した比較では、CLI全体時間の中央値がCPU zlib **0.181秒**、GPU **0.446秒**。この条件ではGPUが約2.47倍遅く、CPUより超高速という目標は未達です。

測定条件・解釈と次の改善対象は[実測記録](docs/measurements-2026-10-01.md)、実装の分担と制約は[構成](docs/architecture.md)を参照してください。
