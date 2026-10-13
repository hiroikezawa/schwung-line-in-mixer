# サードパーティのライセンス

Line In Mixer 本体は MIT ライセンスです（`LICENSE`）。DSP（ミックス、フィルター、
ダック、フィードバックガード）はすべてこのプロジェクトのために書いたコードで、
他のプロジェクトからコピーしたコードは含みません。

## Schwung のヘッダー

ビルド時に Schwung の `src/host/audio_fx_api_v2.h`、`src/host/plugin_api_v1.h`、
`src/host/shadow_constants.h` を読み込みます（構造体と定数の定義が `.so` に入ります）。
Schwung のソースは MIT ライセンスです（https://github.com/charlesvestal/schwung）。

テスト（`tests/test_ui.mjs`）は Schwung の `src/shared/` を実行時に読み込みますが、
配布物には含みません。
