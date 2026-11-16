#pragma once

static const char DASHBOARD_PAGE[] PROGMEM = R"DASHBOARD(
<!doctype html>
<html lang="sk">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Kvalita vzduchu</title>
<style>
:root{font-family:Geist,system-ui,sans-serif;color:#17251e;background:#f1f4ef;color-scheme:light}
*{box-sizing:border-box}
body{margin:0}
main{max-width:1160px;margin:auto;padding:clamp(18px,4vw,52px)}
header{display:flex;align-items:end;justify-content:space-between;gap:16px;flex-wrap:wrap;margin-bottom:24px}
h1{font-size:clamp(28px,5vw,52px);letter-spacing:-.055em;margin:0 0 6px}
#status{color:#526259;font-size:14px;margin:0}
nav{display:flex;gap:4px;padding:4px;border:1px solid #ccd6cc;background:#e4ebe3;border-radius:10px}
button{font:inherit;color:#24352b;background:transparent;border:0;border-radius:7px;padding:8px 14px;cursor:pointer}
button[aria-pressed=true]{background:#234c39;color:#fff}
button:focus-visible{outline:2px solid #245c88;outline-offset:3px}
#charts{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:12px}
article{min-width:0;border:1px solid #d5ded5;border-radius:12px;background:#fff;padding:16px}
h2{margin:0;color:#405449;font-size:14px;font-weight:650}
.value{margin:4px 0 8px;font-size:32px;font-weight:650;font-variant-numeric:tabular-nums}
.unit{font-size:14px;font-weight:400;color:#526259;margin-left:6px}
canvas{display:block;width:100%;height:132px}
@media(max-width:760px){#charts{grid-template-columns:repeat(2,minmax(0,1fr))}}
@media(max-width:480px){#charts{grid-template-columns:1fr}}
</style>
</head>
<body>
<main>
<header>
  <div><h1>Kvalita vzduchu</h1><p id="status" role="status">Načítavam merania…</p></div>
  <nav aria-label="Obdobie grafov">
    <button type="button" data-minutes="60" aria-pressed="true">1 h</button>
    <button type="button" data-minutes="360" aria-pressed="false">6 h</button>
    <button type="button" data-minutes="1440" aria-pressed="false">24 h</button>
  </nav>
</header>
<section id="charts" aria-label="Merania a minútové priemery"></section>
</main>
<script>
const metrics = [
  ['CO₂', 'ppm', 0, '#215b42'],
  ['Teplota', '°C', 1, '#9d451e'],
  ['Vlhkosť', '%', 1, '#245b8c'],
  ['PM1.0', 'µg/m³', 1, '#70509b'],
  ['PM2.5', 'µg/m³', 1, '#875372'],
  ['PM10', 'µg/m³', 1, '#79552d']
];
const statusElement = document.querySelector('#status');
const charts = document.querySelector('#charts');
const buttons = [...document.querySelectorAll('button[data-minutes]')];
let minutes = 60;
let rows = [];
let historyRequest = 0;
const cards = metrics.map(([label, unit]) => {
  const card = document.createElement('article');
  card.innerHTML = '<h2></h2><p class="value"><span>—</span><span class="unit"></span></p><canvas role="img"></canvas>';
  card.querySelector('h2').textContent = label;
  card.querySelector('.unit').textContent = unit;
  const canvas = card.querySelector('canvas');
  canvas.setAttribute('aria-label', 'Graf: ' + label);
  charts.append(card);
  return {canvas, value: card.querySelector('.value span')};
});

function draw({canvas}, index) {
  const width = canvas.clientWidth;
  const height = canvas.clientHeight;
  const ratio = window.devicePixelRatio || 1;
  canvas.width = width * ratio;
  canvas.height = height * ratio;
  const ctx = canvas.getContext('2d');
  ctx.scale(ratio, ratio);
  ctx.font = '11px system-ui';
  ctx.fillStyle = '#526259';
  const series = rows.map(row => row[index]);
  const values = series.filter(Number.isFinite);
  if (!values.length) {
    ctx.fillText('Čakám na minútový priemer', 1, 22);
    return;
  }

  let low = Math.min(...values);
  let high = Math.max(...values);
  const minSpan = index === 0 ? 20 : 1;
  if (high - low < minSpan) {
    const center = (high + low) / 2;
    low = center - minSpan / 2;
    high = center + minSpan / 2;
  }

  const x0 = 45, x1 = width - 7, y0 = 12, y1 = height - 23;
  ctx.strokeStyle = '#e4ebe4';
  for (const [y, label] of [[y0, high], [y1, low]]) {
    ctx.beginPath();
    ctx.moveTo(x0, y);
    ctx.lineTo(x1, y);
    ctx.stroke();
    ctx.fillText(label.toFixed(metrics[index][2]), 1, y + 4);
  }
  ctx.fillText('−' + (minutes / 60) + ' h', x0, height - 4);
  ctx.textAlign = 'right';
  ctx.fillText('teraz', x1, height - 4);
  ctx.strokeStyle = ctx.fillStyle = metrics[index][3];
  ctx.lineWidth = 2;
  ctx.beginPath();
  let previous = false;
  let lastPoint = null;
  series.forEach((value, slot) => {
    if (!Number.isFinite(value)) {
      previous = false;
      return;
    }
    const x = x1 - (series.length - 1 - slot) * (x1 - x0) / minutes;
    const y = y1 - (value - low) * (y1 - y0) / (high - low);
    if (previous) ctx.lineTo(x, y);
    else ctx.moveTo(x, y);
    previous = true;
    lastPoint = [x, y];
  });
  ctx.stroke();
  if (lastPoint) {
    ctx.beginPath();
    ctx.arc(lastPoint[0], lastPoint[1], 3, 0, 2 * Math.PI);
    ctx.fill();
  }
}

async function refreshCurrent() {
  try {
    const response = await fetch('/api/current');
    if (!response.ok) throw Error(response.status);
    const values = await response.json();
    values.forEach((value, index) => {
      cards[index].value.textContent = Number.isFinite(value)
        ? value.toFixed(metrics[index][2]) : '—';
    });
    statusElement.textContent = values.some(Number.isFinite)
      ? 'Posledná kontrola o ' + new Date().toLocaleTimeString('sk-SK')
      : 'Čakám na údaje zo snímačov';
  } catch {
    cards.forEach(card => { card.value.textContent = '—'; });
    statusElement.textContent = 'Spojenie s monitorom je nedostupné';
  }
}

async function refreshHistory() {
  const request = ++historyRequest;
  try {
    const response = await fetch('/api/history?minutes=' + minutes);
    if (!response.ok) throw Error(response.status);
    const csv = (await response.text()).trim();
    if (request !== historyRequest) return;
    rows = csv ? csv.split('\n').map(line => line.split(',').map(value =>
      value === '' ? NaN : Number(value))) : [];
    cards.forEach(draw);
  } catch {
    // Keep the last chart visible during a temporary network interruption.
  }
}

buttons.forEach(button => button.addEventListener('click', () => {
  minutes = Number(button.dataset.minutes);
  buttons.forEach(item => item.setAttribute('aria-pressed', item === button));
  rows = [];
  cards.forEach(draw);
  refreshHistory();
}));
window.addEventListener('resize', () => cards.forEach(draw));
cards.forEach(draw);
refreshCurrent();
refreshHistory();
setInterval(refreshCurrent, 10000);
setInterval(refreshHistory, 60000);
</script>
</body>
</html>
)DASHBOARD";
