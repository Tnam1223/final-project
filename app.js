// ======================================================
// PHẦN 1: CẤU HÌNH & KẾT NỐI SSE (Thời gian thực)
// Server push dữ liệu MQTT → SSE → Browser
// ======================================================

const API_URL         = `${window.location.protocol}//${window.location.host}`;
const MAX_DATA_POINTS = 60;   // Số điểm tối đa trên biểu đồ real-time

// Bảng màu cho tối đa 8 nodes — mỗi node một màu riêng
const NODE_COLORS = [
    { border: 'rgb(40, 167, 69)',   bg: 'rgba(40, 167, 69, 0.15)'   }, // Xanh lá  - Node 1
    { border: 'rgb(0, 123, 255)',   bg: 'rgba(0, 123, 255, 0.15)'   }, // Xanh lam - Node 2
    { border: 'rgb(220, 53, 69)',   bg: 'rgba(220, 53, 69, 0.15)'   }, // Đỏ       - Node 3
    { border: 'rgb(255, 193, 7)',   bg: 'rgba(255, 193, 7, 0.15)'   }, // Vàng     - Node 4
    { border: 'rgb(23, 162, 184)',  bg: 'rgba(23, 162, 184, 0.15)'  }, // Cyan     - Node 5
    { border: 'rgb(111, 66, 193)',  bg: 'rgba(111, 66, 193, 0.15)'  }, // Tím      - Node 6
    { border: 'rgb(253, 126, 20)',  bg: 'rgba(253, 126, 20, 0.15)'  }, // Cam      - Node 7
    { border: 'rgb(32, 201, 151)',  bg: 'rgba(32, 201, 151, 0.15)'  }, // Teal     - Node 8
];

// Buffer dữ liệu real-time cho từng node
const realtimeBuf = {};  // { nodeId: { labels, aqi, pm25, co } }
const knownNodes  = new Set();

// ─── VN_AQI — Mức độ và màu sắc (Quyết định 1459/QĐ-BTNMT) ──────────
const AQI_LEVELS = [
    { max: 50,  label: 'Tốt',        cls: 'aqi-lv0', chartBorder: 'rgb(40,167,69)',  chartBg: 'rgba(40,167,69,0.15)'  },
    { max: 100, label: 'Trung bình', cls: 'aqi-lv1', chartBorder: 'rgb(212,175,0)',  chartBg: 'rgba(212,175,0,0.15)'  },
    { max: 150, label: 'Kém',        cls: 'aqi-lv2', chartBorder: 'rgb(253,126,20)', chartBg: 'rgba(253,126,20,0.15)' },
    { max: 200, label: 'Xấu',        cls: 'aqi-lv3', chartBorder: 'rgb(220,53,69)',  chartBg: 'rgba(220,53,69,0.15)'  },
    { max: 300, label: 'Rất xấu',   cls: 'aqi-lv4', chartBorder: 'rgb(111,66,193)', chartBg: 'rgba(111,66,193,0.15)' },
    { max: 500, label: 'Nguy hại',  cls: 'aqi-lv5', chartBorder: 'rgb(127,0,0)',    chartBg: 'rgba(127,0,0,0.15)'    },
];

function getAQILevel(aqi) {
    for (const lv of AQI_LEVELS) {
        if (aqi <= lv.max) return lv;
    }
    return AQI_LEVELS[AQI_LEVELS.length - 1];
}

// ─── Kết nối SSE để nhận dữ liệu từ MQTT qua server ──────────────
let eventSource = null;

function connectSSE() {
    const sseURL = `${API_URL}/events`;
    updateStatus(`Đang kết nối: ${sseURL}`, 'loading');

    eventSource = new EventSource(sseURL);

    eventSource.onopen = () => {
        updateStatus('✓ Kết nối thành công — Đang nhận dữ liệu MQTT từ các node', 'success');
    };

    eventSource.onmessage = (event) => {
        try {
            const data = JSON.parse(event.data);
            onNodeDataReceived(data);
        } catch (e) {
            console.error('Lỗi parse JSON:', e, event.data);
        }
    };

    eventSource.onerror = (err) => {
        const state = ['CONNECTING','OPEN','CLOSED'][eventSource.readyState] || eventSource.readyState;
        updateStatus(`✗ Lỗi SSE (${state}) → URL: ${sseURL} — Thử lại sau 5s`, 'error');
        eventSource.close();
        setTimeout(connectSSE, 5000);
    };
}

