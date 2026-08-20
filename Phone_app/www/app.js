/**
 * AIR QUALITY MONITOR (AQM) v2.0 — REDESIGNED
 * Created for Partha A B
 *
 * All Firebase logic, downsampling, WiFi config, buzzer control
 * preserved from v1. UI bindings updated for the "Aether" design.
 *
 * Downsampling (client-side):
 *   1D  -> bucket by 10 minutes  -> max ~144 points
 *   1W  -> bucket by 1 hour      -> max ~168 points
 *   1M  -> bucket by 6 hours     -> max ~120 points
 */

// ═══════════════════════════════════════════════════════════
// GLOBAL STATE
// ═══════════════════════════════════════════════════════════
const DEFAULT_FIREBASE_URL = "https://air-quality-monitor-1d94f-default-rtdb.asia-southeast1.firebasedatabase.app";
const LIVE_POLL_INTERVAL_MS = 1000;
const LIVE_REQUEST_TIMEOUT_MS = 5000;
const LIVE_STALE_AFTER_MS = 8000;

function getEmptyHistoryData() {
  return { labels: [], co2: [], aqi: [], pm1_0: [], pm2_5: [], pm10: [], temp: [], hum: [] };
}

const state = {
  currentTab: 'dashboard',
  selectedRange: '1D',
  modalSelectedRange: '1D',
  activeModalMetric: null,
  isBuzzerEnabled: true,
  isFirebaseConnected: true,
  firebaseConfig: {
    dbUrl: localStorage.getItem('aqm_firebase_db_url') || DEFAULT_FIREBASE_URL
  },
  liveData: {
    co2: -1, aqi: -1, iaq: -1,
    pm1_0: -1, pm2_5: -1, pm10: -1,
    temp: -999, hum: -1, pres: -1,
    gas: -1, voc_ppm: -1,
    timestamp: Date.now()
  },
  historyData: getEmptyHistoryData(),
  countdownSeconds: 1
};

// Chart instances
let miniPmChart    = null;
let miniCo2Chart   = null;
let miniAqiChart   = null;
let miniTempChart  = null;
let miniHumChart   = null;
let fullModalChart = null;

// ═══════════════════════════════════════════════════════════
// 1. INITIALIZATION
// ═══════════════════════════════════════════════════════════
document.addEventListener('DOMContentLoaded', () => {
  initSplashScreen();

  try {
    if (window.lucide && typeof window.lucide.createIcons === 'function') {
      window.lucide.createIcons();
    }
  } catch (err) {
    console.warn("Lucide icons deferred:", err);
  }

  setupNavigation();
  setupBuzzerToggle();
  loadBuzzerState();
  setupRangeFilters();
  setupBackButton();
  loadWiFiConfigFromFirebase();

  // Fetch history, then build charts
  setTimeout(() => {
    fetchFirebaseHistory('1D')
      .then(histData => {
        state.historyData = histData;
        initCharts(histData);
        updateUI(state.liveData);
      })
      .catch(() => {
        initCharts(getEmptyHistoryData());
        updateUI(state.liveData);
      });
  }, 400);

  startCountdownTimer();
  connectFirebase();
});

// ═══════════════════════════════════════════════════════════
// 2. SPLASH SCREEN
// ═══════════════════════════════════════════════════════════
function initSplashScreen() {
  const splash = document.getElementById('splash-screen');
  const progress = document.getElementById('splash-progress');
  if (!splash) return;

  if (progress) {
    setTimeout(() => { progress.style.width = '55%'; }, 80);
    setTimeout(() => { progress.style.width = '100%'; }, 500);
  }

  const dismiss = () => {
    splash.style.opacity = '0';
    splash.style.pointerEvents = 'none';
    setTimeout(() => { if (splash.parentNode) splash.remove(); }, 500);
  };

  splash.addEventListener('click', dismiss);
  setTimeout(dismiss, 1200);
}

// ═══════════════════════════════════════════════════════════
// 3. NAVIGATION (3 TABS)
// ═══════════════════════════════════════════════════════════
function setupNavigation() {
  const tabs = {
    dashboard: { btn: document.getElementById('nav-btn-dashboard'), panel: document.getElementById('tab-dashboard') },
    analytics: { btn: document.getElementById('nav-btn-analytics'), panel: document.getElementById('tab-analytics') },
    settings:  { btn: document.getElementById('nav-btn-settings'),  panel: document.getElementById('tab-settings') }
  };

  function switchTab(name) {
    state.currentTab = name;
    Object.keys(tabs).forEach(key => {
      const t = tabs[key];
      if (key === name) {
        t.btn.classList.add('active');
        t.panel.classList.add('active');
      } else {
        t.btn.classList.remove('active');
        t.panel.classList.remove('active');
      }
    });

    // Refresh charts when switching to analytics
    if (name === 'analytics') {
      refreshChartsData(state.selectedRange);
      setTimeout(() => {
        [miniPmChart, miniCo2Chart, miniAqiChart, miniTempChart, miniHumChart].forEach(c => c && c.resize());
      }, 50);
    }

    // Re-render lucide icons when switching to settings
    if (name === 'settings') {
      try { if (window.lucide) window.lucide.createIcons(); } catch(e) {}
    }
  }

  tabs.dashboard.btn.addEventListener('click', () => switchTab('dashboard'));
  tabs.analytics.btn.addEventListener('click', () => switchTab('analytics'));
  tabs.settings.btn.addEventListener('click', () => switchTab('settings'));
}

