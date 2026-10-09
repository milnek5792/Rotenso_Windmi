/** MQTT bridge — windmi/* topicy kompatibilní s Tab5 (net_mqtt_client.cpp). */

/** Retained zprávy ignorované jen krátce po připojení (kvůli blikání poruchy). */
const HYDRATE_SKIP_RETAINED = new Set(['tele/alarm', 'tele/porucha']);
const TELE_NA = '___';

/**
 * Málo subscriptions — EMQX Serverless má nízký limit (~10/client).
 * Individuální tele/* topiců je víc než limit → compressor/defrost se
 * neodebíraly. Wildcard tele/# + availability = 2 odběry.
 */
const SUBSCRIBE_FILTERS = ['availability', 'tele/#'];

/** Fronta příkazů přes krátký reconnect (mobilní PWA často dropne WS). */
const MAX_PENDING_CMDS = 8;

export class MqttBridge {
  constructor(onState) {
    this.onState = onState;
    this.client = null;
    this.prefix = 'windmi';
    this.watchTimer = null;
    this.status = 'idle';
    this.error = '';
    this.hydrateUntil = 0;
    this.hydrateTimer = null;
    this.closing = false;
    this.userStopped = false;
    this.pendingCmds = [];
  }

  sessionActive() {
    if (this.userStopped) {
      return false;
    }
    return this.status === 'connected' || this.status === 'connecting';
  }

  isConnected() {
    return this.client?.connected === true;
  }

  topic(suffix) {
    return `${this.prefix}/${suffix}`;
  }

  normalizePrefix(prefix) {
    return String(prefix || 'windmi')
      .trim()
      .replace(/\/+$/, '')
      .toLowerCase() || 'windmi';
  }

  connect({ url, user, password, prefix }) {
    if (typeof mqtt === 'undefined') {
      this.setStatus('error', 'mqtt.js se nenačetlo');
      return;
    }
    this.disconnect(false);
    this.userStopped = false;
    this.prefix = this.normalizePrefix(prefix);
    this.setStatus('connecting', '');

    const clientId = `WindmiPWA_${Math.random().toString(16).slice(2, 10)}`;
    this.client = mqtt.connect(url, {
      username: user || undefined,
      password: password || undefined,
      clientId,
      reconnectPeriod: 3000,
      connectTimeout: 20_000,
      keepalive: 45,
      clean: true,
      queueQoSZero: true,
    });

    this.client.on('connect', () => {
      this.beginHydrate();
      this.setStatus('connected', '');
      this.subscribeAll();
      this.publishWatch(true);
      this.startWatchKeepalive();
      this.flushPendingCmds();
    });

    this.client.on('reconnect', () => {
      this.setStatus('connecting', 'Obnovuji spojení…');
    });

    this.client.on('close', () => {
      this.stopWatchKeepalive();
      if (this.userStopped) {
        if (this.status !== 'error') {
          this.setStatus('idle', '');
        }
        this.onState({ type: 'mqtt', connected: false, reconnecting: false });
        return;
      }
      this.setStatus('connecting', 'Obnovuji spojení…');
      this.onState({ type: 'mqtt', connected: false, reconnecting: true });
    });

    this.client.on('error', (err) => {
      this.setStatus('error', err?.message || 'Chyba MQTT');
    });

    this.client.on('message', (topic, payload, packet) => {
      this.handleMessage(topic, payload.toString(), packet?.retain === true);
    });
  }

  disconnect(sendWatchOff = true) {
    this.userStopped = true;
    this.closing = true;
    this.pendingCmds = [];
    this.stopWatchKeepalive();
    this.endHydrate();
    if (this.client) {
      const client = this.client;
      this.client = null;
      client.removeAllListeners();
      if (sendWatchOff && client.connected) {
        try {
          client.publish(this.topic('cmd/watch'), 'OFF', { qos: 0 });
        } catch {
          /* ignore */
        }
      }
      try {
        client.end(true);
      } catch {
        /* ignore */
      }
    }
    this.setStatus('idle', '');
    this.onState({ type: 'mqtt', connected: false, reconnecting: false });
    this.closing = false;
  }

  setStatus(status, error) {
    this.status = status;
    this.error = error || '';
    this.onState({ type: 'status', status: this.status, error: this.error });
  }

  beginHydrate() {
    this.endHydrate();
    this.hydrateUntil = Date.now() + 3500;
    this.hydrateTimer = setTimeout(() => {
      this.hydrateUntil = 0;
      this.hydrateTimer = null;
      if (this.client?.connected) {
        this.publishWatch(true);
      }
    }, 3600);
  }

  endHydrate() {
    if (this.hydrateTimer) {
      clearTimeout(this.hydrateTimer);
      this.hydrateTimer = null;
    }
    this.hydrateUntil = 0;
  }

  shouldSkipRetained(rel) {
    return (
      Date.now() < this.hydrateUntil &&
      HYDRATE_SKIP_RETAINED.has(rel)
    );
  }

  subscribeAll() {
    if (!this.client?.connected) {
      return;
    }
    for (const suffix of SUBSCRIBE_FILTERS) {
      this.client.subscribe(this.topic(suffix), { qos: 0 });
    }
  }

  publishWatch(on) {
    if (!this.client?.connected) {
      return;
    }
    this.client.publish(this.topic('cmd/watch'), on ? 'ON' : 'OFF', { qos: 0 });
  }

  startWatchKeepalive() {
    this.stopWatchKeepalive();
    this.watchTimer = setInterval(() => {
      if (this.client?.connected) {
        this.publishWatch(true);
      }
    }, 60_000);
  }