// ─── Xử lý khi nhận dữ liệu node mới ─────────────────────────────
function onNodeDataReceived(data) {
    const nodeId = data.id;
    if (!nodeId) return;

    // Node mới → tạo card, thêm vào biểu đồ và dropdown
    if (!knownNodes.has(nodeId)) {
        knownNodes.add(nodeId);
        createNodeCard(nodeId);
        addNodeToCharts(nodeId);
        addNodeToHistorySelect(nodeId);
    }

    updateNodeCard(nodeId, data);
    updateRealtimeCharts(nodeId, data);
}

// ======================================================
// PHẦN 2: NODE CARDS — Thẻ hiển thị dữ liệu từng node
// ======================================================

// ─── Bảng ngưỡng chất lượng (theo quy chuẩn Việt Nam) ────────
const LEVELS = [
    { label: 'Tốt',                cls: 'lv-good',      textCls: 'lv-text-good'      },
    { label: 'Vừa phải',           cls: 'lv-moderate',  textCls: 'lv-text-moderate'  },
    { label: 'Có hại cho sức khỏe',cls: 'lv-poor',      textCls: 'lv-text-poor'      },
    { label: 'Kém',                cls: 'lv-bad',       textCls: 'lv-text-bad'       },
];

function getLevel(value, thresholds) {
    // thresholds: [good_max, moderate_max, poor_max] — vượt poor_max → kém
    if (value <= thresholds[0]) return LEVELS[0];
    if (value <= thresholds[1]) return LEVELS[1];
    if (value <= thresholds[2]) return LEVELS[2];
    return LEVELS[3];
}

// Ngưỡng từng chỉ số
const THRESH = {
    pm25: [12, 35, 55],         // µg/m³
    pm10: [54, 154, 254],       // µg/m³
    pm1:  [12, 35, 55],         // µg/m³ (dùng cùng ngưỡng PM2.5)
    co:   [4,  9,  15],         // ppm
};

// showBadge = false → ẩn nhãn mức độ (dùng cho nhiệt độ & độ ẩm)
function metricCard(id, icon, label, unit, levelCls, levelLabel, value, showBadge = true) {
    return `
    <div class="metric-card ${levelCls}" id="mc-${id}">
        <div class="mc-icon">${icon}</div>
        <div class="mc-label">${label}</div>
        <div class="mc-value"><span id="${id}">${value}</span> <small>${unit}</small></div>
        ${showBadge ? `<div class="mc-badge">${levelLabel}</div>` : '<div class="mc-badge-empty"></div>'}
    </div>`;
}

function createNodeCard(nodeId) {
    const container = document.getElementById('nodeContainer');
    const placeholder = document.getElementById('noDataMsg');
    if (placeholder) placeholder.remove();

    const c = NODE_COLORS[(nodeId - 1) % NODE_COLORS.length];

    const card = document.createElement('div');
    card.className = 'card';
    card.id = `card-node-${nodeId}`;
    card.style.borderTop = `4px solid ${c.border}`;

    card.innerHTML = `
        <h2 style="color:${c.border}">📡 Node ${nodeId}</h2>

        <!-- ── Banner AQI nổi bật ── -->
        <div class="aqi-banner aqi-lv0" id="aqi-banner-${nodeId}">
            <div class="aqi-number" id="aqi-${nodeId}">--</div>
            <div class="aqi-desc">
                <div class="aqi-title">VN_AQI</div>
                <div class="aqi-status" id="aqi-label-${nodeId}">Chờ dữ liệu...</div>
            </div>
            <div class="aqi-thang">
                <span class="aqi-dot aqi-lv0" title="Tốt (0–50)">●</span>
                <span class="aqi-dot aqi-lv1" title="Trung bình (51–100)">●</span>
                <span class="aqi-dot aqi-lv2" title="Kém (101–150)">●</span>
                <span class="aqi-dot aqi-lv3" title="Xấu (151–200)">●</span>
                <span class="aqi-dot aqi-lv4" title="Rất xấu (201–300)">●</span>
                <span class="aqi-dot aqi-lv5" title="Nguy hại (301–500)">●</span>
            </div>
        </div>

        <!-- ── Lưới chỉ số ── -->
        <div class="metric-grid">
            ${metricCard(`temp-${nodeId}`, '🌡️', 'Nhiệt độ',  '°C',    'lv-good', 'Tốt', '--', false)}
            ${metricCard(`hum-${nodeId}`,  '💧', 'Độ ẩm',     '%',     'lv-good', 'Tốt', '--', false)}
            ${metricCard(`co-${nodeId}`,   '💨', 'CO',         'ppm',   'lv-good', 'Tốt', '--')}
            ${metricCard(`pm1-${nodeId}`,  '🌫️', 'PM1.0',     'µg/m³', 'lv-good', 'Tốt', '--')}
            ${metricCard(`pm25-${nodeId}`, '🌫️', 'PM2.5',     'µg/m³', 'lv-good', 'Tốt', '--')}
            ${metricCard(`pm10-${nodeId}`, '🌫️', 'PM10',      'µg/m³', 'lv-good', 'Tốt', '--')}
        </div>
        <div class="card-footer">
            <span class="last-seen" id="age-${nodeId}"></span>
        </div>
    `;
    container.appendChild(card);
}

