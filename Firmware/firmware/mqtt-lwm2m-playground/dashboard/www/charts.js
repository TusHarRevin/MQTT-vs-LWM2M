/* Tiny dependency-free SVG charts for the playground (works offline).
 * Line (crosshair tooltip), grouped bars (per-bar tooltip), scatter with a
 * mean trend line. Colors come from CSS variables so light/dark just works.
 * All text is set with textContent. */
(function () {
  "use strict";
  const NS = "http://www.w3.org/2000/svg";
  const tip = () => document.getElementById("tip");

  function el(tag, attrs, parent) {
    const e = document.createElementNS(NS, tag);
    for (const k in attrs) {
      const v = attrs[k];
      /* presentation attributes don't resolve var(); styles do */
      if (typeof v === "string" && v.startsWith("var(") && (k === "fill" || k === "stroke")) e.style[k] = v;
      else e.setAttribute(k, v);
    }
    if (parent) parent.appendChild(e);
    return e;
  }
  function text(parent, x, y, s, attrs) {
    const t = el("text", Object.assign({ x, y }, attrs || {}), parent);
    t.textContent = s;
    return t;
  }

  function niceTicks(lo, hi, n) {
    if (!isFinite(lo) || !isFinite(hi)) return [0, 1];
    if (hi === lo) hi = lo + 1;
    const span = hi - lo, step0 = Math.pow(10, Math.floor(Math.log10(span / n)));
    const err = span / n / step0;
    const step = step0 * (err >= 7.5 ? 10 : err >= 3.5 ? 5 : err >= 1.5 ? 2 : 1);
    const ticks = [];
    for (let v = Math.floor(lo / step) * step; v <= hi + step * 0.5; v += step) ticks.push(+v.toPrecision(12));
    return ticks;
  }
  const fmt = (v) => {
    if (v == null || !isFinite(v)) return "–";
    if (Math.abs(v) < 1e-9) v = 0; /* no "-0.00" */
    return Math.abs(v) >= 100 ? v.toFixed(0) : Math.abs(v) >= 10 ? v.toFixed(1) : v.toFixed(2);
  };

  function showTip(evt, rows, title) {
    const t = tip();
    t.textContent = "";
    if (title) {
      const h = document.createElement("div");
      h.className = "muted";
      h.textContent = title;
      t.appendChild(h);
    }
    rows.forEach((r) => {
      const d = document.createElement("div");
      d.className = "row";
      if (r.color) {
        const s = document.createElement("span");
        s.className = "swatch";
        s.style.background = r.color;
        d.appendChild(s);
      }
      const b = document.createElement("b");
      b.textContent = r.value;
      d.appendChild(b);
      const l = document.createElement("span");
      l.className = "muted";
      l.textContent = " " + r.label;
      d.appendChild(l);
      t.appendChild(d);
    });
    t.hidden = false;
    const x = Math.min(evt.clientX + 14, window.innerWidth - t.offsetWidth - 8);
    const y = Math.min(evt.clientY + 14, window.innerHeight - t.offsetHeight - 8);
    t.style.left = x + "px";
    t.style.top = y + "px";
  }
  const hideTip = () => { tip().hidden = true; };

  function legend(root, series) {
    if (series.length < 2) return;
    const lg = document.createElement("div");
    lg.className = "legend";
    series.forEach((s) => {
      const sp = document.createElement("span");
      const i = document.createElement("i");
      i.style.background = s.color;
      sp.appendChild(i);
      sp.appendChild(document.createTextNode(s.name));
      lg.appendChild(sp);
    });
    root.appendChild(lg);
  }

  function frame(root, opts) {
    root.textContent = "";
    legend(root, opts.series || []);
    const W = Math.max(root.clientWidth || 500, 280), H = opts.height || 230;
    const m = { l: 52, r: opts.rightPad || 18, t: 22, b: 38 };
    const svg = el("svg", { viewBox: `0 0 ${W} ${H}`, role: "img", "aria-label": opts.title || "chart" }, root);
    return { svg, W, H, m, iw: W - m.l - m.r, ih: H - m.t - m.b };
  }

  function emptyState(root, msg) {
    root.textContent = "";
    const d = document.createElement("div");
    d.className = "empty";
    d.textContent = msg || "Not measured yet - run an experiment.";
    root.appendChild(d);
  }

  function axes(f, xt, yt, xs, ys, opts) {
    const g = el("g", { class: "grid" }, f.svg);
    yt.forEach((v) => el("line", { x1: f.m.l, x2: f.m.l + f.iw, y1: ys(v), y2: ys(v) }, g));
    const a = el("g", { class: "axis" }, f.svg);
    el("line", { x1: f.m.l, x2: f.m.l + f.iw, y1: f.m.t + f.ih, y2: f.m.t + f.ih }, a);
    yt.forEach((v) => text(f.svg, f.m.l - 6, ys(v) + 4, (opts.yFmt || fmt)(v), { "text-anchor": "end" }));
    if (xt) xt.forEach((v) => text(f.svg, xs(v), f.m.t + f.ih + 15, (opts.xFmt || fmt)(v), { "text-anchor": "middle" }));
    if (opts.yLabel) text(f.svg, 4, 11, opts.yLabel, { "text-anchor": "start" });
    if (opts.xLabel) text(f.svg, f.m.l + f.iw, f.H - 4, opts.xLabel, { "text-anchor": "end" });
  }

  /* ---- line / scatter --------------------------------------------------- */
  function xy(root, opts) {
    const series = (opts.series || []).filter((s) => s.points && s.points.length);
    if (!series.length) return emptyState(root, opts.empty);
    const all = series.flatMap((s) => s.points);
    let x0 = Math.min(...all.map((p) => p.x)), x1 = Math.max(...all.map((p) => p.x));
    let y1 = Math.max(...all.map((p) => p.y));
    if (opts.yMax != null) y1 = Math.max(y1, opts.yMax);
    if (opts.xZero) x0 = Math.min(0, x0);
    if (x0 === x1) { x1 = x0 + (Math.abs(x0) || 1); if (!opts.xZero) x0 -= Math.abs(x0) || 1; }
    const f = frame(root, Object.assign({}, opts, { series, rightPad: opts.directLabels ? 70 : 18 }));
    const yt = niceTicks(0, y1 * 1.05 || 1, 4), xt = opts.xTicks || niceTicks(x0, x1, 6);
    const ymax = yt[yt.length - 1];
    const xs = (v) => f.m.l + ((v - x0) / (x1 - x0)) * f.iw;
    const ys = (v) => f.m.t + f.ih - (v / ymax) * f.ih;
    axes(f, xt.filter((v) => v >= x0 && v <= x1), yt, xs, ys, opts);

    const labels = [];
    series.forEach((s) => {
      const pts = s.points.slice().sort((a, b) => a.x - b.x);
      let linePts = pts;
      if (opts.trend) {
        /* mean per x, so repeated runs at the same setting show their spread as dots */
        const by = new Map();
        pts.forEach((p) => { const k = p.x; const e = by.get(k) || { s: 0, n: 0 }; e.s += p.y; e.n++; by.set(k, e); });
        linePts = [...by.entries()].sort((a, b) => a[0] - b[0]).map(([x, e]) => ({ x, y: e.s / e.n }));
      }
      if (opts.lines !== false && linePts.length > 1) {
        el("path", { d: linePts.map((p, i) => (i ? "L" : "M") + xs(p.x) + "," + ys(p.y)).join(""),
          fill: "none", stroke: s.color, "stroke-width": 2, "stroke-linejoin": "round" }, f.svg);
      }
      if (opts.dots !== false || pts.length < 40) {
        pts.forEach((p) => {
          el("circle", { cx: xs(p.x), cy: ys(p.y), r: opts.dotR || 4, fill: p.hollow ? "var(--surface)" : s.color,
            stroke: p.hollow ? s.color : "var(--surface)", "stroke-width": 2 }, f.svg);
        });
      }
      if (opts.directLabels && linePts.length > 1) {
        const last = linePts[linePts.length - 1];
        labels.push({ x: xs(last.x) + 8, y: ys(last.y) + 4, name: s.name });
      }
    });
    labels.sort((a, b) => a.y - b.y);
    labels.forEach((l, i) => {
      if (i && l.y - labels[i - 1].y < 13) l.y = labels[i - 1].y + 13; /* nudge apart */
      text(f.svg, l.x, l.y, l.name, { class: "dl" });
    });

    /* hover: crosshair snapping to the nearest x, one tooltip listing every series */
    const cross = el("line", { y1: f.m.t, y2: f.m.t + f.ih, stroke: "var(--axis)", "stroke-width": 1, visibility: "hidden" }, f.svg);
    const hit = el("rect", { x: f.m.l, y: f.m.t, width: f.iw, height: f.ih, fill: "transparent" }, f.svg);
    const xsAll = [...new Set(all.map((p) => p.x))].sort((a, b) => a - b);
    hit.addEventListener("pointermove", (evt) => {
      const r = f.svg.getBoundingClientRect();
      const px = ((evt.clientX - r.left) / r.width) * f.W;
      const xv = x0 + ((px - f.m.l) / f.iw) * (x1 - x0);
      let best = xsAll[0];
      xsAll.forEach((v) => { if (Math.abs(v - xv) < Math.abs(best - xv)) best = v; });
      cross.setAttribute("x1", xs(best));
      cross.setAttribute("x2", xs(best));
      cross.setAttribute("visibility", "visible");
      const rows = [];
      series.forEach((s) => s.points.filter((p) => p.x === best).forEach((p) =>
        rows.push({ color: s.color, value: (opts.yFmt || fmt)(p.y) + (opts.yUnit || ""), label: s.name + (p.label ? " · " + p.label : "") })));
      showTip(evt, rows, (opts.xName || "x") + " " + (opts.xFmt || fmt)(best) + (opts.xUnit || ""));
    });
    hit.addEventListener("pointerleave", () => { cross.setAttribute("visibility", "hidden"); hideTip(); });
  }

  /* ---- grouped bars ----------------------------------------------------- */
  function bars(root, opts) {
    const series = opts.series || [];
    const vals = series.flatMap((s) => s.values.filter((v) => v != null && isFinite(v)));
    if (!vals.length) return emptyState(root, opts.empty);
    const f = frame(root, opts);
    const yt = niceTicks(0, Math.max(...vals) * 1.1, 4), ymax = yt[yt.length - 1];
    const ys = (v) => f.m.t + f.ih - (v / ymax) * f.ih;
    axes(f, null, yt, null, ys, opts);
    const cats = opts.categories, gw = f.iw / cats.length;
    const bw = Math.min(28, (gw * 0.7) / series.length);
    cats.forEach((c, ci) => {
      const gx = f.m.l + gw * ci + (gw - bw * series.length - 2 * (series.length - 1)) / 2;
      text(f.svg, f.m.l + gw * ci + gw / 2, f.m.t + f.ih + 15, c, { "text-anchor": "middle" });
      series.forEach((s, si) => {
        const v = s.values[ci], x = gx + si * (bw + 2);
        if (v == null || !isFinite(v)) {
          text(f.svg, x + bw / 2, f.m.t + f.ih - 4, "n/m", { "text-anchor": "middle", "font-size": 9 });
          return;
        }
        const y = ys(v), h = f.m.t + f.ih - y, r = Math.min(4, h / 2, bw / 2);
        /* rounded data-end, square at the baseline */
        const d = `M${x},${f.m.t + f.ih}V${y + r}Q${x},${y} ${x + r},${y}H${x + bw - r}Q${x + bw},${y} ${x + bw},${y + r}V${f.m.t + f.ih}Z`;
        const p = el("path", { d, fill: s.color }, f.svg);
        if (opts.labels) text(f.svg, x + bw / 2, y - 4, (opts.yFmt || fmt)(v), { "text-anchor": "middle", class: "dl" });
        const hitR = el("rect", { x: x - 2, y: f.m.t, width: bw + 4, height: f.ih, fill: "transparent" }, f.svg);
        hitR.addEventListener("pointermove", (evt) => {
          p.setAttribute("opacity", "0.85");
          showTip(evt, [{ color: s.color, value: (opts.yFmt || fmt)(v) + (opts.yUnit || ""), label: s.name }], c);
        });
        hitR.addEventListener("pointerleave", () => { p.removeAttribute("opacity"); hideTip(); });
      });
    });
  }

  window.Charts = { xy, bars, emptyState, fmt };
})();
