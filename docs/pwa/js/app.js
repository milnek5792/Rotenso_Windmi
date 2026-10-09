/** Windmi PWA — ovládání přes MQTT (WSS). */

import { MqttBridge } from './mqtt-client.js?v=w2l';

const STORAGE_KEY = 'windmi-pwa-settings';
const MQTT_AUTO_KEY = 'windmi-pwa-mqtt-auto';

const DEFAULT_WSS =
  'wss://n9e16b3c.ala.eu-central-1.emqxsl.com:8084/mqtt';

const state = {
  mqttConnected: false,
  mqttStatus: 'idle',
  mqttError: '',
  tab5Online: false,
  teleFresh: false,
  watchActive: false,
  regMode: 'room',
  setpoint: null,
  eqOffset: null,
  power: false,
  pump: false,
  compressor: false,
  defrost: false,
  elec: false,
  lin: false,
  alarm: false,
  poruchaText: '',
  faultVisible: false,
  faultText: '',
  temps: {
    room: null,
    outdoor: null,
    inlet: null,
    outlet: null,
  },
};

const $ = (sel) => document.querySelector(sel);
const mqtt = new MqttBridge(applyMqttEvent);

const FAULT_SHOW_MS = 2000;
const FAULT_HIDE_MS = 800;
/** Bez telemetrie z Tab5 (crash bez LWT) zhasni kontrolky. */
const TELE_STALE_MS = 45000;
let faultTimer = null;
let faultPending = null;
let lastTeleAt = 0;
let staleWatchTimer = null;

function markTeleReceived() {
  lastTeleAt = Date.now();
}

function checkTeleStale() {
  if (!state.mqttConnected || state.mqttStatus !== 'connected') {
    return;
  }
  if (!state.tab5Online && !state.teleFresh) {
    return;
  }
  if (!lastTeleAt || Date.now() - lastTeleAt < TELE_STALE_MS) {
    return;
  }
  state.tab5Online = false;
  clearLiveSignals();
  render();
}

function startStaleWatch() {
  if (staleWatchTimer) {
    return;
  }
  staleWatchTimer = setInterval(checkTeleStale, 5000);
}

function resetTelemetryState() {
  state.tab5Online = false;
  state.teleFresh = false;
  state.watchActive = false;
  state.regMode = 'room';
  state.setpoint = null;
  state.eqOffset = null;
  state.power = false;
  state.pump = false;
  state.compressor = false;
  state.defrost = false;
  state.elec = false;
  state.lin = false;
  state.alarm = false;
  state.poruchaText = '';
  state.faultVisible = false;
  state.faultText = '';
  state.temps = { room: null, outdoor: null, inlet: null, outlet: null };
  clearTimeout(faultTimer);
  faultPending = null;
}

/** Zhasni provozní signály (MB/LED) — Tab5 offline nebo MQTT drop. */
function clearLiveSignals() {
  state.teleFresh = false;
  state.watchActive = false;
  state.lin = false;
  state.power = false;
  state.pump = false;
  state.compressor = false;
  state.defrost = false;
  state.elec = false;
  state.setpoint = null;
  state.eqOffset = null;
  state.temps = { room: null, outdoor: null, inlet: null, outlet: null };
  state.poruchaText = '';
  state.alarm = false;
  state.faultVisible = false;
  state.faultText = '';
  lastTeleAt = 0;
  clearTimeout(faultTimer);
  faultPending = null;
}

function isLinOk() {
  // Bez živého Tab5 / MQTT nesvítit MB ze stale retained/lin.
  if (!state.mqttConnected || !state.tab5Online || !state.teleFresh) {
    return false;
  }
  if (state.lin) {
    return true;
  }
  return (
    state.temps.inlet != null ||
    state.temps.outdoor != null ||
    state.pump ||
    state.compressor
  );
}

function scheduleFaultBanner() {
  if (!state.tab5Online || !state.teleFresh) {
    state.faultVisible = false;
    state.faultText = '';
    return;
  }

  const text = state.poruchaText.trim();
  const wantShow = text.length > 0 || state.alarm;
  const nextText = text || (state.alarm ? 'Alarm hlášen přes MQTT' : '');

  if (wantShow && state.faultVisible && state.faultText !== nextText && nextText) {
    state.faultText = nextText;
    return;
  }

  if (faultPending && faultPending.wantShow === wantShow && faultPending.text === nextText) {
    return;
  }

  clearTimeout(faultTimer);
  faultPending = { wantShow, text: nextText };

  faultTimer = setTimeout(() => {
    faultPending = null;
    if (!state.tab5Online || !state.teleFresh) {
      state.faultVisible = false;
      state.faultText = '';
      renderFaultBanner();
      return;
    }
    state.faultVisible = wantShow;
    state.faultText = wantShow ? nextText : '';
    renderFaultBanner();
  }, wantShow ? FAULT_SHOW_MS : FAULT_HIDE_MS);
}