// ═══════════════════════════════════════════════════════════
// 4. BUZZER CONTROL
// ═══════════════════════════════════════════════════════════
function setupBuzzerToggle() {
  const checkbox = document.getElementById('buzzer-mute-toggle');
  checkbox.checked = state.isBuzzerEnabled;
  updateBuzzerUI();

  // The input's change event fires exactly once for both a tap on the switch
  // and keyboard input. Listening on the label as well caused two competing
  // Firebase writes, which could leave the ESP32 in the wrong mute state.
  checkbox.addEventListener('change', async () => {
    const previousValue = state.isBuzzerEnabled;
    state.isBuzzerEnabled = checkbox.checked;
    updateBuzzerUI();
    const url = state.firebaseConfig.dbUrl.replace(/\/$/, '') + '/config/buzzerEnabled.json';
    checkbox.disabled = true;
    try {
      const response = await fetch(url, {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(state.isBuzzerEnabled),
        cache: 'no-store'
      });
      if (!response.ok) throw new Error(`Firebase returned ${response.status}`);
    } catch (err) {
      console.error("Firebase buzzer sync error:", err);
      state.isBuzzerEnabled = previousValue;
      updateBuzzerUI();
    } finally {
      checkbox.disabled = false;
    }
  });
}

async function loadBuzzerState() {
  const url = state.firebaseConfig.dbUrl.replace(/\/$/, '') + '/config/buzzerEnabled.json';
  try {
    const response = await fetch(url, { cache: 'no-store' });
    if (!response.ok) return;
    const enabled = await response.json();
    if (typeof enabled === 'boolean') {
      state.isBuzzerEnabled = enabled;
      updateBuzzerUI();
    }
  } catch (err) {
    console.warn("Firebase buzzer state unavailable:", err);
  }
}

function updateBuzzerUI() {
  const statusDesc = document.getElementById('buzzer-status-desc');
  const iconBox = document.getElementById('buzzer-icon-box');
  const track = document.getElementById('buzzer-toggle-track');
  const checkbox = document.getElementById('buzzer-mute-toggle');
  checkbox.checked = state.isBuzzerEnabled;

  if (state.isBuzzerEnabled) {
    statusDesc.innerText = 'Enabled (Active on hazardous levels)';
    statusDesc.style.color = 'var(--good)';
    iconBox.style.background = 'rgba(245,158,11,0.08)';
    iconBox.style.borderColor = 'rgba(245,158,11,0.2)';
    iconBox.style.color = 'var(--moderate)';
    track.classList.add('on');
  } else {
    statusDesc.innerText = 'Muted (Disabled remotely)';
    statusDesc.style.color = 'var(--hazardous)';
    iconBox.style.background = 'rgba(239,68,68,0.08)';
    iconBox.style.borderColor = 'rgba(239,68,68,0.2)';
    iconBox.style.color = 'var(--hazardous)';
    track.classList.remove('on');
  }
}

// ═══════════════════════════════════════════════════════════
// 5. SAFENESS & UI UPDATER
// ═══════════════════════════════════════════════════════════
function getSafenessLevel(metric, value) {
  if (value === -1 || value === null || value === undefined || value < 0) {
    return { level: 'na', label: 'N/A', color: 'var(--text-dim)' };
  }
  let level = 'good', label = 'GOOD', color = 'var(--good)';

  switch (metric) {
    case 'co2':
      if (value <= 800)       { level='good'; label='SAFE'; color='var(--good)'; }
      else if (value <= 1000) { level='moderate'; label='MODERATE'; color='var(--moderate)'; }
      else if (value <= 1500) { level='unhealthy'; label='HIGH'; color='var(--unhealthy)'; }
      else                    { level='hazardous'; label='DANGEROUS'; color='var(--hazardous)'; }
      break;
    case 'aqi':
      if (value <= 50)        { level='good'; label='GOOD'; color='var(--good)'; }
      else if (value <= 100)  { level='moderate'; label='MODERATE'; color='var(--moderate)'; }
      else if (value <= 150)  { level='unhealthy'; label='UNHEALTHY'; color='var(--unhealthy)'; }
      else                    { level='hazardous'; label='HAZARDOUS'; color='var(--hazardous)'; }
      break;
    case 'iaq':
      if (value <= 50)        { level='good'; label='GOOD'; color='var(--good)'; }
      else if (value <= 100)  { level='moderate'; label='MODERATE'; color='var(--moderate)'; }
      else if (value <= 150)  { level='unhealthy'; label='POOR'; color='var(--unhealthy)'; }
      else                    { level='hazardous'; label='BAD'; color='var(--hazardous)'; }
      break;
    case 'pm1_0':
    case 'pm2_5':
    case 'pm10':
      if (value <= 12)        { level='good'; label='LOW'; color='var(--good)'; }
      else if (value <= 35)   { level='moderate'; label='MODERATE'; color='var(--moderate)'; }
      else if (value <= 55)   { level='unhealthy'; label='ELEVATED'; color='var(--unhealthy)'; }
      else                    { level='hazardous'; label='HIGH'; color='var(--hazardous)'; }
      break;
    default:
      level = value <= 100 ? 'good' : 'moderate';
      break;
  }
  return { level, label, color };
}

