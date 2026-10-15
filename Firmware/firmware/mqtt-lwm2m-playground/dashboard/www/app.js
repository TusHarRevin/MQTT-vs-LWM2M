/* MQTT vs LwM2M playground - dashboard logic.
 * Data sources:
 *   /api/events  Server-Sent Events: device protocol events, lab events,
 *                broker deliveries, Leshan events (all JSON, field "k")
 *   /api/state   servers + devices snapshot (polled)
 *   /api/runs    completed benchmark summaries
 *   /leshan/...  Leshan REST API (proxied by the lab)
 * Every value shown comes from one of those; nothing is invented here. */
(function () {
  "use strict";

  const $ = (s, r) => (r || document).querySelector(s);
  const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));
  const C = { MQTT: "var(--mqtt)", LWM2M: "var(--lwm2m)", OTA: "var(--ota)" };
  const store = {
    get(k, d) { try { const v = localStorage.getItem("pg." + k); return v == null ? d : v; } catch (e) { return d; } },
    set(k, v) { try { localStorage.setItem("pg." + k, v); } catch (e) { /* private mode */ } },
  };

  const S = {
    state: null, runs: [], live: { mqtt: [], lwm2m: [] }, handshake: {}, values: {},
    selected: "/3303/0/5700", filter: "all", sseUp: false, observedFor: null, exitWaiters: {},
    exp: { running: false, cancel: false }, ota: { phase: "", done: 0, total: 0 },
  };

  /* ---------------------------------------------------------------- helpers */
  async function api(path, opts) {
    const r = await fetch(path, opts);
    const txt = await r.text();
    let j = null;
    try { j = txt ? JSON.parse(txt) : {}; } catch (e) { j = { raw: txt }; }
    if (!r.ok) throw new Error((j && j.error) || r.status + " " + r.statusText);
    return j;
  }
  /* Device target: the PC simulation or the real board. Only START needs to
   * say which; the lab remembers where each run went for stop/commands. */
  const targetSel = $("#target");
  targetSel.value = store.get("target", "pc") === "hw" ? "hw" : "pc";
  targetSel.addEventListener("change", () => { store.set("target", targetSel.value); pills(); });
  const post = (path, params) => {
    if (path === "/api/device/start" && targetSel.value === "hw") params = Object.assign({ target: "hw" }, params);
    return api(path + "?" + new URLSearchParams(params || {}), { method: "POST" });
  };
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const clock = (t) => { const d = new Date(t); return d.toTimeString().slice(0, 8) + "." + String(d.getMilliseconds()).padStart(3, "0"); };
  const nm = (v, unit, digits) => (v == null || !isFinite(v) ? null : (+v).toFixed(digits == null ? 1 : digits) + (unit || ""));
  /* Coalesce redraws: events can arrive by the hundred (and thousands when the
   * replay buffer is sent on page load); each view redraws at most ~8x/s. */
  const queued = {};
  function later(name, fn) {
    if (queued[name]) return;
    queued[name] = true;
    setTimeout(() => { queued[name] = false; fn(); }, 120);
  }
  function setKV(dl, rows) {
    dl.textContent = "";
    rows.forEach(([k, v, cls]) => {
      const dt = document.createElement("dt"); dt.textContent = k;
      const dd = document.createElement("dd");
      if (v == null || v === "") { dd.textContent = "—"; dd.className = "nm"; }
      else if (cls) { const s = document.createElement("span"); s.className = "status " + cls; s.textContent = v; dd.appendChild(s); }
      else dd.textContent = v;
      dl.append(dt, dd);
    });
  }
  function segValue(id) { const b = $("#" + id + " button.on"); return b ? b.dataset.v : null; }
  $$(".seg").forEach((seg) => seg.addEventListener("click", (e) => {
    const b = e.target.closest("button"); if (!b) return;
    $$("button", seg).forEach((x) => x.classList.toggle("on", x === b));
    seg.dispatchEvent(new CustomEvent("change", { detail: b.dataset.v }));
  }));
  function hint(id, msg, bad) { const h = $(id); h.textContent = msg || ""; h.style.color = bad ? "var(--critical)" : ""; }

  /* ---------------------------------------------------------------- tabs & theme */
  function showTab(name) {
    $$(".tabs button").forEach((b) => b.classList.toggle("active", b.dataset.tab === name));
    $$("section.tab").forEach((s) => (s.hidden = s.id !== "tab-" + name));
    store.set("tab", name);
    renderCharts();
  }
  $$(".tabs button").forEach((b) => b.addEventListener("click", () => showTab(b.dataset.tab)));
  const savedTheme = store.get("theme", "");
  if (savedTheme) document.documentElement.dataset.theme = savedTheme;
  $("#theme").addEventListener("click", () => {
    const dark = document.documentElement.dataset.theme
      ? document.documentElement.dataset.theme === "dark"
      : matchMedia("(prefers-color-scheme: dark)").matches;
    document.documentElement.dataset.theme = dark ? "light" : "dark";
    store.set("theme", document.documentElement.dataset.theme);
    renderCharts();
  });

  /* ---------------------------------------------------------------- network controls */
  const PROFILES = { excellent: [0, 0], good: [50, 1], poor: [250, 5], "very-poor": [1000, 10] };
  $$(".netbox").forEach((box) => {
    const p = box.dataset.net;
    box.innerHTML =
      `<div class="field"><label for="${p}-prof">Network profile</label><select id="${p}-prof">
        <option value="excellent">Excellent · 0 ms, 0 %</option><option value="good">Good · 50 ms, 1 %</option>
        <option value="poor">Poor · 250 ms, 5 %</option><option value="very-poor">Very Poor · 1000 ms, 10 %</option>
        <option value="custom">Custom</option></select></div>
      <div class="netrow"><div class="field"><label for="${p}-lat">Network latency</label><select id="${p}-lat">
        <option value="0">Normal</option><option>50</option><option>100</option><option>250</option><option>500</option><option>1000</option></select></div>
      <div class="field"><label for="${p}-loss">Packet loss</label><select id="${p}-loss">
        <option value="0">0 %</option><option value="1">1 %</option><option value="2">2 %</option><option value="5">5 %</option><option value="10">10 %</option></select></div></div>`;
    $$("#" + p + "-lat option").forEach((o) => {
      o.value = o.value; /* pin the value before relabelling (no value attr = text) */
      if (o.value !== "0") o.textContent = o.value + " ms";
    });
    $("#" + p + "-prof").addEventListener("change", (e) => {
      const v = PROFILES[e.target.value];
      if (v) { $("#" + p + "-lat").value = String(v[0]); $("#" + p + "-loss").value = String(v[1]); }
    });
    ["lat", "loss"].forEach((k) => $("#" + p + "-" + k).addEventListener("change", () => {
      const lat = +$("#" + p + "-lat").value, loss = +$("#" + p + "-loss").value;
      const match = Object.entries(PROFILES).find(([, v]) => v[0] === lat && v[1] === loss);
      $("#" + p + "-prof").value = match ? match[0] : "custom";
    }));
  });
  const net = (p) => ({ latency: $("#" + p + "-lat").value, loss: $("#" + p + "-loss").value });

  /* ---------------------------------------------------------------- sequence diagrams */
  function Seq(root) {
    const left = root.dataset.left, right = root.dataset.right, max = 11, items = [];
    function layout() {
      root.textContent = "";
      const lx = 18, rx = 82;
      [[left, lx], [right, rx]].forEach(([n, x]) => {
        const a = document.createElement("div"); a.className = "actor"; a.style.left = x + "%"; a.textContent = n; root.appendChild(a);
        const l = document.createElement("div"); l.className = "lifeline"; l.style.left = x + "%"; root.appendChild(l);
      });
      if (!items.length) {
        const e = document.createElement("div"); e.className = "empty";
        e.textContent = "Press START - every protocol message appears here as an arrow. Click one to inspect it.";
        root.appendChild(e);
      }
      items.forEach((it, i) => {
        const toRight = it.ev.from === "DEVICE";
        const m = document.createElement("div");
        m.className = "msg " + (toRight ? "r" : "l") + (i === items.length - 1 && it.fresh ? " new" : "");
        m.style.left = lx + "%"; m.style.right = 100 - rx + "%"; m.style.top = 36 + i * 26 + "px";
        m.style.setProperty("--c", C[it.ev.proto] || "var(--ink-2)");
        m.style.setProperty("--from", toRight ? "left" : "right");
        const line = document.createElement("div"); line.className = "line";
        const head = document.createElement("div"); head.className = "head";
        const lbl = document.createElement("div"); lbl.className = "lbl";
        const b = document.createElement("b"); b.textContent = it.ev.type;
        lbl.appendChild(b);
        const extra = shortDetail(it.ev);
        if (extra) { const s = document.createElement("span"); s.className = "muted"; s.textContent = "  " + extra; lbl.appendChild(s); }
        m.title = (it.ev.detail || "") + (it.ev.lat != null ? " · " + it.ev.lat.toFixed(1) + " ms" : "");
        m.append(line, head, lbl);
        m.addEventListener("click", () => inspect(it.ev));
        root.appendChild(m);
        it.fresh = false;
      });
    }
    layout();
    return {
      add(ev) { items.push({ ev, fresh: true }); while (items.length > max) items.shift(); later("seq-" + root.id, layout); },
      clear() { items.length = 0; layout(); },
    };
  }
  function shortDetail(ev) {
    const parts = [];
    if (ev.qos != null && ev.proto === "MQTT") parts.push("q" + ev.qos);
    if (ev.uri && ev.proto === "LWM2M") parts.push(ev.uri.length > 22 ? ev.uri.slice(0, 22) + "…" : ev.uri);
    if (ev.lat != null) parts.push(ev.lat.toFixed(1) + " ms");
    else if (ev.len != null) parts.push(ev.len + " B");
    return parts.join(" · ");
  }
  const seq = { mqtt: Seq($("#m-seq")), lwm2m: Seq($("#l-seq")) };

  const QOSFLOW = {
    0: "CLIENT\n  |\n  | PUBLISH (QoS 0, no packet id)\n  v\nBROKER          \"at most once\" - no acknowledgement.\n\nThe playground follows it with PINGREQ -> PINGRESP:\nthe broker answers in TCP order, so PINGRESP proves\nthe PUBLISH was received. That time is the QoS 0 latency.",
    1: "CLIENT\n  |\n  | PUBLISH (QoS 1, packet id n)\n  v\nBROKER\n  |\n  | PUBACK (id n)          \"at least once\"\n  v\nCLIENT\n\nLatency = PUBLISH -> PUBACK.",
    2: "CLIENT\n  | PUBLISH (QoS 2, id n)\n  v\nBROKER   stores message\n  | PUBREC (id n)\n  v\nCLIENT\n  | PUBREL (id n)\n  v\nBROKER   delivers, forgets id\n  | PUBCOMP (id n)         \"exactly once\"\n  v\nCLIENT\n\nLatency = PUBLISH -> PUBCOMP (two round trips).",
  };
  function showQos() { $("#m-qosflow").textContent = QOSFLOW[segValue("m-qos")]; $("#m-flowtitle").textContent = "QoS " + segValue("m-qos"); }
  $("#m-qos").addEventListener("change", showQos);
  showQos();

  /* ---------------------------------------------------------------- event log + inspector */
  const logEl = $("#log");
  let logCount = 0;
  function logLine(ev, cls, text, lat) {
    const d = document.createElement("div");
    d.dataset.cat = cls;
    const t = document.createElement("span"); t.className = "t"; t.textContent = "[" + clock(ev.t || Date.now()) + "] ";
    const p = document.createElement("span"); p.className = "p " + (ev.proto || cls); p.textContent = ev.proto || cls;
    d.append(t, p, document.createTextNode(text));
    if (lat != null) { const l = document.createElement("span"); l.className = "lat"; l.textContent = "  " + lat.toFixed(1) + " ms"; d.appendChild(l); }
    d.addEventListener("click", () => { $$(".logbody div.sel").forEach((x) => x.classList.remove("sel")); d.classList.add("sel"); inspect(ev); });
    d.hidden = !matchFilter(cls);
    logEl.appendChild(d);
    if (++logCount > 1500) { logEl.removeChild(logEl.firstChild); logCount--; }
    if ($("#log-follow").checked) later("scroll", () => (logEl.scrollTop = logEl.scrollHeight));
  }
  function matchFilter(cat) { return S.filter === "all" || S.filter === cat; }
  $("#log-filter").addEventListener("change", (e) => {
    S.filter = e.detail;
    $$("#log > div").forEach((d) => (d.hidden = !matchFilter(d.dataset.cat)));
  });

  function hexdump(hex, total) {
    const bytes = hex.match(/../g) || [];
    const lines = [];
    for (let i = 0; i < bytes.length; i += 16) {
      const row = bytes.slice(i, i + 16);
      const ascii = row.map((h) => { const c = parseInt(h, 16); return c >= 32 && c < 127 ? String.fromCharCode(c) : "."; }).join("");
      lines.push(i.toString(16).padStart(4, "0") + "  " + row.join(" ").padEnd(48) + "  " + ascii);
    }
    if (total > bytes.length) lines.push(`… ${total - bytes.length} more bytes not shown`);
    return lines.join("\n");
  }
  function inspect(ev) {
    const box = $("#insp");
    box.textContent = "";
    const dl = document.createElement("dl"); dl.className = "kv";
    const dir = ev.from ? ev.from + " → " + (ev.to || "") : ev.k === "broker" ? "BROKER → LAB SUBSCRIBER" : ev.k === "leshan" ? "LESHAN (server view)" : "";
    const rows = [
      ["Protocol", ev.proto || (ev.k === "broker" ? "MQTT" : ev.k === "leshan" ? "LWM2M" : ev.k)],
      ["Direction", dir],
      ["Message type", ev.type || ev.event || ev.k],
      ["Message length", ev.len != null ? ev.len + " B" : null],
      ["Payload length", ev.plen != null ? ev.plen + " B" : null],
      ["Wire bytes", ev.wire != null ? ev.wire + " B (incl. security overhead)" : null],
      ["Security", ev.sec || null],
      ["Timestamp", ev.t ? new Date(ev.t).toISOString().replace("T", " ").replace("Z", " UTC") : null],
      ["Latency", ev.lat != null ? ev.lat.toFixed(3) + " ms" : null],
    ];
    if (ev.qos != null) rows.push(["QoS", String(ev.qos)]);
    if (ev.topic) rows.push(["Topic", ev.topic]);
    if (ev.id != null && ev.proto === "MQTT") rows.push(["Packet id", String(ev.id)]);
    if (ev.mid != null) rows.push(["CoAP", `${ev.ctype} ${ev.code}  mid=${ev.mid}`]);
    if (ev.uri) rows.push(["URI", ev.uri]);
    if (ev.cf) rows.push(["Content-Format", ev.cf]);
    if (ev.retransmit) rows.push(["Note", "CoAP retransmission (same Message ID)"]);
    if (ev.detail) rows.push(["Detail", ev.detail]);
    if (ev.cipher) rows.push(["Cipher", ev.cipher]);
    if (ev.hs_tx != null) rows.push(["Handshake bytes", `sent ${ev.hs_tx} B · received ${ev.hs_rx} B`]);
    if (ev.dev) rows.push(["Device", ev.dev]);
    if (ev.run) rows.push(["Run", ev.run]);
    setKV(dl, rows);
    box.appendChild(dl);
    const payload = ev.payload || ev.msg || (ev.k === "leshan" ? JSON.stringify(ev.data, null, 1) : null);
    if (payload) { const p = document.createElement("div"); p.className = "payload"; p.textContent = payload; box.appendChild(p); }
    if (ev.raw) {
      const h = document.createElement("div"); h.className = "muted"; h.style.marginTop = "8px";
      h.textContent = "Raw bytes (" + (ev.rawlen || ev.raw.length / 2) + " B" + (ev.sec && ev.sec !== "none" ? ", before encryption" : "") + ")";
      const pre = document.createElement("pre"); pre.className = "hex"; pre.textContent = hexdump(ev.raw, ev.rawlen || 0);
      box.append(h, pre);
    }
  }

  /* ---------------------------------------------------------------- SSE */
  function connectEvents() {
    const es = new EventSource("/api/events");
    es.onopen = () => { S.sseUp = true; pills(); };
    es.onerror = () => { S.sseUp = false; pills(); };
    es.onmessage = (m) => { let ev; try { ev = JSON.parse(m.data); } catch (e) { return; } onEvent(ev); };
  }

  function onEvent(ev) {
    switch (ev.k) {
      case "ev": {
        const cat = ev.kind === "mqtt" ? "MQTT" : ev.kind === "lwm2m" ? "LWM2M" : "OTA";
        const text = ` ${ev.from} → ${ev.to}  ${ev.type}${ev.detail ? "  " + ev.detail : ""}${ev.len != null ? "  (" + ev.len + " B)" : ""}`;
        logLine(ev, cat, text, ev.lat);
        if (ev.kind === "mqtt" || ev.kind === "lwm2m") seq[ev.kind].add(ev);
        if (ev.type === "HANDSHAKE") S.handshake[ev.kind] = ev;
        if (ev.kind === "mqtt" && ev.from === "BROKER" && ev.lat != null &&
            (ev.type === "PUBACK" || ev.type === "PUBCOMP" || (ev.type === "PINGRESP" && /PUBLISH/.test(ev.detail || "")))) {
          S.live.mqtt.push({ x: S.live.mqtt.length + 1, y: ev.lat }); later("live-mqtt", () => liveChart("mqtt"));
        }
        if (ev.kind === "lwm2m" && ev.type === "ACK" && ev.lat != null) {
          S.live.lwm2m.push({ x: S.live.lwm2m.length + 1, y: ev.lat }); later("live-lwm2m", () => liveChart("lwm2m"));
        }
        if (ev.kind === "lwm2m" && ev.type === "REGISTER RESPONSE") autoObserve(ev);
        break;
      }
      case "log":
        logLine(ev, ev.kind === "mqtt" ? "MQTT" : ev.kind === "lwm2m" ? "LWM2M" : ev.kind === "ota" ? "OTA" : "SERVER", "  " + ev.msg);
        break;
      case "state":
        if (!S.state) S.state = { devices: {} };
        if (!S.state.devices[ev.kind]) S.state.devices[ev.kind] = {};
        S.state.devices[ev.kind].state = ev;
        later("devices", renderDevices);
        break;
      case "run":
        logLine({ t: ev.t, proto: "BENCH" }, "SERVER", `  run ${ev.summary && ev.summary.run} complete - see MQTT vs LwM2M tab`);
        if (ev.kind === "ota" && ev.summary) otaSummary(ev.summary);
        later("runs", loadRuns);
        break;
      case "broker":
        logLine({ t: ev.t, proto: "BROKER", k: "broker", topic: ev.topic, payload: ev.payload, len: ev.len, qos: ev.qos }, "SERVER",
          `  BROKER → LAB SUBSCRIBER  PUBLISH ${ev.topic} (${ev.len} B)`);
        break;
      case "leshan": {
        const d = ev.data || {};
        if (ev.event === "NOTIFICATION" && d.res && d.val) { S.values[d.res] = valueOf(d.val); later("res", renderResources); }
        let what = ev.event;
        if (d.res) what += " " + d.res + (d.val && d.val.value != null ? " = " + String(d.val.value).slice(0, 40) : "");
        else if (d.endpoint) what += " " + d.endpoint;
        logLine({ t: ev.t, proto: "LESHAN", k: "leshan", event: ev.event, data: d }, "SERVER", "  " + what);
        break;
      }
      case "lab":
        logLine({ t: ev.t, proto: "LAB", k: "lab", msg: ev.msg }, "SERVER", "  " + ev.msg);
        if (ev.event === "exit" && S.exitWaiters[ev.kind]) S.exitWaiters[ev.kind]();
        break;
      case "ota":
        otaProgress(ev);
        break;
    }
  }

  /* ---------------------------------------------------------------- state polling & panels */
  async function poll() {
    try {
      const st = await api("/api/state");
      S.state = st;
      pills(); renderServers(); renderDevices();
    } catch (e) { S.state = null; pills(); }
  }
  function pills() {
    const st = S.state;
    const set = (id, ok, txt) => { const p = $(id); p.classList.toggle("ok", ok === true); p.classList.toggle("bad", ok === false); p.querySelector("b").textContent = txt; };
    set("#pill-broker", st ? st.mqtt.online : null, st ? (st.mqtt.online ? "ONLINE" : "OFFLINE") : "?");
    set("#pill-leshan", st ? st.lwm2m.online : null, st ? (st.lwm2m.online ? "ONLINE" : "OFFLINE") : "?");
    set("#pill-lab", S.sseUp && !!st, S.sseUp && st ? "LIVE" : "DOWN");
    const hw = st && st.hw, ph = $("#pill-hw");
    ph.hidden = !(targetSel.value === "hw" || (hw && hw.configured));
    if (!hw || !hw.configured) {
      set("#pill-hw", false, "NOT SET UP");
      ph.title = "Start the lab with --hw-serial COMx and the --hw-mqtt/--hw-coap tunnel addresses (hardware/stm32l452-ec200u/README.md)";
      return;
    }
    const busy = hw.online && hw.busy;
    const ok = hw.online && (hw.state === "ready" || !!busy);
    const bad = !hw.online || hw.state === "modem_error" || hw.state === "fault";
    set("#pill-hw", ok ? true : bad ? false : null,
      !hw.online ? "OFFLINE" : busy ? "RUNNING " + busy.toUpperCase() : hw.state === "modem" ? "CONNECTING" : hw.state.toUpperCase().replace("_", " "));
    const s = hw.status || {};
    ph.title = [`${hw.port}`, s.operator ? `${s.operator} ${s.rat || ""} · CSQ ${s.rssi} · IP ${s.ip}` : "", s.imei ? "IMEI " + s.imei : "",
      `APN ${s.apn || "?"}`, hw.msg || ""].filter(Boolean).join("\n");
  }
  function lastTelemetry(payload) { try { return JSON.parse(payload); } catch (e) { return null; } }

  function renderServers() {
    const st = S.state; if (!st || !st.mqtt) return;
    const m = st.mqtt, mdev = (st.devices.mqtt && st.devices.mqtt.state) || {};
    const tel = lastTelemetry(m.last_payload);
    setKV($("#m-broker"), [
      ["Broker", m.online ? "ONLINE" : "OFFLINE", m.online ? "ok" : "bad"],
      ["Address", `${m.host}:${m.port} · TLS ${m.tls_port}`],
      ["Version", m.version || null],
      ["Connected devices", m.online ? String(Math.max(0, m.clients_connected - 1)) : null],
      ["Published (device)", mdev.published != null ? String(mdev.published) : null],
      ["Received (subscriber)", String(m.lab_received)],
      ["Broker PUBLISH in / out", m.online ? `${m.publish_received} / ${m.publish_sent}` : null],
      ["Last topic", m.last_topic || null],
      ["Temperature", tel ? tel.temperature.toFixed(1) + " °C" : null],
      ["Humidity", tel ? tel.humidity.toFixed(1) + " %" : null],
      ["Pressure", tel ? tel.pressure.toFixed(1) + " hPa" : null],
      ["Battery", tel ? tel.battery + " %" : null],
    ]);
    const l = st.lwm2m;
    const rv = (p) => { const r = (l.resources || []).find((x) => x.path === p); return r ? r.value : null; };
    setKV($("#l-server"), [
      ["Server", l.online ? "ONLINE" : "OFFLINE", l.online ? "ok" : "bad"],
      ["REST / CoAP / DTLS", `${l.web_port} / ${l.coap_port} / ${l.coaps_port}`],
      ["Registered devices", l.online ? String(l.clients) : null],
      ["Endpoints", l.endpoints || null],
      ["Notifications received", String(l.notifications)],
      ["/3/0/9 Battery", rv("/3/0/9") != null ? rv("/3/0/9") + " %" : null],
      ["/3303/0/5700 Temperature", rv("/3303/0/5700") != null ? (+rv("/3303/0/5700")).toFixed(1) + " °C" : null],
      ["/3304/0/5700 Humidity", rv("/3304/0/5700") != null ? (+rv("/3304/0/5700")).toFixed(1) + " %" : null],
      ["/3315/0/5700 Pressure", rv("/3315/0/5700") != null ? (+rv("/3315/0/5700")).toFixed(1) + " hPa" : null],
    ]);
  }

  function devStatus(dev) {
    if (!dev) return ["not started", "idle"];
    if (dev.running === false) return ["stopped" + (dev.exit_code ? " (exit " + dev.exit_code + ")" : ""), "idle"];
    const s = dev.state && dev.state.state;
    if (s === "connected" || s === "registered") return [s, "ok"];
    if (s === "error") return ["error", "bad"];
    return [s || "starting", "idle"];
  }
  function renderDevices() {
    const st = S.state; if (!st || !st.devices) return;
    const md = st.devices.mqtt, m = (md && md.state) || {}, hs = S.handshake.mqtt;
    const [ms, mc] = devStatus(md);
    const where = (d) => (d && d.run ? (d.hw ? "STM32L452 + EC200U (LTE)" : "PC simulation") : null);
    setKV($("#m-device"), [
      ["State", ms, mc],
      ["Runs on", where(md)],
      ["Run", md && md.run ? md.run : null],
      ["QoS", m.qos != null ? String(m.qos) : null],
      ["Interval", m.interval_ms != null ? m.interval_ms + " ms" : null],
      ["Published / acked / failed", m.published != null ? `${m.published} / ${m.acked} / ${m.failed}` : null],
      ["Target", m.target ? String(m.target) : m.published != null ? "∞" : null],
      ["Bytes sent / received", m.tx_bytes != null ? `${m.tx_bytes} / ${m.rx_bytes}` : null],
      ["Security", hs && md && md.running ? hs.sec : m.state ? "none" : null],
      ["Handshake", hs && md && md.running ? `${hs.lat.toFixed(1)} ms · ${hs.len} B` : null],
      ["Network sim", m.net ? `${m.net.latency_ms} ms · ${m.net.loss_pct} % · TCP retx ${m.net.tcp_retx}` : null],
      ["Sensor", m.temperature != null ? `${m.temperature.toFixed(1)} °C · ${m.humidity.toFixed(1)} % · ${m.battery} %` : null],
    ]);
    const ld = st.devices.lwm2m, l = (ld && ld.state) || {}, lhs = S.handshake.lwm2m;
    const [ls, lc] = devStatus(ld);
    setKV($("#l-device"), [
      ["State", ls, lc],
      ["Runs on", where(ld)],
      ["Endpoint", l.endpoint || null],
      ["Observed", l.observed || (l.state ? "nothing yet" : null)],
      ["Samples", l.samples != null ? String(l.samples) : null],
      ["Notify ok / failed / pending", l.notifies != null ? `${l.notify_ok} / ${l.notify_fail} / ${l.pending}` : null],
      ["Mode", l.notify_con != null ? (l.notify_con ? "CON (acked)" : "NON (unacked)") : null],
      ["Bytes sent / received", l.tx_bytes != null ? `${l.tx_bytes} / ${l.rx_bytes}` : null],
      ["Security", lhs && ld && ld.running ? lhs.sec : l.state ? "none" : null],
      ["Handshake", lhs && ld && ld.running ? `${lhs.lat.toFixed(1)} ms · ${lhs.len} B` : null],
      ["Network sim", l.net ? `${l.net.latency_ms} ms · ${l.net.loss_pct} % · dropped ${l.net.dropped_tx}↑ ${l.net.dropped_rx}↓` : null],
    ]);
    const obs = (l.observed || "").split(",").filter(Boolean);
    $$("#l-res tbody tr").forEach((tr) => { tr.lastChild.textContent = obs.includes(tr.dataset.path) ? "● observed" : ""; });
    ["m-start", "l-start", "o-start"].forEach((id, i) => {
      const k = ["mqtt", "lwm2m", "ota"][i];
      $("#" + id).disabled = !!(st.devices[k] && st.devices[k].running);
    });
  }

  /* ---------------------------------------------------------------- MQTT controls */
  function mqttParams() {
    return Object.assign({ kind: "mqtt", qos: segValue("m-qos"), interval: $("#m-interval").value, count: $("#m-count").value,
      payload: $("#m-payload").value, secure: segValue("m-sec") }, net("m"));
  }
  async function startDevice(params, hintId) {
    try {
      const r = await post("/api/device/start", params);
      hint(hintId, "Started run " + r.run);
      return r;
    } catch (e) { hint(hintId, e.message, true); throw e; }
  }
  $("#m-start").addEventListener("click", () => { S.live.mqtt = []; S.handshake.mqtt = null; seq.mqtt.clear(); liveChart("mqtt"); startDevice(mqttParams(), "#m-hint").catch(() => {}); });
  $("#m-stop").addEventListener("click", () => post("/api/device/stop", { kind: "mqtt" }).then(() => hint("#m-hint", "Stopped (DISCONNECT sent)."), (e) => hint("#m-hint", e.message, true)));
  $("#m-reset").addEventListener("click", resetAll);
  $("#m-pubnow").addEventListener("click", () => post("/api/device/cmd", { kind: "mqtt", cmd: "publish" }));
  $("#m-cmd").addEventListener("click", () => {
    const st = S.state && S.state.devices.mqtt && S.state.devices.mqtt.state;
    const id = (st && st.dev) || "mqtt-dev-1";
    const topic = `${(S.state && S.state.config.topic) || "playground"}/${id}/cmd`;
    post("/api/mqtt/publish", { topic, payload: "interval=2000", qos: 1 })
      .then(() => hint("#m-hint", `Published "interval=2000" to ${topic} - watch the device receive it.`), (e) => hint("#m-hint", e.message, true));
  });
  async function resetAll() {
    await post("/api/reset").catch(() => {});
    logEl.textContent = ""; logCount = 0;
    seq.mqtt.clear(); seq.lwm2m.clear();
    S.live = { mqtt: [], lwm2m: [] }; S.values = {};
    renderResources(); liveChart("mqtt"); liveChart("lwm2m");
    $("#insp").textContent = "Click an event to inspect it.";
  }

  /* ---------------------------------------------------------------- LwM2M controls & resources */
  const RES = [
    ["/3/0/0", "Manufacturer", "R", "string"], ["/3/0/1", "Model Number", "R", "string"], ["/3/0/2", "Serial Number", "R", "string"],
    ["/3/0/3", "Firmware Version", "R", "string"], ["/3/0/4", "Reboot", "E"], ["/3/0/9", "Battery Level (%)", "R", "integer"],
    ["/3/0/13", "Current Time", "RW", "time"], ["/3/0/14", "UTC Offset", "RW", "string"], ["/3/0/15", "Timezone", "RW", "string"],
    ["/3303/0/5700", "Temperature (Cel)", "R", "float"], ["/3303/0/5601", "Temperature min measured", "R", "float"],
    ["/3303/0/5602", "Temperature max measured", "R", "float"], ["/3303/0/5605", "Reset min/max", "E"],
    ["/3303/0/5750", "Application Type", "RW", "string"], ["/3304/0/5700", "Humidity (%RH)", "R", "float"],
    ["/3304/0/5750", "Application Type", "RW", "string"], ["/3315/0/5700", "Pressure (hPa)", "R", "float"],
    ["/19/0/0", "Telemetry blob (opaque JSON)", "R", "opaque"], ["/1/0/1", "Lifetime (s)", "RW", "integer"],
    ["/1/0/8", "Registration Update Trigger", "E"],
  ];
  function valueOf(node) {
    if (!node) return null;
    if (node.kind === "singleResource") return node.type === "OPAQUE" ? "(" + (node.value.length / 2) + " bytes)" : node.value;
    if (node.kind === "multiResource") return Object.values(node.values || {}).map((v) => (typeof v === "string" && /^[0-9a-f]+$/i.test(v) && v.length > 16 ? "(" + v.length / 2 + " bytes)" : v)).join(", ");
    return null;
  }
  function renderResources() {
    const tb = $("#l-res tbody");
    if (!tb.children.length) {
      RES.forEach(([p, n, ops]) => {
        const tr = document.createElement("tr"); tr.dataset.path = p;
        [p, n, "", ops, ""].forEach((v, i) => { const td = document.createElement("td"); td.textContent = v; if (i === 0) td.className = "mono"; tr.appendChild(td); });
        tr.addEventListener("click", () => { S.selected = p; renderResources(); hint("#l-ophint", "Selected " + p + " · " + n + " (" + ops + ")"); });
        tb.appendChild(tr);
      });
    }
    $$("tr", tb).forEach((tr) => {
      tr.classList.toggle("sel", tr.dataset.path === S.selected);
      const v = S.values[tr.dataset.path];
      tr.children[2].textContent = v == null ? "" : String(v).length > 48 ? String(v).slice(0, 48) + "…" : String(v);
    });
  }
  function endpoint() {
    const st = S.state && S.state.devices.lwm2m && S.state.devices.lwm2m.state;
    return (st && st.endpoint) || "lwm2m-dev-1";
  }
  async function leshan(method, path, body) {
    const opts = { method };
    if (body) { opts.body = JSON.stringify(body); opts.headers = { "Content-Type": "application/json" }; }
    return api("/leshan/api/clients/" + encodeURIComponent(endpoint()) + path, opts);
  }
  function absorb(path, r) {
    const c = r && r.content;
    if (!c) return;
    if (c.kind === "instance") c.resources.forEach((x) => (S.values[path.split("/").slice(0, 3).join("/") + "/" + x.id] = valueOf(x)));
    else S.values[path] = valueOf(c);
    renderResources();
  }
  async function op(name) {
    const p = S.selected, meta = RES.find((r) => r[0] === p) || [p, p, "R", "string"];
    try {
      let r;
      switch (name) {
        case "register": case "deregister": case "notify":
          await post("/api/device/cmd", { kind: "lwm2m", cmd: name });
          return hint("#l-ophint", name === "notify" ? "Device sampled now - every observed resource sends NOTIFY." : "Sent '" + name + "' to the device.");
        case "read": r = await leshan("GET", p + "?timeout=5"); absorb(p, r); break;
        case "observe": r = await leshan("POST", p + "/observe?timeout=5"); absorb(p, r); break;
        case "cancel": r = await leshan("DELETE", p + "/observe?active=true&timeout=5"); break;
        case "execute":
          if (!meta[2].includes("E")) return hint("#l-ophint", p + " is not executable - pick one with E.", true);
          r = await leshan("POST", p + "?timeout=5"); break;
        case "write": {
          if (!meta[2].includes("W")) return hint("#l-ophint", p + " is not writable - pick one with W (e.g. /3303/0/5750).", true);
          const v = $("#l-wval").value.trim();
          if (!v) return hint("#l-ophint", "Type the new value for " + p + " (" + meta[3] + (meta[3] === "time" ? ", Unix seconds" : "") + ") in the box next to Write.", true);
          const rid = +p.split("/")[3];
          const value = meta[3] === "integer" ? parseInt(v, 10) : meta[3] === "time" ? parseInt(v, 10) * 1000 : v;
          r = await leshan("PUT", p + "?timeout=5", { id: rid, kind: "singleResource", value, type: meta[3] });
          break;
        }
      }
      hint("#l-ophint", `${name.toUpperCase()} ${p}: ${r && r.status ? r.status : "done"}`, r && r.failure);
    } catch (e) { hint("#l-ophint", name + " failed: " + e.message, true); }
  }
  $$(".ops button").forEach((b) => b.addEventListener("click", () => op(b.dataset.op)));

  function lwParams() {
    return Object.assign({ kind: "lwm2m", interval: $("#l-interval").value, count: $("#l-count").value, payload: $("#l-payload").value,
      secure: segValue("l-sec"), notify: segValue("l-notify") }, net("l"));
  }
  function observeTarget() { return +$("#l-payload").value > 0 ? "/19/0/0" : "/3303/0/5700"; }
  async function autoObserve(ev) {
    if (!$("#l-autoobs").checked && !S.exp.running) return;
    const key = ev.run + ":" + ev.t; /* once per registration */
    if (S.observedFor === key) return;
    S.observedFor = key;
    await sleep(300); /* let Leshan finish registering */
    const target = observeTarget();
    try { S.selected = target; renderResources(); const r = await leshan("POST", target + "/observe?timeout=10"); absorb(target, r); hint("#l-ophint", "Auto-OBSERVE " + target + ": " + (r.status || "ok")); }
    catch (e) { hint("#l-ophint", "Auto-OBSERVE failed: " + e.message, true); }
    return key;
  }
  $("#l-start").addEventListener("click", () => { S.live.lwm2m = []; S.handshake.lwm2m = null; seq.lwm2m.clear(); liveChart("lwm2m"); startDevice(lwParams(), "#l-hint").catch(() => {}); });
  $("#l-stop").addEventListener("click", () => post("/api/device/stop", { kind: "lwm2m" }).then(() => hint("#l-hint", "Stopped (De-register sent)."), (e) => hint("#l-hint", e.message, true)));
  $("#l-reset").addEventListener("click", resetAll);

  /* ---------------------------------------------------------------- live charts */
  function liveChart(kind) {
    if (S.live[kind].length > 500) S.live[kind] = S.live[kind].slice(-500);
    const pts = S.live[kind];
    const elId = kind === "mqtt" ? "#m-live" : "#l-live";
    if ($(elId).offsetParent === null) return;
    Charts.xy($(elId), { series: [{ name: kind === "mqtt" ? "MQTT" : "LwM2M", color: kind === "mqtt" ? C.MQTT : C.LWM2M, points: pts }],
      xName: "message", yUnit: " ms", yLabel: "ms", dots: pts.length < 60, dotR: 3, xFmt: (v) => v.toFixed(0),
      empty: kind === "mqtt" ? "Start the MQTT device - acknowledged messages are plotted here." : "Start the LwM2M device - acknowledged notifications are plotted here." });
  }

  /* ---------------------------------------------------------------- comparison */
  async function loadRuns() {
    try { S.runs = await api("/api/runs"); } catch (e) { S.runs = []; }
    renderCompare(); renderCharts();
  }
  const latest = (proto, pred) => { for (let i = S.runs.length - 1; i >= 0; i--) { const r = S.runs[i]; if (r.proto === proto && (!pred || pred(r))) return r; } return null; };
  function renderCompare() {
    const mq = latest("MQTT"), lw = latest("LWM2M");
    const rows = [
      ["Latency (avg)", (r) => nm(r.lat_avg, " ms", 2)], ["Median", (r) => nm(r.lat_median, " ms", 2)],
      ["P95", (r) => nm(r.lat_p95, " ms", 2)], ["P99", (r) => nm(r.lat_p99, " ms", 2)],
      ["Min / Max", (r) => (r.lat_min == null ? null : nm(r.lat_min, "", 2) + " / " + nm(r.lat_max, " ms", 2))],
      ["Bytes / message (wire)", (r) => nm(r.bytes_per_msg, " B", 1)], ["Payload / message", (r) => nm(r.avg_payload, " B", 1)],
      ["Connection (time to ready)", (r) => nm(r.session_ms, " ms", 1)],
      ["Handshake", (r) => (r.handshake_ms == null ? (r.secure ? null : "— security off") : nm(r.handshake_ms, " ms", 1) + " · " + r.hs_bytes + " B")],
      ["Success rate", (r) => nm(r.success_pct, " %", 1)], ["Messages", (r) => r.ok + " ok / " + r.n],
      ["Security", (r) => r.security], ["Network", (r) => r.latency_ms + " ms · " + r.loss_pct + " % loss"],
      ["Mode", (r) => (r.proto === "MQTT" ? "QoS " + r.qos + (r.qos === 0 && r.probe ? " + probe" : "") : "notify " + r.notify)],
    ];
    const t = $("#cmp-table");
    t.textContent = "";
    const head = t.insertRow();
    [["", ""], ["MQTT", "p-mqtt"], ["LwM2M", "p-lwm2m"]].forEach(([n, cls]) => {
      const th = document.createElement("th"); th.className = cls;
      if (n) { const s = document.createElement("span"); s.className = "swatch"; s.style.background = n === "MQTT" ? C.MQTT : C.LWM2M; th.appendChild(s); }
      th.appendChild(document.createTextNode(n)); head.appendChild(th);
    });
    rows.forEach(([label, f]) => {
      const tr = t.insertRow();
      tr.insertCell().textContent = label;
      [mq, lw].forEach((r) => {
        const td = tr.insertCell(); td.className = "num";
        const v = r ? f(r) : null;
        if (v == null) { td.textContent = "Not measured"; td.classList.add("nm"); } else td.textContent = v;
      });
    });
    /* runs table */
    const rt = $("#runs"); rt.textContent = "";
    const h = rt.insertRow();
    ["Time", "Protocol", "Security", "Mode", "Network", "Payload", "Success", "Avg", "P95", "B/msg", "Ready", "CSV"].forEach((c, i) => {
      const th = document.createElement("th"); th.textContent = c; if (i >= 6 && i < 11) th.className = "num"; h.appendChild(th);
    });
    S.runs.slice().reverse().forEach((r) => {
      const tr = rt.insertRow();
      const cells = r.proto === "OTA"
        ? [clock(r.ended || 0), "OTA", "—", (r.image || "") + " · " + r.chunk + " B chunks", r.latency_ms + " ms · " + r.loss_pct + " %", (r.size / 1024).toFixed(0) + " KiB",
          r.ok ? "100 %" : "failed", nm(r.total_ms, " ms", 0) || "—", "—", nm((r.wire_tx + r.wire_rx), " B", 0), "—", null]
        : [clock(r.ended || 0), r.proto, r.security, r.proto === "MQTT" ? "QoS " + r.qos : "notify " + r.notify, r.latency_ms + " ms · " + r.loss_pct + " %",
          nm(r.avg_payload, " B", 0), nm(r.success_pct, " %", 1), nm(r.lat_avg, " ms", 2) || "n/m", nm(r.lat_p95, " ms", 2) || "n/m",
          nm(r.bytes_per_msg, "", 1), nm(r.session_ms, " ms", 1) || "—", r.run];
      cells.forEach((v, i) => {
        const td = tr.insertCell();
        if (i === 11 && v) { const a = document.createElement("a"); a.href = "/results/" + encodeURIComponent(v) + ".csv"; a.textContent = "csv"; td.appendChild(a); }
        else td.textContent = v == null ? "" : v;
        if (i >= 6 && i < 11) td.className = "num";
      });
    });
  }

  function renderCharts() {
    if (!$("#tab-compare").hidden) {
      const mq = latest("MQTT"), lw = latest("LWM2M");
      const keys = ["lat_min", "lat_median", "lat_avg", "lat_p95", "lat_p99", "lat_max"];
      Charts.bars($("#c-lat"), { categories: ["min", "median", "avg", "p95", "p99", "max"], yUnit: " ms", yLabel: "ms",
        series: [{ name: "MQTT", color: C.MQTT, values: keys.map((k) => (mq ? mq[k] : null)) }, { name: "LwM2M", color: C.LWM2M, values: keys.map((k) => (lw ? lw[k] : null)) }] });
      const sc = (fx, fy, proto) => S.runs.filter((r) => r.proto === proto && fy(r) != null).map((r) => ({ x: fx(r), y: fy(r), label: r.security + (r.proto === "MQTT" ? " · QoS " + r.qos : "") }));
      const two = (fx, fy) => [{ name: "MQTT", color: C.MQTT, points: sc(fx, fy, "MQTT") }, { name: "LwM2M", color: C.LWM2M, points: sc(fx, fy, "LWM2M") }];
      Charts.xy($("#c-loss"), { series: two((r) => r.loss_pct, (r) => r.success_pct), trend: true, directLabels: true, yMax: 100, xName: "loss", xUnit: " %", yUnit: " %", yLabel: "success %", xLabel: "packet loss %", xZero: true });
      Charts.xy($("#c-netlat"), { series: two((r) => r.latency_ms, (r) => r.lat_median), trend: true, directLabels: true, xName: "network", xUnit: " ms", yUnit: " ms", yLabel: "median ms", xLabel: "one-way network latency ms", xZero: true });
      Charts.xy($("#c-payload"), { series: two((r) => r.avg_payload, (r) => r.bytes_per_msg), trend: true, directLabels: true, xName: "payload", xUnit: " B", yUnit: " B", yLabel: "wire B/msg", xLabel: "payload bytes", xZero: true });
      const secv = (proto, on, k) => { const r = latest(proto, (x) => !!x.secure === on && x.latency_ms === 0 && x.loss_pct === 0); return r ? r[k] : null; };
      const secSeries = (k) => [{ name: "MQTT", color: C.MQTT, values: [secv("MQTT", false, k), secv("MQTT", true, k)] }, { name: "LwM2M", color: C.LWM2M, values: [secv("LWM2M", false, k), secv("LWM2M", true, k)] }];
      Charts.bars($("#c-sec"), { categories: ["Security OFF", "Security ON (TLS / DTLS)"], series: secSeries("session_ms"), yUnit: " ms", yLabel: "ms", labels: true, empty: "Run the 'Security OFF vs ON' experiment (uses runs on an unimpaired network)." });
      Charts.bars($("#c-secb"), { categories: ["Security OFF", "Security ON (TLS / DTLS)"], series: secSeries("bytes_per_msg"), yUnit: " B", yLabel: "B/msg", labels: true, empty: "Run the 'Security OFF vs ON' experiment (uses runs on an unimpaired network)." });
    }
    if (!$("#tab-ota").hidden) {
      const pts = S.runs.filter((r) => r.proto === "OTA" && r.size).map((r) => ({ x: r.size / 1024, y: r.total_ms, hollow: !r.ok, label: (r.ok ? "ok" : "FAILED: " + r.reason) + " · " + r.latency_ms + " ms/" + r.loss_pct + " %" }));
      Charts.xy($("#c-ota"), { series: [{ name: "OTA", color: C.OTA, points: pts }], lines: false, xName: "image", xUnit: " KiB", yUnit: " ms", yLabel: "total ms", xLabel: "firmware KiB (hollow = failed)", xZero: true, empty: "Run an OTA to plot firmware size against update time." });
    }
    if (!$("#tab-mqtt").hidden) liveChart("mqtt");
    if (!$("#tab-lwm2m").hidden) liveChart("lwm2m");
  }
  window.addEventListener("resize", () => { clearTimeout(S.rz); S.rz = setTimeout(renderCharts, 150); });
  $("#runs-reset").addEventListener("click", async () => { if (confirm("Archive all comparison results and start fresh?")) { await post("/api/runs/reset"); loadRuns(); } });

  /* ---------------------------------------------------------------- experiments */
  /* Wakes on the lab's "exit" event (pushed over SSE, so it isn't slowed by
   * the timer throttling browsers apply to background tabs), or after 3 s. */
  function exitOrTimeout(kind, ms) {
    return new Promise((resolve) => {
      const t = setTimeout(resolve, ms);
      S.exitWaiters[kind] = () => { clearTimeout(t); S.exitWaiters[kind] = null; resolve(); };
    });
  }
  async function waitDone(kind, t0, maxMs) {
    const end = Date.now() + maxMs;
    while (Date.now() < end) {
      await exitOrTimeout(kind, 3000);
      if (S.exp.cancel) { await post("/api/device/stop", { kind }).catch(() => {}); throw new Error("cancelled"); }
      try {
        const st = await api("/api/state");
        const d = st.devices[kind];
        if (d && !d.running && d.started >= t0) return d;
      } catch (e) { /* lab hiccup, keep waiting */ }
    }
    await post("/api/device/stop", { kind }).catch(() => {});
    throw new Error(kind + " run timed out");
  }
  async function runOne(params, label) {
    const t0 = Date.now() - 1000;
    $("#x-status").textContent = label;
    if (params.kind === "lwm2m") S.observedFor = null;
    await post("/api/device/start", params);
    const est = (+params.count || 20) * (parseFloat(params.interval) || 1) * (String(params.interval).endsWith("ms") ? 0.001 : 1) * 1000;
    return waitDone(params.kind, t0, est * 6 + 120000);
  }
  async function experiment(name, plan) {
    if (S.exp.running) return;
    S.exp = { running: true, cancel: false };
    await post("/api/device/stop", { kind: "mqtt" }).catch(() => {});
    await post("/api/device/stop", { kind: "lwm2m" }).catch(() => {});
    try {
      for (let i = 0; i < plan.length; i++) {
        $("#x-bar").style.width = (100 * i) / plan.length + "%";
        await runOne(plan[i].params, `${name}: ${i + 1}/${plan.length} - ${plan[i].label}`);
        await loadRuns();
      }
      $("#x-bar").style.width = "100%";
      $("#x-status").textContent = name + ": done (" + plan.length + " runs). Results below.";
    } catch (e) {
      $("#x-status").textContent = name + ": " + e.message;
    } finally { S.exp.running = false; loadRuns(); }
  }
  const base = (kind, extra) => Object.assign(kind === "mqtt" ? mqttParams() : lwParams(), { latency: 0, loss: 0, interval: "200ms" }, extra);
  const both = (extra, label) => [
    { params: base("mqtt", extra), label: "MQTT " + label },
    { params: base("lwm2m", extra), label: "LwM2M " + label },
  ];
  $("#x-bench").addEventListener("click", () => experiment("Benchmark", both(Object.assign({ count: 100 }, net("m")), "100 messages")));
  $("#x-loss").addEventListener("click", () => experiment("Loss sweep", [0, 1, 2, 5, 10].flatMap((l) => both({ count: 50, loss: l }, l + " % loss"))));
  $("#x-lat").addEventListener("click", () => experiment("Latency sweep", [0, 50, 100, 250, 500, 1000].flatMap((l) => both({ count: 20, latency: l, interval: "500ms" }, l + " ms"))));
  $("#x-payload").addEventListener("click", () => experiment("Payload sweep", [0, 256, 512, 1024].flatMap((p) => both({ count: 20, payload: p }, (p || "natural") + " B"))
    .concat([{ params: base("mqtt", { count: 20, payload: 4096 }), label: "MQTT 4096 B" }])));
  $("#x-sec").addEventListener("click", () => experiment("Security", [0, 1].flatMap((s) => both({ count: 30, secure: s }, s ? "security ON" : "security OFF"))));
  $("#x-cancel").addEventListener("click", () => { S.exp.cancel = true; post("/api/device/stop", { kind: "ota" }).catch(() => {}); });

  /* ---------------------------------------------------------------- OTA */
  const STEPS = ["available", "download", "store", "verify", "done"];
  function otaSteps(active, failed) {
    const ai = STEPS.indexOf(active);
    $$("#o-flow li").forEach((li) => {
      const i = STEPS.indexOf(li.dataset.step);
      li.className = failed && (i === ai || (active === "download" && li.dataset.step === "store")) ? "fail"
        : i < ai || active === "done" ? "done" : i === ai || (active === "download" && li.dataset.step === "store") ? "active" : "";
    });
  }
  function otaProgress(ev) {
    S.ota = ev;
    const pct = ev.total ? ev.pct : 0;
    $("#o-bar").style.width = pct + "%";
    $("#o-pct").textContent = ev.phase === "reset" ? "reset to v1.0.0" : pct.toFixed(0) + " %  " + ev.phase;
    const phase = ev.phase === "interrupted" || ev.phase === "failed" ? "download" : ev.phase === "reset" ? "" : ev.phase;
    otaSteps(phase, ev.phase === "interrupted" || ev.phase === "failed");
    setKV($("#o-stats"), [
      ["Firmware", ev.version ? "v" + ev.version : null], ["Size", ev.total ? ev.total + " B" : null],
      ["Downloaded", ev.total ? ev.done + " B" : null], ["Wire bytes sent / received", `${ev.tx_bytes} / ${ev.rx_bytes}`],
      ["Status", ev.phase, ev.phase === "done" ? "ok" : ev.phase === "failed" || ev.phase === "interrupted" ? "bad" : "idle"],
    ]);
  }
  function otaSummary(s) {
    setKV($("#o-stats"), [
      ["Result", s.ok ? "COMPLETED" : "FAILED", s.ok ? "ok" : "bad"], ["Reason", s.ok ? null : s.reason],
      ["Firmware", s.image + " v" + s.version], ["Firmware size", s.size + " B"],
      ["Download time", nm(s.download_ms, " ms", 0)], ["Verify time", nm(s.verify_ms, " ms", 1)], ["Total", nm(s.total_ms, " ms", 0)],
      ["Bytes transferred", (s.wire_tx + s.wire_rx) + " B (sent " + s.wire_tx + ", received " + s.wire_rx + ")"],
      ["Chunks", s.chunks + " × " + s.chunk + " B" + (s.retries ? " · " + s.retries + " retries" : "")],
      ["Network", s.latency_ms + " ms · " + s.loss_pct + " % loss"],
    ]);
  }
  const otaParams = (extra) => Object.assign({ kind: "ota", image: $("#o-image").value, chunk: $("#o-chunk").value }, net("o"), extra || {});
  $("#o-start").addEventListener("click", () => { otaSteps("available"); post("/api/device/start", otaParams()).catch((e) => alert(e.message)); });
  $("#o-int").addEventListener("click", () => post("/api/device/cmd", { kind: "ota", cmd: "interrupt" }));
  $("#o-reset").addEventListener("click", async () => {
    await post("/api/device/stop", { kind: "ota" }).catch(() => {});
    await post("/api/device/start", { kind: "ota", action: "reset" }).catch((e) => alert(e.message));
    otaSteps(""); $("#o-bar").style.width = "0";
  });
  $("#o-sweep").addEventListener("click", () => experiment("OTA size sweep",
    ["fw_32k.bin", "fw_128k.bin", "firmware_v2.bin", "fw_512k.bin", "fw_1024k.bin"].map((img) => ({ params: otaParams({ image: img }), label: img }))));

  /* ---------------------------------------------------------------- boot */
  renderResources();
  connectEvents();
  poll();
  setInterval(poll, 1500);
  loadRuns();
  showTab(store.get("tab", "mqtt"));
})();
