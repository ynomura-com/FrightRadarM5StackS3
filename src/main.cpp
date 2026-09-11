// =========================================================
// M5Stack CoreS3 - Flight Radar
//
// 自宅の緯度経度を中心に半径 RADAR_RADIUS_KM 以内を飛行する
// 航空機を OpenSky Network API から取得し、レーダー画面風に表示する。
// ・円形レーダー + スイープ(掃引線)アニメーション
// ・機影(ドット)をタップすると便名と目的地(空港コード)を表示
// ・何もない場所をタップすると表示を消す
// =========================================================

#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include "secrets.h"

// ---------------------------------------------------------
// 画面・レーダー形状の定義 (CoreS3: 320x240)
// ---------------------------------------------------------
static constexpr int16_t SCREEN_W = 320;
static constexpr int16_t SCREEN_H = 240;
static constexpr int16_t RADAR_CX = 160;   // レーダー中心 X
static constexpr int16_t RADAR_CY = 128;   // レーダー中心 Y (上に少しステータス行を確保)
static constexpr int16_t RADAR_R_PX = 100; // レーダー半径 [px]
static constexpr float   PX_PER_KM = RADAR_R_PX / (float)RADAR_RADIUS_KM;

static constexpr int MAX_AIRCRAFT = 40;
static constexpr float TAP_HIT_RADIUS_PX = 35.0f; // ドットのタップ許容半径(指でのタップを考慮しやや広め)

// ---------------------------------------------------------
// 機体情報
// ---------------------------------------------------------
struct Aircraft {
    char  icao24[8];
    char  callsign[9];
    float lat, lon;
    int16_t x, y; // レーダー上の座標(px)
};

static Aircraft g_aircraft[MAX_AIRCRAFT];
static int      g_aircraftCount = 0;
static int      g_selected = -1; // タップで選択中の機体index (-1: 未選択)

static SemaphoreHandle_t g_dataMutex;

static M5Canvas g_canvas(&M5.Display);

// OAuth2 トークン
static String        g_accessToken;
static unsigned long g_tokenExpiryMillis = 0;

// 通信状態表示用
static volatile bool g_wifiOk = false;
static volatile bool g_lastFetchOk = false;

// 経路(出発地-目的地空港コード)ルックアップ用の状態
// hexdb.io の無料API (認証不要) にコールサインを問い合わせて取得する。
static char              g_routeQueryCallsign[16] = {0};
static volatile uint32_t g_routeRequestId = 0;   // タップの度にインクリメント
static volatile uint32_t g_routeProcessedId = 0; // networkTask側で処理済みのID
static volatile bool     g_routeLookupOk = false;
static char              g_routeOrigin[8] = {0};
static char              g_routeDest[8] = {0};

// ---------------------------------------------------------
// 座標変換ユーティリティ
// ---------------------------------------------------------
static inline double deg2rad(double d) { return d * M_PI / 180.0; }

// 2点間の距離[km] (Haversine)
static double haversineKm(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371.0;
    double dLat = deg2rad(lat2 - lat1);
    double dLon = deg2rad(lon2 - lon1);
    double a = sin(dLat / 2) * sin(dLat / 2) +
               cos(deg2rad(lat1)) * cos(deg2rad(lat2)) *
               sin(dLon / 2) * sin(dLon / 2);
    double c = 2 * atan2(sqrt(a), sqrt(1 - a));
    return R * c;
}

// 緯度経度 -> レーダー上のスクリーン座標(自宅を中心とした簡易平面近似。20km程度の範囲なら十分な精度)
static void latLonToXY(double lat, double lon, int16_t &x, int16_t &y) {
    double kmPerDegLat = 111.32;
    double kmPerDegLon = 111.32 * cos(deg2rad(HOME_LAT));
    double dxKm = (lon - HOME_LON) * kmPerDegLon;
    double dyKm = (lat - HOME_LAT) * kmPerDegLat;
    x = RADAR_CX + (int16_t)lround(dxKm * PX_PER_KM);
    y = RADAR_CY - (int16_t)lround(dyKm * PX_PER_KM); // 北(緯度+)が上になるようY反転
}