function updateUI(d) {
  // Hero gauge
  const aqiVal = d.aqi >= 0 ? d.aqi : '--';
  const co2Val = d.co2 >= 0 ? d.co2 : '--';
  document.getElementById('main-aqi-value').innerText = aqiVal;
  document.getElementById('main-co2-value').innerText = co2Val;

  // AQI status badge
  const aqiInfo = getSafenessLevel('aqi', d.aqi);
  const statusEl = document.getElementById('main-aqi-status');
  statusEl.innerText = aqiInfo.label;
  statusEl.style.color = aqiInfo.color;
  statusEl.style.background = aqiInfo.level === 'na' ? 'rgba(100,116,139,0.1)' :
    aqiInfo.level === 'good' ? 'rgba(16,185,129,0.1)' :
    aqiInfo.level === 'moderate' ? 'rgba(245,158,11,0.1)' :
    aqiInfo.level === 'unhealthy' ? 'rgba(249,115,22,0.1)' : 'rgba(239,68,68,0.12)';
  statusEl.style.borderColor = aqiInfo.level === 'na' ? 'rgba(100,116,139,0.2)' :
    aqiInfo.level === 'good' ? 'rgba(16,185,129,0.25)' :
    aqiInfo.level === 'moderate' ? 'rgba(245,158,11,0.25)' :
    aqiInfo.level === 'unhealthy' ? 'rgba(249,115,22,0.25)' : 'rgba(239,68,68,0.3)';

  // Update gauge arc
  updateGauge(d.aqi, aqiInfo);

  // Sensor cards
  const metrics = [
    { key:'co2',     val: d.co2 >= 0 ? d.co2 : '--' },
    { key:'aqi',     val: d.aqi >= 0 ? d.aqi : '--' },
    { key:'iaq',     val: d.iaq >= 0 ? d.iaq : '--' },
    { key:'pm2_5',   val: d.pm2_5 >= 0 ? d.pm2_5 : '--' },
    { key:'pm1_0',   val: d.pm1_0 >= 0 ? d.pm1_0 : '--' },
    { key:'pm10',    val: d.pm10 >= 0 ? d.pm10 : '--' },
    { key:'temp',    val: d.temp > -900 ? d.temp : '--' },
    { key:'hum',     val: d.hum >= 0 ? d.hum : '--' },
    { key:'pres',    val: d.pres >= 0 ? d.pres : '--' },
    { key:'voc_ppm', val: d.voc_ppm >= 0 ? d.voc_ppm : '--' }
  ];

  metrics.forEach(m => {
    const valEl = document.getElementById('val-' + m.key);
    const cardEl = document.getElementById('card-' + m.key);
    if (valEl) valEl.innerText = m.val;
    if (cardEl) {
      const info = getSafenessLevel(m.key, typeof m.val === 'number' ? m.val : -1);
      cardEl.style.setProperty('--card-accent', info.color);
    }
  });

  updateColorBar(d);
}

// ═══════════════════════════════════════════════════════════
// 6. RADIAL GAUGE
// ═══════════════════════════════════════════════════════════
function updateGauge(aqiValue, aqiInfo) {
  const arc = document.getElementById('gauge-arc');
  const glow = document.getElementById('gauge-glow');
  if (!arc) return;

  const maxAqi = 300;
  const arcLength = 376.99; // 270deg of circumference for r=80
  const circumference = 502.65;

  let pct = 0;
  if (aqiValue >= 0) {
    pct = Math.min(1, Math.max(0, aqiValue / maxAqi));
  }

  const dashLen = pct * arcLength;
  arc.style.strokeDasharray = dashLen + ' ' + circumference;
  glow.style.strokeDasharray = dashLen + ' ' + circumference;

  // Color the gauge based on level
  const colorMap = {
    'na':       '#475569',
    'good':     '#10b981',
    'moderate': '#f59e0b',
    'unhealthy':'#f97316',
    'hazardous':'#ef4444'
  };
  const strokeColor = colorMap[aqiInfo.level] || colorMap.na;
  arc.style.stroke = strokeColor;
  glow.style.stroke = strokeColor;
  arc.style.filter = 'drop-shadow(0 0 8px ' + strokeColor + '40)';
}