function updateMetricCard(spanId, value, thresh, decimals) {
    const el  = document.getElementById(spanId);
    const mc  = document.getElementById(`mc-${spanId}`);
    if (!el || !mc) return;

    const n = parseFloat(value);
    el.textContent = isNaN(n) ? '--' : n.toFixed(decimals);

    if (!isNaN(n)) {
        const lv = getLevel(n, thresh);
        mc.classList.remove('lv-good', 'lv-moderate', 'lv-poor', 'lv-bad');
        mc.classList.add(lv.cls);
        const badge = mc.querySelector('.mc-badge');
        if (badge) badge.textContent = lv.label;
    }
}

// Chỉ cập nhật giá trị, không đổi màu — dùng cho nhiệt độ & độ ẩm
function updateMetricValue(spanId, value, decimals) {
    const el = document.getElementById(spanId);
    if (!el) return;
    const n = parseFloat(value);
    el.textContent = isNaN(n) ? '--' : n.toFixed(decimals);
}

function updateAQIBanner(nodeId, aqiValue) {
    const aqi    = parseInt(aqiValue);
    const banner = document.getElementById(`aqi-banner-${nodeId}`);
    const numEl  = document.getElementById(`aqi-${nodeId}`);
    const lblEl  = document.getElementById(`aqi-label-${nodeId}`);
    if (!banner || !numEl || !lblEl) return;

    if (isNaN(aqi)) {
        numEl.textContent = '--';
        lblEl.textContent = 'Chờ dữ liệu...';
        return;
    }

    const lv = getAQILevel(aqi);
    numEl.textContent = aqi;
    lblEl.textContent = lv.label;

    banner.classList.remove('aqi-lv0','aqi-lv1','aqi-lv2','aqi-lv3','aqi-lv4','aqi-lv5');
    banner.classList.add(lv.cls);

    const dots = banner.querySelectorAll('.aqi-dot');
    dots.forEach((dot, i) => {
        dot.classList.toggle('aqi-dot-active', AQI_LEVELS[i].cls === lv.cls);
    });
}

function updateNodeCard(nodeId, data) {
    updateAQIBanner(nodeId, data.aqi);
    updateMetricValue(`temp-${nodeId}`, data.temperature, 1);
    updateMetricValue(`hum-${nodeId}`,  data.humidity,    1);
    updateMetricCard(`co-${nodeId}`,   data.co,          THRESH.co,   1);
    updateMetricCard(`pm1-${nodeId}`,  data.pm1,         THRESH.pm1,  0);
    updateMetricCard(`pm25-${nodeId}`, data.pm25,        THRESH.pm25, 0);
    updateMetricCard(`pm10-${nodeId}`, data.pm10,        THRESH.pm10, 0);
    setEl(`age-${nodeId}`, 'Cập nhật: ' + new Date().toLocaleTimeString('vi-VN'));
}

// ─── Tiện ích ──────────────────────────────────────────────────────
function setEl(id, value) {
    const el = document.getElementById(id);
    if (el) el.innerText = value;
}