// application/x-www-form-urlencoded 用の簡易URLエンコード
static String urlEncode(const String &s) {
    String out;
    out.reserve(s.length() * 3);
    const char *hex = "0123456789ABCDEF";
    for (size_t i = 0; i < s.length(); i++) {
        uint8_t c = (uint8_t)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

// ---------------------------------------------------------
// OpenSky OAuth2: アクセストークン取得(必要な時だけ更新)
// ---------------------------------------------------------
static bool ensureAccessToken() {
    if (g_accessToken.length() > 0 && millis() < g_tokenExpiryMillis) {
        return true; // まだ有効
    }

    WiFiClientSecure client;
    client.setInsecure(); // 簡易化のため証明書検証は省略(実運用ではルートCA検証を推奨)

    HTTPClient https;
    const char *url =
        "https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token";

    if (!https.begin(client, url)) {
        Serial.println("[auth] https.begin failed");
        return false;
    }
    https.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String body = "grant_type=client_credentials&client_id=" + urlEncode(OPENSKY_CLIENT_ID) +
                  "&client_secret=" + urlEncode(OPENSKY_CLIENT_SECRET);

    int code = https.POST(body);
    String payload = https.getString(); // 成功/失敗どちらでも本文を読んでおく
    https.end();

    if (code != 200) {
        Serial.printf("[auth] token request failed, HTTP %d\n", code);
        Serial.printf("[auth] response body: %s\n", payload.c_str());
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("[auth] JSON parse error: %s\n", err.c_str());
        return false;
    }

    const char *token = doc["access_token"];
    long expiresIn = doc["expires_in"] | 1800;
    if (!token) return false;

    g_accessToken = String(token);
    // 期限の60秒前に更新するよう余裕を持たせる
    g_tokenExpiryMillis = millis() + (unsigned long)(expiresIn > 60 ? expiresIn - 60 : expiresIn) * 1000UL;
    Serial.println("[auth] access token acquired");
    return true;
}

// ---------------------------------------------------------
// hexdb.io: ICAO空港コード -> IATA空港コードへの変換
// 例: GET https://hexdb.io/api/v1/airport/icao/RJCC -> {"iata":"CTS", ...}
// ---------------------------------------------------------
static String airportIcaoToIata(const String &icao) {
    if (icao.length() == 0) return "";

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    String url = "https://hexdb.io/api/v1/airport/icao/" + icao;
    String iata;

    if (https.begin(client, url)) {
        https.setTimeout(6000);
        int code = https.GET();
        if (code == 200) {
            String payload = https.getString();
            JsonDocument doc;
            if (!deserializeJson(doc, payload)) {
                const char *iataCode = doc["iata"];
                if (iataCode) iata = String(iataCode);
            }
        }
        https.end();
    }
    return iata;
}

// ---------------------------------------------------------
// hexdb.io: コールサインから経路(出発地-目的地空港ICAOコード)を取得
// 例: GET https://hexdb.io/api/v1/route/icao/ANA642
//     -> {"flight":"ANA642","route":"RJCC-RJTT", ...}
// 認証不要の無料API。タップされた機体についてのみオンデマンドで呼ぶ。
// ---------------------------------------------------------
static void lookupRoute(const String &callsign, uint32_t reqId) {
    bool ok = false;
    String origin, dest;

    if (callsign.length() > 0) {
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient https;
        String url = "https://hexdb.io/api/v1/route/icao/" + callsign;

        if (https.begin(client, url)) {
            https.setTimeout(6000);
            int code = https.GET();
            if (code == 200) {
                String payload = https.getString();
                JsonDocument doc;
                if (!deserializeJson(doc, payload)) {
                    const char *route = doc["route"]; // 例: "RJCC-RJTT"
                    if (route) {
                        String r(route);
                        int dash = r.indexOf('-');
                        if (dash > 0) {
                            origin = r.substring(0, dash);
                            dest = r.substring(dash + 1);
                            ok = true;

                            // ICAO -> IATA変換を試みる。取得できればIATAコードに差し替え、
                            // 取得できなければICAOコードのまま表示する。
                            String originIata = airportIcaoToIata(origin);
                            String destIata = airportIcaoToIata(dest);
                            if (originIata.length() > 0) origin = originIata;
                            if (destIata.length() > 0) dest = destIata;
                        }
                    }
                }
            } else {
                Serial.printf("[route] lookup failed for %s, HTTP %d\n", callsign.c_str(), code);
            }
            https.end();
        }
    }

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    // 処理中にさらに新しいタップがあった場合は、その結果を古い方の応答で上書きしない
    if (reqId == g_routeRequestId) {
        g_routeLookupOk = ok;
        strlcpy(g_routeOrigin, origin.c_str(), sizeof(g_routeOrigin));
        strlcpy(g_routeDest, dest.c_str(), sizeof(g_routeDest));
    }
    g_routeProcessedId = reqId;
    xSemaphoreGive(g_dataMutex);

    if (ok) {
        Serial.printf("[route] %s: %s -> %s\n", callsign.c_str(), origin.c_str(), dest.c_str());
    }
}

// デバッグ用: 実データが取得できない状況でもタップ動作を検証できるよう、
// ダミー機体を1機だけレーダー中心付近に強制表示する。確認できたら 0 に戻すこと。
#define DEBUG_INJECT_FAKE_AIRCRAFT 0
#if DEBUG_INJECT_FAKE_AIRCRAFT
static void injectFakeAircraft(Aircraft temp[], int &count) {
    if (count >= MAX_AIRCRAFT) return;
    strlcpy(temp[count].icao24, "TEST01", sizeof(temp[count].icao24));
    strlcpy(temp[count].callsign, "DUMMY01", sizeof(temp[count].callsign));
    temp[count].lat = HOME_LAT;
    temp[count].lon = HOME_LON;
    temp[count].x = RADAR_CX;
    temp[count].y = RADAR_CY - 40; // 中心より少し上に表示
    count++;
}
#endif

// ---------------------------------------------------------
// OpenSky states/all を取得し、範囲内(RADAR_RADIUS_KM)の機体だけを抽出
// ---------------------------------------------------------
static void fetchAircraftOnce() {
    if (WiFi.status() != WL_CONNECTED) {
        g_wifiOk = false;
        return;
    }
    g_wifiOk = true;

    if (!ensureAccessToken()) {
        g_lastFetchOk = false;
        return;
    }

    double dLat = RADAR_RADIUS_KM / 111.32;
    double dLon = RADAR_RADIUS_KM / (111.32 * cos(deg2rad(HOME_LAT)));

    char url[256];
    snprintf(url, sizeof(url),
             "https://opensky-network.org/api/states/all?lamin=%.5f&lomin=%.5f&lamax=%.5f&lomax=%.5f",
             HOME_LAT - dLat, HOME_LON - dLon, HOME_LAT + dLat, HOME_LON + dLon);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;

    if (!https.begin(client, url)) {
        g_lastFetchOk = false;
        return;
    }
    https.addHeader("Authorization", "Bearer " + g_accessToken);
    https.setTimeout(8000);

    int code = https.GET();
    if (code != 200) {
        Serial.printf("[fetch] states request failed, HTTP %d\n", code);
        https.end();
        g_lastFetchOk = false;
        return;
    }

    // レスポンスは範囲を絞っているのでサイズは小さめ。
    // getStream()を直接渡すとchunked transfer encodingのまま読んでしまい
    // ArduinoJsonがパースできない(InvalidInput)ことがあるため、
    // getString()でボディ全体(自動でデチャンク済み)を取得してからパースする。
    String payload = https.getString();
    https.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);

    if (err) {
        Serial.printf("[fetch] JSON parse error: %s\n", err.c_str());
        g_lastFetchOk = false;
        return;
    }

    Aircraft temp[MAX_AIRCRAFT];
    int count = 0;

    JsonArray states = doc["states"].as<JsonArray>();
    if (!states.isNull()) {
        for (JsonVariant sv : states) {
            if (count >= MAX_AIRCRAFT) break;
            JsonArray s = sv.as<JsonArray>();
            if (s.isNull() || s.size() < 7) continue;

            bool hasLon = !s[5].isNull();
            bool hasLat = !s[6].isNull();
            if (!hasLon || !hasLat) continue; // 位置情報なし(地上局圏外など)

            double lon = s[5].as<double>();
            double lat = s[6].as<double>();

            double dist = haversineKm(HOME_LAT, HOME_LON, lat, lon);
            if (dist > RADAR_RADIUS_KM) continue; // 矩形範囲のうち円の外側は除外

            const char *icao24 = s[0] | "";
            const char *callsign = s[1] | "";

            strlcpy(temp[count].icao24, icao24, sizeof(temp[count].icao24));
            // callsignは末尾スペースが付くことが多いのでトリム
            String cs = String(callsign);
            cs.trim();
            strlcpy(temp[count].callsign, cs.c_str(), sizeof(temp[count].callsign));

            temp[count].lat = lat;
            temp[count].lon = lon;
            latLonToXY(lat, lon, temp[count].x, temp[count].y);
            count++;
        }
    }

#if DEBUG_INJECT_FAKE_AIRCRAFT
    injectFakeAircraft(temp, count);
#endif

    // 共有データを更新
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    memcpy(g_aircraft, temp, sizeof(Aircraft) * count);
    g_aircraftCount = count;
    g_selected = -1; // データが入れ替わるので選択状態はリセット
    xSemaphoreGive(g_dataMutex);

    g_lastFetchOk = true;
    Serial.printf("[fetch] %d aircraft in range\n", count);
}

// ---------------------------------------------------------
// ネットワーク取得タスク (Core0で動作させ、描画をブロックしないようにする)
// ---------------------------------------------------------
static void networkTask(void *param) {
    // WiFi接続
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    g_wifiOk = true;
    Serial.printf("[wifi] connected, IP=%s\n", WiFi.localIP().toString().c_str());

    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            g_wifiOk = false;
            WiFi.reconnect();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        static unsigned long lastFetch = 0;
        unsigned long now = millis();
        if (lastFetch == 0 || now - lastFetch >= FETCH_INTERVAL_MS) {
            fetchAircraftOnce();
            lastFetch = millis();
        }

        // 経路ルックアップ要求があれば処理(タップされたときだけ発生)
        uint32_t reqId;
        char callsignCopy[16];
        xSemaphoreTake(g_dataMutex, portMAX_DELAY);
        reqId = g_routeRequestId;
        strlcpy(callsignCopy, g_routeQueryCallsign, sizeof(callsignCopy));
        xSemaphoreGive(g_dataMutex);

        if (reqId != g_routeProcessedId) {
            lookupRoute(String(callsignCopy), reqId);
        }

        vTaskDelay(pdMS_TO_TICKS(300)); // ルックアップ要求をすぐ拾えるよう短い周期でポーリング
    }
}

