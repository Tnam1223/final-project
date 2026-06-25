const express    = require('express');
const { Pool }   = require('pg');
const path       = require('path');
const mqtt       = require('mqtt');
const nodemailer = require('nodemailer');

const app  = express();
const PORT = 3000;

// ─── Cấu hình Email ──────────────────────────────────────────────
// Hướng dẫn Gmail: Bật 2FA → https://myaccount.google.com/apppasswords → Tạo App Password
const EMAIL_CONFIG = {
    enable:      true,                          // Tắt/bật toàn bộ cảnh báo email
    from:        'nam19916@gmail.com',        // ← Email người gửi (Gmail)
    appPassword: 'tqcq xbma vhik qglk',         // ← App Password Gmail (16 ký tự)
    to:          'ptrungnam1702@gmail.com',         // ← Email người nhận cảnh báo
    cooldownMs:  10 * 60 * 1000,               // 10 phút — tránh spam cùng 1 cảnh báo
};

// ─── Ngưỡng nguy hiểm (mức "Kém") ────────────────────────────────
const DANGER_THRESH = {
    pm25: { limit: 55,  unit: 'µg/m³', label: 'PM2.5'     },
    pm10: { limit: 254, unit: 'µg/m³', label: 'PM10'      },
    pm1:  { limit: 55,  unit: 'µg/m³', label: 'PM1.0'     },
    co:   { limit: 15,  unit: 'ppm',   label: 'CO'        },
    temp: { limit: 35,  unit: '°C',    label: 'Nhiệt độ'  },
    hum:  { limit: 80,  unit: '%',     label: 'Độ ẩm'     },
};

// Map cooldown: key = "nodeId_metric", value = timestamp lần gửi cuối
const alertCooldown = new Map();

// ─── Nodemailer transporter ───────────────────────────────────────
const mailer = nodemailer.createTransport({
    service: 'gmail',
    auth: {
        user: EMAIL_CONFIG.from,
        pass: EMAIL_CONFIG.appPassword,
    },
});

// ─── Kiểm tra và gửi cảnh báo ────────────────────────────────────
async function checkAndAlert(data) {
    if (!EMAIL_CONFIG.enable) return;

    const now     = Date.now();
    const nodeId  = data.id;
    const time    = new Date().toLocaleString('vi-VN', { timeZone: 'Asia/Ho_Chi_Minh' });

    // Thu thập tất cả các chỉ số vượt ngưỡng trong lần đọc này
    const violations = [];

    const checks = {
        pm25: data.pm25,
        pm10: data.pm10,
        pm1:  data.pm1,
        co:   data.co,
        temp: data.temperature,
        hum:  data.humidity,
    };

    for (const [metric, value] of Object.entries(checks)) {
        if (value == null) continue;
        const thresh = DANGER_THRESH[metric];
        if (parseFloat(value) > thresh.limit) {
            const cooldownKey = `${nodeId}_${metric}`;
            const lastSent    = alertCooldown.get(cooldownKey) || 0;

            if (now - lastSent >= EMAIL_CONFIG.cooldownMs) {
                violations.push({ metric, value: parseFloat(value), ...thresh });
                alertCooldown.set(cooldownKey, now);
            }
        }
    }

    if (violations.length === 0) return;

    // Tạo HTML email
    const rows = violations.map(v => `
        <tr>
            <td style="padding:8px 12px;border:1px solid #ddd;font-weight:bold">${v.label}</td>
            <td style="padding:8px 12px;border:1px solid #ddd;color:#dc3545;font-weight:bold">
                ${v.value.toFixed(v.unit === 'ppm' ? 2 : 1)} ${v.unit}
            </td>
            <td style="padding:8px 12px;border:1px solid #ddd;color:#666">
                &gt; ${v.limit} ${v.unit}
            </td>
        </tr>`).join('');

    const html = `
    <div style="font-family:Arial,sans-serif;max-width:600px;margin:0 auto">
        <div style="background:#dc3545;color:#fff;padding:20px 24px;border-radius:8px 8px 0 0">
            <h2 style="margin:0">⚠️ Cảnh báo Chất lượng Môi trường</h2>
            <p style="margin:6px 0 0;opacity:.9">Hệ thống giám sát LoRa tự động</p>
        </div>

        <div style="background:#fff8f8;border:1px solid #f5c6cb;padding:20px 24px">
            <p style="margin:0 0 12px">
                <strong>Node:</strong> ${nodeId} &nbsp;|&nbsp;
                <strong>Thời gian:</strong> ${time}
            </p>
            <p style="margin:0 0 16px;color:#721c24">
                Phát hiện <strong>${violations.length}</strong> chỉ số vượt mức nguy hiểm:
            </p>

            <table style="width:100%;border-collapse:collapse;font-size:14px">
                <thead>
                    <tr style="background:#f8d7da">
                        <th style="padding:8px 12px;border:1px solid #ddd;text-align:left">Chỉ số</th>
                        <th style="padding:8px 12px;border:1px solid #ddd;text-align:left">Giá trị đo</th>
                        <th style="padding:8px 12px;border:1px solid #ddd;text-align:left">Ngưỡng an toàn</th>
                    </tr>
                </thead>
                <tbody>${rows}</tbody>
            </table>

            <div style="margin-top:20px;padding:12px;background:#fff3cd;border-left:4px solid #ffc107;border-radius:4px">
                <strong>Khuyến nghị:</strong> Kiểm tra ngay khu vực cảm biến Node ${nodeId}.
                Nếu môi trường trong nhà, hãy mở cửa thông thoáng hoặc sử dụng thiết bị lọc không khí.
            </div>
        </div>

        <div style="background:#f8f9fa;padding:12px 24px;border-radius:0 0 8px 8px;font-size:12px;color:#888">
            Email này được gửi tự động. Cảnh báo sẽ không lặp lại trong 10 phút tiếp theo cho cùng chỉ số.
        </div>
    </div>`;

    const subject = `⚠️ [Node ${nodeId}] ${violations.map(v => v.label).join(', ')} vượt ngưỡng nguy hiểm`;

    try {
        await mailer.sendMail({
            from: `"LoRa Monitor" <${EMAIL_CONFIG.from}>`,
            to:   EMAIL_CONFIG.to,
            subject,
            html,
        });
        console.log(`[ALERT] Email canh bao da gui: Node ${nodeId} — ${violations.map(v => v.label).join(', ')}`);
    } catch (err) {
        console.error('[ALERT] Loi gui email:', err.message);
    }
}