function fmt(val, decimals) {
    const n = parseFloat(val);
    return isNaN(n) ? '--' : n.toFixed(decimals);
}

function updateStatus(message, type) {
    const el = document.getElementById('status');
    if (!el) return;
    el.innerText  = message;
    el.className  = type ? `status-${type}` : '';
}

// ======================================================
// PHẦN 3: BIỂU ĐỒ THỜI GIAN THỰC
// ======================================================

// Per-node chart instances: { nodeId: { aqi, pm25, co } }
const nodeCharts = {};

function initRealtimeCharts() { /* charts created dynamically per node in addNodeToCharts */ }

function addNodeToCharts(nodeId) {
    const c       = NODE_COLORS[(nodeId - 1) % NODE_COLORS.length];
    const wrapper = document.getElementById('realtimeChartsWrapper');

    const placeholder = wrapper.querySelector('.no-chart-data');
    if (placeholder) placeholder.remove();

    const col = document.createElement('div');
    col.className    = 'node-charts-col';
    col.id           = `node-charts-col-${nodeId}`;
    col.style.borderTopColor = c.border;

    col.innerHTML = `
        <div class="node-charts-col-title" style="color:${c.border}">📡 Node ${nodeId}</div>
        <div class="node-chart-wrap"><canvas id="aqi-rt-${nodeId}"></canvas></div>
        <div class="node-chart-wrap"><canvas id="pm25-rt-${nodeId}"></canvas></div>
        <div class="node-chart-wrap"><canvas id="co-rt-${nodeId}"></canvas></div>
    `;
    wrapper.appendChild(col);

    nodeCharts[nodeId] = {
        aqi:  createSmallChart(`aqi-rt-${nodeId}`,  'VN_AQI', 'AQI',    c, true),
        pm25: createSmallChart(`pm25-rt-${nodeId}`, 'PM2.5',  'µg/m³',  c),
        co:   createSmallChart(`co-rt-${nodeId}`,   'CO',     'ppm',    c),
    };

    realtimeBuf[nodeId] = { labels: [], aqi: [], pm25: [], co: [] };
}

function updateRealtimeCharts(nodeId, data) {
    const buf = realtimeBuf[nodeId];
    if (!buf) return;

    const now = new Date().toLocaleTimeString('vi-VN');
    buf.labels.push(now);
    buf.aqi.push(parseInt(data.aqi)    || 0);
    buf.pm25.push(parseFloat(data.pm25) || 0);
    buf.co.push(parseFloat(data.co)    || 0);

    if (buf.labels.length > MAX_DATA_POINTS) {
        buf.labels.shift(); buf.aqi.shift();
        buf.pm25.shift();   buf.co.shift();
    }

    const charts = nodeCharts[nodeId];
    if (!charts) return;
    pushToNodeChart(charts.aqi,  buf.labels, buf.aqi);
    pushToNodeChart(charts.pm25, buf.labels, buf.pm25);
    pushToNodeChart(charts.co,   buf.labels, buf.co);
}

function pushToNodeChart(chart, labels, values) {
    if (!chart) return;
    chart.data.labels            = [...labels];
    chart.data.datasets[0].data  = [...values];
    chart.update('none');
}

