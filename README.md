# Line In Mixer（Schwung Audio FX）

Ableton Move の音声入力（3.5 mm LINE IN、または USB-C 経由のデジタルオーディオ）を、
Schwung スロットの**内部音源の上に加算ミックス**する Audio FX です。

- スロットの Sound Generator はそのまま残ります。外部入力のために Sound Generator を
  置き換えたり占有したりしません。
- MIX・SOURCE・MUTE・DUCK を動かしても、内部音源の音量・音質は変わりません。
  外部入力の分が 0 のときは、出力は入力（内部音源）とビット単位で同一です。
- 後段の Audio FX（Reverb / Delay など）と Master FX は、内部音源と外部入力の
  両方にかかります。

```text
Schwung Track 4
  Sound Generator ─▶ FX 1: Line In Mixer ─▶ FX 2: Reverb / Delay … ─▶ Master FX ─▶ OUT
                          ▲
                          └─ Move の音声入力（ANALOG または USB-C、どちらか一方）
```

## 入力ソースについて（重要な制約）

調査の結果、**物理 LINE IN と USB-C Audio を同時に取り込むこと（BOTH）はできません**。
そのため BOTH モードは実装せず、SOURCE は ANALOG / USB-C の 2 択にしています。

理由（Schwung 1.7.3 のソースと Move の SPI プロトコルから確認）:

1. Move の音声入力は、XMOS オーディオチップから SPI 経由で Linux 側に届く
   **1 本のステレオストリーム**だけです（128 フレーム × 2ch の int16、44.1 kHz。
   SPI メールボックスの `audio_in_offset`）。
2. その 1 本に何を流すかは XMOS が決めます。LINE IN（ケーブル未接続時は内蔵マイク）か、
   USB-C から届くオーディオかを、XMOS のルーティング設定（SysEx `37 12` の bit0）で
   切り替えます。この設定は **Move 本体のサンプリング入力設定**が持っています
   （Schwung の `docs/SPI_PROTOCOL.md`）。
3. USB-C のオーディオは XMOS が扱っています（USB-C 出力の Mic / Main Out 切替も
   XMOS の SysEx）。Linux の音声 API（ALSA）から USB-C 入力を別系統として取り出せる
   デバイスがあるかは、**まだ実機で確認していません**（`/proc/asound/cards` の確認待ち）。
   仮にあっても、Move 本体（MoveOriginal）が使用中のデバイスを Audio FX から
   共有できるかは別問題で、現時点では SPI ストリームだけを使っています。
4. Schwung 内蔵の `linein` Sound Generator も、`timncox/schwung-smack` の `smack-in`
   も、同じ 1 本のストリームを読んでいます。smack-in のソースには
   「入力は Move で最後に選んだもの（mic/line/USB-C）に従う」と書かれています。

このため、このモジュールは**どちらの入力が来ているかを自分で知ることも、
切り替えることもできません**。

- **入力の切替は Move 本体で行います**（サンプリングの入力ソース設定で
  LINE IN か USB-C を選ぶ）。
- このモジュールの **SOURCE は「どちらのチャンネルストリップ
  （GAIN・LEVEL・HPF・LPF・PAN）を使うか」**を選びます。Move 側の設定に
  合わせてください。SOURCE を切り替えると 15 ms でクロスフェードします。
- XMOS の SysEx を Schwung から送って自動で切り替える方法は、意図的に使っていません。
  Schwung 本体が同じ系統の SysEx の再送を 1.3.2 で廃止しています（USB-C 出力の
  設定と実際の音がずれ、内蔵スピーカーがミュートされる不具合が実機で報告されたため）。

### USB-C まわり

- USB-C 入力は、Move を USB オーディオデバイスとして認識するホスト
  （iPad Pro、iPhone、PC など）から送られる音声を想定しています。Move の USB-C に
  一般的なオーディオインターフェースをつなぐ使い方は対象外です。
