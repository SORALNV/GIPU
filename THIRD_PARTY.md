# 依存ライブラリと公開範囲

このリポジトリにはGIPUのソースと試験・測定スクリプトを保存し、ダウンロードした依存、実行バイナリ、Kaggle画像、試験ZIP、LLM重みは含めません。GIPU本体のライセンス選択と、各依存のライセンスは別です。

| 依存 | 利用経路 | 上流のライセンス・確認先 |
|---|---|---|
| zlib | 最小CPU、Deflate参照 | [zlibライセンス](https://www.zlib.net/zlib_license.html) |
| libdeflate | 任意の高速CPU解凍・CRC | [MIT](https://github.com/ebiggers/libdeflate/blob/master/COPYING) |
| ISA-L | 任意のCPU Streaming | [BSD-3-Clause](https://github.com/intel/isa-l/blob/master/LICENSE) |
| Rapidgzip／librapidarchive | 任意の単一ストリームCPU並列 | [MITまたはApache-2.0](https://github.com/mxmlnkn/rapidgzip)、修正版ISA-Lなど付属依存は別途確認 |
| NVIDIA nvCOMP／CUDA runtime | 任意のNVIDIA GPU | [nvCOMPのSDKライセンス](https://docs.nvidia.com/cuda/nvcomp/license.html)。取得したSDKのLICENSE／NOTICEとCUDA側の条件も確認 |
| 7-Zip、Info-ZIP | 外部比較・互換性fixture作成だけ | [7-Zip](https://www.7-zip.org/license.txt)、[Info-ZIP](https://infozip.sourceforge.net/license.html)の条件に従う |

GPU依存は「GIPUのソースが公開されている」ことによって同じOSSライセンスになるわけではありません。CPU最小構成はnvCOMP／CUDAを必要としません。SDKやそれを含むバイナリを別途再配布する場合、SDKの配布条件と各依存の通知要件を個別に確認してください。この文書は法的適合性を保証するものではありません。

2026年10月3日に上流のライセンス表示と、ローカルで取得したnvCOMPのLICENSE／NOTICE、RapidgzipのMIT／Apache-2.0ライセンスを確認しました。下流の全依存を含むバイナリの再配布監査が完了したという意味ではありません。

比較用の`reference_zip.py`はPython標準ライブラリのzlib／zipfileを使う独立したCPU参照実装です。GIPUやlibdeflateのラッパーではなく、汎用の安全な解凍製品としての利用を意図しません。

合成コーパスは固定seedから再生成できます。Kaggle実データは利用者が取得したローカルデータを読み取り、公開結果には元ファイル名・内容を含めません。測定報告にデータ再配布の権利を含めません。
