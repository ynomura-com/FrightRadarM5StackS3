# M5Stack CoreS3 Flight Radar

自宅の緯度経度を中心に半径20km以内を飛行中の航空機を、レーダー画面風に表示するアプリです。

- 円形レーダー + スイープ(掃引線)アニメーション
- 範囲内の航空機をドットで表示
- ドットをタップ → コールサイン(またはICAO24)を画面下部に表示
- 何もない場所をタップ → 表示を消す
- 航空機データ取得は `OpenSky Network API`、15秒ごとに自動更新
- WiFi/通信処理は別コア(別FreeRTOSタスク)で実行するため、レーダーのアニメーションは滑らかに動き続けます

## セットアップ

### 1. VSCode + PlatformIO

- VSCode拡張機能「PlatformIO IDE」をインストール
- 本フォルダ(`flight-radar`)を VSCode で開く(PlatformIOプロジェクトとして自動認識されます)

### 2. `include/secrets.h` を編集

```cpp
#define WIFI_SSID     "..."
#define WIFI_PASSWORD "..."

#define OPENSKY_CLIENT_ID     "..."
#define OPENSKY_CLIENT_SECRET "..."

#define HOME_LAT 35.681236   // 自宅の緯度に置き換え
#define HOME_LON 139.767125  // 自宅の経度に置き換え
```

#### OpenSkyのクライアントID/シークレットの取得方法

2026年3月以降、OpenSkyはID/パスワードでのBasic認証を廃止し、OAuth2のクライアントクレデンシャル方式のみをサポートしています。

1. https://opensky-network.org/ でアカウントを作成・ログイン
2. アカウントページの「API Client」セクションで新しいクライアントを作成
3. 発行される `client_id` / `client_secret` を `secrets.h` に設定

登録済みアカウントでのAPIクライアントを使うことで、匿名アクセスよりも緩いレート制限で利用できます。

### 3. ビルド・書き込み

PlatformIOのツールバーから Build → Upload、またはVSCodeのコマンドパレットから実行してください。

```
pio run -t upload
pio device monitor
```

## ハードウェア前提

- **M5Stack CoreS3**(ESP32-S3、320x240 静電容量タッチIPS液晶)を前提にしています
- 他のM5Stack S3系機種(StickC PlusS3, AtomS3など)を使う場合は、画面サイズ・タッチ座標系・`M5.Display.setRotation()` の値を実機に合わせて調整してください

## 調整できるパラメータ (`src/main.cpp` 上部)

| 定数 | 内容 |
|---|---|
| `RADAR_CX / RADAR_CY / RADAR_R_PX` | レーダー円の中心座標・半径(px) |
| `MAX_AIRCRAFT` | 同時表示する最大機数 |
| `TAP_HIT_RADIUS_PX` | ドットのタップ判定半径 |
| `TRAIL_STEPS / TRAIL_STEP_DEG`(drawSweep内) | スイープの残光の長さ・粗さ |
| `g_sweepAngleDeg += 4.0f`(loop内) | スイープの回転速度 |

`include/secrets.h` の `RADAR_RADIUS_KM` / `FETCH_INTERVAL_MS` で表示範囲・更新間隔を変更できます。

## 実装上の注意・簡略化している点

- **TLS証明書検証を省略**しています(`WiFiClientSecure::setInsecure()`)。組み込み向けの簡易実装のためで、本番運用ではOpenSky/認証サーバーのルート証明書をピン留めすることを推奨します。
- 座標変換は自宅を原点とした平面近似(等距円筒図法に近い簡易計算)です。20km程度の範囲であれば実用上十分な精度ですが、より高精度が必要な場合はUTM座標系などへの変更を検討してください。
- OpenSkyのbounding box検索は矩形範囲を返すため、コード内で `haversineKm()` により円形20km以内かどうかを再フィルタしています。
- ネットワーク取得はCore0のFreeRTOSタスクで行い、共有データは `SemaphoreHandle_t`(mutex)で保護しています。