// ─── PostgreSQL ───────────────────────────────────────────────
const pool = new Pool({
    host:     'localhost',
    database: 'lora_monitor',
    user:     'postgres',
    password: '123456',   
    port:     5432,
});

pool.connect()
    .then(() => console.log('[DB] Ket noi PostgreSQL thanh cong'))
    .catch(e => console.error('[DB] Loi ket noi:', e.message));

// ─── SSE clients ─────────────────────────────────────────────
const sseClients = new Set();

// Cache trạng thái mới nhất của mỗi node — gửi lại khi browser kết nối mới
const nodeCache = new Map();  // nodeId → data

function broadcastSSE(data) {
    // Lưu vào cache
    nodeCache.set(data.id, data);

    const payload = `data: ${JSON.stringify(data)}\n\n`;
    sseClients.forEach(client => {
        try { client.write(payload); } catch { sseClients.delete(client); }
    });
}

// ─── Middleware ───────────────────────────────────────────────
app.use(express.json());
app.use((req, res, next) => {
    res.header('Access-Control-Allow-Origin', '*');
    res.header('Access-Control-Allow-Headers', 'Content-Type');
    next();
});

// Phục vụ file tĩnh (index.html, app.js, body.css)
app.use(express.static(path.join(__dirname, '../')));

// ─── MQTT — subscribe nhận dữ liệu từ ESP32 ──────────────────
// Topic: lora/node/<nodeId>  payload JSON: {"id":1,"temperature":...}
const mqttClient = mqtt.connect('mqtt://localhost:1883');

mqttClient.on('connect', () => {
    console.log('[MQTT] Ket noi broker thanh cong');
    mqttClient.subscribe('lora/#', err => {
        if (err) console.error('[MQTT] Loi subscribe:', err.message);
        else     console.log('[MQTT] Da subscribe topic: lora/#');
    });
});

mqttClient.on('error', e => console.error('[MQTT] Loi:', e.message));