// ---------------------------------------------------------
// 描画関連
// ---------------------------------------------------------
static float g_sweepAngleDeg = 0.0f; // 0=真上(北)、時計回り

// 角度[deg] -> レーダー外周上の座標
static void sweepEndpoint(float angleDeg, float radiusPx, int16_t &x, int16_t &y) {
    float rad = deg2rad(angleDeg);
    x = RADAR_CX + (int16_t)lround(sinf(rad) * radiusPx);
    y = RADAR_CY - (int16_t)lround(cosf(rad) * radiusPx);
}

static void drawRadarBase() {
    // 背景
    g_canvas.fillSprite(TFT_BLACK);

    // 同心円 (1/3, 2/3, 3/3)
    for (int i = 1; i <= 3; i++) {
        int r = RADAR_R_PX * i / 3;
        g_canvas.drawCircle(RADAR_CX, RADAR_CY, r, TFT_DARKGREEN);
    }
    // 十字線
    g_canvas.drawLine(RADAR_CX - RADAR_R_PX, RADAR_CY, RADAR_CX + RADAR_R_PX, RADAR_CY, TFT_DARKGREEN);
    g_canvas.drawLine(RADAR_CX, RADAR_CY - RADAR_R_PX, RADAR_CX, RADAR_CY + RADAR_R_PX, TFT_DARKGREEN);

    // 距離ラベル
    g_canvas.setTextColor(TFT_DARKGREEN, TFT_BLACK);
    g_canvas.setTextSize(1);
    g_canvas.setCursor(RADAR_CX + 4, RADAR_CY - RADAR_R_PX / 3 - 10);
    g_canvas.printf("%dkm", (int)(RADAR_RADIUS_KM / 3));
    g_canvas.setCursor(RADAR_CX + 4, RADAR_CY - RADAR_R_PX * 2 / 3 - 10);
    g_canvas.printf("%dkm", (int)(RADAR_RADIUS_KM * 2 / 3));
    g_canvas.setCursor(RADAR_CX + 4, RADAR_CY - RADAR_R_PX - 10);
    g_canvas.printf("%dkm", (int)RADAR_RADIUS_KM);
}

