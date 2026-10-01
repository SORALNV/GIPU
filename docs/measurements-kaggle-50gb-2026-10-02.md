# Kaggle実データ50GB ZIPの実測記録

2026年10月1日から2日（日本時間）に測定しました。合成ゼロデータではなく、2TB SSDに置かれたRSNA Knee Abnormality DetectionのDICOMを元の内容のまま使っています。元データは変更していません。

## 入力と環境

| 項目 | 値 |
|---|---|
| 圧縮ZIP容量 | 50,001,081,292 bytes（50.001GB、約46.57GiB） |
| 展開量 | 109,556,630,214 bytes（109.557GB、約102.03GiB） |
| ファイル数 | 155,925 |
| 形式 | 通常のZIP64、Raw Deflate level 6、ファイル単位の独立ストリーム |
| ZIP配置先 | Optane上の`/srv/workspace/sora/gipu-bench/kaggle-50GB.zip` |
| 実展開先 | 2TB SSD上の`/home/sora/gipu-bench-output/`内の試験用一時ディレクトリ |
| CPU | AMD Ryzen 9 7945HX、16コア／32スレッド |
| GPU | RTX 3090、24GiB、NVIDIAドライバ595.91.07 |
| OS | Ubuntu 26.04.1、Linux 7.0.0-34-generic |
| 実装依存 | CUDAランタイム13.0、nvCOMP 5.3.0.16、zlib 1.3.2、libdeflate 1.26 |
| 生成時間 | 354.74秒、圧縮worker 12 |

50GBは展開量ではなく**圧縮ZIPそのものの容量**です。圧縮入力はGPUの24GiB VRAMより大きく、既定経路の明示的GPU作業領域は4GiB以下です。50GB全体や109.6GB全体を一括でGPUへ置かず、独立エントリをバッチへ分けます。

ZIPのSHA256: `e9b3ecf6d22489d5373729660d2ce0ac19e5a9c2c8bb9d294e37626c47b3e898`

## 測定方法と限界

- `test`はZIP解析、CUDA初期化、入力読み込み、解凍、全件CRC確認を含みます。展開ファイルは書きません。
- `extract`は上記に全ファイルの作成、書き込み、検証済みファイルの確定を含みます。**通常のwrite完了までの測定で、fsyncによる全件永続化は含みません。** 永続化を含める場合は測定スクリプトにも`--sync`を指定してください。
- 外部からCLI起動〜終了を測ったwall時間を比較します。CRC／サイズの検査は時間内です。測定後の全出力サイズ照合と元データ128サンプルのSHA256照合は時間外です。
- 各回、対象ZIPだけに`POSIX_FADV_DONTNEED`を助言します。再読み込みやメタデータ解析によるreadaheadがあり、完全なcold cacheは保証しません。繰り返し時は測定順序を交互にします。
- 生成ジョブと大容量測定は同時に走らせていません。通常のデスクトップ稼働中の実測で、専有・完全無負荷の実験機ではありません。
- zlib単一スレッドとlibdeflate単一スレッドの初回値は1回の基準値です。最終GPU／libdeflate 16スレッドは3回測定の中央値を使います。7-ZipやすべてのZIPへの倍率ではありません。
- pipelineのCPU側には先読み・GPU制御に加え、既定最大16個（2バッチ×8個）のファイル出力workerがあります。GPU単体だけの性能差ではありません。

## 解凍＋CRC（書き込みなし）

| 経路 | wall秒 | 回数・集計 |
|---|---:|---|
| zlib、1スレッド | 277.066 | 初回1回 |
| libdeflate、1スレッド | 118.096 | 初回1回 |
| libdeflate、16スレッド | 13.903 | 最終3回中央値 |
| GPU、256件バッチ | 66.827 | 初回1回 |
| GPU、4096件バッチ、重畳なし | 31.216 | 3回中央値 |
| GPU、再確保型pipeline | 22.359 | 改善途中の3回中央値 |
| GPU、事前確保型pipeline、4GiB | 19.007 | 最終3回中央値 |
| GPU、事前確保型pipeline、8GiB | 19.549 | 最終3回中央値 |
| GPU、事前確保型pipeline、16GiB | 22.341 | 最終3回中央値 |

最終4GiB経路はzlibの初回値に対して**約14.58倍**、libdeflate単一スレッドに対して**約6.21倍**でした。一方、16スレッドlibdeflateはGPUより速く、GPUは約1.37倍時間がかかっています。CPUの比較対象を単一スレッドだけに限定して、最速CPU比の高速化と解釈してはいけません。

