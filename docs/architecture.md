# 実装の構成

参照会話の最終方針（Ubuntu＋RTX 3090＋nvCOMP）に沿った構成です。

| ファイル | 役割 |
|---|---|
| `src/zip.cpp` | `pread`によるZIP32／ZIP64中央ディレクトリ、ローカルヘッダ、Data Descriptorの検証 |
| `src/io.cpp` | 出力パスを`openat`／`O_NOFOLLOW`でたどる処理、一時出力、サイズ制限付きsink、仮想Gzip入力 |
| `src/gpu.cpp` | nvCOMPのバッチDeflate、Streaming Gzip、GPU CRC、再利用arena、I/Oパイプライン |
| `src/cpu.cpp` | zlibのCPU参照実装。GPU失敗時の暗黙のフォールバックには使わない |
| `src/libdeflate.cpp` | workerごとの予算付き全エントリCPU解凍、巨大ファイルのStreaming切替、CRC、原子的な出力 |
| `src/rapidgzip.cpp` | seek可能な仮想Gzip、任意のRapidgzip依存による単一ストリームCPU並列実験 |
| `src/checksum.cpp` | libdeflate／zlibのCPU増分CRC共通処理 |
| `src/hybrid.cpp` | エントリ特性とバイト割合によるCPU/GPUの分担、並行実行、GPU不在時のCPU切替 |
| `src/adaptive.cpp` | CPU affinity、既定CPU選択、明示許可したGPU／単一ファイル並列の選択 |
| `src/stream_worker.cpp` | GPU Streaming子プロセスの起動・再利用・pipe出力監視・停止・回収 |
| `src/backend.cpp` | 並行／順次処理の共通統計集計 |
| `src/main.cpp` | CLI、合計出力上限、JSON統計 |
| `tests/integration.py` | 標準zipfileで作ったZIPとの互換性、不正メタデータ、出力保護、FD／ファイルサイズ不足、予算による経路選択 |
| `tests/cancellation.py` | 自分が起動した解凍プロセスのSIGINT／SIGTERM／SIGKILLと未確定出力の保護 |

## バッチ経路

ZIP解析では中央ディレクトリ256KiBとローカルヘッダ4KiBの窓を使い、短い`pread`の繰り返しを抑えます。ASCII名はUTF-8／CP437共通なのでiconvを省き、非ASCII名には従来どおり変換と妥当性検査を適用します。中央ディレクトリの宣言サイズは確保前に`--metadata-limit`で確認します。

中央のパス・属性検証とローカル検証を分離し、多数のローカルヘッダには最大4／8 workerを適応的に使います。各workerが独立した先読み窓と担当エントリを持ち、例外を集約して全workerをjoinします。最後の範囲重複検査まで成功することが、出力先を開く前提です。

Data Descriptorは先頭4byteだけで署名の有無を決めず、中央のCRC・圧縮サイズ・展開サイズを照合します。CRC自体が署名値`0x08074b50`と同じ場合も、署名なし形式を正しく扱います。ZIP64 descriptorの幅はローカル追加フィールドの存在も確認します。FIFO入力は非blockingで開いた直後に通常ファイル検査で拒否し、writer待ちになりません。

中央・ローカル・ZIP64終端の必要バージョンは6.3以下に限定し、それに加えて対応する方式・フラグだけを許可します。6.3以下の全機能を実装したという意味ではありません。追加ファジングで見つかった、将来版17.3を黙って受け入れる差分を回帰テストにしています。

1. 圧縮／展開サイズの上限を確認する。単一Deflateストリームを任意の位置では分割しない。
2. APIでアラインメントとscratchサイズを取得する。
3. 入力・出力・scratch・メタデータ・共通CRC領域を予算化する。
4. 既定最大4096エントリを固定化ホスト入力からGPUへ転送する。候補全体を計画し、予算不足時だけ二分探索で縮める。
5. バッチDeflateを実行する。
6. GPU上の出力を既定1MiB区間に分けてCRCを計算し、status・実際の展開サイズ・CRCをホストへ戻す。区間CRC値だけをCPUで結合する。`extract`だけ展開データも戻す。
7. バッチ全体のstatus・サイズ・CRCを確認した後、ホスト出力をファイルへ流し、検証済みファイルを確定する。

GPU領域は256byte境界の単一arenaとし、必要容量が増えた場合だけ再確保します。旧arenaを解放してから新arenaを作り、Streamingへ移る場合にもarenaを解放します。固定化ホストバッファも再利用します。バッチ計画は候補全体を一度見積もり、予算を超える場合だけ二分探索で縮めます。