// ═══════════════════════════════════════════════════════════
// 7. HEALTH COLOR BAR
// ═══════════════════════════════════════════════════════════
function updateColorBar(d) {
  let worst = 0;

  if (d.co2 >= 0) {
    if (d.co2 > 1500)      worst = Math.max(worst, 3);
    else if (d.co2 > 1000) worst = Math.max(worst, 2);
    else if (d.co2 > 800)  worst = Math.max(worst, 1);
  }
  if (d.aqi >= 0) {
    if (d.aqi > 150)       worst = Math.max(worst, 3);
    else if (d.aqi > 100)  worst = Math.max(worst, 2);
    else if (d.aqi > 50)   worst = Math.max(worst, 1);
  }
  if (d.iaq >= 0) {
    if (d.iaq > 150)       worst = Math.max(worst, 3);
    else if (d.iaq > 100)  worst = Math.max(worst, 2);
    else if (d.iaq > 50)   worst = Math.max(worst, 1);
  }

  const palette = [
    { bg: '#10b981', glow: 'rgba(16,185,129,0.4)' },
    { bg: '#f59e0b', glow: 'rgba(245,158,11,0.4)' },
    { bg: '#f97316', glow: 'rgba(249,115,22,0.4)' },
    { bg: '#ef4444', glow: 'rgba(239,68,68,0.5)' }
  ];
  const { bg, glow } = palette[worst];
  const bar = document.getElementById('aq-color-bar');
  if (bar) {
    bar.style.background = bg;
    bar.style.boxShadow = '0 0 12px ' + glow;
  }
}

function startCountdownTimer() {
  const el = document.getElementById('next-update-timer');
  if (el) el.innerText = 'Live • every 1s';
}

// ═══════════════════════════════════════════════════════════
// 8. CHART.JS
// ═══════════════════════════════════════════════════════════
function initCharts(d) {
  if (!d) d = getEmptyHistoryData();

  const base = {
    responsive: true,
    maintainAspectRatio: false,
    plugins: { legend: { display: false } },
    scales: {
      x: { grid: { display: false }, ticks: { color: '#64748b', font: { size: 9 }, maxTicksLimit: 6, maxRotation: 0 } },
      y: { grid: { color: 'rgba(255,255,255,0.04)' }, ticks: { color: '#64748b', font: { size: 9 } } }
    }
  };

  // PM Combined
  const pmCtx = document.getElementById('mini-pm-chart').getContext('2d');
  miniPmChart = new Chart(pmCtx, {
    type: 'line',
    data: {
      labels: d.labels,
      datasets: [
        { label: 'PM1.0', data: d.pm1_0, borderColor: '#38bdf8', pointBackgroundColor: '#38bdf8', pointBorderWidth: 0, pointRadius: 3, borderWidth: 2, tension: 0.4, pointRadius: 0 },
        { label: 'PM2.5', data: d.pm2_5, borderColor: '#34d399', pointBackgroundColor: '#34d399', pointBorderWidth: 0, pointRadius: 3, borderWidth: 2, tension: 0.4, pointRadius: 0 },
        { label: 'PM10',  data: d.pm10,  borderColor: '#fbbf24', pointBackgroundColor: '#fbbf24', pointBorderWidth: 0, pointRadius: 3, borderWidth: 2, tension: 0.4, pointRadius: 0 }
      ]
    },
    options: base
  });

  // CO2
  const co2Ctx = document.getElementById('mini-co2-chart').getContext('2d');
  miniCo2Chart = new Chart(co2Ctx, {
    type: 'line',
    data: {
      labels: d.labels,
      datasets: [{ label: 'CO2', data: d.co2, borderColor: '#22d3ee', pointBackgroundColor: '#22d3ee', pointBorderWidth: 0, pointRadius: 3, backgroundColor: 'rgba(34,211,238,0.08)', fill: true, borderWidth: 2, tension: 0.4, pointRadius: 0 }]
    },
    options: base
  });

  // AQI
  const aqiCtx = document.getElementById('mini-aqi-chart').getContext('2d');
  miniAqiChart = new Chart(aqiCtx, {
    type: 'line',
    data: {
      labels: d.labels,
      datasets: [{ label: 'AQI', data: d.aqi, borderColor: '#34d399', pointBackgroundColor: '#34d399', pointBorderWidth: 0, pointRadius: 3, backgroundColor: 'rgba(52,211,153,0.08)', fill: true, borderWidth: 2, tension: 0.4, pointRadius: 0 }]
    },
    options: base
  });

  // Temperature
  const tempCtx = document.getElementById('mini-temp-chart').getContext('2d');
  miniTempChart = new Chart(tempCtx, {
    type: 'line',
    data: {
      labels: d.labels,
      datasets: [{ label: 'Temp', data: d.temp, borderColor: '#fb7185', pointBackgroundColor: '#fb7185', pointBorderWidth: 0, pointRadius: 3, backgroundColor: 'rgba(251,113,133,0.08)', fill: true, borderWidth: 2, tension: 0.4, pointRadius: 0 }]
    },
    options: base
  });

  // Humidity
  const humCtx = document.getElementById('mini-hum-chart').getContext('2d');
  miniHumChart = new Chart(humCtx, {
    type: 'line',
    data: {
      labels: d.labels,
      datasets: [{ label: 'Humidity', data: d.hum, borderColor: '#60a5fa', pointBackgroundColor: '#60a5fa', pointBorderWidth: 0, pointRadius: 3, backgroundColor: 'rgba(96,165,250,0.08)', fill: true, borderWidth: 2, tension: 0.4, pointRadius: 0 }]
    },
    options: base
  });
}

