# ESP32-C3 MouseMove

ESP32-C3をBLE HIDマウスとして動作させるESP-IDFプロジェクトです。PCとペアリングすると、接続完了から3秒後に小さな四角を1回描き、その後は既定で60秒ごとに同じ動きを繰り返します。4方向の相対移動を行うため、通常は元の位置へ戻ります。

## 開発環境

- ボード: ESP32-C3（内蔵USB-Serial/JTAG）
- ESP-IDF: v5.5.5
- ターゲット: `esp32c3`
- Bluetooth: BLE HID / NimBLE（Bluetooth Classicは使用しません）

## ビルド

ESP-IDF v5.5.5の環境を有効にしたターミナルで、プロジェクトのルートへ移動して実行します。COMポートはビルドだけなら不要です。

```console
idf.py build
```

## 設定変更

次のコマンドで `MouseMove Configuration` を開きます。

```console
idf.py menuconfig
```

変更できる項目:

- Bluetooth表示名（既定: `ESP32-C3 MouseMove`）
- 自動移動の間隔（既定: 60秒）
- 1ステップの移動量（既定: 3）

## 書き込みとペアリング

実際のCOMポートを確認したうえで、次の `COMx` を置き換えて書き込みます。

```console
idf.py -p COMx flash monitor
```

Windowsでは「設定」→「Bluetoothとデバイス」→「デバイスの追加」から `ESP32-C3 MouseMove` を選択します。PIN入力は不要です。一度登録すればボンディング情報がESP32-C3のNVSに保存され、切断後も自動で再接続待ちになります。

HID定義や機器名を変更した後に認識がおかしい場合は、Windows側のBluetoothデバイス一覧から一度削除して再ペアリングしてください。