通常バッチの大きな出力は、全量のホスト出力バッファに代えてworkerごと1MiBの固定化窓へD2Hし、その窓を再利用して書き出せます。CRC確認後にだけ開始し、各workerが専用CUDA stream／event／窓を所有します。全D2Hと書き込みをjoinするまでGPU arenaを保持します。窓の容量をホスト予算に追加し、方式は二分探索の候補全体で固定して予算判定の単調性を保ちます。pipelineではこの方式を使いません。

通常のGPU auto／batchはStored・空・ディレクトリをCPUで先に処理してバッチ分断を抑えます。少数の64MiB以上と多数の1MiB以下が混ざる場合だけ、大ファイルを先頭へ安定分割します。検証・出力内容は変えず、確定順は変わり得ます。pipelineの物理順読み取りは保持します。

## I/Oパイプライン

各バックエンドは検証済みArchiveとエントリポインタの選択配列を受け取ります。ZIPの再解析・元データの複製は不要です。hybridはCPU向きファイルを先に分離し、残りをCPU担当バイト割合で分割します。CPUとGPUは重ならない出力名にだけ書き込み、共通のOutputRoot／OutputFileによる保護を使います。両経路が終わるまで参照元と配列を保持し、最初の例外を保存して停止要求を共有します。

`--pipeline`では全ファイルがバッチ経路に収まることを事前に確認します。GPU arenaは一つだけ、ホスト入力・出力は2組です。別workerで次の圧縮入力を先読みし、GPUによる解凍・CRC・結果転送の間に、検証済みの前バッチを出力workerが書き出します。次の入力を準備するworkerと出力workerが同じホストスロットを使う場合も、入力領域と出力領域は別です。出力領域を再利用する前に、そのスロットの出力完了を待ちます。

全バッチ計画の最大(input＋output)／arena容量を処理前に確保します。各slotは単一の固定化領域とし、入力は先頭、出力は末尾へ置きます。先読みworkerが処理中に固定化メモリを再確保することによるCUDA同期を減らし、異なるバッチの入力最大と出力最大を別々に予約する無駄を省きます。次入力が前出力へ重なる場合だけwriterをjoinし、`pipeline_overlap_waits`に記録します。

`--host-limit`から固定分12MiBを引いた予算を固定化input/outputへ割り当てます。pipelineの2組は事前に最大容量を確認します。hybridはCPU・GPUへホスト予算を分配し、GPU側の計画でホスト／VRAM双方の上限を守ります。ホスト予算にCUDA/nvCOMP内部、ZIPエントリ一覧、OSページキャッシュは含みません。

1バッチのファイル操作は既定8 workerで行います（`--write-threads`）。pipelineでは最大2バッチを出力中に保持するため、最大16個の出力workerになります。エントリ番号をatomicで割り当て、最初の例外を共有して新規処理を止め、workerをjoinしてから再送出します。ファイル単位の検証・上書き拒否・原子的な確定は共通OutputFileを使います。

例外時にもfutureをjoinしてからバッファ・ZIP・出力rootを破棄します。全バッチの出力完了を確認してから成功を返します。Stored・空Deflate・予算に収まらないエントリを含む場合は明示的に拒否する実験経路で、通常のauto経路には影響しません。

CUDA eventでH2D／D2H、decode、CRCを分離し、CPU側でread／write／ZIP解析時間を測ります。重畳した工程の時間は足し合わせても実経過時間にはなりません。CPU libdeflateでは各workerの工程時間を加算します。

## ストリーミング経路

仮想Gzipは10バイトのヘッダ、ZIP内の指定範囲のRaw Deflate、8バイトのCRC／サイズトレーラを生成します。再圧縮や中間Gzipファイルは作りません。入力側は1MiBのバッファを再利用します。

Streaming APIは専用子プロセス内で動かします。既に検証した圧縮範囲・展開サイズ・CRCを小さな要求レコードで渡し、元ZIPのread-only FDを共有します。再解析は不要です。子の出力はpipe経由で親のサイズ制限sinkへ送り、親でCPU CRCを増分計算し、`extract`だけ一時ファイルへ書きます。CRC・サイズ・workerの完了レコードが一致した出力だけを確定します。`test`は展開データを保存しません。

callbackの例外後にnvCOMP内部の`std::thread::join`が終了しないことをGDBで確認しました。子は出力パスを一切持たず、親が100ms間隔で停止要求と無進捗timeoutを監視します。異常時は自分の子だけをSIGKILL／waitpidで回収してから、親が未確定出力を片付けます。親のSIGKILLにはPR_SET_PDEATHSIGで対応します。正常な連続Streamingは同じ子を再利用し、バッチへ戻る前に破棄します。プロセス分離は破損Deflateの安全性を保証するものではありません。