// ═══════════════════════════════════════════════════════════
// 9. FULL GRAPH MODAL
// ═══════════════════════════════════════════════════════════
function setupRangeFilters() {
  document.querySelectorAll('.range-btn:not(.modal-range-btn)').forEach(btn => {
    btn.addEventListener('click', (e) => {
      document.querySelectorAll('.range-btn:not(.modal-range-btn)').forEach(b => b.classList.remove('active'));
      e.target.classList.add('active');
      state.selectedRange = e.target.dataset.range;
      refreshChartsData(state.selectedRange);
    });
  });

  document.querySelectorAll('.modal-range-btn').forEach(btn => {
    btn.addEventListener('click', (e) => {
      document.querySelectorAll('.modal-range-btn').forEach(b => b.classList.remove('active'));
      e.target.classList.add('active');
      state.modalSelectedRange = e.target.dataset.modalRange;
      if (state.activeModalMetric) renderFullModalChart(state.activeModalMetric, state.modalSelectedRange);
    });
  });
}

function openFullModal(metric) {
  state.activeModalMetric = metric;
  const modal = document.getElementById('full-graph-modal');
  const title = document.getElementById('modal-graph-title');
  const subtitle = document.getElementById('modal-graph-subtitle');
  modal.classList.add('open');

  const meta = {
    'PM_COMBINED': ['PARTICULATE MATTER', 'PM1.0 + PM2.5 + PM10 readings (µg/m³)'],
    'CO2':         ['CARBON DIOXIDE',     'NDIR sensor readings (PPM)'],
    'AQI':         ['AIR QUALITY INDEX',  'US EPA 0-500 scale'],
    'TEMP':        ['TEMPERATURE',        'BME680 ambient (°C)'],
    'HUM':         ['RELATIVE HUMIDITY',  'BME680 ambient (%)']
  };
  const [t, s] = meta[metric] || ['ANALYTICS', 'Trend data'];
  title.innerText = t;
  subtitle.innerText = s;

  renderFullModalChart(metric, state.modalSelectedRange);
}

function closeFullModal() {
  document.getElementById('full-graph-modal').classList.remove('open');
}

// ═══════════════════════════════════════════════════════════
// 10. FIREBASE HISTORY + DOWNSAMPLING
// ═══════════════════════════════════════════════════════════
function downsample(entries, bucketMs, range) {
  if (!entries.length) return [];

  const buckets = {};
  entries.forEach(e => {
    const key = Math.floor(e.timestamp / bucketMs) * bucketMs;
    if (!buckets[key]) buckets[key] = [];
    buckets[key].push(e);
  });

  const avg = (group, field, minVal = 0) => {
    const vals = group.map(e => e[field]).filter(v => v !== null && v !== undefined && Number(v) >= minVal);
    return vals.length ? vals.reduce((a, b) => a + Number(b), 0) / vals.length : null;
  };

  return Object.keys(buckets)
    .map(Number)
    .sort((a, b) => a - b)
    .map(key => {
      const group = buckets[key];
      const midTs = key + bucketMs / 2;
      return {
        timestamp: midTs,
        label: formatTimestamp(midTs, range),
        co2: avg(group, 'co2'),
        aqi: avg(group, 'aqi'),
        pm1_0: avg(group, 'pm1_0'),
        pm2_5: avg(group, 'pm2_5'),
        pm10: avg(group, 'pm10'),
        temp: avg(group, 'temp', -900),
        hum: avg(group, 'hum')
      };
    });
}

function formatTimestamp(ms, range) {
  const d = new Date(ms);
  if (range === '1D') {
    return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
  } else if (range === '1W') {
    return d.toLocaleDateString([], { weekday: 'short' }) + ' ' +
           d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
  } else {
    return d.toLocaleDateString([], { month: 'short', day: 'numeric' }) + ' ' +
           d.toLocaleTimeString([], { hour: '2-digit' }) + ':00';
  }
}