function renderFaultBanner() {
  const faultBanner = $('#fault-banner');
  if (!faultBanner) {
    return;
  }
  if (state.faultVisible && state.faultText) {
    faultBanner.classList.remove('hidden');
    $('#fault-text').textContent = state.faultText;
  } else {
    faultBanner.classList.add('hidden');
    $('#fault-text').textContent = '';
  }
}

function loadSettings() {
  try {
    let raw = localStorage.getItem(STORAGE_KEY);
    // Migrace ze staré LG Therma PWA (stejný broker, nový topic prefix).
    if (!raw) {
      const legacy = localStorage.getItem('lgtherma-pwa-settings');
      if (legacy) {
        const parsed = JSON.parse(legacy);
        if (parsed && typeof parsed === 'object') {
          if (!parsed.prefix || parsed.prefix === 'lgtherma') {
            parsed.prefix = 'windmi';
          }
          localStorage.setItem(STORAGE_KEY, JSON.stringify(parsed));
          raw = JSON.stringify(parsed);
        }
      }
      const legacyAuto = localStorage.getItem('lgtherma-pwa-mqtt-auto');
      if (legacyAuto != null && localStorage.getItem(MQTT_AUTO_KEY) == null) {
        localStorage.setItem(MQTT_AUTO_KEY, legacyAuto);
      }
    }
    return raw ? JSON.parse(raw) : {};
  } catch {
    return {};
  }
}

function saveSettings(data) {
  const prev = loadSettings();
  localStorage.setItem(STORAGE_KEY, JSON.stringify({ ...prev, ...data }));
}

function formatTemp(v, decimals = 1) {
  if (v === null || v === undefined || Number.isNaN(v)) {
    return '—';
  }
  return decimals === 0 ? String(Math.round(v)) : v.toFixed(decimals);
}

function brokerConnected() {
  return mqtt.isConnected();
}

function displaySetpoint() {
  if (!brokerConnected()) {
    return '—';
  }
  if (state.regMode === 'room') {
    return formatTemp(state.setpoint, 1);
  }
  return formatTemp(state.setpoint, 0);
}

function formatOffset(v) {
  if (v === null || v === undefined || Number.isNaN(v)) {
    return '—';
  }
  const n = Math.round(v);
  return n > 0 ? `+${n}` : String(n);
}

function requireMqtt() {
  if (mqtt.isConnected() || mqtt.sessionActive()) {
    return true;
  }
  flashStatus('Nejdřív připoj MQTT v Nastavení');
  return false;
}

function sendCmd(suffix, payload, okLabel) {
  if (!requireMqtt()) {
    return false;
  }
  const rc = mqtt.publishCmd(suffix, payload);
  if (rc === 'fail') {
    flashStatus('MQTT neodpovídá — znovu Připojit');
    return false;
  }
  flashStatus(rc === 'queued' ? `… ${okLabel}` : `→ ${okLabel}`);
  return true;
}

function applyMqttEvent(ev) {
  if (ev.type === 'status') {
    state.mqttStatus = ev.status;
    state.mqttError = ev.error || '';
    state.mqttConnected = ev.status === 'connected';
    if (ev.status === 'connecting' || ev.status === 'idle' || ev.status === 'error') {
      state.tab5Online = false;
      clearLiveSignals();
    }
    render();
    return;
  }

  if (ev.type === 'mqtt') {
    state.mqttConnected = ev.connected;
    if (!ev.connected) {
      state.tab5Online = false;
      if (ev.reconnecting) {
        clearLiveSignals();
      } else {
        resetTelemetryState();
      }
    }
    render();
    return;
  }

  if (ev.type === 'tele') {
    markTeleReceived();
    state.teleFresh = true;
    const p = ev.patch;
    if (p.temps) {
      state.temps = { ...state.temps, ...p.temps };
    }
    if (p.setpoint !== undefined) {
      state.setpoint = p.setpoint;
    }
    if (p.regMode !== undefined) {
      state.regMode = p.regMode;
    }
    if (p.eqOffset !== undefined) {
      state.eqOffset = p.eqOffset;
    }
    if (p.power !== undefined) {
      state.power = p.power;
    }
    if (p.pump !== undefined) {
      state.pump = p.pump;
    }
    if (p.compressor !== undefined) {
      state.compressor = p.compressor;
    }
    if (p.lin !== undefined) {
      state.lin = p.lin;
    }
    if (p.defrost !== undefined) {
      state.defrost = p.defrost;
    }
    if (p.elec !== undefined) {
      state.elec = p.elec;
    }
    if (p.alarm !== undefined) {
      state.alarm = p.alarm;
    }
    if (p.poruchaText !== undefined) {
      state.poruchaText = p.poruchaText;
    }
    if (p.watchActive !== undefined) {
      state.watchActive = p.watchActive;
    }
    if (p.tab5Online !== undefined) {
      state.tab5Online = p.tab5Online;
      if (!p.tab5Online) {
        clearLiveSignals();
      }
    }
    scheduleFaultBanner();
    render();
  }
}