4GiB経路の3回wall値は19.007／19.049／18.992秒です。8GiB・16GiBではGPU decode区間は短くなりますが、事前確保時間と最初／最後のバッチの待ちが増え、CLI全体では既定4GiBが最速でした。

## 実展開の改善途中の比較

| 経路 | wall秒 | 備考 |
|---|---:|---|
| zlib、1スレッド | 311.931 | 基準、1回 |
| libdeflate、16スレッド | 49.663 | 改善途中、1回 |
| GPU、重畳なし | 78.363 | 1回、出力並列化前 |
| GPU、事前確保pipeline | 62.013 | 1回、1バッチ1出力worker |

この時点でもzlib基準比は約5.03倍でした。出力workerが最大2個のままだったため、ファイル操作と書き込みの待ちが目立ちました。既定8 worker／バッチに増やした最終版の繰り返し値は次の節に記録します。

## 最終実展開

| 経路 | wall秒（3回） | 中央値 |
|---|---|---:|
| GPU、事前確保pipeline、4GiB、8出力worker／バッチ | 47.017／67.423／54.847 | **54.847秒** |
| libdeflate、16スレッド | 60.178／41.936／61.751 | **60.178秒** |

zlib初回基準311.931秒に対し、GPU中央値は**約5.69倍**高速でした。選んだ実データ・通常write完了までの条件では、目標5倍を中央値で超えています。ただしGPUの各回は47.0〜67.4秒で、最も遅い回はzlib基準比約4.63倍です。すべての回で5倍を保証した結果ではありません。

16スレッドlibdeflateとの中央値比較は**約1.10倍**です。CPUの各回は41.9〜61.8秒でGPUと範囲が重なり、安定した優位性や最速CPU比5倍を示すものではありません。書き込みなしの`test`では16スレッドCPUの方が速い結果でした。

GPU decode区間は実展開3回でも11.706／11.469／11.695秒と比較的安定しています。一方、出力workerの加算時間は48.097／93.908／69.357秒でした（最大2バッチの重畳があるためwall時間とは異なります）。この測定で大きく変動しているのは主に出力側であり、GPU解凍カーネルの変更だけで全体の変動や最速CPU比5倍を解消できるとは判断しません。

GPUの明示確保量は4,294,916,632 bytesで4GiB上限内、41バッチ・Streaming 0でした。これはプロセス全体のVRAMピークではなく、CUDAコンテキストやnvCOMP内部の領域を含みません。

全工程・各回の値は[集計JSON](benchmarks/kaggle50-2026-10-02.json)に保存しています。改善途中の値と最終版を区別し、元データやファイル名は含めていません。最終実装のGitHub commitは`5211398ecb7b656ff5d2945b2a46277eaa836f29`です。

## 正確性・保存物

全エントリで宣言展開サイズとCRCが一致しました。実展開時は全155,925ファイルの出力サイズと、固定seedで選んだ128ファイルの元データとのSHA256も一致しました。

CPU zlib／CPU libdeflate／GPU auto／GPU stream／GPU batchの5統合テストスイートが成功しています。各スイート24テストを定義し、該当しない経路はskipします。pipelineの多バッチ出力、サイズ予算、未対応入力の拒否、CRC不一致時に出力workerへ渡さないことを追加確認しました。CPU側はASan／UBSanと4スレッドlibdeflateでも検証し、GitHub ActionsのCPU／ASan CIも成功しました。

ZIP・元ファイル名を含むローカルマニフェスト・生成サマリはOptaneに残します。試験で作った109.6GBの展開ディレクトリだけ各回終了後に削除しています。元のKaggleデータとZIPは削除していません。医用画像・元データのファイル名・ローカルマニフェストはGitHubへ公開せず、コードと集計値だけ公開します。

## 再現コマンド

```bash
python3 scripts/prepare_kaggle_zip.py --source /path/to/train_series \
  --output /optane/workspace/kaggle-50GB.zip --target-gb 50 --workers 12
python3 scripts/benchmark_archive.py --archive /optane/workspace/kaggle-50GB.zip \
  --cases gpupipeline gpupipeline8g gpupipeline16g libdeflate16 --repeats 3 \
  --results bench-results/kaggle50-test-final.json
python3 scripts/benchmark_archive.py --archive /optane/workspace/kaggle-50GB.zip \
  --cases gpupipeline libdeflate16 --repeats 3 \
  --extract-root /ssd/gipu-bench-output --source /path/to/train_series \
  --results bench-results/kaggle50-extract-final.json
```

同じ結果ファイルや生成先は上書きしません。再測定では新しい結果名を使ってください。