function fetchFirebaseHistory(range) {
  range = range || '1D';
  const url = state.firebaseConfig.dbUrl.replace(/\/$/, '') + '/history.json';
  const bucketMs = { '1D': 10*60*1000, '1W': 60*60*1000, '1M': 6*60*60*1000 };
  const cutoffMs = { '1D': 86400000, '1W': 604800000, '1M': 2592000000 };

  return fetch(url)
    .then(res => res.json())
    .then(rawData => {
      if (!rawData || typeof rawData !== 'object') return getEmptyHistoryData();

      let entries = Object.values(rawData);
      if (!entries.length) return getEmptyHistoryData();

      const lastTs = entries[entries.length - 1].timestamp || 0;
      const isWallClock = lastTs > 1000000000000;

      if (isWallClock) {
        entries.sort((a, b) => (a.timestamp || 0) - (b.timestamp || 0));
        const now = Date.now();
        const cutoff = cutoffMs[range] || cutoffMs['1D'];
        entries = entries.filter(e => (now - (e.timestamp || 0)) <= cutoff);
        if (!entries.length) return getEmptyHistoryData();

        const sampled = downsample(entries, bucketMs[range] || bucketMs['1D'], range);
        const labels=[], co2=[], aqi=[], pm1_0=[], pm2_5=[], pm10=[], temp=[], hum=[];
        sampled.forEach(pt => {
          labels.push(pt.label);
          co2.push(pt.co2); aqi.push(pt.aqi);
          pm1_0.push(pt.pm1_0); pm2_5.push(pt.pm2_5); pm10.push(pt.pm10);
          temp.push(pt.temp); hum.push(pt.hum);
        });
        return { labels, co2, aqi, pm1_0, pm2_5, pm10, temp, hum };
      } else {
        entries = entries.slice(-60);
        const labels=[], co2=[], aqi=[], pm1_0=[], pm2_5=[], pm10=[], temp=[], hum=[];
        entries.forEach((e, i) => {
          labels.push(String(i + 1));
          co2.push(e.co2 >= 0 ? e.co2 : null);
          aqi.push(e.aqi >= 0 ? e.aqi : null);
          pm1_0.push(e.pm1_0 >= 0 ? e.pm1_0 : null);
          pm2_5.push(e.pm2_5 >= 0 ? e.pm2_5 : null);
          pm10.push(e.pm10 >= 0 ? e.pm10 : null);
          temp.push(e.temp > -900 ? e.temp : null);
          hum.push(e.hum >= 0 ? e.hum : null);
        });
        return { labels, co2, aqi, pm1_0, pm2_5, pm10, temp, hum };
      }
    })
    .catch(err => {
      console.error("Firebase history error:", err);
      return getEmptyHistoryData();
    });
}

function refreshChartsData(range) {
  fetchFirebaseHistory(range).then(d => {
    state.historyData = d;
    const update = (chart, newLabels, ...datasets) => {
      if (!chart) return;
      chart.data.labels = newLabels;
      datasets.forEach((data, i) => { chart.data.datasets[i].data = data; });
      chart.update();
    };
    update(miniPmChart, d.labels, d.pm1_0, d.pm2_5, d.pm10);
    update(miniCo2Chart, d.labels, d.co2);
    update(miniAqiChart, d.labels, d.aqi);
    update(miniTempChart, d.labels, d.temp);
    update(miniHumChart, d.labels, d.hum);
  });
}

function renderFullModalChart(metric, range) {
  const canvas = document.getElementById('full-modal-chart');
  const ctx = canvas.getContext('2d');
  if (fullModalChart) fullModalChart.destroy();

  fetchFirebaseHistory(range).then(data => {
    const datasetMap = {
      'PM_COMBINED': [
        { label: 'PM1.0', data: data.pm1_0, borderColor: '#38bdf8', pointBackgroundColor: '#38bdf8', pointBorderWidth: 0, pointRadius: 3, backgroundColor: 'rgba(56,189,248,0.08)', borderWidth: 2.5, tension: 0.4 },
        { label: 'PM2.5', data: data.pm2_5, borderColor: '#34d399', pointBackgroundColor: '#34d399', pointBorderWidth: 0, pointRadius: 3, backgroundColor: 'rgba(52,211,153,0.08)', borderWidth: 2.5, tension: 0.4 },
        { label: 'PM10',  data: data.pm10,  borderColor: '#fbbf24', pointBackgroundColor: '#fbbf24', pointBorderWidth: 0, pointRadius: 3, backgroundColor: 'rgba(251,191,36,0.08)', borderWidth: 2.5, tension: 0.4 }
      ],
      'CO2':  [{ label: 'CO2 (PPM)', data: data.co2, borderColor: '#22d3ee', pointBackgroundColor: '#22d3ee', pointBorderWidth: 0, pointRadius: 3, backgroundColor: 'rgba(34,211,238,0.1)', fill: true, borderWidth: 2.5, tension: 0.4 }],
      'AQI':  [{ label: 'AQI', data: data.aqi, borderColor: '#34d399', pointBackgroundColor: '#34d399', pointBorderWidth: 0, pointRadius: 3, backgroundColor: 'rgba(52,211,153,0.1)', fill: true, borderWidth: 2.5, tension: 0.4 }],
      'TEMP': [{ label: 'Temp (°C)', data: data.temp, borderColor: '#fb7185', pointBackgroundColor: '#fb7185', pointBorderWidth: 0, pointRadius: 3, backgroundColor: 'rgba(251,113,133,0.1)', fill: true, borderWidth: 2.5, tension: 0.4 }],
      'HUM':  [{ label: 'Humidity (%)', data: data.hum, borderColor: '#60a5fa', pointBackgroundColor: '#60a5fa', pointBorderWidth: 0, pointRadius: 3, backgroundColor: 'rgba(96,165,250,0.1)', fill: true, borderWidth: 2.5, tension: 0.4 }]
    };

    fullModalChart = new Chart(ctx, {
      type: 'line',
      data: { labels: data.labels, datasets: datasetMap[metric] || [] },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        plugins: {
          legend: { display: true, labels: { color: '#94a3b8', font: { size: 11 } } }
        },
        scales: {
          x: { grid: { color: 'rgba(255,255,255,0.04)' }, ticks: { color: '#94a3b8', maxTicksLimit: 8, maxRotation: 30 } },
          y: { grid: { color: 'rgba(255,255,255,0.06)' }, ticks: { color: '#94a3b8' } }
        }
      }
    });
  });
}