- サンプルレート変換・クロックドリフト・バッファ管理は XMOS と SPI 転送が行います。
  このモジュールに届く時点で、DSP と同じ 44.1 kHz・同じクロックの 128 フレーム
  ブロックになっているため、モジュール側にリサンプラーやリングバッファはありません
  （アンダーラン・オーバーランが起きる場所がありません）。
- USB-C を抜くと入力はゼロになるだけで、内部音源は鳴り続けます。モジュールは
  USB の接続状態を待ったり読んだりしないので、ホットプラグでクラッシュする経路も
  ありません。再接続時は、XMOS が音を流し始めればそのまま聞こえます。
- USB-C MIDI と USB-C Audio の併用、USB-C 給電との併用はモジュールでは扱いません
  （XMOS と Move 本体の機能です）。**実機では未確認**です。

## フィードバックガード

SOURCE が ANALOG で、内蔵スピーカーから音が出ていて、LINE IN にケーブルが
刺さっていないとき、入力は内蔵マイクがスピーカーの音を拾っている状態です。
Schwung 本体のフィードバック保護は Sound Generator にしか効かないため、
このモジュールはこの状態で外部入力を自動でミュートし、INFO ページの STATE に
`FB GUARD` と表示します。ケーブルを刺すかヘッドホンを使うと解除されます。
USB-C ストリップ（SOURCE = USB-C）はマイクではないので対象外です。

ジャックの状態は Schwung の共有メモリ `/schwung-control`
（`shadow_control_t.speaker_active` / `line_in_connected`）を読み取り専用で参照しています。

## パラメーター

### Page 1：MIX

| ノブ | 名前 | 内容 | 範囲（初期値） |
|---|---|---|---|
| 1 | MIX | 外部入力全体の加算量。内部音源は変わらない | 0–100 %（100 %） |
| 2 | AN LVL | ANALOG ストリップのレベル | 0–100 %（100 %） |
| 3 | USB LVL | USB ストリップのレベル | 0–100 %（100 %） |
| 4 | SOURCE | 使うストリップ：ANALOG / USB-C | ANALOG |
| 5 | AN GN | ANALOG ストリップのゲイン | −24〜+24 dB（0 dB） |
| 6 | USB GN | USB ストリップのゲイン | −24〜+24 dB（0 dB） |
| 7 | DUCK | 内部音源が鳴っている間、外部入力を下げる量 | 0–100 %（0 %） |
| 8 | MUTE | 外部入力をミュート（内部音源はそのまま） | off / on（off） |

### Page 2：TONE

| ノブ | 名前 | 内容 | 範囲（初期値） |
|---|---|---|---|
| 1 | MIX | Page 1 と同じパラメーター | |
| 2 | AN HPF | ANALOG ハイパス（12 dB/oct）。20 Hz = オフ | 20–1000 Hz（20） |
| 3 | USB HPF | USB ハイパス。20 Hz = オフ | 20–1000 Hz（20） |
| 4 | AN LPF | ANALOG ローパス（12 dB/oct）。20 kHz = オフ | 1–20 kHz（20 k） |
| 5 | USB LPF | USB ローパス。20 kHz = オフ | 1–20 kHz（20 k） |
| 6 | AN PAN | ANALOG のバランス（ステレオ入力の左右バランス） | −100〜+100（0） |
| 7 | USB PAN | USB のバランス | −100〜+100（0） |
| 8 | WIDTH | 外部入力の M/S 幅。0 % モノ、100 % そのまま、200 % 広げる | 0–200 %（100 %） |

### Page 3：INFO（読み取り専用）

| 名前 | 内容 |
|---|---|
| STATE | OK / MUTED / FB GUARD / NO SIG（入力が −70 dBFS 未満で 2 秒以上） |
| INPUT | 入力のピークレベル（ゲイン前、dBFS） |

Schwung のページ構成の都合で、Page 1 の見出しは「Main」と表示されます。

