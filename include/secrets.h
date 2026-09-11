#pragma once

// =========================================================
// WiFi 設定
// =========================================================
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// =========================================================
// OpenSky Network API 認証情報 (OAuth2 Client Credentials)
//
// 2026年3月以降、OpenSkyはID/パスワードのBasic認証を廃止し、
// OAuth2のクライアントクレデンシャル方式のみになっています。
//
// 取得手順:
//   1. https://opensky-network.org/ でアカウント作成・ログイン
//   2. アカウントページの "API Client" から新しいクライアントを作成
//   3. 発行される client_id / client_secret を下に設定
// =========================================================
#define OPENSKY_CLIENT_ID     "YOUR_OPENSKY_CLIENT_ID"
#define OPENSKY_CLIENT_SECRET "YOUR_OPENSKY_CLIENT_SECRET"

// =========================================================
// レーダー中心座標 (自宅の緯度経度)
// 下記はサンプル値(東京駅)です。必ずご自宅の座標に置き換えてください。
// =========================================================
#define HOME_LAT 35.681236
#define HOME_LON 139.767125

// レーダー表示半径 [km]
#define RADAR_RADIUS_KM 20.0

// 航空機データの取得間隔 [ms] (OpenSkyのレート制限を考慮し短くしすぎない)
#define FETCH_INTERVAL_MS 15000UL