// ═══════════════════════════════════════════════════════════
// 11. FIREBASE LIVE POLLING
// ═══════════════════════════════════════════════════════════
function connectFirebase() {
  const url = state.firebaseConfig.dbUrl.replace(/\/$/, '') + '/readings/current.json';

  document.getElementById('connection-status-dot').className = 'status-dot';
  document.getElementById('connection-status-text').innerText = 'ONLINE';

  let lastReadingTime = Date.now();
  let pollInFlight = false;

  const poll = async () => {
    if (pollInFlight) return;
    pollInFlight = true;
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), LIVE_REQUEST_TIMEOUT_MS);
    try {
      const response = await fetch(url, { cache: 'no-store', signal: controller.signal });
      if (!response.ok) throw new Error(`Firebase returned ${response.status}`);
      const data = await response.json();
      if (data && data.timestamp !== state.liveData.timestamp) {
        lastReadingTime = Date.now();
        state.liveData = { ...state.liveData, ...data };
        updateUI(state.liveData);
      }
    } catch (err) {
      if (err.name !== 'AbortError') console.error("Firebase fetch error:", err);
    } finally {
      clearTimeout(timeoutId);
      pollInFlight = false;
    }
  };

  poll();
  setInterval(poll, LIVE_POLL_INTERVAL_MS);

  setInterval(() => {
    const age = Date.now() - lastFetchSuccessTime;
    const dot = document.getElementById('connection-status-dot');
    const text = document.getElementById('connection-status-text');
    if (dot && text) {
      if (age > LIVE_STALE_AFTER_MS) {
        dot.className = 'status-dot offline';
        text.innerText = 'OFFLINE';
        text.style.color = 'var(--hazardous)';
      } else {
        dot.className = 'status-dot';
        text.innerText = 'ONLINE';
        text.style.color = 'var(--accent)';
      }
    }
  }, 5000);
}

// ═══════════════════════════════════════════════════════════
// 12. BACK BUTTON (Capacitor)
// ═══════════════════════════════════════════════════════════
function setupBackButton() {
  // Cordova-style
  document.addEventListener('backbutton', handleBackButton, false);
  // Capacitor App plugin
  if (window.Capacitor && window.Capacitor.Plugins && window.Capacitor.Plugins.App) {
    window.Capacitor.Plugins.App.addListener('backButton', handleBackButton);
  }
}

function handleBackButton(ev) {
  // 1. Full graph modal open? Close it.
  const modal = document.getElementById('full-graph-modal');
  if (modal && modal.classList.contains('open')) {
    closeFullModal();
    return;
  }

  // 2. Settings sub-view open? Go back to settings menu.
  if (state.currentTab === 'settings') {
    const menu = document.getElementById('settings-menu');
    if (menu.classList.contains('hidden-left')) {
      // A sub-view is open
      const wifi = document.getElementById('settings-wifi');
      if (wifi.classList.contains('visible')) { closeSettingsSubView('settings-wifi'); return; }
      const about = document.getElementById('settings-about');
      if (about.classList.contains('visible')) { closeSettingsSubView('settings-about'); return; }
    }
  }

  // 3. On analytics or settings tab? Go to dashboard.
  if (state.currentTab === 'analytics' || state.currentTab === 'settings') {
    document.getElementById('nav-btn-dashboard').click();
    return;
  }

  // 4. On dashboard — exit app
  if (window.Capacitor && window.Capacitor.Plugins && window.Capacitor.Plugins.App) {
    window.Capacitor.Plugins.App.exitApp();
  }
}

// ═══════════════════════════════════════════════════════════
// 13. SETTINGS SUB-VIEWS
// ═══════════════════════════════════════════════════════════
function openSettingsSubView(id) {
  document.getElementById('settings-menu').classList.add('hidden-left');
  document.getElementById(id).classList.add('visible');
  try { if (window.lucide) window.lucide.createIcons(); } catch(e) {}
}

