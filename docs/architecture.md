# 初期実装の構成

参照会話の最終方針（Ubuntu＋RTX 3090＋nvCOMP）に沿った構成です。

| ファイル | 役割 |
|---|---|
| `src/zip.cpp` | `pread`によるZIP32／ZIP64中央ディレクトリ、ローカルヘッダ、Data Descriptorの検証 |
| `src/io.cpp` | 出力パスを`openat`／`O_NOFOLLOW`でたどる処理、一時出力、サイズ制限付きsink、仮想Gzip入力 |
| `src/gpu.cpp` | nvCOMPのバッチDeflate、Streaming Gzip、GPU CRC、再利用arena、I/Oパイプライン |
| `src/cpu.cpp` | zlibのCPU参照実装。GPU失敗時の暗黙のフォールバックには使わない |
| `src/libdeflate.cpp` | workerごとの全エントリCPU解凍、CRC、原子的な出力。比較用の1〜32 worker経路 |
| `src/main.cpp` | CLI、合計出力上限、JSON統計 |
| `tests/integration.py` | 標準zipfileで作ったZIPとの互換性、不正メタデータ、出力保護、予算による経路選択 |

## バッチ経路

1. 圧縮／展開サイズの上限を確認する。単一Deflateストリームを任意の位置では分割しない。
2. APIでアラインメントとscratchサイズを取得する。
3. 入力・出力・scratch・メタデータ・共通CRC領域を予算化する。
4. 既定最大4096エントリを固定化ホスト入力からGPUへ転送する。候補全体を計画し、予算不足時だけ二分探索で縮める。
5. バッチDeflateを実行する。
6. GPU上の出力に対してバッチCRCを実行し、status・実際の展開サイズ・CRCをホストへ戻す。`extract`だけ展開データも戻す。
7. バッチ全体のstatus・サイズ・CRCを確認した後、ホスト出力をファイルへ流し、検証済みファイルを確定する。

GPU領域は256byte境界の単一arenaとし、必要容量が増えた場合だけ再確保します。旧arenaを解放してから新arenaを作り、Streamingへ移る場合にもarenaを解放します。固定化ホストバッファも再利用します。バッチ計画は候補全体を一度見積もり、予算を超える場合だけ二分探索で縮めます。

## I/Oパイプライン

`--pipeline`では全ファイルがバッチ経路に収まることを事前に確認します。GPU arenaは一つだけ、ホスト入力・出力は2組です。別workerで次の圧縮入力を先読みし、GPUによる解凍・CRC・結果転送の間に、検証済みの前バッチを出力workerが書き出します。次の入力を準備するworkerと出力workerが同じホストスロットを使う場合も、入力領域と出力領域は別です。出力領域を再利用する前に、そのスロットの出力完了を待ちます。

全バッチ計画の最大input／output／arena容量を処理前に確保します。先読みworkerが処理中に固定化メモリを再確保することによるCUDA同期を減らすためです。入出力の最大値が別バッチに現れる場合もあるため、RAM消費は一つのバッチ実容量の厳密な2倍とは限りません。

1バッチのファイル操作は既定8 workerで行います（`--write-threads`）。pipelineでは最大2バッチを出力中に保持するため、最大16個の出力workerになります。エントリ番号をatomicで割り当て、最初の例外を共有して新規処理を止め、workerをjoinしてから再送出します。ファイル単位の検証・上書き拒否・原子的な確定は共通OutputFileを使います。

例外時にもfutureをjoinしてからバッファ・ZIP・出力rootを破棄します。全バッチの出力完了を確認してから成功を返します。Stored・空Deflate・予算に収まらないエントリを含む場合は明示的に拒否する実験経路で、通常のauto経路には影響しません。

CUDA eventでH2D／D2H、decode、CRCを分離し、CPU側でread／write／ZIP解析時間を測ります。重畳した工程の時間は足し合わせても実経過時間にはなりません。CPU libdeflateでは各workerの工程時間を加算します。

