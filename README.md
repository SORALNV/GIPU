# GIPU

LinuxでStored／DeflateのZIP32・ZIP64を展開するCLIを開発しています。CPUだけでも動作し、NVIDIA GPUは任意の実験経路です。実験用GPUはRTX 3090です。

GIPU本体は[MITライセンス](LICENSE)です。依存ライブラリ・GPU SDK・実データには、それぞれ別の利用条件が適用されます。

目標は、GPUとCPUを使い分け、大小・多数のファイルを含む標準ZIPを高速に展開することです。VRAMより大きいアーカイブや単一ファイルもストリーミング展開します。性能は解凍・CRCだけでなく、ファイル操作とI/Oを含めて実測で判断します。

測定版6da1908の単一8GiB実データZIPでは、実験用CPU並列が同GIPUのGPU Streamingより**約9.3倍高速**でした。この入力・実機・CLI全体時間に限った比較で、全ZIPでのGPU比ではありません。

50GB ZIPのSSD実展開3回中央値はCPU auto **49.64秒**、GPU pipeline **52.38秒**、通常7-Zip **410.57秒**、独立16プロセス並列7-Zip **62.38秒**でした（測定版6da1908、索引除外、通常write完了まで、fsyncなし）。CPU autoは通常7-Zip比約8.27倍ですが、並列7-Zip比では約1.26倍で、全条件・最速CPU比5倍は未達です。入力と環境で優劣が変わるため、通常はCPUを選ぶ`auto`を既定にしています。改良版の新旧対照試験、全件ハッシュ照合、不利な条件も含む[最新の検証記録](docs/measurements-validation-2026-10-03.md)を参照してください。

## 実装状況

- ZIP32／ZIP64、Stored／Deflate、Data Descriptor、UTF-8／CP437／Unicode Path追加フィールドのファイル名解析。
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
- Linuxの名前なし一時出力（`--temp-mode auto`）と、CPU全量バッファ／Streamingの切り替え。
- CPU affinityを尊重する自動選択。実験用GPU／単一ストリーム並列は明示許可時だけ候補にします。

## 対応OSとWindowsについて

現在の対応OSはLinuxです。Windows／macOSネイティブ版は未対応で、CMakeでもビルドを拒否します。安全なファイル出力・CPU affinity・GPU worker制御がLinuxのAPIや`/proc`に依存しているため、単に`.exe`へビルドし直すだけでは動作しません。

