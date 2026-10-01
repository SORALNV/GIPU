# 初期実装の構成

参照会話の最終方針（Ubuntu＋RTX 3090＋nvCOMP）に沿った構成です。

| ファイル | 役割 |
|---|---|
| `src/zip.cpp` | `pread`によるZIP32／ZIP64中央ディレクトリ、ローカルヘッダ、Data Descriptorの検証 |
| `src/io.cpp` | 出力パスを`openat`／`O_NOFOLLOW`でたどる処理、一時出力、サイズ制限付きsink、仮想Gzip入力 |
| `src/gpu.cpp` | nvCOMPのバッチDeflate、Streaming Gzip、GPU CRC、メモリ予算に基づく選択 |
| `src/cpu.cpp` | zlibのCPU参照実装。GPU失敗時の暗黙のフォールバックには使わない |
| `src/main.cpp` | CLI、合計出力上限、JSON統計 |
| `tests/integration.py` | 標準zipfileで作ったZIPとの互換性、不正メタデータ、出力保護、予算による経路選択 |

## バッチ経路

1. 圧縮／展開サイズの上限を確認する。単一Deflateストリームを任意の位置では分割しない。
2. APIでアラインメントとscratchサイズを取得する。
3. 入力・出力・scratch・メタデータ・共通CRC領域を予算化する。
4. 既定最大4096エントリを固定化ホスト入力からGPUへ転送する。候補全体を計画し、予算不足時だけ二分探索で縮める。
5. バッチDeflateを実行し、各エントリのstatusと実際の展開サイズを確認する。
6. GPU上の出力に対してバッチCRCを実行する。
7. 全エントリのCRC成功後、ホストへ戻した出力をファイルへ流し、検証済みファイルを確定する。

## ストリーミング経路

仮想Gzipは10バイトのヘッダ、ZIP内の指定範囲のRaw Deflate、8バイトのCRC／サイズトレーラを生成します。再圧縮や中間Gzipファイルは作りません。入力側は1MiBのバッファを再利用します。

Streaming APIの出力を、宣言サイズを超えないsinkから一時ファイルへ書きます。APIが完了してから、そのファイルを4MiBずつ再読み込みし、GPUでCRCを増分計算します。CRCとサイズが一致した出力だけを確定します。`test`では名前なしの一時ファイルを使います。

この二段階処理は実機で確認した停止問題への対処です。nvCOMPのpersistent解凍カーネル実行中に、出力callbackから別CUDAストリームのCRCカーネルへ同期すると、32MiBの高圧縮率エントリで進行が止まりました。CRCを軽いwarpカーネルへ変えても解消しなかったため、初期版では解凍とCRCを時間的に分離します。カーネル資源の競合が原因である可能性はありますが、ライブラリ内部の根本原因までは特定していません。

Storedはデコードがないため、コピーしながらGPUでCRCを計算できます。

## 保証範囲

- アーカイブ本体や単一展開データをRAM／VRAMへ一括で確保しません。中央ディレクトリのエントリ一覧はCPUメモリに保持するため、その部分はエントリ数に比例します（上限100万）。
- `--vram-limit`はアプリの明示的な確保量の上限です。CUDA／nvCOMP内部、他アプリを含む総VRAMの上限ではありません。
- 解凍とCRC計算はGPUへ送ります。名前処理、ZIPメタデータの検査、I/O、CRC値の最終比較などの制御処理はCPUです。nvCOMP内部の全処理を監査したわけではありません。
- パス検証と展開後CRCは、破損DeflateをGPUへ渡したときのライブラリの安全性を保証しません。初期版のGPU経路は既知の正しい入力向けです。
- SIGINT／SIGTERMは読み込み・出力・CRCの境界で検出します。長時間のライブラリ内部処理を即座に中断することは保証しません。
- 初期版は同一アーカイブの並列読み書きやCUDA転送の本格的なパイプライン化を行いません。先に正確性と測定基盤を作っています。

## 次の実装順序

1. libdeflate／7-Zipとの比較、入力サイズ・エントリ数・圧縮率別の性能測定。
2. CUDA初期化、ホスト固定化メモリの確保、I/O、GPU decode、GPU CRCを区別できる計測。
3. バッチ用バッファの再利用と入力・転送・書き込みの重畳。
4. 大きい単一バッファのGzip LOOKAHEAD比較と、Streaming CRC二度読みを減らす組み込み方の調査。
5. 不正圧縮入力の安全な検証、ファジング、キャンセル応答の改善。

依存API: [nvCOMP Native API](https://docs.nvidia.com/cuda/nvcomp/native_api.html)、[nvCOMP C API](https://docs.nvidia.com/cuda/nvcomp/c_api.html)、[nvlzcatの要件](https://docs.nvidia.com/cuda/nvcomp/nvlzcat.html)、[ZIP形式仕様](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT)。
