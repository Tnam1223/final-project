/*
 * receiver_freertos.ino — ESP32 LoRa Gateway → WiFi → MQTT + ILI9341 2.8" SPI
 *
 * Phần cứng:
 *   LoRa AS32    → UART1  RX=25 TX=26  (M0=4 M1=5)
 *   ILI9341 SPI  → CS=15 DC=2 RST=13  (MOSI=23 SCK=18)
 *   WiFi         → built-in ESP32
 *
 * Màn hình (landscape 320×240):
 *   ┌─ NODE 1 (cyan) ───────┬─ NODE 2 (yellow) ──────┐  y:  0-18
 *   │ T:28.5C  H:65.3%      │ T:27.1C  H:63.2%       │  y: 20-38
 *   │ PM2.5: 25 ug/m3       │ PM2.5: 31 ug/m3        │  y: 39-57
 *   │ PM10:  40 ug/m3       │ PM10:  48 ug/m3        │  y: 58-76
 *   │ CO: 2.500 ppm         │ CO: 1.800 ppm          │  y: 77-95
 *   │ Loss: 2.3%            │ Loss: 0.0%             │  y: 96-114
 *   │ AQI:  85 [TB]         │ AQI:  72 [TB]          │  y:115-133
 *   ├── PM2.5 history (cyan=N1, yellow=N2) ──────────┤  y:136
 *   │  [line graph, 60 điểm = 5 phút]                │  y:146-189
 *   ├── CO history ──────────────────────────────────┤  y:192
 *   │  [line graph, 60 điểm = 5 phút]                │  y:202-237
 *   └────────────────────────────────────────────────┘
 *
 * VN_AQI (Quyết định 1459/QĐ-BTNMT):
 *   0–50   Tốt        — xanh lá
 *   51–100 Trung bình — vàng
 *  101–150 Kém        — cam
 *  151–200 Xấu        — đỏ
 *  201–300 Rất xấu    — tím
 *  301–500 Nguy hại   — đỏ sẫm
 *
 * FreeRTOS Tasks & Priorities:
 *   taskDisplay      — P1, Core 0 — cập nhật màn hình mỗi 1s
 *   taskWifiWatchdog — P2, Core 0 — kiểm tra WiFi mỗi 10s
 *   taskMqttPublish  — P3, Core 0 — parse gói, publish MQTT, cập nhật data màn hình
 *   taskMqttLoop     — P4, Core 0 — gọi mqttClient.loop() mỗi 10ms
 *   taskLoraRx       — P5, Core 1 — đọc UART LoRa liên tục
 *
 * Tính tỷ lệ mất gói:
 *   - Dựa vào số thứ tự (seq 0–999) trong gói tin từ sender
 *   - Phát hiện khoảng trống seq → cộng dồn vào lostCount
 *   - Loss% = lostCount / (rxCount + lostCount) × 100
 *   - Nếu gap > LOSS_GAP_RESET → coi node khởi động lại, reset bộ đếm
 *
 * Thư viện cần cài:
 *   - PubSubClient    (by Nick O'Leary)
 *   - ArduinoJson     (by Benoit Blanchon) v6.x
 *   - Adafruit_ILI9341
 *   - Adafruit_GFX
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <WiFiClient.h>

// ─── WiFi ──────────────────────────────────────────────────────────────
#define WIFI_SSID         "Trung Nam"
#define WIFI_PASSWORD     "19761976"
#define WIFI_TIMEOUT_MS   15000

// ─── MQTT ──────────────────────────────────────────────────────────────
#define MQTT_HOST         "192.168.1.10"
#define MQTT_PORT         1883
#define MQTT_CLIENT_ID    "lora-gateway-01"
#define MQTT_RECONNECT_MS 3000

// ─── LoRa AS32 ────────────────────────────────────────────────────────
#define LORA_ADDR_HIGH    0xBE
#define LORA_ADDR_LOW     0xEF
#define LORA_CHAN          0x18
#define LORA_RX_PIN       25
#define LORA_TX_PIN       26
#define LORA_M0_PIN        4
#define LORA_M1_PIN        5

// ─── ILI9341 2.8" SPI ─────────────────────────────────────────────────
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  13

// ─── Queue ────────────────────────────────────────────────────────────
#define PACKET_MAX_LEN   128
#define QUEUE_DEPTH        8

// ─── Graph / node config ──────────────────────────────────────────────
#define MAX_NODES         2
#define GRAPH_POINTS     60    // 60 × 5s = 5 phút lịch sử

// ─── Ngưỡng gap seq để phát hiện node khởi động lại ──────────────────
#define LOSS_GAP_RESET   100

// ─── AQIBreakpoint — khai báo sớm để tránh lỗi prototype Arduino IDE ─
// Arduino IDE tự sinh prototype hàm trước khi xử lý file, nên struct
// phải được định nghĩa ở đây (trước mọi hàm) thì prototype mới hợp lệ.
struct AQIBreakpoint { float cLo, cHi; int iLo, iHi; };

// ─── UART ─────────────────────────────────────────────────────────────
HardwareSerial loraSerial(1);

// ─── Objects ──────────────────────────────────────────────────────────
WiFiClient       wifiClient;
PubSubClient     mqttClient(wifiClient);
Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);

// ─── Packet type ──────────────────────────────────────────────────────
struct LoraPacket { char data[PACKET_MAX_LEN]; };

// ─── Dữ liệu hiển thị theo từng node ─────────────────────────────────
struct NodeDisplay {
    float    temp, hum, pm25, pm10, co;
    float    lossRate;    // % gói tin bị mất (0.0 – 100.0)
    int      aqi;         // VN_AQI tổng hợp (0–500)
    bool     valid;
    uint32_t lastMs;
    uint32_t rxCount;     // Tổng gói nhận thành công
    uint32_t lostCount;   // Tổng gói ước tính bị mất
};
static NodeDisplay g_node[MAX_NODES] = {};

// ─── Theo dõi tỷ lệ mất gói (bảo vệ bởi g_dispMutex) ────────────────
struct NodeLoss {
    uint16_t lastSeq;
    uint32_t rxCount;
    uint32_t lostCount;
    bool     hasFirst;
};
static NodeLoss g_loss[MAX_NODES] = {};

// ─── Circular buffer lịch sử đồ thị ─────────────────────────────────
static float   g_pm25H[MAX_NODES][GRAPH_POINTS];
static float   g_coH[MAX_NODES][GRAPH_POINTS];
static uint8_t g_hHead[MAX_NODES] = {0, 0};
static uint8_t g_hCnt[MAX_NODES]  = {0, 0};

// ─── FreeRTOS handles ────────────────────────────────────────────────
QueueHandle_t     g_packetQueue = nullptr;
SemaphoreHandle_t g_mqttMutex   = nullptr;
SemaphoreHandle_t g_dispMutex   = nullptr;

// ─── Bảng màu RGB565 ─────────────────────────────────────────────────
#define C_BG      0x0000u
#define C_WHITE   0xFFFFu
#define C_GREY    0x8410u
#define C_DGREY   0x2945u
#define C_N1      0x07FFu   // Cyan   → Node 1
#define C_N2      0xFFE0u   // Vàng   → Node 2
#define C_GREEN   0x07E0u
#define C_ORANGE  0xFD20u
#define C_RED     0xF800u
#define C_PURPLE  0x780Fu   // Tím    → AQI rất xấu
#define C_MAROON  0x8000u   // Đỏ sẫm → AQI nguy hại
#define C_GBORD   0x39E7u
#define C_GBG     0x0841u

// ─── Layout hằng số (landscape 320 × 240) ────────────────────────────
#define SCR_W  320
#define SCR_H  240
#define COL    160
#define CPAD     4

#define TIT_Y    0
#define TIT_H   19
#define ROW_H   19
#define ROW0    (TIT_Y + TIT_H + 1)    //  20  Temp + Hum
#define ROW1    (ROW0 + ROW_H)          //  39  PM2.5
#define ROW2    (ROW1 + ROW_H)          //  58  PM10
#define ROW3    (ROW2 + ROW_H)          //  77  CO
#define ROW4    (ROW3 + ROW_H)          //  96  Tỷ lệ mất gói
#define ROW5    (ROW4 + ROW_H)          // 115  VN_AQI
#define DEND    (ROW5 + ROW_H)          // 134  Hết phần dữ liệu

// Đồ thị PM2.5
#define GPLBL   (DEND + 2)              // 136  Nhãn
#define GPTOP   (GPLBL + 10)            // 146  Đỉnh graph
#define GPH      44                      //      Chiều cao graph
#define GPBOT   (GPTOP + GPH)           // 190  Đáy graph

// Đồ thị CO
#define GCLBL   (GPBOT + 2)             // 192  Nhãn
#define GCTOP   (GCLBL + 10)            // 202  Đỉnh graph
#define GCH     (SCR_H - GCTOP - 2)    //  36  Chiều cao graph

// =====================================================================
//  Helper: màu cảnh báo theo ngưỡng
// =====================================================================
static uint16_t pm25Color(float v) {
    if (v < 12.0f) return C_GREEN;
    if (v < 35.0f) return C_ORANGE;
    return C_RED;
}
static uint16_t coColor(float v) {
    if (v < 9.0f)  return C_GREEN;
    if (v < 35.0f) return C_ORANGE;
    return C_RED;
}
static uint16_t lossColor(float v) {
    if (v <  5.0f) return C_GREEN;
    if (v < 20.0f) return C_ORANGE;
    return C_RED;
}

// =====================================================================
//  VN_AQI — Tính chỉ số chất lượng không khí (Quyết định 1459/QĐ-BTNMT)
//
//  Công thức sub-index:
//    AQI_p = (I_hi - I_lo) / (C_hi - C_lo) × (C_p - C_lo) + I_lo
//  AQI cuối cùng = max(AQI_PM2.5, AQI_PM10, AQI_CO)
//
//  PM2.5, PM10 : µg/m³
//  CO          : ppm → tự động đổi sang mg/m³ (×1.145 tại 25°C)
// =====================================================================
static const AQIBreakpoint PM25_BP[6] = {
    {  0.f,  25.f,   0,  50},
    { 25.f,  50.f,  51, 100},
    { 50.f, 150.f, 101, 200},
    {150.f, 250.f, 201, 300},
    {250.f, 350.f, 301, 400},
    {350.f, 500.f, 401, 500}
};
static const AQIBreakpoint PM10_BP[6] = {
    {  0.f,  50.f,   0,  50},
    { 50.f, 150.f,  51, 100},
    {150.f, 250.f, 101, 200},
    {250.f, 350.f, 201, 300},
    {350.f, 420.f, 301, 400},
    {420.f, 600.f, 401, 500}
};
// CO theo QCVN 06:2009/BTNMT (mg/m³, trung bình 8 giờ)
static const AQIBreakpoint CO_MG_BP[6] = {
    {  0.f,  10.f,   0,  50},
    { 10.f,  30.f,  51, 100},
    { 30.f,  70.f, 101, 200},
    { 70.f, 150.f, 201, 300},
    {150.f, 200.f, 301, 400},
    {200.f, 300.f, 401, 500}
};

// 1 ppm CO = 1.145 mg/m³ (M=28 g/mol, Vm=24.45 L/mol tại 25°C)
#define CO_PPM_TO_MGPM3  1.145f

static int calcSubAQI(float c, const AQIBreakpoint *bp, int n) {
    if (c < 0.f) return 0;
    for (int i = 0; i < n; i++) {
        if (c <= bp[i].cHi) {
            float v = ((float)(bp[i].iHi - bp[i].iLo) / (bp[i].cHi - bp[i].cLo))
                      * (c - bp[i].cLo) + (float)bp[i].iLo;
            return (int)(v + 0.5f);
        }
    }
    return 500;
}

// Trả về VN_AQI tổng hợp từ PM2.5 (µg/m³), PM10 (µg/m³), CO (ppm)
static int calcAQI(float pm25, float pm10, float co_ppm) {
    int a1 = calcSubAQI(pm25,                   PM25_BP,  6);
    int a2 = calcSubAQI(pm10,                   PM10_BP,  6);
    int a3 = calcSubAQI(co_ppm * CO_PPM_TO_MGPM3, CO_MG_BP, 6);
    return min(max(max(a1, a2), a3), 500);
}

static uint16_t aqiColor(int aqi) {
    if (aqi <=  50) return C_GREEN;
    if (aqi <= 100) return C_N2;      // Vàng
    if (aqi <= 150) return C_ORANGE;
    if (aqi <= 200) return C_RED;
    if (aqi <= 300) return C_PURPLE;
    return C_MAROON;
}

// Nhãn ngắn 6 ký tự (đệm khoảng trắng để xóa giá trị cũ)
static const char *aqiLabel(int aqi) {
    if (aqi <=  50) return "Tot   ";
    if (aqi <= 100) return "TB    ";
    if (aqi <= 150) return "Kem   ";
    if (aqi <= 200) return "Xau   ";
    if (aqi <= 300) return "R.Xau ";
    return "Nguy Hiem ";
}

// =====================================================================
//  Tính và cập nhật tỷ lệ mất gói theo seq
//  Gọi bên trong vùng g_dispMutex đã giữ.
// =====================================================================
static float updateloss(int idx, uint16_t seq) {
    NodeLoss &nl = g_loss[idx];

    if (!nl.hasFirst) {
        nl.lastSeq   = seq;
        nl.rxCount   = 1;
        nl.lostCount = 0;
        nl.hasFirst  = true;
        return 0.0f;
    }

    uint16_t gap;
    if (seq >= nl.lastSeq) {
        gap = seq - nl.lastSeq;
    } else {
        gap = (uint16_t)(1000u - nl.lastSeq + seq);
    }

    if (gap == 0) {
        // Gói trùng lặp — bỏ qua
    } else if (gap > LOSS_GAP_RESET) {
        Serial.printf("[Loss] Node%d: gap=%u > %u → reset\n",
                      idx + 1, gap, (unsigned)LOSS_GAP_RESET);
        nl.lastSeq   = seq;
        nl.rxCount   = 1;
        nl.lostCount = 0;
        return 0.0f;
    } else {
        nl.lostCount += (uint32_t)(gap - 1);
        nl.rxCount++;
        nl.lastSeq = seq;
    }

    uint32_t total = nl.rxCount + nl.lostCount;
    return (total > 0) ? ((float)nl.lostCount * 100.0f / (float)total) : 0.0f;
}

// =====================================================================
//  Helper: đọc circular buffer theo thứ tự thời gian (cũ → mới)
// =====================================================================
static void orderedHist(const float *src, uint8_t head, uint8_t cnt, float *out) {
    if (cnt == 0) return;
    if (cnt < GRAPH_POINTS) {
        memcpy(out, src, cnt * sizeof(float));
    } else {
        uint8_t tail = GRAPH_POINTS - head;
        memcpy(out,        src + head, tail * sizeof(float));
        memcpy(out + tail, src,        head * sizeof(float));
    }
}

// =====================================================================
//  Vẽ đồ thị 2 chuỗi dữ liệu trong hình chữ nhật
// =====================================================================
static void drawGraph(int16_t gx, int16_t gy, int16_t gw, int16_t gh,
                      const float *d1, uint8_t c1,
                      const float *d2, uint8_t c2,
                      float yMin, float yMax) {
    tft.fillRect(gx, gy, gw, gh, C_GBG);
    tft.drawRect(gx, gy, gw, gh, C_GBORD);

    if (yMax <= yMin) return;

    uint8_t nPts = max(c1, c2);
    if (nPts < 2) return;

    float xStep = (float)(gw - 2) / (nPts - 1);
    float yRng  = yMax - yMin;

    for (int s = 0; s < 2; s++) {
        const float *d   = (s == 0) ? d1 : d2;
        uint8_t      cnt = (s == 0) ? c1 : c2;
        uint16_t     col = (s == 0) ? C_N1 : C_N2;
        if (cnt < 2) continue;

        for (int i = 1; i < (int)cnt; i++) {
            int16_t x1 = gx + 1 + (int16_t)((i - 1) * xStep);
            int16_t x2 = gx + 1 + (int16_t)( i      * xStep);
            int16_t y1 = gy + gh - 1 - (int16_t)(((d[i-1] - yMin) / yRng) * (gh - 2));
            int16_t y2 = gy + gh - 1 - (int16_t)(((d[i  ] - yMin) / yRng) * (gh - 2));
            y1 = constrain(y1, gy + 1, gy + gh - 1);
            y2 = constrain(y2, gy + 1, gy + gh - 1);
            tft.drawLine(x1, y1, x2, y2, col);
        }
    }
}

// =====================================================================
//  Vẽ khung tĩnh (gọi 1 lần khi khởi động)
// =====================================================================
static void drawChrome() {
    tft.fillScreen(C_BG);

    // Nền tiêu đề
    tft.fillRect(0,   TIT_Y, COL, TIT_H, 0x0010u);
    tft.fillRect(COL, TIT_Y, COL, TIT_H, 0x3000u);

    // Tên node
    tft.setTextSize(2);
    tft.setTextColor(C_N1);
    tft.setCursor(CPAD, TIT_Y + 2);
    tft.print("NODE 1");
    tft.setTextColor(C_N2);
    tft.setCursor(COL + CPAD, TIT_Y + 2);
    tft.print("NODE 2");

    // Đường chia dọc và ngang
    tft.drawFastVLine(COL, 0,    DEND,        C_DGREY);
    tft.drawFastHLine(0,   DEND, SCR_W,       C_DGREY);
    tft.drawFastHLine(0,   GPTOP - 2, SCR_W,  C_DGREY);
    tft.drawFastHLine(0,   GCTOP - 2, SCR_W,  C_DGREY);

    // Nhãn đồ thị PM2.5
    tft.setTextSize(1);
    tft.setTextColor(C_GREY);
    tft.setCursor(CPAD,        GPLBL + 1); tft.print("PM2.5");
    tft.setCursor(SCR_W - 54,  GPLBL + 1); tft.print("[ug/m3]");
    tft.setTextColor(C_N1);
    tft.setCursor(44, GPLBL + 1); tft.print("-- N1");
    tft.setTextColor(C_N2);
    tft.setCursor(86, GPLBL + 1); tft.print("N2");

    // Nhãn đồ thị CO
    tft.setTextColor(C_GREY);
    tft.setCursor(CPAD,        GCLBL + 1); tft.print("CO");
    tft.setCursor(SCR_W - 36,  GCLBL + 1); tft.print("[ppm]");
    tft.setTextColor(C_N1);
    tft.setCursor(20, GCLBL + 1); tft.print("-- N1");
    tft.setTextColor(C_N2);
    tft.setCursor(62, GCLBL + 1); tft.print("N2");
}

// =====================================================================
//  Vẽ dữ liệu của 1 node (partial update)
// =====================================================================
static void drawNode(int n, const NodeDisplay &nd) {
    const int16_t x0 = (int16_t)(n * COL);
    const bool    ok = nd.valid && (millis() - nd.lastMs < 60000UL);

    tft.setTextSize(1);

    // ── Hàng 0: Temp + Hum ──────────────────────────────────────────
    tft.fillRect(x0 + 1, ROW0, COL - 2, ROW_H - 1, C_BG);
    tft.setCursor(x0 + CPAD, ROW0 + 5);
    if (ok) {
        char b[28];
        tft.setTextColor(C_WHITE);
        snprintf(b, sizeof(b), "T:%.1fC  H:%.1f%%", nd.temp, nd.hum);
        tft.print(b);
    } else {
        tft.setTextColor(C_GREY);
        tft.print(nd.valid ? "Stale..." : "No data");
    }

    // ── Hàng 1: PM2.5 ───────────────────────────────────────────────
    tft.fillRect(x0 + 1, ROW1, COL - 2, ROW_H - 1, C_BG);
    tft.setCursor(x0 + CPAD, ROW1 + 5);
    if (ok) {
        char b[24];
        tft.setTextColor(pm25Color(nd.pm25));
        snprintf(b, sizeof(b), "PM2.5: %.0f ug/m3", nd.pm25);
        tft.print(b);
    } else {
        tft.setTextColor(C_GREY);
        tft.print("PM2.5: ---");
    }

    // ── Hàng 2: PM10 ────────────────────────────────────────────────
    tft.fillRect(x0 + 1, ROW2, COL - 2, ROW_H - 1, C_BG);
    tft.setCursor(x0 + CPAD, ROW2 + 5);
    if (ok) {
        char b[24];
        tft.setTextColor(C_WHITE);
        snprintf(b, sizeof(b), "PM10:  %.0f ug/m3", nd.pm10);
        tft.print(b);
    } else {
        tft.setTextColor(C_GREY);
        tft.print("PM10:  ---");
    }

    // ── Hàng 3: CO ──────────────────────────────────────────────────
    tft.fillRect(x0 + 1, ROW3, COL - 2, ROW_H - 1, C_BG);
    tft.setCursor(x0 + CPAD, ROW3 + 5);
    if (ok) {
        char b[24];
        tft.setTextColor(coColor(nd.co));
        snprintf(b, sizeof(b), "CO: %.3f ppm", nd.co);
        tft.print(b);
    } else {
        tft.setTextColor(C_GREY);
        tft.print("CO: ---");
    }

    // ── Hàng 4: Tỷ lệ mất gói + bộ đếm gói ────────────────────────
    tft.fillRect(x0 + 1, ROW4, COL - 2, ROW_H, C_BG);

    // Dòng 1: Loss%
    tft.setCursor(x0 + CPAD, ROW4 + 1);
    if (nd.valid) {
        char b[24];
        tft.setTextColor(lossColor(nd.lossRate));
        snprintf(b, sizeof(b), "Loss: %.1f%%", nd.lossRate);
        tft.print(b);
        if (!ok) {
            tft.setTextColor(C_GREY);
            tft.print(" ?");
        }
    } else {
        tft.setTextColor(C_GREY);
        tft.print("Loss: ---");
    }

    // Dòng 2: Rx (nhận OK) / Total (tổng ước tính)
    tft.setCursor(x0 + CPAD, ROW4 + 11);
    if (nd.valid) {
        char b[24];
        uint32_t total = nd.rxCount + nd.lostCount;
        tft.setTextColor(C_GREY);
        snprintf(b, sizeof(b), "Rx:%lu/Tot:%lu", nd.rxCount, total);
        tft.print(b);
    } else {
        tft.setTextColor(C_GREY);
        tft.print("Rx:0/Tot:0");
    }

    // ── Hàng 5: VN_AQI ──────────────────────────────────────────────
    tft.fillRect(x0 + 1, ROW5, COL - 2, ROW_H - 1, C_BG);
    tft.setCursor(x0 + CPAD, ROW5 + 5);
    if (ok) {
        char b[24];
        tft.setTextColor(aqiColor(nd.aqi));
        snprintf(b, sizeof(b), "AQI:%3d [%s]", nd.aqi, aqiLabel(nd.aqi));
        tft.print(b);
    } else if (nd.valid) {
        // Dữ liệu cũ — hiển thị AQI nhưng thêm dấu ?
        char b[24];
        tft.setTextColor(aqiColor(nd.aqi));
        snprintf(b, sizeof(b), "AQI:%3d ?", nd.aqi);
        tft.print(b);
    } else {
        tft.setTextColor(C_GREY);
        tft.print("AQI: ---");
    }

    // Vẽ lại đường dọc phòng text tràn sang cột khác
    tft.drawFastVLine(COL, TIT_H, DEND - TIT_H, C_DGREY);
}

// =====================================================================
//  TASK: taskDisplay — Priority 1, Core 0
// =====================================================================
static void taskDisplay(void *pv) {
    drawChrome();

    while (true) {
        NodeDisplay snap[MAX_NODES];
        float       pm25O[MAX_NODES][GRAPH_POINTS];
        float       coO[MAX_NODES][GRAPH_POINTS];
        uint8_t     cnt[MAX_NODES];

        if (xSemaphoreTake(g_dispMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
            for (int n = 0; n < MAX_NODES; n++) {
                snap[n] = g_node[n];
                cnt[n]  = g_hCnt[n];
                orderedHist(g_pm25H[n], g_hHead[n], cnt[n], pm25O[n]);
                orderedHist(g_coH[n],   g_hHead[n], cnt[n], coO[n]);
            }
            xSemaphoreGive(g_dispMutex);
        }

        for (int n = 0; n < MAX_NODES; n++) {
            drawNode(n, snap[n]);
        }

        // Auto-scale đồ thị
        float pmMax = 50.0f;
        float coMax = 10.0f;
        for (int n = 0; n < MAX_NODES; n++) {
            for (uint8_t i = 0; i < cnt[n]; i++) {
                if (pm25O[n][i] > pmMax) pmMax = pm25O[n][i];
                if (coO[n][i]   > coMax) coMax = coO[n][i];
            }
        }
        pmMax *= 1.15f;
        coMax *= 1.15f;

        drawGraph(1, GPTOP, SCR_W - 2, GPH,
                  pm25O[0], cnt[0], pm25O[1], cnt[1],
                  0.0f, pmMax);

        drawGraph(1, GCTOP, SCR_W - 2, GCH,
                  coO[0], cnt[0], coO[1], cnt[1],
                  0.0f, coMax);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// =====================================================================
//  LoRa AS32
// =====================================================================
static void loraSetMode(int m0, int m1) {
    digitalWrite(LORA_M0_PIN, m0);
    digitalWrite(LORA_M1_PIN, m1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void loraInit() {
    loraSerial.begin(9600, SERIAL_8N1, LORA_RX_PIN, LORA_TX_PIN);
    pinMode(LORA_M0_PIN, OUTPUT);
    pinMode(LORA_M1_PIN, OUTPUT);
}

static void loraConfig() {
    loraSetMode(1, 1);
    uint8_t cfg[6] = {
        0xC0,
        LORA_ADDR_HIGH,
        LORA_ADDR_LOW,
        0x1A,
        LORA_CHAN,
        0x44
    };
    loraSerial.write(cfg, 6);
    Serial.println("[LoRa] Ghi cau hinh...");
    vTaskDelay(pdMS_TO_TICKS(200));
    loraSetMode(0, 0);
    Serial.println("[LoRa] Normal mode. Dang doi du lieu...");
}

// =====================================================================
//  WiFi
// =====================================================================
static void connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;

    Serial.printf("[WiFi] Ket noi '%s'", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t > WIFI_TIMEOUT_MS) {
            Serial.println("\n[WiFi] Timeout.");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print('.');
    }
    Serial.printf("\n[WiFi] OK. IP: %s\n", WiFi.localIP().toString().c_str());
}

// =====================================================================
//  MQTT (gọi bên trong vùng g_mqttMutex đã giữ)
// =====================================================================
static void connectMQTT_locked() {
    mqttClient.setServer(MQTT_HOST, MQTT_PORT);
    mqttClient.setKeepAlive(60);
    mqttClient.setSocketTimeout(10);

    while (!mqttClient.connected()) {
        Serial.printf("[MQTT] Ket noi %s:%d ...", MQTT_HOST, MQTT_PORT);

        if (mqttClient.connect(MQTT_CLIENT_ID)) {
            Serial.println(" OK");
        } else {
            Serial.printf(" Loi state=%d, thu lai...\n", mqttClient.state());
            xSemaphoreGive(g_mqttMutex);
            vTaskDelay(pdMS_TO_TICKS(MQTT_RECONNECT_MS));
            if (WiFi.status() != WL_CONNECTED) connectWiFi();
            xSemaphoreTake(g_mqttMutex, portMAX_DELAY);
        }
    }
}

static void publishSensorData_locked(int nodeId,
                                     float temp, float hum,
                                     float pm1,  float pm25, float pm10,
                                     float co,   float lossRate, int aqi) {
    StaticJsonDocument<300> doc;
    doc["id"]          = nodeId;
    doc["temperature"] = round(temp     * 10.0f)   / 10.0f;
    doc["humidity"]    = round(hum      * 10.0f)   / 10.0f;
    doc["pm1"]         = round(pm1      * 10.0f)   / 10.0f;
    doc["pm25"]        = round(pm25     * 10.0f)   / 10.0f;
    doc["pm10"]        = round(pm10     * 10.0f)   / 10.0f;
    doc["co"]          = round(co       * 1000.0f) / 1000.0f;
    doc["loss_pct"]    = round(lossRate * 10.0f)   / 10.0f;
    doc["aqi"]         = aqi;   // VN_AQI tổng hợp

    char topic[32];
    char payload[300];
    snprintf(topic, sizeof(topic), "lora/node/%d", nodeId);
    serializeJson(doc, payload, sizeof(payload));

    if (mqttClient.publish(topic, payload)) {
        Serial.printf("[MQTT] -> %s  %s\n", topic, payload);
    } else {
        Serial.println("[MQTT] Publish that bai!");
    }
}

// =====================================================================
//  Parse gói tin: $<nodeId>,<seq>,<temp>,<hum>,<pm1>,<pm25>,<pm10>,<co>
// =====================================================================
static bool parsePacket(const char *raw,
                        int &nodeId, uint16_t &seq,
                        float &temp, float &hum,
                        float &pm1,  float &pm25, float &pm10, float &co) {
    if (!raw || raw[0] != '$') return false;

    char buf[PACKET_MAX_LEN];
    strncpy(buf, raw + 1, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *fields[8];
    int   count = 0;
    char *saveptr;
    char *token = strtok_r(buf, ",", &saveptr);
    while (token && count < 8) {
        fields[count++] = token;
        token = strtok_r(nullptr, ",", &saveptr);
    }
    if (count < 8) return false;

    nodeId = atoi(fields[0]);
    seq    = (uint16_t)atoi(fields[1]);
    temp   = atof(fields[2]);
    hum    = atof(fields[3]);
    pm1    = atof(fields[4]);
    pm25   = atof(fields[5]);
    pm10   = atof(fields[6]);
    co     = atof(fields[7]);

    if (nodeId <= 0) return false;
    if (temp < -40.0f || temp > 85.0f)  return false;
    if (hum  <   0.0f || hum  > 100.0f) return false;

    return true;
}

// =====================================================================
//  TASK: taskLoraRx — Priority 5, Core 1
// =====================================================================
static void taskLoraRx(void *pvParameters) {
    static char   rxBuf[PACKET_MAX_LEN];
    static size_t rxLen = 0;

    while (true) {
        if (!loraSerial.available()) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        char c = (char)loraSerial.read();

        if (c == '\n') {
            if (rxLen > 0) {
                rxBuf[rxLen] = '\0';

                LoraPacket pkt;
                strncpy(pkt.data, rxBuf, PACKET_MAX_LEN - 1);
                pkt.data[PACKET_MAX_LEN - 1] = '\0';

                if (xQueueSend(g_packetQueue, &pkt, 0) != pdTRUE) {
                    Serial.println("[LoRa] Queue day — bo goi tin!");
                }
                rxLen = 0;
            }
        } else if (c != '\r') {
            if (rxLen < PACKET_MAX_LEN - 1) {
                rxBuf[rxLen++] = c;
            } else {
                Serial.println("[LoRa] Buffer tran — xoa");
                rxLen = 0;
            }
        }
    }
}

// =====================================================================
//  TASK: taskMqttPublish — Priority 3, Core 0
// =====================================================================
static void taskMqttPublish(void *pvParameters) {
    LoraPacket pkt;

    while (true) {
        if (xQueueReceive(g_packetQueue, &pkt, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }

        Serial.printf("[LoRa] Nhan: %s\n", pkt.data);

        int      nodeId;
        uint16_t seq;
        float    temp, hum, pm1, pm25, pm10, co;

        if (!parsePacket(pkt.data, nodeId, seq, temp, hum, pm1, pm25, pm10, co)) {
            Serial.println("[Parse] Goi tin khong hop le, bo qua.");
            continue;
        }

        Serial.printf("[Parse] Node=%d Seq=%03u T=%.1f H=%.1f PM1=%.0f PM2.5=%.0f PM10=%.0f CO=%.3f\n",
                      nodeId, seq, temp, hum, pm1, pm25, pm10, co);

        float lossRate = 0.0f;
        int   aqi      = 0;

        // Cập nhật dữ liệu màn hình, tính loss rate và AQI (cùng 1 mutex)
        if (nodeId >= 1 && nodeId <= MAX_NODES) {
            int idx = nodeId - 1;
            if (xSemaphoreTake(g_dispMutex, pdMS_TO_TICKS(100)) == pdTRUE) {

                lossRate = updateloss(idx, seq);
                aqi      = calcAQI(pm25, pm10, co);   // VN_AQI

                g_node[idx].temp      = temp;
                g_node[idx].hum       = hum;
                g_node[idx].pm25      = pm25;
                g_node[idx].pm10      = pm10;
                g_node[idx].co        = co;
                g_node[idx].lossRate  = lossRate;
                g_node[idx].aqi       = aqi;
                g_node[idx].valid     = true;
                g_node[idx].lastMs    = millis();
                g_node[idx].rxCount   = g_loss[idx].rxCount;
                g_node[idx].lostCount = g_loss[idx].lostCount;

                g_pm25H[idx][g_hHead[idx]] = pm25;
                g_coH[idx][g_hHead[idx]]   = co;
                g_hHead[idx] = (g_hHead[idx] + 1) % GRAPH_POINTS;
                if (g_hCnt[idx] < GRAPH_POINTS) g_hCnt[idx]++;

                xSemaphoreGive(g_dispMutex);
            }
        }

        Serial.printf("[Loss] Node=%d  rx=%lu  lost=%lu  loss=%.1f%%\n",
                      nodeId,
                      (unsigned long)g_loss[nodeId - 1].rxCount,
                      (unsigned long)g_loss[nodeId - 1].lostCount,
                      lossRate);
        Serial.printf("[AQI]  Node=%d  AQI=%d  PM2.5=%.0f PM10=%.0f CO=%.3f\n",
                      nodeId, aqi, pm25, pm10, co);

        // Publish MQTT (bao gồm trường aqi để lưu PostgreSQL)
        if (xSemaphoreTake(g_mqttMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
            if (!mqttClient.connected()) connectMQTT_locked();
            publishSensorData_locked(nodeId, temp, hum, pm1, pm25, pm10, co, lossRate, aqi);
            xSemaphoreGive(g_mqttMutex);
        } else {
            Serial.println("[MQTT] Khong lay duoc mutex, bo goi tin nay.");
        }
    }
}

// =====================================================================
//  TASK: taskMqttLoop — Priority 4, Core 0
// =====================================================================
static void taskMqttLoop(void *pvParameters) {
    while (true) {
        if (xSemaphoreTake(g_mqttMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (mqttClient.connected()) {
                mqttClient.loop();
            }
            xSemaphoreGive(g_mqttMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// =====================================================================
//  TASK: taskWifiWatchdog — Priority 2, Core 0
// =====================================================================
static void taskWifiWatchdog(void *pvParameters) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] Mat ket noi — thu lai...");
            connectWiFi();
        }
    }
}

// =====================================================================
//  SETUP
// =====================================================================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n====== ESP32 LORA GATEWAY (FreeRTOS) ======");
    Serial.printf("  MQTT Broker : %s:%d\n", MQTT_HOST, MQTT_PORT);
    Serial.printf("  Client ID   : %s\n",   MQTT_CLIENT_ID);
    Serial.printf("  Loss tracking: seq 0-999, gap>%d → reset\n", LOSS_GAP_RESET);
    Serial.println("  VN_AQI: PM2.5 / PM10 / CO (QD 1459/QD-BTNMT)");
    Serial.println("==========================================\n");

    g_packetQueue = xQueueCreate(QUEUE_DEPTH, sizeof(LoraPacket));
    g_mqttMutex   = xSemaphoreCreateMutex();
    g_dispMutex   = xSemaphoreCreateMutex();
    configASSERT(g_packetQueue);
    configASSERT(g_mqttMutex);
    configASSERT(g_dispMutex);

    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(C_BG);
    tft.setTextColor(C_WHITE);
    tft.setTextSize(1);
    tft.setCursor(4, 4);
    tft.print("Khoi dong...");
    Serial.println("[TFT] ILI9341 OK");

    loraInit();
    loraConfig();
    connectWiFi(); 

    // Trong setup() sau connectWiFi():
    WiFiClient testClient;
    Serial.println("[TCP] Testing port 1883...");
    if (testClient.connect(MQTT_HOST, MQTT_PORT)) {
        Serial.println("[TCP] Port 1883 OPEN ✅");
        testClient.stop();
    } else {
        Serial.println("[TCP] Port 1883 CLOSED ❌");
    }

    
    if (xSemaphoreTake(g_mqttMutex, portMAX_DELAY) == pdTRUE) {
        connectMQTT_locked();
        xSemaphoreGive(g_mqttMutex);
    }




    Serial.println("Tao cac FreeRTOS tasks...");

    // stack 6144 bytes cho taskDisplay (2×float[60] local + snap arrays)
    xTaskCreatePinnedToCore(taskDisplay,      "Display",  6144, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(taskWifiWatchdog, "WiFiWD",   3072, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(taskMqttPublish,  "MqttPub",  6144, nullptr, 3, nullptr, 0);
    xTaskCreatePinnedToCore(taskMqttLoop,     "MqttLoop", 3072, nullptr, 4, nullptr, 0);
    xTaskCreatePinnedToCore(taskLoraRx,       "LoraRx",   4096, nullptr, 5, nullptr, 1);

    Serial.println("--- GATEWAY READY ---\n");
}

// =====================================================================
//  LOOP — không dùng
// =====================================================================
void loop() {
    vTaskDelay(pdMS_TO_TICKS(10000));
}
