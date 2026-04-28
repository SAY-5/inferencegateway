// inferenceg · monitor — vanilla ES module dashboard.

const POLL_MS = 1000;
const root = document.getElementById("app");
const events = [];

function el(tag, attrs = {}, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") e.className = v;
    else if (k === "html") e.innerHTML = v;
    else if (k.startsWith("on") && typeof v === "function") e.addEventListener(k.slice(2).toLowerCase(), v);
    else if (v != null) e.setAttribute(k, v);
  }
  for (const c of children.flat()) {
    if (c == null) continue;
    e.append(typeof c === "string" || typeof c === "number" ? document.createTextNode(String(c)) : c);
  }
  return e;
}

function svg(tag, attrs = {}, ...children) {
  const e = document.createElementNS("http://www.w3.org/2000/svg", tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  for (const c of children.flat()) if (c) e.append(c);
  return e;
}

function pushEvent(level, msg) {
  events.unshift({ t: new Date().toTimeString().slice(0, 8), lvl: level, msg });
  if (events.length > 80) events.pop();
}

function fmtRate(n) {
  if (n == null) return "—";
  if (n >= 1000) return (n / 1000).toFixed(1) + "k";
  return Math.round(n).toString();
}

function fmtMs(s) {
  if (s == null) return "—";
  return (s * 1000).toFixed(2);
}

let last = { dispatched: 0, ts: 0 };

async function poll() {
  try {
    const [info, rawMetrics] = await Promise.all([
      fetch("/v1/cluster").then((r) => r.json()),
      fetch("/metrics").then((r) => r.text()),
    ]);
    const m = parseMetrics(rawMetrics);
    const now = Date.now();
    const elapsedSec = last.ts === 0 ? 1 : (now - last.ts) / 1000;
    const rps = (info.dispatched - last.dispatched) / elapsedSec;
    last = { dispatched: info.dispatched, ts: now };
    if (info.dropped > 0) pushEvent("warn", `dropped <b>${info.dropped}</b> total`);
    render(info, m, rps);
  } catch (err) {
    pushEvent("alert", "poll failed: " + err.message);
    render(null, null, 0);
  }
}

function parseMetrics(text) {
  const out = { schedHist: [], reqHist: {}, schedSum: 0, schedCount: 0 };
  const lines = text.split("\n");
  for (const ln of lines) {
    const tl = ln.trim();
    if (!tl || tl.startsWith("#")) continue;
    const m = tl.match(/^(\w+)(\{[^}]*\})?\s+([\d.eE+-]+)$/);
    if (!m) continue;
    const name = m[1];
    const labels = m[2] || "";
    const v = parseFloat(m[3]);
    if (name === "ig_scheduler_overhead_seconds_bucket") {
      const le = (labels.match(/le="([^"]+)"/) || [])[1];
      out.schedHist.push({ le: parseFloat(le === "+Inf" ? "Infinity" : le), count: v });
    } else if (name === "ig_scheduler_overhead_seconds_sum") out.schedSum = v;
    else if (name === "ig_scheduler_overhead_seconds_count") out.schedCount = v;
  }
  out.schedHist.sort((a, b) => a.le - b.le);
  return out;
}

function render(info, m, rps) {
  root.innerHTML = "";
  root.append(strip(info, rps), grid(info, m));
}

function strip(info, rps) {
  const n = info ? info.backends.length : 0;
  const live = info ? info.backends.filter((b) => b.healthy).length : 0;
  const queue = info?.queue_depth ?? 0;
  const dropped = info?.dropped ?? 0;
  return el(
    "div",
    { class: "strip" },
    el("div", { class: "brand" },
      el("div", {}, el("b", {}, "ig"), " · gateway"),
      el("small", {}, "inferenceg · monitor"),
    ),
    el("div", { class: "live" },
      bigStat("rps now",        fmtRate(rps),   rps > 100 ? "ok" : ""),
      bigStat("backends",        `${live}/${n}`, live === n ? "ok" : "warn"),
      bigStat("queue depth",     queue,           queue > 50 ? "warn" : ""),
      bigStat("dropped",         dropped,         dropped > 0 ? "alert" : ""),
    ),
    el("div", { class: "policy-pill" }, info ? `policy · ${info.policy}` : "policy · —"),
  );
}

function bigStat(label, v, cls) {
  return el("span", {}, label, el("b", { class: cls || "" }, String(v)));
}

function grid(info, m) {
  return el(
    "main",
    {},
    panel("Backends", "backends", backendsTable(info)),
    panel("Scheduler overhead — p99 target ≤ 10 ms", "hist", overheadHist(m)),
    panel("Throughput", "thru", throughput(info, m)),
    panel("Event feed", "feed", feed()),
  );
}

