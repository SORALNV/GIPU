# OSS公開に向けた対応範囲と検証方法

GIPUは現時点でLinux向けのStored／Deflate ZIP32・ZIP64展開CLIです。「全形式・全環境で最速」「どんなGPUでもCPUより速い」は保証していません。未対応入力を拒否できることと、その形式を解凍できることは別です。

## 形式・互換性

| 条件 | 対応・試験 |
|---|---|
| ZIP32／ZIP64、Stored／Deflate | サイズ・CRC、Data Descriptorの32／64bit・署名有無、ローカル／中央ヘッダの整合を検証 |
| zlib戦略・flush境界、圧縮level 0／1／6／9 | 参照解凍との一致、7-Zip／Info-ZIPが生成したZIPで確認 |
| UTF-8フラグ／CP437／Unicode Path 0x7075 | 版1・元ファイル名CRC一致のUnicode補助情報だけを採用。古いCRC・未知の版はCP437へ戻す |
| フラグもUnicode補助情報もない非ASCII名 | CP437として解釈。UTF-8／Shift-JIS等の自動推測はしない。元ツールと文字化けの解釈が異なる場合がある |
| 大小混在・空ファイル・空ディレクトリ・深い／多数の階層 | 固定seedのコーパスと全出力集合・サイズ・SHA256で確認 |
| Deflate64／BZip2／LZMA／AES／ZipCrypto／分割ZIP | 実際に7-Zipが生成し7-Zipで正常に検証できる入力を、出力作成前に拒否 |
| Zstandard等、その他の圧縮method ID | methodヘッダの拒否試験。実際の全コーデック実装を検証したものではない |
| 7z／gzip／tar.gz | 非ZIP入力として拒否。これらの展開・速度は対象外 |
| 属性・ACL・symlink | 元のタイムスタンプ・実行権限・ACLを復元しない。アーカイブ内のsymlink／特殊ファイルを拒否 |

`tests/integration.py`、`tests/format_matrix.py`、`tests/benchmark_matrix.py`が再現可能な試験です。外部producerの試験は利用可能な`zip`／`7zz`がない場合skipし、その環境で実際に試したとは数えません。

## 環境

