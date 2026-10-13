# Line In Mixer

Ableton Move 上の [Schwung](https://github.com/charlesvestal/schwung) 用 Audio FX です。
Move の音声入力（3.5 mm の LINE IN、または USB-C 経由で iPad や PC から送られる音）を、
Schwung スロットのサウンドジェネレーターの音に**足し合わせて**鳴らします。

- スロットのサウンドジェネレーターはそのまま鳴り続けます。外部入力のために
  サウンドジェネレーターを置き換えたり、スロットを1つ使ったりする必要はありません。
- つまみを動かして変わるのは外部入力の音だけです。サウンドジェネレーターの音量や音質は
  変わりません（外部入力を 0 にすると、出力は元の音とビット単位で同じです）。
- 後ろに入れた Audio FX（リバーブやディレイなど）と Master FX は、サウンドジェネレーターと
  外部入力の両方にかかります。

```text
Schwung のスロット（例：Track 4）
  サウンドジェネレーター ─▶ FX 1: Line In Mixer ─▶ FX 2: リバーブ / ディレイ … ─▶ Master FX ─▶ 出力
                                ▲
                                └─ Move の音声入力（LINE IN または USB-C のどちらか一方）
```

## インストール

### リリースから入れる（おすすめ）

1. [Releases](../../releases) から最新の `line-in-mixer-module.tar.gz` をダウンロードします。
2. Schwung Manager（`http://<MoveのIP>:7700`）の Modules →「Install custom」で、
   そのファイルをアップロードします。
3. **Move の電源を一度入れ直します。**

### ソースからビルドして入れる

aarch64 用のクロスコンパイラ（`gcc-aarch64-linux-gnu`）、Python 3、そして Move で動いている
バージョンの Schwung のソースが必要です（ヘッダーだけを使います）。

```bash
git -C ../schwung archive v1.7.3 src | tar -x -C /path/to/schwung-1.7.3
export SCHWUNG_ROOT=/path/to/schwung-1.7.3

scripts/build.sh arm64                             # dist/line-in-mixer-module.tar.gz を作成
python3 scripts/install.py http://<MoveのIP>:7700  # Schwung Manager 経由でアップロード
```

> **更新したら必ず Move の電源を入れ直してください。** 一度読み込まれた DSP は、
> スロットを読み直しても入れ替わりません。

## 使い方

1. Schwung のスロットにサウンドジェネレーターを入れます。
2. 同じスロットの **FX 1** に Line In Mixer を入れます。リバーブなどは FX 2 以降に入れます。
3. **Move 本体の設定**で、サンプリングの入力ソースを LINE IN か USB-C に切り替えます。
4. Line In Mixer の **SOURCE** を、Move 側と同じもの（ANALOG または USB-C）にします。
5. MIX と LEVEL / GAIN で音量を決めます。

USB-C から入れる場合は、iPad・iPhone・PC など、Move を USB オーディオ機器として認識する
機器から音を送ります。Move の USB-C に一般的なオーディオインターフェースをつなぐ使い方には
対応していません。

## つまみ

### 1 ページ目：MIX（画面の見出しは「Main」）

| ノブ | 表示 | 内容 | 範囲（初期値） |
|---|---|---|---|
| 1 | MIX | 外部入力をどれだけ足すか | 0–100 %（100 %） |
| 2 | AN LVL | ANALOG の音量 | 0–100 %（100 %） |
| 3 | USB LVL | USB-C の音量 | 0–100 %（100 %） |
| 4 | SOURCE | どちらの設定を使うか：ANALOG / USB-C | ANALOG |
| 5 | AN GN | ANALOG のゲイン | −24〜+24 dB（0 dB） |
| 6 | USB GN | USB-C のゲイン | −24〜+24 dB（0 dB） |
| 7 | MUTE | 外部入力だけを消す | off / on（off） |

### 2 ページ目：TONE

| ノブ | 表示 | 内容 | 範囲（初期値） |
|---|---|---|---|
| 1 | AN HPF | ANALOG のローカット（12 dB/oct）。20 Hz で効果なし | 20–1000 Hz（20 Hz） |
| 2 | USB HPF | USB-C のローカット。20 Hz で効果なし | 20–1000 Hz（20 Hz） |
| 3 | AN LPF | ANALOG のハイカット（12 dB/oct）。20 kHz で効果なし | 1–20 kHz（20 kHz） |
| 4 | USB LPF | USB-C のハイカット。20 kHz で効果なし | 1–20 kHz（20 kHz） |
| 5 | AN PAN | ANALOG の左右バランス | −100〜+100（0） |
| 6 | USB PAN | USB-C の左右バランス | −100〜+100（0） |

つまみの変化は 20 ms かけてなめらかに反映されるので、回してもクリックノイズは出ません。
SOURCE を切り替えたときは 15 ms でクロスフェードします。
すべての設定は Schwung のスロット自動保存と User Presets で保存・復元されます。

## LINE IN と USB-C を同時には使えません

Move の音声入力は、Move の中の XMOS オーディオチップから **1 本のステレオ信号**として
Schwung に届きます。LINE IN（ケーブルがないときは内蔵マイク）と USB-C のどちらを流すかは
XMOS が切り替えていて、その設定は Move 本体のサンプリング入力ソースが持っています。

そのため、

- LINE IN と USB-C を**同時に鳴らすことはできません**。
- Line In Mixer は、今どちらの入力が来ているかを知ることも、切り替えることもできません。
  **入力は Move 本体で切り替えます。** SOURCE は「ANALOG 用と USB-C 用、どちらの
  つまみ（ゲイン・音量・フィルター・パン）を使うか」を選ぶだけです。

実機でも確かめています。Move の Linux 側には音声デバイス（ALSA）が一切なく、USB の機能は
USB イーサネットだけで、USB-C のオーディオと MIDI はすべて XMOS が受け持っています。
Schwung 内蔵の Line In や、ほかの入力系モジュールも、同じ 1 本の信号を読んでいます。

USB-C の信号は、Schwung に届いた時点で Move の処理と同じ 44.1 kHz・同じクロックに
そろっています。そのためこのモジュールにはサンプルレート変換やバッファがなく、
音切れやずれが起きる箇所がありません。USB-C を抜くと外部入力が無音になるだけで、
サウンドジェネレーターは鳴り続けます。

## ハウリング防止

SOURCE が ANALOG で、Move の内蔵スピーカーから音が出ていて、LINE IN にケーブルが
刺さっていないとき、入力は内蔵マイクがスピーカーの音を拾っている状態になり、
ハウリングします。Line In Mixer はこのときだけ外部入力を自動で消します（画面表示はありません）。
ケーブルを刺すか、ヘッドホンをつなぐと元に戻ります。USB-C はマイクではないので対象外です。

## 注意

- Schwung の Global Settings でネイティブ Resample ブリッジを使っていると、入力に
  Schwung 自身の音が入り、それを足し合わせることになります。このモジュールを使うときは
  オフにしてください。
- スロットが無音のあいだも外部入力が途切れないよう、処理を止めない設定
  （`requires_continuous_processing`）にしています。

## 仕組み

```c
ext   = (1 - w) * strip(ANALOG) + w * strip(USB)   // w = SOURCE（15 ms クロスフェード）
strip = pan( level * gain * LPF( HPF(input) ) )
outL  = internalL + extL * MIX * (MUTE ? 0 : 1) * (ハウリング防止 ? 0 : 1)
outR  = internalR + extR * MIX * (MUTE ? 0 : 1) * (ハウリング防止 ? 0 : 1)
```

フィルターは 2 次の Butterworth（TPT ステート・バリアブル・フィルター）です。
合計は 16 ビットの範囲でクリップします（リミッターはありません）。

## テスト

```bash
scripts/test.sh
```

実際の Schwung Audio FX v2 インターフェース経由の機能テスト（ダミーの入力を使用）、
AddressSanitizer / UBSan、ハウリング防止、Schwung 本体のページ構成チェック、
ARM64 ビルドをまとめて実行します。

## ライセンス

MIT（`LICENSE`）。Copyright (c) 2026 Hiro Ikezawa。
サードパーティについては `THIRD_PARTY_LICENSES.md` を見てください。