function clampRoomSp(v) {
  return Math.min(24, Math.max(18, Math.round(v * 2) / 2));
}

function adjustSetpoint(delta) {
  // Jen +/- jako Tab — nikdy absolutní hodnota.
  const sign = delta > 0 ? '+' : '-';
  const label =
    state.regMode === 'equitherm' ? `korekce ${sign}` : `setpoint ${sign}`;

  if (!sendCmd('setpoint', sign, label)) {
    return;
  }

  // Optimistický UI preview; tele/temp_set z Tabu je autorita.
  if (state.regMode === 'room' && typeof state.setpoint === 'number') {
    state.setpoint = clampRoomSp(
      state.setpoint + (delta > 0 ? 0.5 : -0.5),
    );
  } else if (state.regMode === 'equitherm') {
    state.eqOffset = (state.eqOffset ?? 0) + (delta > 0 ? 1 : -1);
  } else if (state.regMode === 'water' && typeof state.setpoint === 'number') {
    state.setpoint = Math.round(state.setpoint + (delta > 0 ? 1 : -1));
  }
  render();
}

function setRegMode(mode) {
  const m = mode === 'equitherm' || mode === 'water' ? mode : 'room';
  if (!sendCmd('mode', m, `mode ${m}`)) {
    return;
  }
  state.regMode = m;
  render();
}

function setPower(on) {
  if (!sendCmd('power', on ? 'ON' : 'OFF', on ? 'START' : 'STOP')) {
    return;
  }
  // Okamžitá odezva UI; tele z Tabu potvrdí (power/pump).
  state.power = on;
  if (!on) {
    state.pump = false;
    state.compressor = false;
  }
  render();
}

function flashStatus(msg) {
  const el = $('#sig-mqtt');
  if (!el) {
    return;
  }
  el.classList.add('sig-flash');
  el.setAttribute('aria-label', msg);
  setTimeout(() => {
    el.classList.remove('sig-flash');
    render();
  }, 1200);
}

function mqttSigMeta() {
  if (state.mqttStatus === 'connecting') {
    return { state: 'warn', label: 'MQTT: připojování', pulse: true };
  }
  if (state.mqttStatus === 'error') {
    return { state: 'error', label: `MQTT: ${state.mqttError || 'chyba'}`, pulse: false };
  }
  if (state.mqttConnected && state.tab5Online) {
    return { state: 'ok', label: 'MQTT: připojeno', pulse: false };
  }
  if (state.mqttConnected) {
    return { state: 'warn', label: 'MQTT: čekám na Tab5', pulse: false };
  }
  return { state: 'off', label: 'MQTT: odpojeno', pulse: false };
}

function linSigMeta() {
  if (!state.mqttConnected || state.mqttStatus === 'connecting') {
    return { state: 'off', label: 'MB: odpojeno', pulse: false };
  }
  if (!state.tab5Online) {
    return { state: 'off', label: 'MB: Tab5 offline', pulse: false };
  }
  if (isLinOk()) {
    return { state: 'ok', label: 'MB: OK', pulse: false };
  }
  if (state.teleFresh) {
    return { state: 'warn', label: 'MB: bez spojení', pulse: false };
  }
  return { state: 'off', label: 'MB: čekám na data', pulse: false };
}

function applySig(el, meta, extraClass = '') {
  if (!el) {
    return;
  }
  const extra = extraClass ? ` ${extraClass}` : '';
  el.className = `sig${extra} sig-${meta.state}${meta.pulse ? ' sig-pulse' : ''}`;
  el.setAttribute('aria-label', meta.label);
  el.setAttribute('title', meta.label);
}

const STAT_LABELS = {
  power: 'Provoz TČ',
  pump: 'Čerpadlo',
  compressor: 'Kompresor',
  defrost: 'Odmrazování',
  elec: 'El. topení',
};

