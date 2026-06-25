/*
 * sender_freertos.ino — ESP32 Sensor Node + LoRa Transmitter (FreeRTOS)
 *
 * Phần cứng:
 *   SHT30        → I2C  SDA=21 SCL=22
 *   LCD 20×4 I2C → I2C  0x27
 *   MQ7          → ADC  GPIO34
 *   PMS7003      → UART2 RX=16 TX=17  (SET=33)
 *   LoRa AS32    → UART1 RX=25 TX=26  (M0=4 M1=5)
 *   Pin 2S1P     → ADC  GPIO15  (phân áp R1=100kΩ trên, R2=47kΩ dưới)
 *
 * FreeRTOS Tasks & Priorities:
 *   taskLCD      — P1, Core 0 — cập nhật màn hình mỗi 1s
 *   taskSHT30    — P2, Core 0 — đọc temp/hum mỗi 5s
 *   taskMQ7      — P2, Core 0 — đọc CO mỗi 5s (bù T/H từ SHT30)
 *   taskPMS      — P3, Core 1 — warmup 30s + đọc PM mỗi 5s
 *   taskLoraSend — P4, Core 1 — phát LoRa mỗi 5s
 *   taskBattery  — P1, Core 0 — đọc điện áp pin mỗi 30s
 *
 * Định dạng gói tin LoRa:
 *   $<NodeID>,<Seq>,<Temp>,<Hum>,<PM1>,<PM2.5>,<PM10>,<CO_corr>,<BatV>
 *   Ví dụ: $1,042,28.5,75.3,8,22,35,0.124,7.85
 *
 * LCD 20×4 layout:
 *   Dòng 0: T:28.5C  Hum:75.3%
 *   Dòng 1: CO:0.1p  AQI: 85     ← CO + VN_AQI (khi PMS sẵn sàng)
 *   Dòng 2: PM2.5:22   PM10:35
 *   Dòng 3: PM1:8   N:1 B: 85%
 *
 * VN_AQI (Quyết định 1459/QĐ-BTNMT):
 *   0–50   Tốt        51–100 Trung bình
 *  101–150 Kém       151–200 Xấu
 *  201–300 Rất xấu   301–500 Nguy hại
 *
 * Thư viện cần cài:
 *   - LiquidCrystal_I2C (by Frank de Brabander)
 *   - Adafruit SHT31 Library
 *   - Adafruit BusIO
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_SHT31.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ─── Node ────────────────────────────────────────────────────────────
#define NODE_ID          1

// ─── LCD 20×4 ────────────────────────────────────────────────────────
#define LCD_ADDR         0x27
#define LCD_COLS         20
#define LCD_ROWS         4

// ─── SHT30 ───────────────────────────────────────────────────────────
#define SHT30_ADDR       0x44

// ─── MQ7 ─────────────────────────────────────────────────────────────
#define MQ7_PIN          34
#define MQ7_VCC          3.3f
#define MQ7_ADC_MAX      4095
#define MQ7_A            99.042f
#define MQ7_B           -1.519f
#define MQ7_RL           10000.0f
#define MQ7_R0_DEFAULT  2971.5f

#define MQ7_REF_TEMP     20.0f
#define MQ7_REF_HUM      65.0f
#define MQ7_CALIB_SAMPLES  50

// ─── PMS7003 ─────────────────────────────────────────────────────────
#define PMS_RX_PIN       16
#define PMS_TX_PIN       17
#define PMS_BAUD         9600
#define PMS_SET_PIN      33
#define PMS_WARMUP_MS    30000

// ─── Battery ADC ─────────────────────────────────────────────────────
#define BATTERY_PIN      15
#define BAT_R1           100000.0f   // điện trở trên (Ohm)
#define BAT_R2            47000.0f   // điện trở dưới (Ohm)
#define BAT_CELLS         2          // số cell 18650
#define BAT_CELL_MAX      4.2f       // điện áp đầy mỗi cell (V)
#define BAT_CELL_MIN      3.0f       // điện áp cắt mỗi cell (V)
#define BATTERY_INTERVAL_MS  30000
#define BAT_LOW_CUTOFF_V   6.2f   // ngưỡng ngắt
#define BAT_WARN_V         6.6f   // ngưỡng cảnh báo
#define BAT_SLEEP_SEC      300    // ngủ 5 phút

// ─── LoRa AS32 ───────────────────────────────────────────────────────
#define LORA_ADDR_HIGH   0xBE
#define LORA_ADDR_LOW    0xEF
#define LORA_CHAN         0x18
#define LORA_RX_PIN      25
#define LORA_TX_PIN      26
#define LORA_M0_PIN       4
#define LORA_M1_PIN       5

// ─── Chu kỳ ──────────────────────────────────────────────────────────
#define SENSOR_INTERVAL_MS  5000
#define LCD_INTERVAL_MS     1000

// ─── AQIBreakpoint — khai báo sớm để tránh lỗi prototype Arduino IDE ─
struct AQIBreakpoint { float cLo, cHi; int iLo, iHi; };

// ─── UART ────────────────────────────────────────────────────────────
HardwareSerial pmsSerial(2);
HardwareSerial loraSerial(1);

// ─── Đối tượng I2C ───────────────────────────────────────────────────
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
Adafruit_SHT31    sht30;

// ─── Dữ liệu chia sẻ ─────────────────────────────────────────────────
struct SensorData {
    float    temp;
    float    hum;
    float    co;
    float    co_raw;
    uint16_t pm1;
    uint16_t pm25;
    uint16_t pm10;
    float    batVoltage;
    uint8_t  batPercent;
    bool     sht30Valid;
    bool     pmsValid;
};

SensorData        g_data            = {};
volatile float    g_mq7_R0          = MQ7_R0_DEFAULT;
volatile bool     g_mq7Calibrated   = false;
volatile bool     g_pmsReady        = false;
volatile uint32_t g_pmsWarmupRemSec = PMS_WARMUP_MS / 1000;
volatile uint16_t g_seqNum          = 0;

// ─── Mutex ───────────────────────────────────────────────────────────
SemaphoreHandle_t g_dataMutex = nullptr;
SemaphoreHandle_t g_i2cMutex  = nullptr;

// ─── Struct PMS7003 ──────────────────────────────────────────────────
struct PMS7003Data {
    uint16_t pm1_0;
    uint16_t pm2_5;
    uint16_t pm10;
    bool     valid;
};

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
// CO theo QCVN 06:2009/BTNMT (mg/m³)
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

// Nhãn trạng thái AQI (tối đa 8 ký tự cho LCD)
static const char *aqiLabelLCD(int aqi) {
    if (aqi <=  50) return "Tot    ";
    if (aqi <= 100) return "TB     ";
    if (aqi <= 150) return "Kem    ";
    if (aqi <= 200) return "Xau    ";
    if (aqi <= 300) return "R.Xau  ";
    return "NguyH  ";
}

// =====================================================================
//  MQ7 — Hệ số bù nhiệt độ / độ ẩm
// =====================================================================
static float getCorrectionFactor(float t, float h) {
    float corr = 0.9920f * (-0.0150f * t - 0.0100f * h + 1.7000f);
    if (corr < 0.1f) corr = 0.1f;
    return corr;
}

static float mq7Voltage() {
    int32_t sum = 0;
    for (int i = 0; i < 8; i++) {
        sum += analogRead(MQ7_PIN);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return (float)(sum / 8) * MQ7_VCC / MQ7_ADC_MAX;
}

static float mq7GetRs(float v) {
    if (v < 0.01f) return 999999.0f;
    return MQ7_RL * (MQ7_VCC - v) / v;
}

static float mq7GetPPM_raw(float rs) {
    float rs_scaled = rs * (10000.0f / MQ7_RL);
    float ratio = rs_scaled / g_mq7_R0;
    if (ratio <= 0.0f) return 0.0f;
    return max(0.0f, MQ7_A * powf(ratio, MQ7_B));
}

static float mq7GetPPM_corrected(float rs, float temp, float hum) {
    float k       = getCorrectionFactor(temp, hum);
    float rs_norm = rs / k;
    float ratio   = rs_norm / g_mq7_R0;
    if (ratio <= 0.0f) return 0.0f;
    return max(0.0f, MQ7_A * powf(ratio, MQ7_B));
}

// =====================================================================
//  Battery — Đọc điện áp pin 2 cell 18650 qua mạch phân áp
//
//  Vin = Vout × (R1 + R2) / R2 = Vout × 147/47 ≈ Vout × 3.128
//  ADC 12-bit, Vref = 3.3V → Vout = adc_raw × 3.3 / 4095
//  Lấy trung bình 16 mẫu để giảm nhiễu ADC
// =====================================================================
static float readBatteryVoltage() {
    int32_t sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += analogRead(BATTERY_PIN);
    }
    float vout = (float)(sum / 16) * 3.3f / 4095.0f;
    return vout * (BAT_R1 + BAT_R2) / BAT_R2;
}

static uint8_t batVoltageToPercent(float vbat) {
    float vmax = BAT_CELL_MAX * BAT_CELLS;
    float vmin = BAT_CELL_MIN * BAT_CELLS;
    if (vbat >= vmax) return 100;
    if (vbat <= vmin) return 0;
    return (uint8_t)((vbat - vmin) / (vmax - vmin) * 100.0f + 0.5f);
}

// =====================================================================
//  TASK: taskBattery — Priority 1, Core 0
// =====================================================================
static void taskBattery(void *pvParameters) {
    while (true) {
        float   vbat    = readBatteryVoltage();
        uint8_t percent = batVoltageToPercent(vbat);

        if (xSemaphoreTake(g_dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_data.batVoltage = vbat;
            g_data.batPercent = percent;
            xSemaphoreGive(g_dataMutex);
        }

        Serial.printf("[BAT]  Vbat=%.2fV  %d%%\n", vbat, percent);
        
        // Cảnh báo sớm
        if (vbat < BAT_WARN_V && vbat >= BAT_LOW_CUTOFF_V) {
            Serial.printf("[BAT]  CANH BAO pin thap: %.2fV\n", vbat);
            char warn[64];
            snprintf(warn, sizeof(warn), "!BATWARN,%d,%.2f,%d\n",
                    NODE_ID, vbat, percent);
            loraSerial.print(warn);
        }

        // Bảo vệ pin
        if (vbat < BAT_LOW_CUTOFF_V) {
            Serial.printf("[BAT]  PIN QUA THAP %.2fV — Tat tai va Deep Sleep!\n", vbat);

            // 1. Tắt PMS7003
            digitalWrite(PMS_SET_PIN, LOW);

            // 2. Tắt LCD
            if (xSemaphoreTake(g_i2cMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                lcdLine(0, "!! PIN QUA THAP !!  ");
                lcdLine(1, "He thong tam dung   ");
                char buf[LCD_COLS + 1];
                snprintf(buf, sizeof(buf), "Vbat=%.2fV  %d%%", vbat, percent);
                lcdLine(2, buf);
                lcdLine(3, "Cho sac lai pin...  ");
                delay(3000);
                lcd.noBacklight();
                lcd.noDisplay();
                xSemaphoreGive(g_i2cMutex);
            }

            // 3. Gửi cảnh báo LoRa trước khi ngủ
            char warn[64];
            snprintf(warn, sizeof(warn), "!LOWBAT,%.2f,%d\n", vbat, percent);
            loraSerial.print(warn);
            delay(500);

            // 4. ESP32 Deep Sleep
            esp_sleep_enable_timer_wakeup((uint64_t)BAT_SLEEP_SEC * 1000000ULL);
            esp_deep_sleep_start();
        }

        vTaskDelay(pdMS_TO_TICKS(BATTERY_INTERVAL_MS));
    }
}

// =====================================================================
//  MQ7 — Calibrate R0
// =====================================================================
static void calibrateMQ7() {
    Serial.println("\n[MQ7-CAL] ====== BAT DAU CALIBRATE R0 ======");

    float temp = sht30.readTemperature();
    float hum  = sht30.readHumidity();

    if (isnan(temp) || isnan(hum)) {
        Serial.println("[MQ7-CAL] CANH BAO: SHT30 loi! Dung gia tri mac dinh 25°C/70%RH");
        temp = 25.0f;
        hum  = 70.0f;
    }

    Serial.printf("[MQ7-CAL] Moi truong: T=%.1f°C  H=%.1f%%RH\n", temp, hum);

    float sum    = 0;
    float rs_min = 1e9f, rs_max = 0.0f;

    for (int i = 0; i < MQ7_CALIB_SAMPLES; i++) {
        float v       = mq7Voltage();
        float rs      = mq7GetRs(v);
        float k       = getCorrectionFactor(temp, hum);
        float rs_norm = rs / k;

        sum    += rs_norm;
        rs_min  = min(rs_min, rs_norm);
        rs_max  = max(rs_max, rs_norm);

        if ((i + 1) % 10 == 0) {
            Serial.printf("[MQ7-CAL] Mau %2d/%d: Rs_norm=%.0f Ohm\n",
                          i + 1, MQ7_CALIB_SAMPLES, rs_norm);
        }
        delay(200);
    }

    float R0_new = sum / (float)MQ7_CALIB_SAMPLES;

    if (R0_new < 1000.0f || R0_new > 500000.0f) {
        Serial.printf("[MQ7-CAL] CANH BAO: R0=%.0f Ohm bat thuong! Giu mac dinh.\n", R0_new);
        g_mq7Calibrated = false;
        return;
    }

    g_mq7_R0        = R0_new;
    g_mq7Calibrated = true;

    Serial.printf("[MQ7-CAL] R0 = %.2f Ohm  (min=%.0f max=%.0f)\n",
                  g_mq7_R0, rs_min, rs_max);
    Serial.printf("[MQ7-CAL] → #define MQ7_R0_DEFAULT  %.1ff\n", g_mq7_R0);
}

// =====================================================================
//  PMS7003 — đọc frame 32 byte
// =====================================================================
static PMS7003Data readPMS7003() {
    PMS7003Data d = {0, 0, 0, false};
    while (pmsSerial.available()) pmsSerial.read();

    uint8_t    buf[32];
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(3000);

    while (xTaskGetTickCount() < deadline) {
        if (!pmsSerial.available()) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (pmsSerial.read() != 0x42) continue;

        TickType_t t = xTaskGetTickCount() + pdMS_TO_TICKS(100);
        while (!pmsSerial.available() && xTaskGetTickCount() < t)
            vTaskDelay(pdMS_TO_TICKS(1));
        if (!pmsSerial.available() || pmsSerial.read() != 0x4D) continue;

        t = xTaskGetTickCount() + pdMS_TO_TICKS(200);
        while (pmsSerial.available() < 30 && xTaskGetTickCount() < t)
            vTaskDelay(pdMS_TO_TICKS(5));
        if (pmsSerial.available() < 30) continue;

        pmsSerial.readBytes(buf + 2, 30);
        buf[0] = 0x42;
        buf[1] = 0x4D;

        uint16_t sum  = 0;
        for (int i = 0; i < 30; i++) sum += buf[i];
        uint16_t recv = ((uint16_t)buf[30] << 8) | buf[31];
        if (sum != recv) {
            Serial.println("[PMS] Checksum loi");
            continue;
        }

        d.pm1_0 = ((uint16_t)buf[10] << 8) | buf[11];
        d.pm2_5 = ((uint16_t)buf[12] << 8) | buf[13];
        d.pm10  = ((uint16_t)buf[14] << 8) | buf[15];
        d.valid = true;
        return d;
    }
    return d;
}

// =====================================================================
//  LCD helpers
// =====================================================================
static void lcdLine(uint8_t row, const char *text) {
    char buf[LCD_COLS + 1];
    snprintf(buf, sizeof(buf), "%-*s", LCD_COLS, text);
    lcd.setCursor(0, row);
    lcd.print(buf);
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
    Serial.println("[LoRa] Normal mode. San sang phat.");
}

// =====================================================================
//  TASK: taskSHT30 — Priority 2, Core 0
// =====================================================================
static void taskSHT30(void *pvParameters) {
    bool sht30_ok = sht30.begin(SHT30_ADDR);
    Serial.printf("[SHT30] %s\n", sht30_ok ? "OK" : "FAILED");

    while (true) {
        float tempSum = 0, humSum = 0;
        int   count   = 0;

        for (int i = 0; i < 5; i++) {
            float t = NAN, h = NAN;
            if (xSemaphoreTake(g_i2cMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                if (sht30_ok) {
                    t = sht30.readTemperature();
                    h = sht30.readHumidity();
                }
                if (isnan(t) || isnan(h)) {
                    sht30_ok = sht30.begin(SHT30_ADDR);
                }
                xSemaphoreGive(g_i2cMutex);
            }
            if (!isnan(t) && !isnan(h)) {
                tempSum += t;
                humSum  += h;
                count++;
            }
            if (i < 4) vTaskDelay(pdMS_TO_TICKS(50));
        }

        float temp = (count > 0) ? tempSum / count : 0.0f;
        float hum  = (count > 0) ? humSum  / count : 0.0f;

        if (xSemaphoreTake(g_dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_data.temp       = temp;
            g_data.hum        = hum;
            g_data.sht30Valid = sht30_ok && (count > 0);
            xSemaphoreGive(g_dataMutex);
        }

        Serial.printf("[SHT30] T=%.1f°C  H=%.1f%%  (avg %d/5)\n", temp, hum, count);
        vTaskDelay(pdMS_TO_TICKS(SENSOR_INTERVAL_MS));
    }
}

// =====================================================================
//  TASK: taskMQ7 — Priority 2, Core 0
// =====================================================================
static void taskMQ7(void *pvParameters) {
    while (true) {
        float v  = mq7Voltage();
        float rs = mq7GetRs(v);

        float temp = MQ7_REF_TEMP;
        float hum  = MQ7_REF_HUM;
        bool  sht30Valid = false;

        if (xSemaphoreTake(g_dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (g_data.sht30Valid) {
                temp       = g_data.temp;
                hum        = g_data.hum;
                sht30Valid = true;
            }
            xSemaphoreGive(g_dataMutex);
        }

        float ppm_corrected = mq7GetPPM_corrected(rs, temp, hum);
        float ppm_raw       = mq7GetPPM_raw(rs);
        float k             = getCorrectionFactor(temp, hum);

        if (xSemaphoreTake(g_dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_data.co     = ppm_corrected;
            g_data.co_raw = ppm_raw;
            xSemaphoreGive(g_dataMutex);
        }

        Serial.printf("[MQ7]  V=%.3fV  Rs=%.0f Ohm  k=%.4f  "
                      "CO_raw=%.1f ppm  CO_corr=%.1f ppm  [SHT30:%s]\n",
                      v, rs, k, ppm_raw, ppm_corrected,
                      sht30Valid ? "OK" : "FALLBACK");

        vTaskDelay(pdMS_TO_TICKS(SENSOR_INTERVAL_MS));
    }
}

// =====================================================================
//  TASK: taskPMS — Priority 3, Core 1
// =====================================================================
static void taskPMS(void *pvParameters) {
    TickType_t warmupDeadline = xTaskGetTickCount() + pdMS_TO_TICKS(PMS_WARMUP_MS);
    Serial.printf("[PMS] Warmup %ds...\n", PMS_WARMUP_MS / 1000);

    while (xTaskGetTickCount() < warmupDeadline) {
        uint32_t remMs    = (uint32_t)((warmupDeadline - xTaskGetTickCount()) * portTICK_PERIOD_MS);
        g_pmsWarmupRemSec = remMs / 1000;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    g_pmsReady        = true;
    g_pmsWarmupRemSec = 0;
    Serial.println("[PMS] San sang!");

    while (true) {
        Serial.print("[PMS] Doc... ");

        uint32_t   pm1Sum = 0, pm25Sum = 0, pm10Sum = 0;
        int        count  = 0;
        TickType_t startTick = xTaskGetTickCount();

        for (int i = 0; i < 5; i++) {
            PMS7003Data pms = readPMS7003();
            if (pms.valid) {
                pm1Sum  += pms.pm1_0;
                pm25Sum += pms.pm2_5;
                pm10Sum += pms.pm10;
                count++;
            }
        }

        uint16_t pm1  = (count > 0) ? (uint16_t)(pm1Sum  / count) : 0;
        uint16_t pm25 = (count > 0) ? (uint16_t)(pm25Sum / count) : 0;
        uint16_t pm10 = (count > 0) ? (uint16_t)(pm10Sum / count) : 0;

        if (count > 0) {
            Serial.printf("PM1=%d  PM2.5=%d  PM10=%d ug/m3  (avg %d/5)\n",
                          pm1, pm25, pm10, count);
        } else {
            Serial.println("TIMEOUT/LOI");
        }

        if (xSemaphoreTake(g_dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_data.pm1      = pm1;
            g_data.pm25     = pm25;
            g_data.pm10     = pm10;
            g_data.pmsValid = (count > 0);
            xSemaphoreGive(g_dataMutex);
        }

        TickType_t elapsed = xTaskGetTickCount() - startTick;
        TickType_t period  = pdMS_TO_TICKS(SENSOR_INTERVAL_MS);
        if (elapsed < period) vTaskDelay(period - elapsed);
    }
}

// =====================================================================
//  TASK: taskLoraSend — Priority 4, Core 1
// =====================================================================
static void taskLoraSend(void *pvParameters) {
    while (!g_pmsReady) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    while (true) {
        SensorData snap;
        if (xSemaphoreTake(g_dataMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            snap = g_data;
            xSemaphoreGive(g_dataMutex);
        } else {
            vTaskDelay(pdMS_TO_TICKS(SENSOR_INTERVAL_MS));
            continue;
        }

        // Kiểm tra dữ liệu hợp lệ
        bool dataValid = snap.sht30Valid &&
                         snap.pmsValid   &&
                         snap.mq7Valid   &&
                         snap.temp  > -40.f && snap.temp < 85.f &&
                         snap.hum   >   0.f && snap.hum  < 100.f &&
                         snap.batVoltage > 0.f;

        if (!dataValid) {
            Serial.println("[LoRa] Du lieu khong hop le, bo qua!");
            // KHÔNG tăng g_seqNum
            vTaskDelay(pdMS_TO_TICKS(SENSOR_INTERVAL_MS));
            continue;
        }

        char packet[128];
        snprintf(packet, sizeof(packet),
            "$%d,%03u,%.1f,%.1f,%d,%d,%d,%.3f,%.2f\n",
            NODE_ID, g_seqNum,
            snap.temp, snap.hum,
            snap.pm1, snap.pm25, snap.pm10,
            snap.co, snap.batVoltage);

        loraSerial.print(packet);
        Serial.printf("[LoRa] TX → %s", packet);

        g_seqNum = (g_seqNum + 1) % 1000;  // chỉ tăng khi gửi thật
        vTaskDelay(pdMS_TO_TICKS(SENSOR_INTERVAL_MS));
    }
}

// =====================================================================
//  TASK: taskLCD — Priority 1, Core 0
//
//  LCD layout:
//    Dòng 0: T:28.5C  Hum:75.3%
//    Dòng 1: CO:0.1p  AQI: 85     (khi PMS sẵn sàng)
//            CO:0.1 ppm [BU]       (khi PMS warmup)
//    Dòng 2: PM2.5:22   PM10:35
//    Dòng 3: PM1:8    N:1 #042
// =====================================================================
static void taskLCD(void *pvParameters) {
    while (true) {
        SensorData snap;
        bool     pmsReady  = g_pmsReady;
        uint32_t warmupRem = g_pmsWarmupRemSec;
        uint16_t seq       = g_seqNum;

        if (xSemaphoreTake(g_dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            snap = g_data;
            xSemaphoreGive(g_dataMutex);
        }

        if (xSemaphoreTake(g_i2cMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            char buf[LCD_COLS + 1];

            // Dòng 0: Nhiệt độ + Độ ẩm
            snprintf(buf, sizeof(buf), "T:%.1fC  Hum:%.1f%%", snap.temp, snap.hum);
            lcdLine(0, buf);

            // Dòng 1: CO + AQI (khi PMS sẵn sàng) hoặc chỉ CO
            if (pmsReady && snap.pmsValid) {
                int aqi = calcAQI((float)snap.pm25, (float)snap.pm10, snap.co);
                // "CO:0.1p  AQI: 85" → tối đa 17 ký tự, đệm đủ 20
                snprintf(buf, sizeof(buf), "CO:%.1fp  AQI:%3d", snap.co, aqi);
                lcdLine(1, buf);
                Serial.printf("[AQI]  Node=%d  AQI=%d  PM2.5=%d PM10=%d CO=%.3f\n",
                              NODE_ID, aqi, snap.pm25, snap.pm10, snap.co);
            } else {
                snprintf(buf, sizeof(buf), "CO:%.1f ppm [BU]", snap.co);
                lcdLine(1, buf);
            }

            // Dòng 2 + 3: PM / warmup
            if (!pmsReady) {
                snprintf(buf, sizeof(buf), "PMS warmup: %lus", (unsigned long)warmupRem);
                lcdLine(2, buf);
                lcdLine(3, " Cho cam bien on...");
            } else if (snap.pmsValid) {
                snprintf(buf, sizeof(buf), "PM2.5:%-4d PM10:%-4d", snap.pm25, snap.pm10);
                lcdLine(2, buf);
                snprintf(buf, sizeof(buf), "PM1:%-3dN:%d B:%3d%%", snap.pm1, NODE_ID, snap.batPercent);
                lcdLine(3, buf);
            } else {
                lcdLine(2, "PMS7003: NO DATA    ");
                lcdLine(3, "Kiem tra PMS day!   ");
            }

            xSemaphoreGive(g_i2cMutex);
        }

        vTaskDelay(pdMS_TO_TICKS(LCD_INTERVAL_MS));
    }
}

// =====================================================================
//  SETUP
// =====================================================================
void setup() {
    delay(1000);
    Serial.begin(115200);
    delay(500);
    Serial.println("\n====== ESP32 LORA SENDER (FreeRTOS) ======");
    Serial.printf("[Node] ID=%d\n", NODE_ID);
    Serial.println("  VN_AQI: PM2.5 / PM10 / CO (QD 1459/QD-BTNMT)");

    g_dataMutex = xSemaphoreCreateMutex();
    g_i2cMutex  = xSemaphoreCreateMutex();
    configASSERT(g_dataMutex);
    configASSERT(g_i2cMutex);

    pinMode(MQ7_PIN, INPUT);
    pinMode(BATTERY_PIN, INPUT);
    analogReadResolution(12);

    Wire.begin(21, 22);
    Wire.setClock(100000);

    if (!sht30.begin(SHT30_ADDR)) {
        Serial.println("[SHT30] CANH BAO: Khong tim thay SHT30!");
    } else {
        Serial.println("[SHT30] OK");
    }

    // Để bật calibrate thực tế: uncomment calibrateMQ7() và comment dòng kế tiếp
    // calibrateMQ7();
    g_mq7_R0 = MQ7_R0_DEFAULT;

    pinMode(PMS_SET_PIN, OUTPUT);
    digitalWrite(PMS_SET_PIN, HIGH);
    pmsSerial.begin(PMS_BAUD, SERIAL_8N1, PMS_RX_PIN, PMS_TX_PIN);

    loraInit();
    loraConfig();
    
    lcd.init();
    lcd.noBacklight();
    delay(100);
    lcd.backlight();
    lcd.clear();
    delay(50); 
    lcdLine(0, "  ESP32 LORA SENDER ");
    lcdLine(1, " SHT30|MQ7|PMS7003  ");
    if (g_mq7Calibrated) {
        char buf[LCD_COLS + 1];
        snprintf(buf, sizeof(buf), "MQ7 R0=%.0f OK", g_mq7_R0);
        lcdLine(2, buf);
    } else {
        lcdLine(2, "MQ7 R0: DEFAULT     ");
    }
    lcdLine(3, " PMS warmup 30s...  ");

    Serial.println("Tao cac FreeRTOS tasks...");

    xTaskCreatePinnedToCore(taskLCD,      "LCD",      3072, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(taskBattery,  "Battery",  2048, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(taskSHT30,    "SHT30",    3072, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(taskMQ7,      "MQ7",      2560, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(taskPMS,      "PMS",      4096, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(taskLoraSend, "LoraSend", 4096, nullptr, 4, nullptr, 1);

    Serial.println("Setup xong. Cac task dang chay.");
    Serial.printf("[MQ7] R0 = %.2f Ohm (%s)\n",
                  g_mq7_R0,
                  g_mq7Calibrated ? "da calibrate" : "gia tri mac dinh");
    Serial.println("==========================================\n");
}

// =====================================================================
//  LOOP — nhường CPU hoàn toàn cho FreeRTOS scheduler
// =====================================================================
void loop() {
    vTaskDelay(pdMS_TO_TICKS(10000));
}
