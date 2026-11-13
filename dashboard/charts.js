/**
 * Chart.js wrappers. Accessibility rules applied here (EIA commitment):
 *  - Okabe-Ito colours, and every series ALSO differs by dash pattern and
 *    marker shape and carries a direct text label, so colour is never the
 *    only cue (WCAG 1.4.1);
 *  - no animation (also covers prefers-reduced-motion);
 *  - series are decimated for display only when a view has very many points.
 */
import { decimateMinMax } from './metrics.js';
import { fmtDateTime, fmtTime } from './format.js';

const Chart = globalThis.Chart;
const charts = new Map();
const HOUR = 3_600_000;

/** Series styles: colour comes from CSS variables so light/dark themes stay in styles.css. */
export const STYLES = {
  trk: { cssVar: '--s-trk', dash: [], point: 'circle' },
  fix: { cssVar: '--s-fix', dash: [7, 4], point: 'triangle' },
  use: { cssVar: '--s-use', dash: [2, 3], point: 'rect' },
  net: { cssVar: '--s-net', dash: [10, 3, 2, 3], point: 'rectRot' },
};

function cssVar(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

function theme() {
  return { text: cssVar('--text'), muted: cssVar('--muted'), grid: cssVar('--grid'), surface: cssVar('--surface') };
}

/** Text label at the end of each line, nudged apart so labels do not overlap. */
const directLabels = {
  id: 'directLabels',
  afterDatasetsDraw(chart) {
    const { ctx, chartArea } = chart;
    const labels = [];
    chart.data.datasets.forEach((ds, i) => {
      if (!ds.directLabel || !chart.isDatasetVisible(i)) return;
      const pts = chart.getDatasetMeta(i).data;
      for (let j = pts.length - 1; j >= 0; j--) {
        if (ds.data[j] && ds.data[j].y !== null && Number.isFinite(pts[j].y)) {
          labels.push({ text: ds.directLabel, x: pts[j].x, y: pts[j].y, color: ds.borderColor });
          break;
        }
      }
    });
    if (!labels.length) return;
    const lineH = 15;
    labels.sort((a, b) => a.y - b.y);
    labels.forEach((l) => { l.ly = l.y - 9; });
    for (let i = 1; i < labels.length; i++) labels[i].ly = Math.max(labels[i].ly, labels[i - 1].ly + lineH);
    const overflow = labels[labels.length - 1].ly - (chartArea.bottom - 4);
    if (overflow > 0) labels.forEach((l) => { l.ly -= overflow; });
    const t = theme();
    ctx.save();
    ctx.font = '600 12px system-ui, sans-serif';
    ctx.textBaseline = 'middle';
    ctx.textAlign = 'right';
    for (const l of labels) {
      const x = Math.min(chartArea.right - 4, Math.max(l.x - 6, chartArea.left + ctx.measureText(l.text).width + 8));
      const y = Math.max(chartArea.top + 8, l.ly);
      const w = ctx.measureText(l.text).width;
      ctx.globalAlpha = 0.85;
      ctx.fillStyle = t.surface;
      ctx.fillRect(x - w - 3, y - 8, w + 6, 16);
      ctx.globalAlpha = 1;
      ctx.fillStyle = t.text;
      ctx.fillText(l.text, x, y);
      ctx.fillStyle = l.color;
      ctx.fillRect(x - w - 3, y + 7, w + 6, 2);
    }
    ctx.restore();
  },
};

function niceHourStep(spanMs, maxTicks) {
  for (const h of [1, 2, 3, 4, 6, 12, 24]) if (spanMs / (h * HOUR) <= maxTicks) return h * HOUR;
  return 24 * HOUR;
}

function baseOptions(t) {
  return {
    responsive: true,
    maintainAspectRatio: false,
    animation: false,
    parsing: false,
    normalized: true,
    interaction: { mode: 'nearest', axis: 'x', intersect: false },
    plugins: {
      legend: { display: false },
      tooltip: { callbacks: {} },
    },
    scales: {
      x: { ticks: { color: t.muted, maxRotation: 0 }, grid: { color: t.grid }, border: { color: t.muted } },
      y: { ticks: { color: t.muted }, grid: { color: t.grid }, border: { color: t.muted }, title: { display: true, color: t.muted } },
    },
  };
}

function upsert(canvas, type, data, options, plugins = []) {
  let chart = charts.get(canvas);
  if (!chart) {
    chart = new Chart(canvas, { type, data, options, plugins });
    charts.set(canvas, chart);
  } else {
    chart.data = data;
    chart.options = options;
    chart.update('none');
  }
  return chart;
}

/**
 * Time-series line chart.
 * @param series [{ label, style: keyof STYLES, data: [{x: ms, y: number|null}] }]
 * @param opts   { yTitle, unit, zeroLine, dp, beginAtZero, yMin, yMax, fallbackMin }
 */
export function renderLineChart(canvas, series, opts = {}) {
  const t = theme();
  const width = canvas.parentElement.clientWidth || 360;
  let xMin = Infinity, xMax = -Infinity;
  for (const s of series) for (const p of s.data) { if (p.x < xMin) xMin = p.x; if (p.x > xMax) xMax = p.x; }
  if (!Number.isFinite(xMin)) { xMin = opts.fallbackMin ?? 0; xMax = xMin + 24 * HOUR; }
  const step = niceHourStep(Math.max(HOUR, xMax - xMin), width < 500 ? 5 : 9);
  xMin = Math.floor(xMin / step) * step;
  xMax = Math.max(xMin + step, Math.ceil(xMax / step) * step);

  const datasets = series.map((s, i) => {
    const st = STYLES[s.style];
    const color = cssVar(st.cssVar);
    const data = decimateMinMax(s.data, Math.max(600, width * 2));
    const every = Math.max(1, Math.ceil(data.length / 9));
    const phase = Math.floor((every * (i + 1)) / (series.length + 1));
    return {
      label: s.label,
      directLabel: s.label,
      data,
      borderColor: color,
      backgroundColor: color,
      borderWidth: 2.5,
      borderDash: st.dash,
      pointStyle: st.point,
      pointRadius: (c) => (c.dataIndex % every === phase ? 4.5 : 0),
      pointHoverRadius: 6,
      pointBorderColor: t.surface,
      pointBorderWidth: 1,
      spanGaps: false,
      tension: 0,
    };
  });

  const o = baseOptions(t);
  o.scales.x.type = 'linear';
  o.scales.x.min = xMin;
  o.scales.x.max = xMax;
  o.scales.x.ticks.stepSize = step;
  o.scales.x.ticks.callback = (v) => fmtTime(v);
  o.scales.x.title = { display: true, text: 'Time of day (UK time)', color: t.muted };
  o.scales.y.title.text = opts.yTitle || '';
  if (opts.beginAtZero) o.scales.y.beginAtZero = true;
  if (opts.yMin !== undefined) { o.scales.y.min = opts.yMin; o.scales.y.max = opts.yMax; }
  if (opts.zeroLine) {
    o.scales.y.grid = {
      color: (c) => (c.tick.value === 0 ? t.text : t.grid),
      lineWidth: (c) => (c.tick.value === 0 ? 2 : 1),
    };
  }
  o.plugins.tooltip.callbacks.title = (items) => (items.length ? fmtDateTime(items[0].parsed.x) : '');
  o.plugins.tooltip.callbacks.label = (item) =>
    `${item.dataset.label}: ${item.parsed.y === null ? 'n/a' : item.parsed.y.toFixed(opts.dp ?? 1)} ${opts.unit || ''}`;
  o.plugins.tooltip.usePointStyle = true;
  o.layout = { padding: { right: 6 } };
  return upsert(canvas, 'line', { datasets }, o, [directLabels]);
}

/** Pointing-error histogram: 62 bars (underflow, sixty 1-degree bins, overflow). */
export function renderHistogram(canvas, counts, labels) {
  const t = theme();
  const color = cssVar('--s-trk');
  const edge = cssVar('--s-fix');
  const last = counts.length - 1;
  const o = baseOptions(t);
  o.parsing = true;
  o.normalized = false;
  o.interaction = { mode: 'index', intersect: false };
  o.scales.x.title = { display: true, text: 'Pointing error band (degrees)', color: t.muted };
  o.scales.x.ticks.autoSkip = false;
  o.scales.x.ticks.callback = (v, i) => {
    if (i === 0) return '<−30';
    if (i === last) return '≥30';
    const lo = i - 1 - 30;
    return lo % 10 === 0 && lo !== -30 ? String(lo).replace('-', '−') : '';
  };
  o.scales.x.grid = { display: false };
  o.scales.y.title.text = 'Samples';
  o.scales.y.beginAtZero = true;
  o.scales.y.ticks.precision = 0;
  o.plugins.tooltip.callbacks.title = (items) => (items.length ? `Error ${labels[items[0].dataIndex]} degrees` : '');
  o.plugins.tooltip.callbacks.label = (item) => `${item.parsed.y} samples`;
  const isEnd = (i) => i === 0 || i === last;
  return upsert(canvas, 'bar', {
    labels,
    datasets: [{
      label: 'Samples',
      data: counts,
      backgroundColor: counts.map((_, i) => (isEnd(i) ? t.surface : color)),
      borderColor: counts.map((_, i) => (isEnd(i) ? edge : color)),
      borderWidth: counts.map((_, i) => (isEnd(i) ? 2 : 0)),
      barPercentage: 1,
      categoryPercentage: 0.92,
    }],
  }, o);
}

/** Diagonal hatch on the surface colour: marks loss bars without relying on colour. */
function hatchPattern(ctx, stroke, background) {
  const size = 8;
  const tile = document.createElement('canvas');
  tile.width = size;
  tile.height = size;
  const c = tile.getContext('2d');
  c.fillStyle = background;
  c.fillRect(0, 0, size, size);
  c.strokeStyle = stroke;
  c.lineWidth = 2;
  c.beginPath();
  for (const o of [-size, 0, size]) { c.moveTo(o, size); c.lineTo(o + size, 0); }
  c.stroke();
  return ctx.createPattern(tile, 'repeat');
}

/** Signed value printed at the end of every bar ("+18.4" above a gain, "−3.2" below a loss). */
const barValueLabels = {
  id: 'barValueLabels',
  afterDatasetsDraw(chart) {
    const ds = chart.data.datasets[0];
    const bars = chart.getDatasetMeta(0).data;
    if (!ds?.valueLabels || !bars.length) return;
    const { ctx, chartArea } = chart;
    const slot = (chartArea.right - chartArea.left) / bars.length;
    ctx.save();
    ctx.font = '600 12px system-ui, sans-serif';
    ctx.fillStyle = theme().text;
    ctx.textBaseline = 'middle';
    const widest = Math.max(...ds.valueLabels.map((l) => ctx.measureText(l).width));
    const upright = widest + 4 > slot;      // narrow bars: write the label vertically
    if (slot >= 13) {
      bars.forEach((bar, i) => {
        const up = ds.data[i] >= 0;
        const w = ctx.measureText(ds.valueLabels[i]).width;
        ctx.save();
        if (upright) {
          ctx.translate(bar.x, up ? bar.y - 4 : bar.y + 4);
          ctx.rotate(-Math.PI / 2);
          ctx.textAlign = up ? 'left' : 'right';
          ctx.fillText(ds.valueLabels[i], 0, 0);
        } else {
          ctx.textAlign = 'center';
          const x = Math.min(chartArea.right - w / 2, Math.max(chartArea.left + w / 2, bar.x));
          ctx.fillText(ds.valueLabels[i], x, up ? bar.y - 9 : bar.y + 10);
        }
        ctx.restore();
      });
    }
    ctx.restore();
  },
};

/**
 * Net gain per UTC day as bars. Gains are solid bars above an emphasised zero
 * line; losses hang below it, hatched and outlined, and every bar carries its
 * signed value, so the sign never depends on colour.
 * @param rows [{ label: 'Mon 9 Nov', value: number, text: '+18.4' }]
 */
export function renderDailyGain(canvas, rows) {
  const t = theme();
  const gain = cssVar('--s-trk');
  const loss = cssVar('--s-fix');
  const width = canvas.parentElement.clientWidth || 360;
  const values = rows.map((r) => r.value);
  const lo = Math.min(0, ...values);
  const hi = Math.max(0, ...values);
  const pad = Math.max(2, (hi - lo) * 0.28);   // room for the value labels
  const o = baseOptions(t);
  o.parsing = true;
  o.normalized = false;
  o.interaction = { mode: 'index', intersect: false };
  o.scales.x.title = { display: true, text: 'Day (UTC)', color: t.muted };
  o.scales.x.grid = { display: false };
  o.scales.x.ticks.maxRotation = 60;
  o.scales.x.ticks.autoSkip = true;
  o.scales.x.ticks.maxTicksLimit = width < 500 ? 7 : 14;
  o.scales.y.title.text = 'Net gain (%)';
  o.scales.y.suggestedMin = lo < 0 ? lo - pad : 0;   // "suggested" lets Chart.js round the ends to tidy ticks
  o.scales.y.suggestedMax = hi > 0 ? hi + pad : pad;
  o.scales.y.grid = {
    color: (c) => (c.tick.value === 0 ? t.text : t.grid),
    lineWidth: (c) => (c.tick.value === 0 ? 2 : 1),
  };
  o.plugins.tooltip.callbacks.title = (items) => (items.length ? `${rows[items[0].dataIndex].label} (UTC day)` : '');
  o.plugins.tooltip.callbacks.label = (item) => {
    const r = rows[item.dataIndex];
    return `Net ${r.value < 0 ? 'loss' : 'gain'}: ${r.text} %`;
  };
  const hatch = rows.some((r) => r.value < 0) ? hatchPattern(canvas.getContext('2d'), loss, t.surface) : t.surface;
  return upsert(canvas, 'bar', {
    labels: rows.map((r) => r.label),
    datasets: [{
      label: 'Net gain %',
      data: values,
      valueLabels: rows.map((r) => r.text),
      backgroundColor: values.map((v) => (v < 0 ? hatch : gain)),
      borderColor: values.map((v) => (v < 0 ? loss : gain)),
      borderWidth: values.map((v) => (v < 0 ? 2 : 0)),
      borderSkipped: false,
      maxBarThickness: 56,
      barPercentage: 0.9,
      categoryPercentage: 0.85,
    }],
  }, o, [barValueLabels]);
}

/** HTML legend for the daily net gain bars: a solid swatch (gain) and a hatched, outlined one (loss). */
export function renderBarLegend(ul) {
  if (ul.childElementCount) return;
  for (const [cls, text] of [['swatch-gain', 'Net gain: solid bar above the zero line, labelled +'],
    ['swatch-loss', 'Net loss: hatched, outlined bar below the zero line, labelled −']]) {
    const li = document.createElement('li');
    const sw = document.createElement('span');
    sw.className = `swatch ${cls}`;
    sw.setAttribute('aria-hidden', 'true');
    const label = document.createElement('span');
    label.textContent = text;
    li.append(sw, label);
    ul.appendChild(li);
  }
}

/** HTML legend: a line sample with the series' dash pattern and marker, plus its name. */
export function renderLegend(ul, series) {
  const marker = {
    circle: '<circle cx="18" cy="7" r="4"/>',
    triangle: '<path d="M18 2.2 L22.6 10.6 L13.4 10.6 Z"/>',
    rect: '<rect x="14" y="3" width="8" height="8"/>',
    rectRot: '<path d="M18 1.8 L23.2 7 L18 12.2 L12.8 7 Z"/>',
  };
  ul.innerHTML = series.map((s) => {
    const st = STYLES[s.style];
    return `<li><svg width="36" height="14" viewBox="0 0 36 14" aria-hidden="true" focusable="false" style="color:var(${st.cssVar})">` +
      `<line x1="0" y1="7" x2="36" y2="7" stroke="currentColor" stroke-width="2.5" stroke-dasharray="${st.dash.join(' ')}"/>` +
      `<g fill="currentColor">${marker[st.point]}</g></svg><span></span></li>`;
  }).join('');
  ul.querySelectorAll('span').forEach((el, i) => { el.textContent = series[i].label; });
}

/**
 * Status timeline: one row per state (Tracking, Moving, Holding, Idle, Stowed,
 * Fault, No data), with a floating bar for every period the tracker spent in
 * that state. The row position carries the meaning, so colour is never the
 * only cue; 'No data' bars are hatched.
 * @param segments [{ state, start, end, minutes }]  from metrics.stateSegments
 * @param rows     [{ key, label }] in display order
 * @param opts     { xMin, xMax } ms (axis span, e.g. the UTC day)
 */
export function renderStateTimeline(canvas, segments, rows, opts = {}) {
  const t = theme();
  const o = baseOptions(t);
  o.indexAxis = 'y';
  o.parsing = true;
  o.normalized = false;
  o.interaction = { mode: 'nearest', intersect: true };
  const span = (opts.xMax ?? 0) - (opts.xMin ?? 0);
  const step = niceHourStep(span > 0 ? span : 86_400_000, (canvas.parentElement.clientWidth || 360) < 500 ? 6 : 12);
  o.scales.x = {
    type: 'linear', min: opts.xMin, max: opts.xMax,
    ticks: { color: t.muted, maxRotation: 0, stepSize: step, callback: (v) => fmtTime(v) },
    grid: { color: t.grid }, border: { color: t.muted },
    title: { display: true, text: 'Time (UK)', color: t.muted },
  };
  o.scales.y = {
    type: 'category', labels: rows.map((r) => r.label),
    ticks: { color: t.text, autoSkip: false }, grid: { color: t.grid }, border: { color: t.muted },
  };
  o.plugins.tooltip.callbacks.title = (items) => (items.length ? items[0].raw.y : '');
  o.plugins.tooltip.callbacks.label = (item) => {
    const s = item.raw.seg;
    return `${fmtTime(s.start)}–${fmtTime(s.end)} · ${s.minutes} min`;
  };
  const colour = {
    TRK: cssVar('--s-trk'), MOV: cssVar('--s-net'), HLD: cssVar('--s-fix'),
    IDL: t.muted, STW: t.muted, FLT: cssVar('--s-fix'), GAP: t.muted,
  };
  const labelOf = Object.fromEntries(rows.map((r) => [r.key, r.label]));
  const gapFill = hatchPattern(canvas.getContext('2d'), t.muted, t.surface);
  const data = segments.map((s) => ({ x: [s.start, s.end], y: labelOf[s.state], seg: s }));
  return upsert(canvas, 'bar', {
    datasets: [{
      label: 'State',
      data,
      backgroundColor: segments.map((s) => (s.state === 'GAP' ? gapFill : colour[s.state])),
      borderColor: segments.map((s) => colour[s.state]),
      borderWidth: segments.map((s) => (s.state === 'GAP' || s.state === 'FLT' ? 1.5 : 0)),
      borderSkipped: false,
      barPercentage: 0.8,
      categoryPercentage: 0.9,
      minBarLength: 2,
    }],
  }, o);
}