function renderLeds() {
  const map = {
    power: state.power,
    pump: state.pump,
    compressor: state.compressor,
    defrost: state.defrost,
    elec: state.elec,
  };
  for (const [key, on] of Object.entries(map)) {
    const el = $(`#stat-${key}`);
    if (!el) {
      continue;
    }
    el.classList.toggle('stat-on', !!on);
    el.dataset.on = on ? '1' : '0';
    const name = STAT_LABELS[key] || key;
    const label = `${name}: ${on ? 'zapnuto' : 'vypnuto'}`;
    el.setAttribute('aria-label', label);
    el.setAttribute('title', label);
  }
}

function mqttSessionActive() {
  return mqtt.sessionActive();
}

function render() {
  applySig($('#sig-mqtt'), mqttSigMeta());
  applySig($('#sig-lin'), linSigMeta(), 'sig-label');

  renderFaultBanner();

  const title = $('#sp-title');
  const hint = $('#sp-hint');
  if (state.regMode === 'equitherm') {
    title.textContent = 'Ekvitermní SP vody';
    title.className = 'sp-title sp-ekv';
    if (brokerConnected() && state.eqOffset != null) {
      hint.textContent = `Korekce ${formatOffset(state.eqOffset)} °C`;
      hint.classList.remove('hidden');
    } else {
      hint.textContent = '';
      hint.classList.add('hidden');
    }
  } else if (state.regMode === 'water') {
    title.textContent = 'Nastavení teploty vody';
    title.className = 'sp-title sp-water';
    hint.textContent = '';
    hint.classList.add('hidden');
  } else {
    title.textContent = 'Nastavení pokojové teploty';
    title.className = 'sp-title sp-room';
    hint.textContent = '';
    hint.classList.add('hidden');
  }

  const spVal = $('#sp-value');
  spVal.textContent = displaySetpoint();

  document.querySelectorAll('.mode-row .chip').forEach((btn) => {
    const mode = btn.dataset.mode;
    const active = mode === state.regMode;
    btn.classList.toggle('chip-active', active);
    btn.classList.toggle('chip-ekv', active && mode === 'equitherm');
    btn.classList.toggle('chip-water', active && mode === 'water');
  });

  const live = brokerConnected();
  $('#temp-room').textContent = formatTemp(live ? state.temps.room : null, 1);
  $('#temp-outdoor').textContent = formatTemp(live ? state.temps.outdoor : null, 1);
  $('#temp-inlet').textContent = formatTemp(live ? state.temps.inlet : null, 0);
  $('#temp-outlet').textContent = formatTemp(live ? state.temps.outlet : null, 0);

  renderLeds();

  const btnStart = $('#btn-start');
  const btnStop = $('#btn-stop');
  if (btnStart) {
    btnStart.classList.toggle('btn-power-on', !!state.power);
    btnStart.setAttribute('aria-pressed', state.power ? 'true' : 'false');
  }
  if (btnStop) {
    btnStop.classList.toggle('btn-power-on', !state.power && state.mqttConnected);
    btnStop.setAttribute('aria-pressed', !state.power ? 'true' : 'false');
  }

  const sessionActive = mqttSessionActive();
  $('#btn-connect').disabled = sessionActive;
  $('#btn-disconnect').disabled = !sessionActive;

  const statusEl = $('#cfg-status');
  if (statusEl) {
    if (state.mqttStatus === 'error') {
      statusEl.textContent = state.mqttError;
      statusEl.className = 'cfg-status cfg-status-error';
    } else if (state.mqttConnected) {
      statusEl.textContent = state.tab5Online
        ? 'Připojeno — Tab5 online'
        : 'Připojeno — čekám na windmi/availability';
      statusEl.className = 'cfg-status cfg-status-ok';
    } else if (state.mqttStatus === 'connecting') {
      statusEl.textContent = state.mqttError || 'Připojování…';
      statusEl.className = 'cfg-status';
    } else {
      statusEl.textContent = '';
      statusEl.className = 'cfg-status';
    }
  }
}

function showView(name) {
  $('#view-home').classList.toggle('view-active', name === 'home');
  $('#view-home').hidden = name !== 'home';
  $('#view-settings').classList.toggle('view-active', name === 'settings');
  $('#view-settings').hidden = name !== 'settings';
  document.querySelectorAll('.nav-btn').forEach((btn) => {
    btn.classList.toggle('nav-active', btn.dataset.view === name);
  });
}