### 計算式

```c
ext  = width( (1 - w) * strip(ANALOG) + w * strip(USB) )   // w = SOURCE（クロスフェード）
strip = pan( level * gain * LPF( HPF(input) ) )
outL = internalL + extL * MIX * duck * (MUTE ? 0 : 1) * (FB GUARD ? 0 : 1)
outR = internalR + extR * MIX * duck * (MUTE ? 0 : 1) * (FB GUARD ? 0 : 1)
duck = 1 - DUCK * min(1, env(internal) / 0.25)   // アタック 1 ms、リリース 200 ms
```

DUCK 100 % では、内部音源のピークが −12 dBFS 以上のあいだ外部入力が消えます。
合計は int16 の範囲でクリップします（リミッターはありません）。
ゲイン・レベル・パン・幅・MIX の変化は 20 ms でなめらかに変わります（クリックなし）。

### MIDI CC

スロットの受信チャンネルの CC（絶対値、0–127 がパラメーターの全範囲）:

| CC | パラメーター |
|---|---|
| 20–27 | Page 1 のノブ 1–8（MIX, AN LVL, USB LVL, SOURCE, AN GN, USB GN, DUCK, MUTE） |
| 28–34 | Page 2 のノブ 2–8（AN HPF, USB HPF, AN LPF, USB LPF, AN PAN, USB PAN, WIDTH） |

SOURCE は 0–63 で ANALOG、64–127 で USB-C。MUTE は 0–63 で off、64–127 で on。
ほかのトラックから届く MIDI（FX ブロードキャスト）は無視します。

### プリセット

全 15 パラメーターが `state` として保存・復元されます（Schwung のスロット自動保存、
User Presets）。

## 使い方

1. Schwung のスロット（例：Track 4）に Sound Generator を入れる。
2. そのスロットの FX 1 に **Line In Mixer** を入れる。FX 2 以降に Reverb などを入れる。
3. Move 本体のサンプリング入力ソースで、LINE IN か USB-C を選ぶ。
4. Line In Mixer の SOURCE を Move の設定に合わせる。
5. MIX と各 LEVEL / GAIN で量を調整する。

注意:
- Schwung の Global Settings でネイティブ Resample ブリッジを使っていると、
  入力の領域に Schwung のミックスが書き込まれ、それを加算することになります
  （フィードバックの恐れ）。このモジュールを使うときはオフにしてください。
- スロットが無音でも入力が途切れないように `requires_continuous_processing`
  を宣言しています。

## ビルドとテスト

```bash
# Move と同じ Schwung バージョンのソース（ヘッダーとページプランナー）を用意
git -C ../schwung archive v1.7.3 src | tar -x -C /path/to/schwung-1.7.3
export SCHWUNG_ROOT=/path/to/schwung-1.7.3

scripts/test.sh          # 機能テスト、ASan/UBSan、フィードバックガード、UI 契約、ARM64 ビルド
scripts/build.sh arm64   # dist/line-in-mixer-module.tar.gz
scripts/install.py http://<Move の IP>:7700
```

`tests/test_fx.c` は実際の Audio FX v2 ABI（dlopen）を通して、ダミーの SPI メールボックスに
入力を書き込んでテストします。`tests/test_ui.mjs` は Schwung 本体のページプランナーと
契約バリデーターでページ構成を確認します。

## 検証状況

| 項目 | 実装 | ビルド / PC テスト | 実機 |
|---|---|---|---|
| 物理 LINE IN（ANALOG） | 済 | 済 | **未検証** |
| USB-C Audio | 済（Move 側で入力を USB-C にしたとき） | 済（ダミー入力） | **未検証** |
| BOTH（同時） | **実装しない**（1 本のストリームしかないため） | — | — |

## ライセンス

MIT（`LICENSE`）。Copyright (c) 2026 Hiro Ikezawa。サードパーティについては
`THIRD_PARTY_LICENSES.md`。
