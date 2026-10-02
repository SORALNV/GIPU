# GIPU

Ubuntu＋NVIDIA GPUで、通常のZIPを展開するCLIを開発しています。実験用GPUはRTX 3090です。

目標は、GPUとCPUを使い分け、大小・多数のファイルを含む標準ZIPを高速に展開することです。VRAMより大きいアーカイブや単一ファイルもストリーミング展開します。性能は解凍・CRCだけでなく、ファイル操作とI/Oを含めて実測で判断します。

## 実装状況

- ZIP32／ZIP64、Stored／Deflate、Data Descriptor、UTF-8／CP437ファイル名の解析。
- 絶対パス、`..`、重複パス、シンボリックリンク、データ範囲の重複を拒否。
- 展開サイズとCRCを確認してから、同じディレクトリ内の一時ファイルを確定。既存ファイルは上書きしません。
- nvCOMP 5.3.0によるGPUバッチDeflate／Streaming Gzip。予算に応じて自動選択します。
- バッチではGPU CRC32、Streamingでは既定で高速CPU CRCを併用。従来の増分GPU CRCも比較用に選択できます。
- JSONの処理時間・展開量・バッチ数・ストリーム数・作業領域の出力。
- バッチarena／固定化ホストバッファの再利用、検証時の全展開データD2H転送の省略。
- libdeflateによる並列CPU経路（1〜32 worker、任意の依存）。予算に収まらない単一ファイルは定量メモリのCPU Streamingで処理。
- 大きな単一ファイル向けGzip LOOKAHEADバッチの比較経路（`--gpu-algorithm lookahead`、実験用）。
- ISA-Lによる定量メモリCPU Streaming、Rapidgzipによる単一Deflateの並列CPU経路（後者は実験用・任意の依存）。
- ZIPメタデータの先読み、ASCII名の高速処理、中央ディレクトリのサイズ上限。
- CPUとGPUが異なるエントリ集合を同時処理する`hybrid`経路と、固定化ホストメモリの予算化。

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
build/gipu test archive.zip --backend libdeflate --threads 16 --json
build/gipu test archive.zip --backend isal --json
build/gipu extract archive.zip --output ./out --backend hybrid --json
build/gipu extract archive.zip --output ./out --pipeline --json
```

`test`は展開先を作らずに解凍・サイズ・CRCを検証します。既定の各経路は展開データをディスクへ書きません。比較用の`--stream-crc gpu`でGPU Streamingを使う場合だけ、CRC再読み込み用に名前なし一時ファイルを作るため、展開量と同じ一時ディスク容量が必要です（配置先は`TMPDIR`、未指定時は通常`/tmp`）。終了時に削除されます。`--max-output`の既定値は1TiBです。`--sync`を付けるとファイルと親ディレクトリをfsyncします。失敗時、処理中の一時ファイルは削除されます。既に検証・確定されたファイルは残ります。

`--gpu-mode auto`は予算内の最大4096エントリをまとめ、収まらないエントリをストリーミングへ回します。`--batch-entries`で上限を変更できます。`stream`は全DeflateをStreaming Gzipへ、`batch`はサイズ・予算内のDeflateだけをバッチへ送ります。Storedはコピー＋CRCです。空のDeflateエントリはストリーミング経路で処理します。

`--backend libdeflate`は任意のlibdeflate依存を見つけたビルドで使用できます。既存環境には`libdeflate`を追加して再ビルドしてください。`--threads`はこの経路のworker数です。`--host-limit`（既定8GiB、最小2MiB）をworker数で分配し、入力＋出力がworker予算に収まる場合だけ全バッファ解凍します。収まらないファイルはISA-Lまたはzlib Streaming＋高速CPU CRCで処理するため、従来の256MiB制限はありません。Storedは余分なコピーを省きます。`host_buffer_bytes`はworkerごとの最大データバッファ量の合計であり、ZIPメタデータ・ライブラリ内部・スレッドスタックを含むRSS全体の上限ではありません。

`--backend hybrid`はStored・空ファイル・32KiB未満・64MiB超・ほぼ非圧縮のファイルをCPUへ送り、残りを`--cpu-percent`（既定50%）の展開バイト割合を目安に分割します。GPUに渡す配列からCPU担当ファイルを除くため、混在データでもGPUバッチが分断されません。CPUは既定最大8 worker、GPUはバッチI/Oパイプラインを使い、同時に処理します。片側が失敗すると他方にも停止を要求し、両方をjoinしてから最初のエラーを返します。CPU向きの入力だけ、予算が小さい、GPUが利用不能な場合はCPUだけで完結します。`selected_backend`と`selection_reason`で実際の選択を確認できます。この分割則は調整中で、最速を保証するものではありません。

GPU経路にも`--host-limit`を適用します。バッチ入出力の固定化メモリと、固定バッファ分として保守的に予約する12MiBを予算化します。pipelineは2組の固定化バッファを含み、全計画の最大入力・最大出力が別バッチに現れるケースも事前に確認します。通常経路は必要なら古い固定化バッファを解放して予算を守り、Streamingへ移る前にも解放します。hybridはCPUに1/4、GPUに残りのホスト予算を分け、GPUバッチの最悪ケースを考慮してVRAM予算も縮めます。GPUの`host_buffer_bytes`は固定分の予約を含む上限見積もりです。

`--gpu-algorithm lookahead`はRaw DeflateへGzipの18バイトの外枠をメモリ上で付けて実行します。再圧縮や中間ファイルを作らず、scratchもVRAM予算に含めます。高圧縮率の繰り返しデータでは従来方式より遅い例を確認しているため、既定は`deflate`のままです。4GiBを超える単一出力もAPI上は扱えますが、入力・出力・scratchがVRAM予算に収まる必要があります。収まらなければautoモードではStreamingへ回します。

ISA-Lを検出したビルドでは、`libdeflate`経路の巨大ファイルをISA-L Streamingで処理します。見つからなければzlib Streamingへ戻ります。`--backend isal`は比較用の単一CPU Streamingを明示します。`--backend cpu`は引き続きzlib解凍＋zlib CRCの基準経路です。各経路で展開サイズ・CRC・圧縮ストリームの消費量を確認し、宣言された圧縮範囲の末尾に余分なバイトがある場合も拒否します。

単一Deflate内部の並列CPU実験は以下で有効にします。Rapidgzip 0.16.0のソースとサブモジュールを固定し、標準ZIPの圧縮本体をseek可能な仮想Gzipとして読ませます。再圧縮・中間Gzipは不要です。この構成のRapidgzip内部デコーダはzlibで、修正版ISA-Lはまだ使用していません。

```bash
bash scripts/bootstrap_rapidgzip.sh
bash scripts/build.sh -DGIPU_ENABLE_RAPIDGZIP=ON \
  -DRAPIDGZIP_ROOT="$PWD/.deps/rapidgzip/librapidarchive"
