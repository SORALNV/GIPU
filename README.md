# GIPU

Ubuntu＋NVIDIA GPUで、通常のZIPを展開するCLIを開発しています。実験用GPUはRTX 3090です。

目標は、Deflate解凍とCRC32計算をGPUで行い、VRAMより大きいアーカイブや単一ファイルをストリーミング展開することです。CPUはZIP構造の解析とファイル操作を担当します。CPUより高速かどうかは実測で判断します。

## 実装状況

- ZIP32／ZIP64、Stored／Deflate、Data Descriptor、UTF-8／CP437ファイル名の解析。
- 絶対パス、`..`、重複パス、シンボリックリンク、データ範囲の重複を拒否。
- 展開サイズとCRCを確認してから、同じディレクトリ内の一時ファイルを確定。既存ファイルは上書きしません。
- CPU参照経路（zlib）。GPU経路の実装を進めています。

## ビルド

C++20コンパイラ、CMake 3.24以上、zlib開発パッケージが必要です。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## 使い方

```bash
build/gipu list archive.zip
build/gipu test archive.zip --backend cpu
build/gipu extract archive.zip --backend cpu --output ./out
```

`test`はファイルを書き出さずに解凍・サイズ・CRCを検証します。`--max-output`の既定値は1TiBです。`--sync`を付けるとファイルと親ディレクトリをfsyncします。失敗時、処理中の一時ファイルは削除されます。既に検証・確定されたファイルは残ります。

## 開発方針

1. ZIP解析と安全な出力、CPU参照経路を検証する。
2. ZIPの圧縮本体にGzipヘッダとトレーラを仮想的に付け、nvCOMPのStreaming Gzipへ接続する。
3. メモリ予算内の複数エントリをnvCOMPバッチDeflateへまとめる。
4. GPU CRC32、実機での正確性・メモリ使用量・速度を検証する。
5. 7-Zip／libdeflateと比較し、測定で判明したボトルネックを改善する。

暗号化ZIP、Deflate64、BZip2、LZMA、分割ZIP、特殊ファイル、Windowsは初期版の対象外です。

設計の依存API: [nvCOMP Native API](https://docs.nvidia.com/cuda/nvcomp/native_api.html)、[C API](https://docs.nvidia.com/cuda/nvcomp/c_api.html)、[CRC32](https://docs.nvidia.com/cuda/nvcomp/crc32.html)。Native APIは実験的なため、SDKのバージョンを固定します。