  stopWatchKeepalive() {
    if (this.watchTimer) {
      clearInterval(this.watchTimer);
      this.watchTimer = null;
    }
  }

  enqueuePending(suffix, payload) {
    this.pendingCmds.push({ suffix, payload });
    while (this.pendingCmds.length > MAX_PENDING_CMDS) {
      this.pendingCmds.shift();
    }
  }

  flushPendingCmds() {
    if (!this.client?.connected || this.pendingCmds.length === 0) {
      return;
    }
    const batch = this.pendingCmds.splice(0, this.pendingCmds.length);
    for (const cmd of batch) {
      this.publishCmdNow(cmd.suffix, cmd.payload);
    }
  }

  publishCmdNow(suffix, payload) {
    const topic = this.topic(`cmd/${suffix}`);
    // qos 0 — qos 1 po pádu Tab5 znovu doručí burst a znovu ho shodí
    this.client.publish(topic, String(payload), { qos: 0 }, (err) => {
      if (err) {
        console.warn('[MQTT] publish fail', topic, err);
        this.enqueuePending(suffix, payload);
      }
    });
    if (suffix !== 'watch') {
      this.publishWatch(true);
    }
  }

  /**
   * Pošle cmd/* (qos 1). Při krátkém výpadku frontuje a odešle po reconnectu.
   * @returns {'ok'|'queued'|'fail'}
   */
  publishCmd(suffix, payload) {
    if (this.userStopped) {
      return 'fail';
    }
    if (this.client?.connected) {
      this.publishCmdNow(suffix, payload);
      return 'ok';
    }
    if (this.sessionActive()) {
      this.enqueuePending(suffix, payload);
      return 'queued';
    }
    return 'fail';
  }

  handleMessage(topic, raw, retained = false) {
    try {
      this.handleMessageInner(topic, raw, retained);
    } catch (err) {
      console.error('[MQTT] parse', topic, err);
    }
  }

  handleMessageInner(topic, raw, retained = false) {
    const base = `${this.prefix}/`;
    if (!topic.startsWith(base)) {
      return;
    }
    const rel = topic.slice(base.length);
    if (retained && this.shouldSkipRetained(rel)) {
      return;
    }
    const msg = raw.trim();
    const patch = {};

    switch (rel) {
      case 'availability':
        patch.tab5Online = msg.toLowerCase() === 'online';
        break;
      case 'tele/temp_room': {
        const v = parseTemp(msg);
        if (v !== null) {
          patch.temps = { room: v };
        }
        break;
      }
      case 'tele/temp_outdoor': {
        const v = parseTemp(msg);
        if (v !== null) {
          patch.temps = { outdoor: v };
        }
        break;
      }
      case 'tele/temp_inlet': {
        const v = parseTemp(msg);
        if (v !== null) {
          patch.temps = { inlet: Math.round(v) };
        }
        break;
      }
      case 'tele/temp_outlet': {
        const v = parseTemp(msg);
        if (v !== null) {
          patch.temps = { outlet: Math.round(v) };
        }
        break;
      }
      case 'tele/temp_set': {
        const v = parseTemp(msg);
        if (v !== null) {
          patch.setpoint = v;
        } else if (msg === TELE_NA || msg === '---' || msg.toLowerCase() === 'off') {
          patch.setpoint = null;
        }
        break;
      }
      case 'tele/reg_mode':
        patch.regMode = normalizeRegMode(msg);
        break;
      case 'tele/eq_offset': {
        const v = parseTemp(msg);
        if (v !== null) {
          patch.eqOffset = v;
        }
        break;
      }
      case 'tele/power':
        patch.power = parseOnOff(msg);
        break;
      case 'tele/pump':
        patch.pump = parseOnOff(msg);
        break;
      case 'tele/compressor':
        patch.compressor = parseOnOff(msg);
        break;
      case 'tele/lin':
        patch.lin = parseOnOff(msg);
        break;
      case 'tele/defrost':
        patch.defrost = parseOnOff(msg);
        break;
      case 'tele/elec_heat':
        patch.elec = parseOnOff(msg);
        break;
      case 'tele/alarm':
        patch.alarm = parseOnOff(msg);
        break;
      case 'tele/porucha':
        patch.poruchaText = msg;
        break;
      case 'tele/watch':
        patch.watchActive = parseOnOff(msg);
        break;
      default:
        return;
    }

    this.onState({ type: 'tele', patch });
  }
}

function normalizeRegMode(v) {
  const s = String(v || '').trim().toLowerCase();
  if (s === 'room' || s === 'auto' || s === 'pokoj') {
    return 'room';
  }
  if (s === 'equitherm' || s === 'ekv' || s === 'ekviterm' || s === 'ekvitermá') {
    return 'equitherm';
  }
  if (s === 'water' || s === 'manual' || s === 'voda' || s === 'rucni' || s === 'vystupni') {
    return 'water';
  }
  return 'room';
}

function parseOnOff(v) {
  const s = String(v).trim().toUpperCase();
  if (!s || s === 'OFF' || s === '0' || s === 'FALSE' || s === 'STOP' ||
      s === TELE_NA || s === '---') {
    return false;
  }
  if (s === 'ON' || s === '1' || s === 'TRUE' || s === 'START' || s === 'YES') {
    return true;
  }
  // Číselná frekvence (Hz) — kompresor běží
  const n = Number.parseFloat(s);
  return Number.isFinite(n) && n > 0;
}

function parseTemp(v) {
  if (!v || v === TELE_NA || v === '---' || v === 'off') {
    return null;
  }
  const n = Number.parseFloat(v);
  return Number.isFinite(n) ? n : null;
}