## ストリーミング経路

仮想Gzipは10バイトのヘッダ、ZIP内の指定範囲のRaw Deflate、8バイトのCRC／サイズトレーラを生成します。再圧縮や中間Gzipファイルは作りません。入力側は1MiBのバッファを再利用します。

Streaming APIの出力を、宣言サイズを超えないsinkへ送ります。既定はCPU CRCをcallbackで増分計算し、`extract`だけ一時ファイルへ書きます。CRCとサイズが一致した出力だけを確定します。`test`は展開データを保存しません。libdeflate未導入でもzlib CRCを使用できます。

従来の二段階方式も`--stream-crc gpu`で比較できます。API完了後に出力を4MiBずつ読み直しGPU CRCを増分計算するため、`test`でも名前なし一時ファイルが必要です。nvCOMPのpersistent解凍カーネル実行中に、出力callbackから別CUDAストリームのCRCカーネルへ同期すると、32MiBの高圧縮率エントリで進行が止まりました。CRCを軽いwarpカーネルへ変えても解消しなかったため、GPU CRCを選ぶ場合は解凍とCRCを時間的に分離します。カーネル資源の競合が原因である可能性はありますが、ライブラリ内部の根本原因までは特定していません。

Storedはデコードがないため、コピーしながら指定したCPU／GPU CRCを計算できます。Streamingの`decode_seconds`は公開APIの入出力とCPU CRC callbackを含み、バッチのCUDA eventによる純粋なdecode時間とは定義が異なります。

## 保証範囲

- GPU経路はアーカイブ本体やVRAMを超える単一展開データをRAM／VRAMへ一括で確保しません。中央ディレクトリのエントリ一覧はCPUメモリに保持するため、その部分はエントリ数に比例します（上限100万）。比較用libdeflateだけはworkerごとにエントリ全体をCPU RAMへ置き、圧縮／展開サイズ各256MiBまでに制限します。
- `--vram-limit`はアプリの明示的な確保量の上限です。CUDA／nvCOMP内部、他アプリを含む総VRAMの上限ではありません。
- GPUバッチでは解凍とCRCをGPUへ送り、Streamingでは既定でCPU CRCを併用します。名前処理、ZIPメタデータの検査、I/O、CRC値の最終比較などの制御処理はCPUです。nvCOMP内部の全処理を監査したわけではありません。
- パス検証と展開後CRCは、破損DeflateをGPUへ渡したときのライブラリの安全性を保証しません。初期版のGPU経路は既知の正しい入力向けです。
- SIGINT／SIGTERMは読み込み・出力・CRCの境界で検出します。長時間のライブラリ内部処理を即座に中断することは保証しません。
- パイプラインはCPUの読み込み・GPU処理・CPUの書き込みを重畳します。GPU decodeとGPU CRC自体の並列化や、Streaming callbackとの並行CRCは行いません。

## 次の実装順序

1. 7-Zipとの比較、入力サイズ・エントリ数・圧縮率別の性能測定を広げる。
2. 50GB実データの工程別測定に基づいて、固定化メモリ再確保、ファイル操作、ストレージの律速を詰める。
3. GPU処理とDMAを複数CUDA streamで重ねる場合の予算・正確性を検証する。
4. 大きい単一バッファのGzip LOOKAHEAD比較と、Streaming CRC二度読みを減らす組み込み方の調査。
5. 不正圧縮入力の安全な検証、ファジング、キャンセル応答の改善。

依存API: [nvCOMP Native API](https://docs.nvidia.com/cuda/nvcomp/native_api.html)、[nvCOMP C API](https://docs.nvidia.com/cuda/nvcomp/c_api.html)、[nvlzcatの要件](https://docs.nvidia.com/cuda/nvcomp/nvlzcat.html)、[ZIP形式仕様](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT)。