static void drawSweep() {
    // 掃引線 + フェードするトレイル(疑似残光)
    const int TRAIL_STEPS = 18;
    const float TRAIL_STEP_DEG = 2.2f;

    for (int i = TRAIL_STEPS; i >= 0; i--) {
        float angle = g_sweepAngleDeg - i * TRAIL_STEP_DEG;
        int16_t ex, ey;
        sweepEndpoint(angle, RADAR_R_PX, ex, ey);

        // 明るさを段階的に落とす(緑の輝度を下げていく)
        uint8_t g = (uint8_t)(255 * (1.0f - (float)i / (TRAIL_STEPS + 1)));
        uint16_t color = g_canvas.color565(0, g, 0);
        g_canvas.drawLine(RADAR_CX, RADAR_CY, ex, ey, color);
    }
}

static void drawAircraftAndLabel() {
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    int count = g_aircraftCount;
    Aircraft local[MAX_AIRCRAFT];
    memcpy(local, g_aircraft, sizeof(Aircraft) * count);
    int selected = g_selected;
    uint32_t routeReqId = g_routeRequestId;
    uint32_t routeProcId = g_routeProcessedId;
    bool routeOk = g_routeLookupOk;
    char routeOrigin[8], routeDest[8];
    strlcpy(routeOrigin, g_routeOrigin, sizeof(routeOrigin));
    strlcpy(routeDest, g_routeDest, sizeof(routeDest));
    xSemaphoreGive(g_dataMutex);

    for (int i = 0; i < count; i++) {
        bool isSel = (i == selected);
        uint16_t color = isSel ? TFT_YELLOW : TFT_GREEN;
        int r = isSel ? 4 : 3;
        g_canvas.fillCircle(local[i].x, local[i].y, r, color);
        if (isSel) {
            g_canvas.drawCircle(local[i].x, local[i].y, r + 3, TFT_YELLOW);
        }
    }

    // 上部ステータス行
    int32_t batLevel = M5.Power.getBatteryLevel();       // 0-100 (未接続/取得不可なら -1)
    bool    charging = M5.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging;

    g_canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    g_canvas.setCursor(4, 2);
    g_canvas.printf("WiFi:%s  API:%s  AC:%d  BAT:%s%s",
                     g_wifiOk ? "OK" : "--",
                     g_lastFetchOk ? "OK" : "--",
                     count,
                     batLevel >= 0 ? (String(batLevel) + "%").c_str() : "--",
                     charging ? "+" : "");

    // 下部: 選択中の機体情報(便名 + 目的地空港コード)
    g_canvas.fillRect(0, SCREEN_H - 28, SCREEN_W, 28, TFT_BLACK);
    g_canvas.drawFastHLine(0, SCREEN_H - 28, SCREEN_W, TFT_DARKGREEN);
    g_canvas.setTextSize(2);
    if (selected >= 0 && selected < count) {
        const char *flightNo = local[selected].callsign[0] != '\0'
                                    ? local[selected].callsign
                                    : local[selected].icao24;
        g_canvas.setCursor(8, SCREEN_H - 22);
        g_canvas.setTextColor(TFT_YELLOW, TFT_BLACK);

        if (local[selected].callsign[0] == '\0') {
            // コールサインが無い機体は経路検索できない
            g_canvas.printf("%s (no callsign)", flightNo);
        } else if (routeReqId != routeProcId) {
            g_canvas.printf("%s (looking up dest...)", flightNo);
        } else if (routeOk) {
            g_canvas.printf("%s - %s", routeOrigin, routeDest);
        } else {
            g_canvas.printf("%s (dest unknown)", flightNo);
        }
    } else {
        g_canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
        g_canvas.setCursor(8, SCREEN_H - 22);
        g_canvas.print("Tap a dot to show flight info");
    }
    g_canvas.setTextSize(1);
}

