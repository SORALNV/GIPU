# 2026年10月1日の初期実装・実測記録

## 環境

| 項目 | 値 |
|---|---|
| OS | Ubuntu 26.04.1 LTS |
| カーネル | Linux 7.0.0-34-generic |
| CPU | AMD Ryzen 9 7945HX（16コア／32スレッド） |
| GPU | NVIDIA GeForce RTX 3090、24GiB |
| NVIDIAドライバ | 595.91.07 |
| CUDAランタイム | 13.0.96 |
| nvCOMP | 5.3.0.16、CUDA 13版 |
| コンパイラ | conda-forge GCC 14.4.0 |
| CPU参照 | zlib 1.3.2、1スレッド |

デスクトップを表示中のGPUで測定しており、専有環境ではありません。一般的なCPU解凍ツール、特にマルチスレッド7-Zipの性能を代表する比較ではありません。

## 正確性

`ctest`でCPU参照／GPU auto／GPU stream／GPU batchの4スイートが通りました。各スイート22テストを定義しています（CPUではGPU専用2テスト、stream／batchではautoスケジューラ1テストをskip）。ASan／UBSanを有効にしたCPUビルドも通りました。

ZIP64終端・中央ディレクトリ64bitフィールド、32bit／64bit Data Descriptor、署名なしDescriptor、Stored、Deflate、空ファイル、複数エントリ、UTF-8／CP437、パス攻撃、既存シンボリックリンク、既存ファイル、重複パス、CRC不一致時の一時出力削除、予算によるバッチ分割とストリーミングへの切り替えを検証しました。

破損DeflateをGPUへ投入するファジングは行っていません。ZIPメタデータの不正はGPU起動前に拒否し、CRC不一致テストはStoredの正しいデータに誤ったCRC値を付けて行いました。

## VRAMを超える単一エントリ

```bash
python3 scripts/verify_large.py --binary build/gipu --gib 26 \
  --vram-limit 64M --save bench-results/large-26gib.json
```

| 項目 | 結果 |
|---|---|
| 展開量 | 27,917,287,424 bytes（26GiB）、1エントリ |
| 圧縮ZIPサイズ | 121,821,708 bytes |
| 入力パターン | ゼロデータ、ZIP Deflate level 1、ZIP64 |
| 解凍経路 | nvCOMP Streaming Gzip、GPU増分CRC |
| アプリ明示確保量 | 9,633,904 bytes（約9.19MiB） |
| プロセスVRAMのサンプリング最大値 | 270MiB（1,074サンプル） |
| CLI内部時間 | 127.037秒 |
| 外部時間 | 127.094秒 |
| 展開サイズ・CRC照合 | 成功 |

名前なし一時ファイルに全出力を流し、解凍完了後にGPU CRCを検証しています。終了時、一時出力と検証用ZIPは削除しました。速度のベンチマークではなく、容量と正確性の成立確認です。実行中に小規模な統合テストも走らせたため、時間値は専有環境の速度として扱いません。

この結果は**展開後の単一ファイルがVRAMを超える場合**の確認です。圧縮入力自体がVRAMを超えるZIPや、より低圧縮率の大容量データの実機検証は未実施です。VRAMは約0.1秒間隔の外部サンプリング値であり、瞬間的な割り当てピークを保証する値ではありません。`--vram-limit 64M`の64MiBよりプロセスVRAMが大きいのは、CUDAコンテキストや内部メモリを含めているためです。

## CPUとの初回比較

```bash
python3 scripts/benchmark.py --binary build/gipu --total-mib 256 \
  --entries 16 --repeats 3 --extract --sync \
  --save bench-results/initial-extract.json
```

同じASCII文字列を繰り返す256MiBを16エントリへ分け、Deflate level 6で圧縮しました。ZIPサイズは784,998 bytesです。CPU／GPUの実行順序を交互に変更し、3回の中央値を使っています。OSキャッシュを排除していません。

| 経路 | CLI全体時間の中央値 | CLI内部時間（3回） |
|---|---:|---|
| CPU zlib | 0.181002秒 | 0.180035、0.178228、0.177757秒 |
| GPUバッチDeflate＋GPU CRC | 0.446378秒 | 0.455009、0.389964、0.385798秒 |

GPUは1バッチ、明示確保量273,413,721 bytesでした。各ファイルのfsyncと親ディレクトリのfsyncを含み、測定終了後にSHA256を元データと照合しました。SHA256計算自体は測定時間に含めていません。CUDA初期化とCLI終了のコストは外部時間に含めています。

**この条件ではGPUが約2.47倍遅く、速度目標は未達です。** 正確性・大容量処理の基盤は成立しましたが、一般的なZIPへの高速化効果は確認できていません。GPU処理、固定化ホストメモリの確保、I/O、初期化を分けた計測がまだないため、この測定だけでボトルネックを断定しません。

次はエントリ数・サイズ・圧縮率別の測定とlibdeflate／7-Zip比較を行い、バッファの再利用・転送とI/Oの重畳、Gzip LOOKAHEADを比較します。ストリーミングCRCの二度読みを減らす方法も調査対象です。

生のJSON記録はローカルの`bench-results/`に保存しています。合成ZIPと展開ファイルは測定スクリプトの一時ディレクトリから削除済みです。