function createSmallChart(canvasId, title, yLabel, color, showAQIZones = false) {
    const ctx = document.getElementById(canvasId)?.getContext('2d');
    if (!ctx) return null;

    const aqiZonePlugin = showAQIZones ? {
        id: `aqiZones-${canvasId}`,
        beforeDraw(chart) {
            const { ctx: c, chartArea: area, scales: { y } } = chart;
            if (!area) return;
            const zones = [
                { from: 0,   to: 50,  color: 'rgba(40,167,69,0.08)'  },
                { from: 50,  to: 100, color: 'rgba(212,175,0,0.08)'  },
                { from: 100, to: 150, color: 'rgba(253,126,20,0.08)' },
                { from: 150, to: 200, color: 'rgba(220,53,69,0.08)'  },
                { from: 200, to: 300, color: 'rgba(111,66,193,0.08)' },
                { from: 300, to: 500, color: 'rgba(127,0,0,0.08)'    },
            ];
            zones.forEach(({ from, to, color }) => {
                const yTop = y.getPixelForValue(to);
                const yBot = y.getPixelForValue(from);
                c.fillStyle = color;
                c.fillRect(area.left, yTop, area.width, yBot - yTop);
            });
        },
    } : null;

    return new Chart(ctx, {
        type: 'line',
        data: {
            labels:   [],
            datasets: [{
                label:           title,
                data:            [],
                borderColor:     color.border,
                backgroundColor: color.bg,
                borderWidth:     2,
                fill:            true,
                tension:         0.3,
                pointRadius:     2,
            }],
        },
        plugins: aqiZonePlugin ? [aqiZonePlugin] : [],
        options: {
            responsive:          true,
            maintainAspectRatio: false,
            animation:           false,
            plugins: {
                title:  { display: true, text: title, font: { size: 11, weight: 'bold' } },
                legend: { display: false },
            },
            scales: {
                x: {
                    display: true,
                    title:  { display: false },
                    ticks:  { maxTicksLimit: 5, font: { size: 9 } },
                },
                y: {
                    display: true,
                    title:  { display: true, text: yLabel, font: { size: 9 } },
                    beginAtZero: false,
                    ticks: { font: { size: 9 } },
                    ...(showAQIZones ? { min: 0, max: 300 } : {}),
                },
            },
        },
    });
}

// Hàm tạo biểu đồ dùng chung — showAQIZones: vẽ dải màu ngưỡng AQI
function createChart(canvasId, title, yLabel, isTimeSeries = false, showAQIZones = false) {
    const ctx = document.getElementById(canvasId)?.getContext('2d');
    if (!ctx) return null;

    const xScale = isTimeSeries
        ? {
              type: 'time',
              time: {
                  tooltipFormat: 'HH:mm:ss dd/MM/yyyy',
                  displayFormats: { minute: 'HH:mm', hour: 'HH:mm dd/MM', day: 'dd/MM/yy' },
              },
              title: { display: true, text: 'Thời gian' },
          }
        : { display: true, title: { display: true, text: 'Thời gian' } };

    const aqiZonePlugin = showAQIZones ? {
        id: 'aqiZones',
        beforeDraw(chart) {
            const { ctx: c, chartArea: area, scales: { y } } = chart;
            if (!area) return;
            const zones = [
                { from: 0,   to: 50,  color: 'rgba(40,167,69,0.08)'  },
                { from: 50,  to: 100, color: 'rgba(212,175,0,0.08)'  },
                { from: 100, to: 150, color: 'rgba(253,126,20,0.08)' },
                { from: 150, to: 200, color: 'rgba(220,53,69,0.08)'  },
                { from: 200, to: 300, color: 'rgba(111,66,193,0.08)' },
                { from: 300, to: 500, color: 'rgba(127,0,0,0.08)'    },
            ];
            zones.forEach(({ from, to, color }) => {
                const yTop = y.getPixelForValue(to);
                const yBot = y.getPixelForValue(from);
                c.fillStyle = color;
                c.fillRect(area.left, yTop, area.width, yBot - yTop);
            });
        },
    } : null;

    return new Chart(ctx, {
        type: 'line',
        data: { labels: [], datasets: [] },
        plugins: aqiZonePlugin ? [aqiZonePlugin] : [],
        options: {
            responsive: true,
            animation:  false,
            plugins: {
                title:  { display: true, text: title, font: { size: 13, weight: 'bold' } },
                legend: { position: 'top' },
            },
            scales: {
                x: xScale,
                y: {
                    display: true,
                    title: { display: true, text: yLabel },
                    beginAtZero: false,
                    ...(showAQIZones ? { min: 0, max: 300, suggestedMax: 300 } : {}),
                },
            },
        },
    });
}

// ─── Thêm node vào dropdown lịch sử ──────────────────────────────
function addNodeToHistorySelect(nodeId) {
    const sel = document.getElementById('historyNodeSelect');
    if (!sel || sel.querySelector(`option[value="${nodeId}"]`)) return;
    const opt = document.createElement('option');
    opt.value = nodeId;
    opt.text  = `Node ${nodeId}`;
    sel.appendChild(opt);
}

// ======================================================
// PHẦN 4: LỊCH SỬ — Lấy từ PostgreSQL qua REST API
// ======================================================

let histPm1Chart, histPm25Chart, histPm10Chart, histCoChart;