function bindNav() {
  document.querySelectorAll('.nav-btn').forEach((btn) => {
    btn.addEventListener('click', () => showView(btn.dataset.view));
  });
}

function bindTap(el, fn) {
  if (!el) {
    return;
  }
  let last = 0;
  const run = (ev) => {
    if (ev) {
      ev.preventDefault();
    }
    const now = Date.now();
    if (now - last < 350) {
      return;
    }
    last = now;
    fn();
  };
  // pointerup = spolehlivý tap na mobilu; click = klávesnice / desktop fallback
  el.addEventListener('pointerup', (ev) => {
    if (ev.button != null && ev.button !== 0) {
      return;
    }
    run(ev);
  });
  el.addEventListener('click', (ev) => {
    if (ev.detail === 0) {
      run(ev);
    }
  });
}

function bindControls() {
  bindTap($('#btn-minus'), () => adjustSetpoint(-1));
  bindTap($('#btn-plus'), () => adjustSetpoint(1));
  bindTap($('#btn-start'), () => setPower(true));
  bindTap($('#btn-stop'), () => setPower(false));
  document.querySelectorAll('.mode-row .chip').forEach((btn) => {
    bindTap(btn, () => setRegMode(btn.dataset.mode));
  });
}

function bindSettings() {
  const saved = loadSettings();
  $('#cfg-host').value = saved.host || DEFAULT_WSS;
  $('#cfg-user').value = saved.user || '';
  $('#cfg-pass').value = saved.password || '';
  $('#cfg-prefix').value = saved.prefix || 'windmi';

  $('#btn-connect').addEventListener('click', () => {
    const host = $('#cfg-host').value.trim();
    const user = $('#cfg-user').value.trim();
    const password = $('#cfg-pass').value;
    const prefix = mqtt.normalizePrefix($('#cfg-prefix').value);
    $('#cfg-prefix').value = prefix;
    if (!host) {
      state.mqttStatus = 'error';
      state.mqttError = 'Zadej URL brokeru (WSS)';
      render();
      return;
    }
    saveSettings({ host, user, password, prefix });
    localStorage.setItem(MQTT_AUTO_KEY, 'true');
    mqtt.connect({ url: host, user, password, prefix });
  });

  $('#btn-disconnect').addEventListener('click', () => {
    localStorage.setItem(MQTT_AUTO_KEY, 'false');
    mqtt.disconnect(true);
  });
}

function tryAutoConnect() {
  if (localStorage.getItem(MQTT_AUTO_KEY) === 'false') {
    return;
  }
  const saved = loadSettings();
  if (saved.host && saved.user && saved.password) {
    const prefix = mqtt.normalizePrefix(saved.prefix);
    if (prefix !== saved.prefix) {
      saveSettings({ prefix });
    }
    mqtt.connect({
      url: saved.host,
      user: saved.user,
      password: saved.password,
      prefix,
    });
  }
}

async function purgeStaleWorkersAndCaches() {
  if (!('serviceWorker' in navigator)) {
    return;
  }
  const here = `${location.origin}${location.pathname.replace(/\/[^/]*$/, '/')}`;
  const regs = await navigator.serviceWorker.getRegistrations();
  await Promise.all(
    regs.map(async (reg) => {
      const scope = reg.scope || '';
      // Starý SW z kořene /Rotenso_Windmi/ by jinak ovládal i /w2/.
      if (!scope.startsWith(here)) {
        await reg.unregister();
      }
    }),
  );
  if (window.caches) {
    const keys = await caches.keys();
    await Promise.all(
      keys
        .filter((k) => k.startsWith('lg-therma') || k.startsWith('windmi-pwa-v'))
        .map((k) => caches.delete(k)),
    );
  }
}

function registerSw() {
  if (!('serviceWorker' in navigator)) {
    return;
  }
  purgeStaleWorkersAndCaches()
    .catch(() => {})
    .finally(() => {
      navigator.serviceWorker.register('./sw.js?v=w2l').catch(() => {});
    });
}

function init() {
  bindNav();
  bindControls();
  bindSettings();
  startStaleWatch();
  render();
  // Jen watch OFF při opravdovém odchodu — ne full disconnect (mobilní bfcache/resume).
  window.addEventListener('pagehide', (ev) => {
    if (ev.persisted) {
      return;
    }
    if (mqtt.isConnected()) {
      mqtt.publishWatch(false);
    }
  });
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible' && mqtt.isConnected()) {
      mqtt.publishWatch(true);
    }
  });
  registerSw();
  tryAutoConnect();
}

document.addEventListener('DOMContentLoaded', init);