mqttClient.on('message', async (topic, message) => {
    // [FIX 1] Trim whitespace/newline từ ESP32 trước khi parse
    const raw = message.toString().trim();
    console.log(`[MQTT RAW] topic="${topic}" payload="${raw}"`);

    // [FIX 2] Tách parse ra khỏi try/catch chung để log rõ lỗi
    let d;
    try {
        d = JSON.parse(raw);
    } catch (e) {
        console.error(`[MQTT] JSON parse THAT BAI | raw="${raw}" | err=${e.message}`);
        return;
    }

    // [FIX 3] Kiểm tra id chặt chẽ hơn — !d.id bắt cả id=0
    if (d.id === undefined || d.id === null || d.id === 0) {
        console.warn(`[MQTT] Thieu truong 'id' hoac id=0, bo qua | payload=${raw}`);
        return;
    }

    // [FIX 4] Ép kiểu số — ESP32 đôi khi serialize ra string
    d.id          = Number(d.id);
    d.temperature = Number(d.temperature ?? 0);
    d.humidity    = Number(d.humidity    ?? 0);
    d.pm1         = Number(d.pm1         ?? 0);
    d.pm25        = Number(d.pm25        ?? 0);
    d.pm10        = Number(d.pm10        ?? 0);
    d.co          = Number(d.co          ?? 0);
    d.aqi         = d.aqi      != null ? Number(d.aqi)      : null;
    d.loss_pct    = d.loss_pct != null ? Number(d.loss_pct) : null;

    console.log(`[MQTT] Node ${d.id} | T=${d.temperature} H=${d.humidity} PM2.5=${d.pm25} CO=${d.co} AQI=${d.aqi ?? '--'}`);

    broadcastSSE(d);
    checkAndAlert(d);   // Kiểm tra ngưỡng nguy hiểm, gửi email nếu cần

    // [FIX 5] Tách try/catch riêng cho DB để log rõ lỗi INSERT
    try {
        await pool.query(`
            INSERT INTO readings (node_id, temperature, humidity, pm1, pm25, pm10, co, aqi, loss_pct)
            VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)
        `, [d.id, d.temperature, d.humidity, d.pm1, d.pm25, d.pm10, d.co,
            d.aqi, d.loss_pct]);
    } catch (e) {
        console.error(`[DB] INSERT THAT BAI | node=${d.id} | err=${e.message}`);
    }
});

// ─── SSE — browser subscribe nhận dữ liệu real-time ─────────
app.get('/events', (req, res) => {
    res.setHeader('Content-Type',  'text/event-stream');
    res.setHeader('Cache-Control', 'no-cache');
    res.setHeader('Connection',    'keep-alive');
    res.flushHeaders();

    sseClients.add(res);
    console.log(`[SSE] Client ket noi — tong: ${sseClients.size}`);

    // Gửi ngay trạng thái mới nhất của tất cả node đã biết
    nodeCache.forEach(data => {
        try { res.write(`data: ${JSON.stringify(data)}\n\n`); } catch {}
    });

    // Heartbeat mỗi 25s để giữ kết nối
    const hb = setInterval(() => {
        try { res.write(': heartbeat\n\n'); } catch { clearInterval(hb); }
    }, 25000);

    req.on('close', () => {
        clearInterval(hb);
        sseClients.delete(res);
        console.log(`[SSE] Client ngat — con lai: ${sseClients.size}`);
    });
});

// ─── POST /data — nhận dữ liệu từ ESP32 receiver ─────────────
app.post('/data', async (req, res) => {
    const d = req.body;

    // Validate
    if (!d.id || d.temperature === undefined) {
        return res.status(400).json({ error: 'Thieu truong bat buoc (id, temperature)' });
    }

    console.log(`[DATA] Node ${d.id} | T=${d.temperature} H=${d.humidity} PM2.5=${d.pm25} CO=${d.co} AQI=${d.aqi ?? '--'}`);

    // Broadcast SSE ngay lập tức
    broadcastSSE(d);

    // Lưu PostgreSQL
    try {
        await pool.query(`
            INSERT INTO readings (node_id, temperature, humidity, pm1, pm25, pm10, co, aqi, loss_pct)
            VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)
        `, [d.id, d.temperature, d.humidity, d.pm1 ?? 0, d.pm25 ?? 0, d.pm10 ?? 0, d.co ?? 0,
            d.aqi ?? null, d.loss_pct ?? null]);
    } catch (e) {
        console.error('[DB] Loi INSERT:', e.message);
    }

    res.json({ ok: true });
});