function initHistoryCharts() {
    histPm1Chart   = createChart('historyPm1Chart',   'Lịch sử PM1.0',  'µg/m³', true);
    histPm25Chart  = createChart('historyPm25Chart',  'Lịch sử PM2.5',  'µg/m³', true);
    histPm10Chart  = createChart('historyPm10Chart',  'Lịch sử PM10',   'µg/m³', true);
    histCoChart    = createChart('historyCoChart',    'Lịch sử CO',      'ppm',   true);
}

function formatDate(date) {
    return date.getFullYear() + '-' +
           String(date.getMonth() + 1).padStart(2, '0') + '-' +
           String(date.getDate()).padStart(2, '0');
}

// Xử lý chọn nhanh thời gian (giống web folder gốc)
function handleQuickTimeChange() {
    const minutes   = document.getElementById('quickTimeSelect').value;
    const startInp  = document.getElementById('startDate');
    const endInp    = document.getElementById('endDate');

    if (minutes === 'custom') {
        startInp.disabled = false;
        endInp.disabled   = false;
        return;
    }

    const end   = new Date();
    const start = new Date(end.getTime() - parseInt(minutes) * 60 * 1000);
    startInp.value    = formatDate(start);
    endInp.value      = formatDate(end);
    startInp.disabled = true;
    endInp.disabled   = true;

    fetchHistoricalData();
}

// ─── Tải dữ liệu lịch sử từ API ──────────────────────────────────
async function fetchHistoricalData() {
    const startDate = document.getElementById('startDate').value;
    const endDate   = document.getElementById('endDate').value;
    const nodeId    = document.getElementById('historyNodeSelect').value;
    const statusEl  = document.getElementById('history-status');

    if (!startDate || !endDate) {
        setStatus(statusEl, 'Lỗi: Vui lòng chọn ngày bắt đầu và kết thúc.', 'error');
        return;
    }

    setStatus(statusEl, 'Đang tải từ PostgreSQL...', 'loading');

    try {
        let url = `${API_URL}/api/history?start=${startDate}&end=${endDate}`;
        if (nodeId) url += `&nodeId=${nodeId}`;

        const res  = await fetch(url);
        if (!res.ok) throw new Error(`HTTP ${res.status}: ${res.statusText}`);

        const data  = await res.json();   // { "1": [...], "2": [...] }
        const total = Object.values(data).reduce((s, a) => s + a.length, 0);

        if (total === 0) {
            setStatus(statusEl, 'Không có dữ liệu trong khoảng thời gian này.', 'orange');
        } else {
            const detail = Object.entries(data)
                .map(([nid, arr]) => `Node ${nid}: ${arr.length.toLocaleString()} điểm`)
                .join(' | ');
            setStatus(statusEl, `✓ Tải thành công — ${detail}`, 'success');
        }

        populateHistoryCharts(data);
        await fetchAndShowStats(nodeId, startDate, endDate);

    } catch (err) {
        console.error('Lỗi fetch lịch sử:', err);
        setStatus(statusEl, `✗ Lỗi: ${err.message} — Kiểm tra server đang chạy!`, 'error');
    }
}

function populateHistoryCharts(data) {
    // Xoá datasets cũ
    [histPm1Chart, histPm25Chart, histPm10Chart, histCoChart].forEach((chart) => {
        if (chart) { chart.data.datasets = []; chart.update('none'); }
    });

    Object.entries(data).forEach(([nid, rows]) => {
        const nodeId = parseInt(nid);
        const c = NODE_COLORS[(nodeId - 1) % NODE_COLORS.length];

        const makeDs = (field) => ({
            label:           `Node ${nodeId}`,
            data:            rows.map((r) => ({ x: new Date(r.time), y: r[field] })),
            borderColor:     c.border,
            backgroundColor: c.bg,
            borderWidth:     2,
            fill:            false,
            tension:         0.2,
            pointRadius:     rows.length > 500 ? 0 : 2,
        });

        histPm1Chart?.data.datasets.push(makeDs('pm1'));
        histPm25Chart?.data.datasets.push(makeDs('pm25'));
        histPm10Chart?.data.datasets.push(makeDs('pm10'));
        histCoChart?.data.datasets.push(makeDs('co'));
    });

    [histPm1Chart, histPm25Chart, histPm10Chart, histCoChart].forEach((c) => c?.update('none'));
}