Windows上で試す候補は[WSL2](https://learn.microsoft.com/en-us/windows/wsl/about)内のLinux版です。ただしGIPUのWSL2実機検証はまだ行っておらず、CPU／GPUの動作や速度を保証しません。WSL2でCUDAを利用する仕組みはありますが、それだけでGIPUのGPU経路の動作確認とは扱いません。[Microsoftの説明](https://learn.microsoft.com/en-us/windows/wsl/tutorials/gpu-compute)を参照してください。Linux実機の測定倍率もWSL2へ一般化しません。

## ビルド

Ubuntu x86_64で、CUDA 13系に対応するNVIDIAドライバが必要です。管理者権限なしで開発環境を用意できます。ダウンロードした依存は`.deps/`に保存し、Gitには含めません。nvCOMPはNVIDIAのライセンスに従います。

```bash
bash scripts/bootstrap.sh
bash scripts/build.sh
build/gipu doctor
```

既存の開発環境を使う場合は、C++20コンパイラ、CMake 3.22以上、zlib開発パッケージ、CUDAのランタイムとヘッダ、nvCOMP 5.3.0を用意します。CUDAカーネルを自作していないため、ビルド自体にnvccは不要です。

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
build/gipu test archive.zip --backend gpu --gpu-mode stream --json
build/gipu test archive.zip --backend cpu
build/gipu test archive.zip --backend libdeflate --threads 16 --json
build/gipu test archive.zip --backend isal --json
build/gipu extract archive.zip --output ./out --backend hybrid --json
build/gipu extract archive.zip --output ./out --pipeline --json
build/gipu extract known-good.zip --output ./out --auto-gpu --auto-parallel --json
```

既定の`--backend auto`は、libdeflateがあれば最大16 workerのCPU経路を選び、なければISA-L／zlibを使います。worker数はCPU affinityとメモリ予算で制限され、`--threads`で変更できます。小ファイルは全量バッファ、大ファイルは64MiBの閾値を超えたら定量メモリStreamingへ回します。極小ファイルだけの入力では仕事の取得を16件ずつまとめ、巨大な外れ値がある場合は1件ずつ分配します。

自動選択で実験経路を許可する場合:

- `--auto-gpu`: 既知の正しい入力専用です。実展開時、GPU向きの32KiB〜64MiBのDeflateが4,096件以上・合計16GiB以上・展開量の80%以上ならGPUを候補にします。残りはCPUへ送ります。GPU不在・予算不足ならCPUへ戻ります。`test`と小規模入力ではGPUを初期化しません。
- `--auto-parallel`: Rapidgzipビルドかつホスト予算1GiB以上の場合、256MiB以上・圧縮後サイズが展開量の12.5〜98%の少数大ファイルを内部並列処理します。高圧縮率・ほぼ非圧縮・多数の独立大ファイルは通常CPU経路のままです。ライブラリ内部メモリの厳密な上限は保証しません。

両者とも実測に基づく暫定則で、任意の機種・入力で最速とは限りません。JSONの`selected_backend`／`selection_reason`で選択を追跡できます。明示した`--backend gpu`／`hybrid`／`rapidgzip`は従来どおりその経路を使います。互換性のため、`--pipeline`だけの指定は明示GPUとして扱います。失敗後にエラーを隠して別方式で再実行することはありません。

`test`は展開先を作らずに解凍・サイズ・CRCを検証します。既定の各経路は展開データをディスクへ書きません。比較用の`--stream-crc gpu`でGPU Streamingを使う場合だけ、CRC再読み込み用に名前なし一時ファイルを作るため、展開量と同じ一時ディスク容量が必要です（配置先は`TMPDIR`、未指定時は通常`/tmp`）。終了時に削除されます。`--max-output`の既定値は1TiBです。`--sync`を付けるとファイルと親ディレクトリをfsyncします。失敗時、処理中の一時ファイルは削除されます。既に検証・確定されたファイルは残ります。

`--gpu-mode auto`は予算内の最大4096エントリをまとめ、収まらないエントリをストリーミングへ回します。`--batch-entries`で上限を変更できます。`stream`は非空DeflateをStreaming Gzipへ、`batch`はサイズ・予算内のDeflateだけをバッチへ送ります。Storedはコピー＋CRCです。空のDeflateエントリはCPUで圧縮本体を検証します。

GPU auto／batchでは、Stored・空ファイル・ディレクトリを先にCPUで処理し、残りのDeflateを連続したバッチへまとめます。ZIP内の並びで数千回の小さなGPU起動が発生するのを防ぎます。ファイルごとの検証・非上書き確定は維持しますが、失敗までに確定するファイルの順序は中央ディレクトリ順とは限りません。

`--gpu-order auto`は64MiB以上が2〜64件、1MiB以下が64件以上という大小混在時に、大きいファイルを先頭へ安定分割します。大ファイルを別々のバッチへ散らして直列化するのを避ける実験則です。サイズが近い入力とpipelineは並びを変えません。`--gpu-order archive`で集約後の元順を使え、`gpu_size_reorders`で並べ替えの有無を確認できます。

`--backend libdeflate`は任意のlibdeflate依存を見つけたビルドで使用できます。既存環境には`libdeflate`を追加して再ビルドしてください。`--threads`はこの経路のworker数です。`--host-limit`（既定8GiB、最小2MiB）をworker数で分配し、入力＋出力がworker予算に収まる場合だけ全バッファ解凍します。収まらないファイルはISA-Lまたはzlib Streaming＋高速CPU CRCで処理するため、従来の256MiB制限はありません。Storedは余分なコピーを省きます。`host_buffer_bytes`はworkerごとの最大データバッファ量の合計であり、ZIPメタデータ・ライブラリ内部・スレッドスタックを含むRSS全体の上限ではありません。

`--backend hybrid`はStored・空ファイル・32KiB未満・64MiB超・ほぼ非圧縮のファイルをCPUへ送り、残りを`--cpu-percent`（既定50%）の展開バイト割合を目安に分割します。GPUに渡す配列からCPU担当ファイルを除くため、混在データでもGPUバッチが分断されません。CPUは既定最大8 worker、GPUはバッチI/Oパイプラインを使い、同時に処理します。片側が失敗すると他方にも停止を要求し、両方をjoinしてから最初のエラーを返します。CPU向きの入力だけ、予算が小さい、GPUが利用不能な場合はCPUだけで完結します。`selected_backend`と`selection_reason`で実際の選択を確認できます。この分割則は調整中で、最速を保証するものではありません。

CPUの全量バッファには、worker予算に加えて`--cpu-buffer-limit`（既定64MiB）を適用します。巨大ファイルでは全量のメモリ確保・コピーを避けた方が速かった測定から採用しました。超過時も定量メモリのStreamingへ切り替えるため、ファイルサイズ制限ではありません。

ISA-Lがある場合、8MiB以上のDeflateで圧縮後サイズが元サイズの98%以上なら、予算に収まってもCPU Streamingへ回します。非圧縮に近い512MiB／256MiBの複数ファイル入力では、旧版autoと同じ条件で比較して約2倍高速になり、ピークRSSも減りました。Stored・小ファイル・ISA-Lなし構成は従来の選択を保ちます。この条件も実測に基づく暫定則で、全機種での最速保証ではありません。

既定の`--temp-mode auto`はLinuxの`O_TMPFILE`を試し、検証済みのファイルだけ`linkat`で確定します。未確定ファイルは名前を持たないため、強制終了でも一時名が残りません。対応しないfilesystemや`/proc/self/fd`を開けない環境では、従来の名前付き`.part`へ戻ります。比較には`--temp-mode named`を使えます。どちらも既存出力を上書きせず、容量不足・FD不足を成功扱いしません。JSONの`anonymous_output_files`／`named_output_files`で実際の方式を確認できます。

pipelineは各slotの単一固定化バッファに、入力を先頭、出力を末尾から配置します。最大入力と最大出力が別バッチでも、最大の「入力＋出力」だけを予約すれば足ります。次入力が前出力へ重なる場合だけ前の書き込みを待ち、未完了の出力を上書きしません。`pipeline_overlap_waits`でこの待機回数を記録します。一つのエントリでも予算に収まらない場合は、出力を書き始める前にエラーを返します。

`--path-mode auto`は、作成済みの親ディレクトリをLinux `openat2`のBENEATH／NO_SYMLINKS制約で一度に開きます。途中のsymlinkも禁止し、アプリ側のディレクトリFDキャッシュは持ちません。未作成の階層・PATH_MAXを超える長い相対パス・未対応kernelでは、成分ごとの`mkdirat`／`openat(O_NOFOLLOW)`へ戻ります。`--path-mode portable`で従来経路を選択でき、JSONの`fast_parent_opens`／`portable_parent_walks`で実際の利用を確認できます。権限・symlink・境界違反をfallbackで無視することはありません。

GPU経路にも`--host-limit`を適用します。バッチ入出力の固定化メモリと、固定バッファ分として保守的に予約する12MiBを予算化します。pipelineは2組の固定化バッファを含みます。通常経路は必要なら古い固定化バッファを解放して予算を守ります。固定分を常に予約しているため、Stored／Streamingを挟んでも予算内の固定化バッファは再利用できます。hybridはCPUに1/4、GPUに残りのホスト予算を分け、GPU側でホスト／VRAMの両予算に収まるバッチを計画します。GPUの`host_buffer_bytes`は固定分の予約を含む上限見積もりです。

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

中央ディレクトリは既定で256MiB／100万エントリまでです。`--metadata-limit`は前者、`--max-entries`はディレクトリを含むエントリ数上限を変更します。解析時の先読みバッファは中央256KiB・ローカルヘッダ4KiBですが、解析済みエントリ・ファイル名・衝突検査の一覧は別途RAMに保持します。多数の極小ファイルで上限を引き上げる場合は、メタデータ用RAMと展開先の空きinodeにも余裕を確保してください。`--host-limit`はこのメタデータ全体の上限ではありません。

`--metadata-threads auto`は、4096件以上かつローカル領域の平均間隔4KiB以上なら最大8 worker、密集した16,384件以上なら最大4 workerでローカルヘッダを検証します。それより小さいZIPは直列の先読みを使います。CPU affinityも考慮します。`--metadata-threads 1`で従来同様の直列検証、1〜32の指定で比較できます。中央ディレクトリ・パス検証、全ローカルヘッダ・descriptor検証、重複範囲検査がすべて成功するまで出力先は開きません。JSONの`metadata_threads`は実際の最大worker数です。

Rapidgzip内部の修正版ISA-Lを使う追加実験は、`bash scripts/bootstrap_rapidgzip.sh --with-isal`と`-DGIPU_RAPIDGZIP_ISAL=ON`で有効にできます。ビルドにはCコンパイラとNASMも必要です。通常のISA-Lとは`inflate_state`のABIが異なるため、同時リンクせず、GIPUのStreaming経路も同じ修正版ヘッダ／静的ライブラリへ揃えます。既定OFFで、速度と正確性を別途比較するための構成です。

2026年10月2日の作業環境では`build/gipu`と`build-unified/gipu`にGPU・libdeflate・Rapidgzip修正版ISA-Lを統合し、10統合スイートと容量検査スイートの計11スイートを検証しました。10月3日の更新版`build/gipu`では外部比較スクリプトの試験を加えた計12スイートを検証しています。単一の大きな実データは`--backend auto --auto-parallel`、既知の正しい多数ファイルでGPUを試す場合は`--backend auto --auto-gpu`を使えます。実験経路の明示許可とメモリ上限の制約は変わりません。

`--pipeline`は全ファイルが非空Deflateバッチに収まるZIP専用の実験経路です。固定化ホスト入出力を二重化し、次バッチの読み込み・GPU処理・前バッチの書き込みを重畳します。GPU arenaは一つだけでVRAM予算は変わりませんが、ホストRAMの必要量はバッチ入出力の約2倍になります。Stored／空ファイル／Streamingが必要な入力にはこの指定を外してください。各バッチ全体のGPU CRC確認が済むまでは、そのバッチの出力を書き出しません。

GPUバッチ出力は`--write-threads`（既定8、最大32）で並列化します。pipelineは最大2バッチの出力を重ねるため、既定では最大16個の出力workerに加えて先読み／GPU制御用CPUを使います。GPU処理だけでなくCPUによるファイル操作も性能に寄与します。メモリ容量は全バッチの計画から最大値を先に確保し、処理中に`cudaMallocHost`を呼び直すことによる同期を減らします。`allocation_seconds`に事前確保時間を記録します。

通常バッチの`--gpu-output auto`は、最大ファイル256KiB以上・バッチ出力128MiB以上・平均32KiB以上なら、workerごとの1MiB固定化バッファでGPUから少しずつ転送・出力します。出力全量の固定化RAMを確保しません。全バッチのサイズ・CRCを確認してから転送し、全workerが完了するまでGPU arenaを再利用しません。小窓分も`--host-limit`に含めます。`buffered`で従来の全量転送、`stream`で小さいバッチも小窓転送にでき、後者の明示指定はpipeline／hybrid／auto-gpuとは併用しません。pipelineは入力・GPU処理・出力の重畳を優先し、全量転送を維持します。この転送方式はnvCOMPのStreaming解凍とは別です。

`gpu_streamed_output_bytes`が小窓転送で出力したバイト数です。この方式の`write_seconds`は小窓確保・D2H待ち・ファイル出力の経過時間を含み、`transfer_seconds`にもCUDA eventで測った各workerのD2H時間を加算します。工程時間は重複を含むため単純に加算できません。

`--vram-limit`は、GIPUが明示的に確保する入力・出力・nvCOMP作業領域・メタデータ・CRC領域の合計を制限します。CUDAコンテキストやライブラリ内部の割り当て、他プロセスの使用量は含まれません。総VRAMの厳密な上限を保証するオプションではありません。ホストRAMにはバッチ入出力と同程度の固定化メモリが必要です。Streaming経路はアーカイブ／展開量の全体バッファを確保しません。

公開Streaming APIの出力はホスト上です。解凍カーネルが稼働中にGPU CRCカーネルを同期実行すると、大量出力で処理が進まなくなることを3090で確認しました。現在の既定`--stream-crc cpu`は出力callbackでCPU CRCを増分計算し、二度読み・CRC用のGPU再転送を省きます。libdeflateがあればその高速CRC、なければzlib CRCを使用します。バッチ経路は展開済みVRAM上でCRCを計算します。`cpu_crc_bytes`と`gpu_crc_bytes`で分担を記録します。従来の二度読み方式は`--stream-crc gpu`で選べます。どちらもRAM／VRAMを展開量に比例して確保しません。

バッチCRCは既定1MiBの区間へ分け、GPU上の展開済みデータを並列に検査します。CPUは区間のCRC値だけを`crc32_combine`でファイル順に結合するため、展開本体をCPUへ戻す必要はありません。大小のファイルが混ざったバッチで、最大ファイルだけを基準にしたCRCカーネル設定が不利になるのを避ける狙いです。`--gpu-crc-chunk whole`でファイル単位方式、4KiB〜64MiBのサイズ指定で比較できます。追加のGPU配列はVRAM予算に含みます。`gpu_crc_chunks`と`crc_combine_seconds`が区間数とCPU結合時間です。

GPU Streamingは専用workerプロセスで実行し、親へpipeで出力を送ります。親が展開サイズ・CPU CRC・一時ファイル・確定を管理します。連続したStreamingエントリではworkerを再利用し、GPUバッチへ戻る前に破棄してscratchとarenaの同時保持を防ぎます。nvCOMPのI/O callbackから例外を投げると内部joinで停止する事象を確認したための分離です。停止・出力エラー・worker異常時は親が自分のworkerだけを終了／回収します。親が強制終了した場合もLinuxのparent-death signalでworkerを止めます。`--stream-timeout`（既定120秒）はpipe出力が来ない時間の上限で、親自身のCRC／filesystem待ちは除きます。子は入力のread-only FDとpipeだけを受け取り、出力パスを開きません。`/proc/self/exe`が必要です。明示stream、または全非空DeflateがバッチAPIのサイズ上限を超える場合、CPU CRCなら親でCUDAを初期化しません。バッチ／GPU CRCを併用する場合は親子両方のCUDAコンテキストが必要です。

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

100万件超の極小ファイル試験は`--cases million-tiny --variants auto-million`で明示上限200万を使います。通常CLIの既定上限100万は維持しています。

`scripts/make_real_single.py`は既存バイナリの先頭N GiBを読み取り、単一エントリの比較用ZIP64を作ります。入力と既存出力を上書きしません。`scripts/fuzz_zip.py`はCPU経路に限り、固定seedでヘッダ・圧縮本体・切り詰め等の変異入力を試し、成功例をPython zipfileと照合します。GPUへの不正Deflate投入は行いません。

多形状比較と制約は[適応型解凍の実測](docs/measurements-adaptive-2026-10-02.md)を参照してください。`tests/cancellation.py`は単一大ファイルの処理中にSIGINT／SIGTERM／SIGKILLを送り、未確定出力とworker残留を検査します。`--worker-faults`では所有するGPU子プロセスの停止／異常終了も試します。`/usr/bin/time`のピークRSSは、GPU Streamingの親子合計ピークではない点に注意してください。

`scripts/benchmark_7zip.py`は公式7-Zipの外部比較用です。CLIバージョン・バイナリSHA256・外部時間・CPU時間・RSSを記録します。`-mmt=16`は要求値で、Deflate解凍が16並列になる保証ではありません。実展開は各回新規の一時ディレクトリに限定し、全件サイズと指定元データ128件のSHA256を検査します。既知の正しい比較用ZIPにだけ使ってください。

### OSS公開向けの互換性・形状・制約試験

対応形式、Linux／ARM64／依存構成の確認と未検証事項は[検証範囲](docs/validation-scope.md)、独立並列CPU参照を含む結果は[2026年10月3日の検証記録](docs/measurements-validation-2026-10-03.md)にまとめています。未対応形式の拒否を解凍対応とは数えず、CPU affinityや媒体の異なる結果を混ぜて速度倍率を作りません。

```bash
python3 scripts/make_validation_corpus.py --output /optane/workspace/validation-corpus
python3 scripts/benchmark_matrix.py --corpus /optane/workspace/validation-corpus \
  --cases documents-level1 deflate-level0 incompressible stored-medium \
  --variants auto gpu 7zip 7zip-parallel python-parallel --repeats 3 \
  --output-root /ssd/gipu-validation-output --report bench-results/validation.jsonl
python3 scripts/summarize_matrix.py --input bench-results/validation.jsonl \
  --output bench-results/validation-summary.json
```

`benchmark_matrix.py`は全出力集合・サイズと、合成コーパスの全ファイルSHA256を確認します。報告にはバイナリ／入力／比較スクリプトのSHA256、各回の時間、実際のCPU／GPU経路を保存します。途中中断、欠けた反復、異なる入力、失敗を含む比較からは公開用倍率を作りません。既存のコーパス・報告は上書きしません。

外部比較には7-Zipとunzipも用意し、`--sevenzip /path/to/7zz --unzip /path/to/unzip`で指定できます。本測定の7-Zipは公式26.03です。CIで使う旧版p7zipは互換性確認用であり、同じ性能の比較対象とは扱いません。

`reference_7zip_parallel.py`は最大16個の7-Zipプロセスへ独立エントリを分担する比較用CPU参照です。単一Deflate内部の並列処理や、最速のCPU製品を代表するものではありません。未知のZIPに使う汎用安全解凍ツールではなく、既知の正しい試験入力だけに使用します。

改善前のバイナリを比較する場合は`--reference-binary /path/to/previous-gipu --variants auto auto-reference`を使えます。旧版と新版を同じ条件で交互に測り、両方のバイナリSHA256を報告へ記録します。過去の別報告から時間を寄せ集めて改善倍率を作りません。

本体は[MITライセンス](LICENSE)ですが、GPU SDKなど依存の再配布条件とは別です。依存と公開物の境界は[THIRD_PARTY.md](THIRD_PARTY.md)を参照してください。本体のMIT採用によって、SDKを含むバイナリの再配布監査が完了したとは扱いません。

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

試験スクリプトは小ファイルの論理サイズだけでなく、filesystemのblock単位へ切り上げたデータ量、親ディレクトリ・inodeの見積もりも加え、各反復前に空き容量と空きinodeを確認します。filesystem固有の実使用量や、他プロセスとの空き容量競合まで保証するものではありません。

実展開の既定は通常のwrite完了までで、永続化まで測る場合は`--sync`を指定します。GPUの`decode_seconds`／`crc_seconds`／`transfer_seconds`はCUDA event時間、読み込み／出力はCPU側の経過時間です。libdeflateの各工程時間はworkerの加算値で、並列時の実経過時間とは異なります。

初期版のGPU経路は既知の正しいDeflateストリームで検証する実験実装です。nvCOMPは破損した圧縮入力に対する動作を保証しておらず、ZIPヘッダ検査と展開後CRCだけで解凍中の安全性は保証できません。不明な配布元や破損が疑われる入力は、まず`--backend cpu`で検証してください。圧縮ストリームの安全なGPU検証は未実装です。

## 開発方針

1. ZIP解析と安全な出力、CPU参照経路を検証する。
2. ZIPの圧縮本体にGzipヘッダとトレーラを仮想的に付け、nvCOMPのStreaming Gzipへ接続する。
3. メモリ予算内の複数エントリをnvCOMPバッチDeflateへまとめる。
4. GPU CRC32、実機での正確性・メモリ使用量・速度を検証する。
5. 7-Zip／libdeflateと比較し、測定で判明したボトルネックを改善する。

暗号化ZIP、Deflate64、BZip2、LZMA、分割ZIP、特殊ファイル、Windowsは初期版の対象外です。

現時点では元ZIPのタイムスタンプ・実行権限などの属性は復元せず、通常ファイルとして出力します。確定時の権限は`0644 & ~umask`とし、利用者の制限を緩めません。新規ディレクトリは`0755`にumask／filesystem側のACLが適用されます。ZIP由来のACLは復元しません。対応していない方式を黙って読み飛ばすことはありません。

`--sync`では出力ファイル・直接の親だけでなく、内部で新設する階層と、出力先自体の上位ディレクトリも同期します。後者は起動時に一度だけ行います。上位ディレクトリを開けない、またはfilesystemがディレクトリのfsyncを扱えない場合は、同期成功とせずエラーにします。通常の非同期モードの処理量は増やしません。電源断の実機試験をしたという意味ではありません。

設計の依存API: [nvCOMP Native API](https://docs.nvidia.com/cuda/nvcomp/native_api.html)、[C API](https://docs.nvidia.com/cuda/nvcomp/c_api.html)、[CRC32](https://docs.nvidia.com/cuda/nvcomp/crc32.html)。Native APIは実験的なため、SDKのバージョンを固定します。

## 2026年10月1日の実機検証

Ubuntu 26.04.1、RTX 3090（24GiB）、CUDAランタイム13.0、nvCOMP 5.3.0で確認しました。

- CPU／GPU auto／GPU stream／GPU batchの4統合テストスイートが成功。1スイート22テストで、経路に該当しないテストはskipしています。
- CPU経路のAddressSanitizer／UndefinedBehaviorSanitizer検証が成功。
- **26GiBの単一ZIP64エントリ**で展開サイズとGPU CRCが一致。プロセスVRAMのサンプリング最大値は**270MiB**、アプリが明示的に確保する作業領域は9,633,904 bytes。高圧縮率のゼロデータでの成立確認であり、26GiBの圧縮入力自体を処理した検証ではありません。
- 256MiB／16エントリの合成ZIPを展開・fsync・SHA256確認した比較では、CLI全体時間の中央値がCPU zlib **0.181秒**、GPU **0.446秒**。この条件ではGPUが約2.47倍遅く、CPUより超高速という目標は未達です。

測定条件・解釈と次の改善対象は[実測記録](docs/measurements-2026-10-01.md)、実装の分担と制約は[構成](docs/architecture.md)を参照してください。

## 2026年10月2日：Kaggle実データ50GBの初期比較

以下は5時間の追加改善を始める前の測定です。最新値と自動選択は冒頭と[適応型解凍の実測](docs/measurements-adaptive-2026-10-02.md)を参照してください。

Optaneのworkspaceに**50.001GBの標準ZIP64**を作成しました。元データは2TB SSD側のKaggle RSNA DICOM、155,925ファイル・展開後109.557GBです。元データは変更していません。

| 経路 | 解凍＋CRC（書き込みなし） | SSDへの実展開 |
|---|---:|---:|
| zlib単一CPU、初回1回の基準値 | 277.07秒 | 311.93秒 |
| libdeflate 16スレッド、当時の3回中央値 | 13.90秒 | 60.18秒 |
| GPU pipeline、4GiB、当時の3回中央値 | 19.01秒 | **54.85秒** |

単一CPU zlib基準比は、書き込みなしで約14.58倍、実展開で**約5.69倍**でした。選んだデータの中央値では5倍目標を超えています。ただし最速CPU比5倍ではありません。実展開の16スレッドCPU比は中央値で約1.10倍、時間の範囲も重なり、安定した優位性までは示せていません。書き込みなしでは並列CPUの方が速い結果です。

GPUの実展開3回は47.02／67.42／54.85秒でした。**fsyncによる全件永続化は含まない**通常write完了までの比較で、全件CRC・出力サイズと元データ128件のSHA256を確認しています。試験用の展開物だけ削除し、50GB ZIPと元データは残しました。GPU解凍区間は約11.5〜11.7秒と安定し、残る主要な変動は書き込み側です。CUDAバッファ、固定化メモリ、GPU CRC、I/O重畳、並列ファイル出力を最適化し、カーネルやドライバ自体は変更していません。

このZIPでGPUを明示比較するコマンド（最新の既定autoはCPU）:

```bash
build/gipu extract /srv/workspace/sora/gipu-bench/kaggle-50GB.zip \
  --output /home/sora/gipu-extracted --pipeline --vram-limit 4G \
  --batch-entries 4096 --write-threads 8 --json
```

展開先には約110GB＋余裕が必要です。pipelineは最大16個のCPU出力workerを使います。元データ・医用画像・ローカルマニフェストはGitHubへ公開していません。測定方法、各回の値、比較対象の制約、ZIPのSHA256は[50GB実測記録](docs/measurements-kaggle-50gb-2026-10-02.md)と[集計JSON](docs/benchmarks/kaggle50-2026-10-02.json)を参照してください。