build/gipu test archive.zip --backend rapidgzip --threads 16 --json
```

Rapidgzipは大きな単一ファイル向けの実験経路で、小ファイルごとに起動すると不利です。`--host-limit`からworker数とchunkの目安を決め、不要な履歴を解放しますが、**ライブラリ内部のメモリに厳密な上限を設けるものではありません**。chunk上限もDeflateブロック境界でしか効かない場合があります。厳密に小さなデータバッファで処理したい場合は`isal`、`cpu`、または予算を小さくした`libdeflate`を選んでください。全出力を実際にデコードし、共通sinkでサイズとCPU CRCを独立検証します。

中央ディレクトリは既定で256MiB／100万エントリまでです。`--metadata-limit`は前者を変更します。解析時の先読みバッファは中央256KiB・ローカルヘッダ4KiBですが、解析済みのエントリ・ファイル名一覧は別途RAMに保持します。

`--pipeline`は全ファイルが非空Deflateバッチに収まるZIP専用の実験経路です。固定化ホスト入出力を二重化し、次バッチの読み込み・GPU処理・前バッチの書き込みを重畳します。GPU arenaは一つだけでVRAM予算は変わりませんが、ホストRAMの必要量はバッチ入出力の約2倍になります。Stored／空ファイル／Streamingが必要な入力にはこの指定を外してください。各バッチ全体のGPU CRC確認が済むまでは、そのバッチの出力を書き出しません。

GPUバッチ出力は`--write-threads`（既定8、最大32）で並列化します。pipelineは最大2バッチの出力を重ねるため、既定では最大16個の出力workerに加えて先読み／GPU制御用CPUを使います。GPU処理だけでなくCPUによるファイル操作も性能に寄与します。メモリ容量は全バッチの計画から最大値を先に確保し、処理中に`cudaMallocHost`を呼び直すことによる同期を減らします。`allocation_seconds`に事前確保時間を記録します。

`--vram-limit`は、GIPUが明示的に確保する入力・出力・nvCOMP作業領域・メタデータ・CRC領域の合計を制限します。CUDAコンテキストやライブラリ内部の割り当て、他プロセスの使用量は含まれません。総VRAMの厳密な上限を保証するオプションではありません。ホストRAMにはバッチ入出力と同程度の固定化メモリが必要です。Streaming経路はアーカイブ／展開量の全体バッファを確保しません。

公開Streaming APIの出力はホスト上です。解凍カーネルが稼働中にGPU CRCカーネルを同期実行すると、大量出力で処理が進まなくなることを3090で確認しました。現在の既定`--stream-crc cpu`は出力callbackでCPU CRCを増分計算し、二度読み・CRC用のGPU再転送を省きます。libdeflateがあればその高速CRC、なければzlib CRCを使用します。バッチ経路は展開済みVRAM上でCRCを計算します。`cpu_crc_bytes`と`gpu_crc_bytes`で分担を記録します。従来の二度読み方式は`--stream-crc gpu`で選べます。どちらもRAM／VRAMを展開量に比例して確保しません。

## 検証と測定

```bash
ctest --test-dir build --output-on-failure
GIPU_TEST_BACKEND=gpu python3 tests/integration.py build/gipu
GIPU_TEST_BACKEND=gpu GIPU_TEST_MODE=stream python3 tests/integration.py build/gipu
python3 scripts/benchmark.py --binary build/gipu --total-mib 256 --entries 16 --repeats 3
python3 scripts/verify_large.py --binary build/gipu --gib 26 --vram-limit 64M
```

CPU比較対象にはzlibとlibdeflateを使います。`test`はデコード＋CRC（比較用`--stream-crc gpu`のStreaming時は一時出力のI/Oも含む）の測定で、`benchmark.py --extract`はZIP解析・ファイル生成・書き込みも含む測定です。CUDA初期化を含むCLI全体時間を外部から測り、CLI内部時間も保存します。速度倍率を一般的なZIPや7-Zipへの倍率として解釈しないでください。

`scripts/make_corpus.py`と`scripts/benchmark_corpus.py`で、極小・小・中・単一大ファイル、高低圧縮率、Stored／空ファイル混在を再現比較できます。後者はピークRSS・CLI全体時間・内部工程・全出力サイズ・固定seedのSHA256サンプルを記録し、元ZIPを残して自分の一時展開先だけを削除します。5時間の改善作業は[研究・実験ログ](docs/research-2026-10-02.md)に記録します。

### Kaggle実データの大容量測定

元データは読み取り専用です。ZIP／ローカルマニフェストは既存ファイルを上書きせず、リポジトリには公開しません。

```bash
python3 scripts/prepare_kaggle_zip.py --source /path/to/train_series \
  --output /optane/workspace/kaggle-50GB.zip --target-gb 50 --workers 12