// ─── GET /api/history — lịch sử theo khoảng ngày ─────────────
app.get('/api/history', async (req, res) => {
    const { start, end, nodeId } = req.query;
    if (!start || !end) return res.status(400).json({ error: 'Thieu start/end' });

    try {
        let q = `
            SELECT time, node_id, temperature, humidity, pm1, pm25, pm10, co, aqi, loss_pct
            FROM readings
            WHERE time >= $1 AND time < ($2::date + INTERVAL '1 day')
        `;
        const params = [start, end];

        if (nodeId) {
            q += ` AND node_id = $3`;
            params.push(parseInt(nodeId));
        }
        q += ` ORDER BY time ASC`;

        // Giới hạn 10.000 điểm để tránh timeout
        q += ` LIMIT 10000`;

        const result = await pool.query(q, params);

        // Nhóm theo node_id: { "1": [...], "2": [...] }
        const grouped = {};
        for (const row of result.rows) {
            const nid = row.node_id.toString();
            if (!grouped[nid]) grouped[nid] = [];
            grouped[nid].push({
                time:        row.time,
                temperature: parseFloat(row.temperature),
                humidity:    parseFloat(row.humidity),
                pm1:         parseFloat(row.pm1),
                pm25:        parseFloat(row.pm25),
                pm10:        parseFloat(row.pm10),
                co:          parseFloat(row.co),
                aqi:         row.aqi != null ? parseInt(row.aqi) : null,
                loss_pct:    row.loss_pct != null ? parseFloat(row.loss_pct) : null,
            });
        }
        res.json(grouped);
    } catch (e) {
        console.error('[DB] Loi history:', e.message);
        res.status(500).json({ error: e.message });
    }
});

// ─── GET /api/stats — thống kê theo ngày ─────────────────────
app.get('/api/stats', async (req, res) => {
    const { start, end, nodeId } = req.query;
    if (!start || !end) return res.status(400).json({ error: 'Thieu start/end' });

    try {
        let q = `
            SELECT
                DATE(time)               AS ngay,
                node_id,
                COUNT(*)                 AS so_ban_ghi,
                ROUND(AVG(temperature)::numeric, 1) AS nhiet_do_tb,
                ROUND(MAX(temperature)::numeric, 1) AS nhiet_do_max,
                ROUND(MIN(temperature)::numeric, 1) AS nhiet_do_min,
                ROUND(AVG(humidity)::numeric,    1) AS do_am_tb,
                ROUND(AVG(pm25)::numeric,        1) AS pm25_tb,
                ROUND(MAX(pm25)::numeric,        1) AS pm25_max,
                ROUND(AVG(co)::numeric,          3) AS co_tb,
                ROUND(AVG(aqi)::numeric,         0) AS aqi_tb,
                MAX(aqi)                            AS aqi_max
            FROM readings
            WHERE time >= $1 AND time < ($2::date + INTERVAL '1 day')
        `;
        const params = [start, end];

        if (nodeId) {
            q += ` AND node_id = $3`;
            params.push(parseInt(nodeId));
        }
        q += ` GROUP BY DATE(time), node_id ORDER BY ngay DESC, node_id`;

        const result = await pool.query(q, params);
        res.json(result.rows);
    } catch (e) {
        console.error('[DB] Loi stats:', e.message);
        res.status(500).json({ error: e.message });
    }
});

// ─── GET /api/export — xuất CSV ──────────────────────────────
app.get('/api/export', async (req, res) => {
    const { start, end, nodeId } = req.query;
    if (!start || !end) return res.status(400).json({ error: 'Thieu start/end' });

    try {
        let q = `
            SELECT time, node_id, temperature, humidity, pm1, pm25, pm10, co, aqi, loss_pct
            FROM readings
            WHERE time >= $1 AND time < ($2::date + INTERVAL '1 day')
        `;
        const params = [start, end];

        if (nodeId) {
            q += ` AND node_id = $3`;
            params.push(parseInt(nodeId));
        }
        q += ` ORDER BY time ASC LIMIT 50000`;

        const result = await pool.query(q, params);

        const filename = `lora_data_${start}_${end}${nodeId ? '_node' + nodeId : ''}.csv`;
        res.setHeader('Content-Type',        'text/csv; charset=utf-8');
        res.setHeader('Content-Disposition', `attachment; filename="${filename}"`);

        res.write('﻿'); // BOM cho Excel đọc được tiếng Việt
        res.write('Thoi gian,Node,Nhiet do (C),Do am (%),PM1.0 (ug/m3),PM2.5 (ug/m3),PM10 (ug/m3),CO (ppm),AQI,Loss (%)\n');

        for (const row of result.rows) {
            const time = new Date(row.time).toLocaleString('vi-VN');
            res.write(`${time},${row.node_id},${row.temperature},${row.humidity},${row.pm1},${row.pm25},${row.pm10},${row.co},${row.aqi ?? ''},${row.loss_pct ?? ''}\n`);
        }
        res.end();
    } catch (e) {
        console.error('[DB] Loi export:', e.message);
        res.status(500).json({ error: e.message });
    }
});

// ─── Start ────────────────────────────────────────────────────
app.listen(PORT, '0.0.0.0', () => {
    console.log(`\n=============================`);
    console.log(` Server chay tren port ${PORT}`);
    console.log(` Mo trinh duyet: http://localhost:${PORT}`);
    console.log(`=============================\n`);
});