2026年10月3日の[CI](https://github.com/SORALNV/GIPU/actions/runs/37039233087)では11ジョブが成功しました。

| 環境 | 実施した検証 | 保証しないもの |
|---|---|---|
| ローカルUbuntu 26.04、x86_64、Ryzen 9 7945HX | CPU最小・高速CPU・ASan／UBSan・実機GPU・外部比較 | 他のCPU機種・OSでの同じ速度 |
| GitHub Ubuntu 22.04／24.04 x86_64 | GCC、ASan／UBSan、依存あり／zlibのみ、外部ZIP producer | hosted runnerの速度をローカル速度倍率へ適用すること |
| GitHub Ubuntu 24.04 ARM64 | zlibのみ／libdeflateのCPUビルド・実行、4 worker | ARM64のGPU、ISA-L、全依存のbootstrap |
| GitHub Ubuntu 24.04 Clang | CPUビルド・実行 | 全コンパイラ・最適化指定の組み合わせ |
| RTX 3090、driver 595.91.07、CUDA runtime 13.0、nvCOMP 5.3.0 | GPUバッチ・Streaming・LOOKAHEAD、CPUとの比較 | 他のNVIDIA GPU、AMD／Intel GPU、他のdriver／CUDA／nvCOMP版 |
| GPUを不可視にしたGPU対応ビルド | 既定autoがCPUで成立すること | あらゆるGPU故障・driver障害からの復旧 |

Windows／macOSは現在の実装対象外で、CMakeで明示的に拒否します。`--path-mode portable`はLinux内の安全な成分別パス探索を選ぶ指定であり、Windows／macOS対応を意味しません。付属bootstrapはUbuntu x86_64向けです。ARM64最小構成は通常のLinuxビルド依存で構成します。

SFX／実行ファイルに付加されたZIP、全producer・全文字コードの組み合わせ、NTFS／exFAT／ネットワークfilesystem、電源断・全種類のデバイス故障は今回の検証対象外です。出力の確定はファイルごとです。GPUバッチのCRCをまとめて確認することも、バッチ／アーカイブ全体のrollbackを保証するものではありません。後半のエントリでCRC・I/Oエラーが起きると、前半の検証済み出力が残る場合があります。名前付き一時出力をSIGKILLした場合の`.part`残留も想定します。

## 速度比較

- 合成入力は固定seedから再生成し、実データZIPは変更しない。
- 同じZIP、同じ出力媒体、同じCPU affinityで実行し、同時に別の速度試験を行わない。
- 3回反復し、順序を巡回・入れ替える。中央値だけでなくmin／maxと失敗も保存する。
- CLI起動・GPU初期化・ZIP解析・CRC・ファイル生成・write完了までを含める。解凍カーネルだけの速度を実展開速度として扱わない。
- 既定比較はfsyncなし。永続化までの計測は別条件とする。
- `drop-advised`は対象ZIPへのDONTNEED助言で、完全なcold cacheを保証しない。`warm`も全データのキャッシュ常駐を保証しない。
- 全件CRC、出力集合・サイズを確認し、合成コーパスは全ファイルSHA256、Kaggleは固定seedサンプルの元データSHA256も確認する。
- SHA256照合、試験が作成した一時出力の削除は速度計測外。元データ・元ZIPは削除しない。
- CPUのworker数とGPUの出力worker数は別。GPU比較は既定の出力8 worker、pipelineは最大2バッチを重畳する。
- GNU timeのRSSは親子プロセス全体の同時ピークを保証しない。
- `--backend gpu`の指定だけでGPU解凍の実行を数えない。Stored／空ファイルではGPU解凍が不要な場合があり、`gpu_batches`／`gpu_streams`／CRC分担も結果に残す。
- デスクトップの索引処理も測定へ干渉し得る。50GBの最初の比較で検索インデクサーの高いCPU使用を観測したため、後続の対照試験は同じSSD上の隠し実験フォルダ＋`.trackerignore`で実行する。他のアプリやシステムの索引設定は変更しない。除外方法は[GNOME LocalSearchの説明](https://gnome.pages.gitlab.gnome.org/localsearch/indexed-data.html)と実機の設定を確認した。マーカーは実展開される`out`の外側に置き、ZIPの出力集合は変えない。

外部比較は公式7-Zip 26.03、Info-ZIP unzip 6.00、Python zipfile＋zlibの独立した並列CPU参照です。7-Zipの`-mmt`は要求値で、Deflateの内部16並列を保証しません。Python参照にはGIL・ZipFileのworker別解析などのコストがあり、最速の並列CPU製品を代表する保証はありません。GIPUと外部ツールでは属性復元や一時ファイルの確定方法も異なります。

追加の`reference_7zip_parallel.py`は、最大16個の公式7-Zipプロセスへファイルを分担し、各プロセスは`-mmt=1`で動きます。ZIP本体の解析を各workerが繰り返すコスト、出力の親ディレクトリ・選択リストの作成も測定内です。全workerのCRC検証件数・展開量を確認し、実展開を独立SHA256で照合します。単一Deflate内部の並列化ではありません。旧版p7zipで日本語名を保つためUTF-8ロケールを指定します。

CPU affinityを1／2／4等へ制限した試験は、同一Ryzen上で使えるCPU数を減らした試験です。低価格CPU・別マイクロアーキテクチャ・別RAM容量を検証したとは扱いません。Optane／SSDのext4とtmpfsの結果も、NTFS・exFAT・ネットワークfilesystemの証明ではありません。

## 公開時の訴求

採用できる表現は「入力・環境・比較対象・測定範囲を特定した実測倍率」です。「最大値」だけを抜き出して、全入力での倍率やGPUそのものの優劣へ一般化しません。既定CPU経路と明示GPU経路を比較する場合は、同じGIPU内の経路比較であることを明記します。

性能の限界・未対応・失敗も公開することを原則とします。GPUはnvCOMPの入力安全性の制約から、既知の正しい圧縮データ向けの実験経路です。CPUでのfuzz試験は安全なGPUデコードの証明になりません。未確認の破損入力をGPUに送る前提の宣伝はしません。

本体ライセンスの選択と依存の条件は[THIRD_PARTY.md](../THIRD_PARTY.md)を参照してください。単に公開GitHubに置くだけでは、利用・再配布の許諾を示すLICENSEの代わりになりません。