// ─── Lấy và hiển thị bảng thống kê ───────────────────────────────
async function fetchAndShowStats(nodeId, start, end) {
    try {
        let url = `${API_URL}/api/stats?start=${start}&end=${end}`;
        if (nodeId) url += `&nodeId=${nodeId}`;

        const res  = await fetch(url);
        if (!res.ok) return;
        const rows = await res.json();
        if (!rows.length) return;

        renderStatsTable(rows);
    } catch { /* Bỏ qua lỗi stats */ }
}

function renderStatsTable(rows) {
    const section = document.getElementById('statsSection');
    const wrap    = document.getElementById('statsTable');
    if (!section || !wrap) return;

    let html = `
        <table class="stats-table">
            <thead>
                <tr>
                    <th>Ngày</th>
                    <th>Node</th>
                    <th>Số BG</th>
                    <th>Nhiệt độ TB (°C)</th>
                    <th>Max</th>
                    <th>Min</th>
                    <th>Độ ẩm TB (%)</th>
                    <th>PM2.5 TB</th>
                    <th>PM2.5 Max</th>
                    <th>CO TB (ppm)</th>
                </tr>
            </thead>
            <tbody>
    `;

    for (const r of rows) {
        const date = r.ngay ? new Date(r.ngay).toLocaleDateString('vi-VN') : '--';
        html += `
            <tr>
                <td>${date}</td>
                <td><strong>Node ${r.node_id}</strong></td>
                <td>${parseInt(r.so_ban_ghi).toLocaleString()}</td>
                <td>${r.nhiet_do_tb}</td>
                <td style="color:#dc3545">${r.nhiet_do_max}</td>
                <td style="color:#17a2b8">${r.nhiet_do_min}</td>
                <td>${r.do_am_tb}</td>
                <td>${r.pm25_tb}</td>
                <td style="color:${r.pm25_max > 35 ? '#dc3545' : '#333'}">${r.pm25_max}</td>
                <td>${r.co_tb}</td>
            </tr>
        `;
    }

    html += '</tbody></table>';
    wrap.innerHTML = html;
    section.style.display = 'block';
}

// ======================================================
// PHẦN 5: XUẤT CSV (Trích xuất dữ liệu)
// ======================================================

function exportCSV() {
    const startDate = document.getElementById('startDate').value;
    const endDate   = document.getElementById('endDate').value;
    const nodeId    = document.getElementById('historyNodeSelect').value;
    const statusEl  = document.getElementById('history-status');

    if (!startDate || !endDate) {
        setStatus(statusEl, 'Lỗi: Chọn ngày trước khi xuất CSV.', 'error');
        return;
    }

    let url = `${API_URL}/api/export?start=${startDate}&end=${endDate}`;
    if (nodeId) url += `&nodeId=${nodeId}`;

    setStatus(statusEl, '⬇ Đang chuẩn bị file CSV...', 'loading');

    // Mở URL để trình duyệt tự động tải file
    window.open(url, '_blank');

    setTimeout(() => {
        setStatus(statusEl, '✓ Đã mở file CSV — Kiểm tra thư mục Downloads.', 'success');
    }, 1500);
}

// ─── Tiện ích status ──────────────────────────────────────────────
function setStatus(el, msg, type) {
    if (!el) return;
    el.innerText  = msg;
    el.className  = type ? `history-status ${type}` : 'history-status';
}

// ======================================================
// KHỞI TẠO KHI TRANG TẢI XONG
// ======================================================
window.addEventListener('DOMContentLoaded', () => {
    const today     = new Date();
    const yesterday = new Date();
    yesterday.setDate(today.getDate() - 1);

    document.getElementById('startDate').value = formatDate(yesterday);
    document.getElementById('endDate').value   = formatDate(today);

    // Gắn sự kiện các nút
    document.getElementById('fetchHistoryBtn').addEventListener('click', fetchHistoricalData);
    document.getElementById('exportCsvBtn').addEventListener('click', exportCSV);
    document.getElementById('quickTimeSelect').addEventListener('change', handleQuickTimeChange);

    // Khởi tạo 8 biểu đồ
    initRealtimeCharts();
    initHistoryCharts();

    // Kết nối SSE để nhận dữ liệu real-time
    connectSSE();
});