function closeSettingsSubView(id) {
  document.getElementById(id).classList.remove('visible');
  document.getElementById('settings-menu').classList.remove('hidden-left');
}

// ═══════════════════════════════════════════════════════════
// 14. WIFI CONFIG
// ═══════════════════════════════════════════════════════════
let activeWiFiSSID = '';

function loadWiFiConfigFromFirebase() {
  const base = state.firebaseConfig.dbUrl.replace(/\/$/, '');

  Promise.all([
    fetch(base + '/config/wifi.json').then(r => r.json()).catch(() => null),
    fetch(base + '/status/wifi.json').then(r => r.json()).catch(() => null)
  ]).then(([cfg, status]) => {
    activeWiFiSSID = (status && status.activeSSID) ? status.activeSSID : '';

    document.getElementById('wifi-ssid-1').value = (cfg && cfg.ssid1) ? cfg.ssid1 : 'Appu';
    document.getElementById('wifi-pass-1').value = (cfg && cfg.pass1) ? cfg.pass1 : 'appu09875';
    document.getElementById('wifi-ssid-2').value = (cfg && cfg.ssid2) ? cfg.ssid2 : '';
    document.getElementById('wifi-pass-2').value = (cfg && cfg.pass2) ? cfg.pass2 : '';
    document.getElementById('wifi-ssid-3').value = (cfg && cfg.ssid3) ? cfg.ssid3 : '';
    document.getElementById('wifi-pass-3').value = (cfg && cfg.pass3) ? cfg.pass3 : '';

    updateActiveWiFiBadges();
  });
}

function updateActiveWiFiBadges() {
  for (let i = 1; i <= 3; i++) {
    const ssid = document.getElementById('wifi-ssid-' + i).value.trim();
    const label = document.getElementById('wifi-label-' + i);
    if (!label) continue;
    const baseText = label.getAttribute('data-base-text') || '';
    if (ssid && ssid === activeWiFiSSID) {
      label.innerHTML = baseText + ' <span class="active-badge">ACTIVE</span>';
    } else {
      label.innerHTML = baseText;
    }
  }
}

function applyWiFiConfig() {
  const s1 = document.getElementById('wifi-ssid-1').value.trim();
  const p1 = document.getElementById('wifi-pass-1').value.trim();
  const s2 = document.getElementById('wifi-ssid-2').value.trim();
  const p2 = document.getElementById('wifi-pass-2').value.trim();
  const s3 = document.getElementById('wifi-ssid-3').value.trim();
  const p3 = document.getElementById('wifi-pass-3').value.trim();

  if (!s1) { showWiFiError('WiFi 1 (Primary) cannot be empty.'); return; }

  if (activeWiFiSSID && s1 !== activeWiFiSSID && s2 !== activeWiFiSSID && s3 !== activeWiFiSSID) {
    showWiFiError('Cannot remove "' + activeWiFiSSID + '" — it\'s the WiFi the device is currently using.');
    return;
  }

  const data = { ssid1: s1, pass1: p1, ssid2: s2, pass2: p2, ssid3: s3, pass3: p3, updatedAt: Date.now() };
  const btn = document.getElementById('wifi-apply-btn');
  const statusEl = document.getElementById('wifi-save-status');
  btn.innerText = 'Saving...';
  btn.disabled = true;

  const url = state.firebaseConfig.dbUrl.replace(/\/$/, '') + '/config/wifi.json';
  fetch(url, { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(data) })
    .then(res => {
      if (res.ok) {
        btn.innerText = '\u2713 Saved!';
        statusEl.classList.add('visible');
        statusEl.classList.remove('error');
        setTimeout(() => {
          btn.innerText = 'Apply Changes';
          btn.disabled = false;
          statusEl.classList.remove('visible');
        }, 3000);
      } else {
        throw new Error('HTTP ' + res.status);
      }
    })
    .catch(err => {
      btn.innerText = 'Error! Retry';
      btn.disabled = false;
      console.error('[WiFi] Save error:', err);
    });
}

function showWiFiError(msg) {
  const statusEl = document.getElementById('wifi-save-status');
  statusEl.innerText = '\u26A0 ' + msg;
  statusEl.classList.add('visible', 'error');
  setTimeout(() => {
    statusEl.classList.remove('visible', 'error');
    statusEl.innerText = '\u2713 Saved! ESP32 will pick up changes within ~5s.';
  }, 5000);
}

function togglePasswordVisibility(inputId, iconId) {
  const input = document.getElementById(inputId);
  const icon = document.getElementById(iconId);
  if (input.type === 'password') {
    input.type = 'text';
    icon.setAttribute('data-lucide', 'eye-off');
  } else {
    input.type = 'password';
    icon.setAttribute('data-lucide', 'eye');
  }
  try { if (window.lucide) window.lucide.createIcons(); } catch(e) {}
}