function panel(title, key, body) {
  return el(
    "div",
    { class: "panel" },
    el("header", {}, el("span", { class: "title" }, title)),
    el("div", { class: "body" }, body),
  );
}

function backendsTable(info) {
  if (!info) return el("div", { class: "feed" }, "(no data)");
  const t = el("table", { class: "backends" });
  const head = el("tr", {},
    ["BACKEND", "URL", "INFLIGHT", "REQUESTS", "ERRORS", "STATUS"].map((h) => el("th", {}, h)),
  );
  t.append(head);
  for (const b of info.backends) {
    const tr = el("tr", { class: b.healthy ? "" : "unhealthy" },
      el("td", { class: "id" }, b.id),
      el("td", { class: "url" }, b.url),
      el("td", {}, b.inflight),
      el("td", {}, b.requests),
      el("td", {}, b.errors),
      el("td", {},
        el("span", { class: `health-dot${b.healthy ? "" : " down"}` }),
        b.healthy ? "UP" : "DOWN"),
    );
    t.append(tr);
  }
  return t;
}

function overheadHist(m) {
  if (!m || m.schedHist.length === 0) return el("div", { class: "feed" }, "(no overhead data yet)");
  // Convert cumulative to per-bucket counts.
  const W = 720, H = 240, PAD = 36;
  const buckets = [];
  let prev = 0;
  for (const r of m.schedHist) {
    if (!isFinite(r.le)) continue;
    buckets.push({ le: r.le, count: Math.max(0, r.count - prev) });
    prev = r.count;
  }
  const max = Math.max(1, ...buckets.map((b) => b.count));
  const sv = svg("svg", { class: "hist", viewBox: `0 0 ${W} ${H}` });
  // grid lines
  const grid = svg("g", { class: "grid" });
  for (let i = 0; i <= 4; ++i) {
    const y = PAD + ((H - 2 * PAD) * i) / 4;
    grid.append(svg("line", { x1: PAD, x2: W - PAD, y1: y, y2: y }));
  }
  sv.append(grid);
  // bars
  const bw = (W - 2 * PAD) / buckets.length - 4;
  buckets.forEach((b, i) => {
    const h = ((H - 2 * PAD) * b.count) / max;
    const x = PAD + i * (bw + 4);
    const y = H - PAD - h;
    const rect = svg("rect", { class: "bar", x, y, width: bw, height: h });
    sv.append(rect);
    const label = svg("text", { class: "axis", x: x + bw / 2, y: H - PAD + 14, "text-anchor": "middle" });
    label.textContent = (b.le * 1000).toFixed(b.le >= 0.05 ? 0 : 1) + "ms";
    sv.append(label);
  });
  // 10ms threshold line
  const buckIdx = buckets.findIndex((b) => b.le >= 0.01);
  if (buckIdx >= 0) {
    const x = PAD + buckIdx * (bw + 4) + bw / 2;
    sv.append(svg("line", { x1: x, x2: x, y1: PAD, y2: H - PAD, stroke: "#ff8a4c", "stroke-dasharray": "4 4" }));
    const t = svg("text", { class: "label-thresh", x: x + 6, y: PAD + 14 });
    t.textContent = "p99 budget";
    sv.append(t);
  }
  return sv;
}

function throughput(info, m) {
  const dispatched = info?.dispatched ?? 0;
  const dropped = info?.dropped ?? 0;
  const queue = info?.queue_depth ?? 0;
  const totalUs = m ? (m.schedSum * 1e6 / Math.max(1, m.schedCount)) : 0;
  return el(
    "div",
    { class: "bignums" },
    bignum("dispatched", dispatched.toLocaleString(), "total since boot", dispatched > 0 ? "ok" : ""),
    bignum("queue depth", queue, "current", queue > 50 ? "warn" : ""),
    bignum("dropped", dropped, "no healthy backend", dropped > 0 ? "alert" : ""),
    bignum("avg overhead", totalUs.toFixed(1) + " µs", "enqueue → dispatch", totalUs > 1000 ? "warn" : "ok"),
  );
}

function bignum(label, v, sub, cls) {
  return el("div", { class: "bignum" },
    el("div", { class: "label" }, label),
    el("div", { class: `value ${cls || ""}` }, v),
    el("div", { class: "sub" }, sub),
  );
}

function feed() {
  return el("div", { class: "feed" }, events.slice(0, 30).map((e) =>
    el("div", { class: "row" },
      el("span", { class: "t" }, e.t),
      el("span", { class: `lvl ${e.lvl}` }, e.lvl.toUpperCase()),
      el("span", { class: "msg", html: e.msg }),
    ),
  ));
}

pushEvent("info", "monitor booted");
poll();
setInterval(poll, POLL_MS);