static void renderFrame() {
    drawRadarBase();
    drawSweep();
    drawAircraftAndLabel();
    g_canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------
// タッチ処理: ドットに近ければ選択、それ以外は選択解除
// ---------------------------------------------------------
static void handleTouch() {
    auto t = M5.Touch.getDetail();
    if (!t.wasPressed()) return;

    int16_t tx = t.x, ty = t.y;
    Serial.printf("[touch] pressed at (%d, %d)\n", tx, ty);

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    int best = -1;
    float bestDist2 = TAP_HIT_RADIUS_PX * TAP_HIT_RADIUS_PX;
    for (int i = 0; i < g_aircraftCount; i++) {
        float dx = tx - g_aircraft[i].x;
        float dy = ty - g_aircraft[i].y;
        float d2 = dx * dx + dy * dy;
        Serial.printf("[touch]   aircraft[%d] dot=(%d,%d) dist=%.1fpx\n",
                      i, g_aircraft[i].x, g_aircraft[i].y, sqrtf(d2));
        if (d2 <= bestDist2) {
            bestDist2 = d2;
            best = i;
        }
    }
    g_selected = best; // 何もヒットしなければ -1 = 表示解除

    if (best >= 0 && g_aircraft[best].callsign[0] != '\0') {
        // 新しい経路ルックアップ要求を発行(networkTask側が拾って処理する)
        strlcpy(g_routeQueryCallsign, g_aircraft[best].callsign, sizeof(g_routeQueryCallsign));
        g_routeLookupOk = false;
        g_routeOrigin[0] = '\0';
        g_routeDest[0] = '\0';
        g_routeRequestId++;
    }

    Serial.printf("[touch]   -> selected=%d\n", best);
    xSemaphoreGive(g_dataMutex);
}

// ---------------------------------------------------------
// setup / loop
// ---------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(300); // ネイティブUSB CDCがホスト側で認識されるまで少し待つ
    Serial.println("[boot] start");

    auto cfg = M5.config();
    M5.begin(cfg);
    Serial.println("[boot] M5.begin done");

    M5.Display.setRotation(1); // 横向き 320x240 を想定。実機に合わせて調整してください
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(4, 4);
    M5.Display.print("booting...");
    Serial.println("[boot] display cleared");

    g_canvas.setColorDepth(16);
    if (!g_canvas.createSprite(SCREEN_W, SCREEN_H)) {
        Serial.println("[boot] ERROR: createSprite failed (out of memory?)");
    }
    g_canvas.setTextFont(1);
    Serial.println("[boot] sprite created");

    g_dataMutex = xSemaphoreCreateMutex();

    // 通信(WiFi接続 + 定期フェッチ)はCore0の別タスクで実行し、
    // 描画/タッチ処理(Core1のloop)をブロックしないようにする
    xTaskCreatePinnedToCore(networkTask, "networkTask", 24576, nullptr, 1, nullptr, 0);
    Serial.println("[boot] networkTask created, entering loop()");
}

void loop() {
    M5.update();
    handleTouch();

    g_sweepAngleDeg += 4.0f; // スイープ速度(1フレームあたりの回転角)
    if (g_sweepAngleDeg >= 360.0f) g_sweepAngleDeg -= 360.0f;

    renderFrame();

    vTaskDelay(pdMS_TO_TICKS(33)); // 約30fps
}