Streaming＋CPU CRCだけで完結すると判断できる場合は、親でCUDAを初期化しません。子がデバイス要件とscratch予算を確認し、完了レコードでscratch量も返します。親はそのレコードと予算を照合し、不要な親コンテキストと4MiB GPU CRC領域を省きます。バッチを併用する場合は従来どおり親側の領域を予算から控除して子へ渡します。

従来の二段階方式も`--stream-crc gpu`で比較できます。API完了後に出力を4MiBずつ読み直しGPU CRCを増分計算するため、`test`でも名前なし一時ファイルが必要です。nvCOMPのpersistent解凍カーネル実行中に、出力callbackから別CUDAストリームのCRCカーネルへ同期すると、32MiBの高圧縮率エントリで進行が止まりました。CRCを軽いwarpカーネルへ変えても解消しなかったため、GPU CRCを選ぶ場合は解凍とCRCを時間的に分離します。カーネル資源の競合が原因である可能性はありますが、ライブラリ内部の根本原因までは特定していません。

Storedはデコードがないため、コピーしながら指定したCPU／GPU CRCを計算できます。Streamingの`decode_seconds`は公開APIの入出力とCPU CRC callbackを含み、バッチのCUDA eventによる純粋なdecode時間とは定義が異なります。

## 保証範囲

出力権限はworker起動前に取得したumaskを反映し、検証後のfchmodでも制限を緩めません。`--sync`では新しく作るディレクトリのinodeと親の名前も同期し、出力rootの上位は初期化時に一度同期します。同期エラーは成功扱いしません。電源断試験やすべてのfilesystemの永続性を保証するものではありません。

- GPU経路はVRAM予算を超える単一展開データをRAM／VRAMへ一括で確保しません。中央ディレクトリのエントリ一覧はCPUメモリに保持するため、その部分はエントリ数に比例します（上限100万）。libdeflateは`--host-limit`をworker間で分配し、予算を超える単一ファイルだけCPU Streamingへ切り替えます。全量バッファを解放してから2MiBのStreamingバッファを確保し、同時保持しません。バッファのゼロ初期化を省き、Storedの不要なコピーも省きます。
- `--vram-limit`はアプリの明示的な確保量の上限です。CUDA／nvCOMP内部、他アプリを含む総VRAMの上限ではありません。
- GPUバッチでは解凍とCRCをGPUへ送り、Streamingでは既定でCPU CRCを併用します。名前処理、ZIPメタデータの検査、I/O、CRC値の最終比較などの制御処理はCPUです。nvCOMP内部の全処理を監査したわけではありません。
- パス検証と展開後CRCは、破損DeflateをGPUへ渡したときのライブラリの安全性を保証しません。初期版のGPU経路は既知の正しい入力向けです。
- SIGINT／SIGTERMは読み込み・出力・CRCの境界で検出します。長時間のライブラリ内部処理を即座に中断することは保証しません。
- Rapidgzipのchunk・worker設定は内部メモリの目安であり、RSSの厳密な上限ではありません。巨大な単一Deflateブロックではchunk上限を超えることがあります。自動的に有効にせず、明示した実験経路だけで使用します。
- パイプラインはCPUの読み込み・GPU処理・CPUの書き込みを重畳します。GPU decodeとGPU CRC自体の並列化や、Streaming callbackとの並行CRCは行いません。

## 残る調査対象

1. 7-Zipとの比較、入力サイズ・エントリ数・圧縮率別の性能測定を広げる。
2. 50GB実データの工程別測定に基づいて、固定化メモリ再確保、ファイル操作、ストレージの律速を詰める。
3. GPU処理とDMAを複数CUDA streamで重ねる場合の予算・正確性を検証する。
4. LOOKAHEAD・Rapidgzip・ISA-Lの得意な圧縮率を比較し、単一ストリームの選択則を他のデータへ検証する。
5. 不正圧縮入力の安全なGPU事前検証と、バッチ内部停止の応答改善。CPUファジングとStreamingの停止分離は実装済みだが、GPUの不正入力安全性は未保証。

依存API: [nvCOMP Native API](https://docs.nvidia.com/cuda/nvcomp/native_api.html)、[nvCOMP C API](https://docs.nvidia.com/cuda/nvcomp/c_api.html)、[nvlzcatの要件](https://docs.nvidia.com/cuda/nvcomp/nvlzcat.html)、[ZIP形式仕様](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT)。