python3 scripts/benchmark_archive.py --archive /optane/workspace/kaggle-50GB.zip \
  --results bench-results/kaggle50-test.json
python3 scripts/benchmark_archive.py --archive /optane/workspace/kaggle-50GB.zip \
  --extract-root /ssd/gipu-bench-output --source /path/to/train_series \
  --cases cpu libdeflate16 gpu4096 --results bench-results/kaggle50-extract.json
```

容量は圧縮ZIPの10進GBです。生成には目標容量＋15GB、展開には展開量＋20GBの空きを要求します。試験が作った一時展開ディレクトリだけ終了後に削除し、ZIPと元データを残します。全エントリのCRC／サイズを検証し、実展開では全出力サイズと元データ128サンプルのSHA256も確認します。既定は対象ZIPへ`POSIX_FADV_DONTNEED`を助言しますが、完全なcold cacheを保証しません。繰り返し時は測定順序を交互にします。

実展開の既定は通常のwrite完了までで、永続化まで測る場合は`--sync`を指定します。GPUの`decode_seconds`／`crc_seconds`／`transfer_seconds`はCUDA event時間、読み込み／出力はCPU側の経過時間です。libdeflateの各工程時間はworkerの加算値で、並列時の実経過時間とは異なります。

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

## 2026年10月2日：Kaggle実データ50GBの比較

Optaneのworkspaceに**50.001GBの標準ZIP64**を作成しました。元データは2TB SSD側のKaggle RSNA DICOM、155,925ファイル・展開後109.557GBです。元データは変更していません。

| 経路 | 解凍＋CRC（書き込みなし） | SSDへの実展開 |
|---|---:|---:|
| zlib単一CPU、初回1回の基準値 | 277.07秒 | 311.93秒 |
| libdeflate 16スレッド、最終3回中央値 | 13.90秒 | 60.18秒 |
| GPU pipeline、4GiB、最終3回中央値 | 19.01秒 | **54.85秒** |

単一CPU zlib基準比は、書き込みなしで約14.58倍、実展開で**約5.69倍**でした。選んだデータの中央値では5倍目標を超えています。ただし最速CPU比5倍ではありません。実展開の16スレッドCPU比は中央値で約1.10倍、時間の範囲も重なり、安定した優位性までは示せていません。書き込みなしでは並列CPUの方が速い結果です。

GPUの実展開3回は47.02／67.42／54.85秒でした。**fsyncによる全件永続化は含まない**通常write完了までの比較で、全件CRC・出力サイズと元データ128件のSHA256を確認しています。試験用の展開物だけ削除し、50GB ZIPと元データは残しました。GPU解凍区間は約11.5〜11.7秒と安定し、残る主要な変動は書き込み側です。CUDAバッファ、固定化メモリ、GPU CRC、I/O重畳、並列ファイル出力を最適化し、カーネルやドライバ自体は変更していません。

このZIPで使う推奨経路:

```bash
build/gipu extract /srv/workspace/sora/gipu-bench/kaggle-50GB.zip \
  --output /home/sora/gipu-extracted --pipeline --vram-limit 4G \
  --batch-entries 4096 --write-threads 8 --json
```

展開先には約110GB＋余裕が必要です。pipelineは最大16個のCPU出力workerを使います。元データ・医用画像・ローカルマニフェストはGitHubへ公開していません。測定方法、各回の値、比較対象の制約、ZIPのSHA256は[50GB実測記録](docs/measurements-kaggle-50gb-2026-10-02.md)と[集計JSON](docs/benchmarks/kaggle50-2026-10-02.json)を参照してください。
