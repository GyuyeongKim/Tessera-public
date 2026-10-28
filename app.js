import { decode } from './dataset.js';
import { openDuck, loadBase, useArticles, runDuck } from './duck_run.js';

const SIZES = [1000, 2000, 5000, 10000];
const fmt = new Intl.NumberFormat('en-US');
const $ = (id) => document.getElementById(id);
const state = { articles: 2000, query: 'top', pct: 2, busy: true };
let words = [], stats = {}, duck = null, duckArticles = 0, tess = null, nextId = 1;

function seconds(ms) {
  if (ms < 1000) return `${(ms / 1000).toFixed(3)} s`;
  if (ms < 60000) return `${(ms / 1000).toFixed(2)} s`;
  return `${(ms / 60000).toFixed(1)} min`;
}

function wordText(i) {
  const w = words[i] ?? String(i);
  if (!w.startsWith('zzz_')) return w;
  return w.slice(4).split('_').map(s => s.charAt(0).toUpperCase() + s.slice(1)).join(' ');
}

function tau() { return Math.ceil(state.articles * state.pct / 100); }

function sql() {
  const inner = 'SELECT a.word AS w1, b.word AS w2, COUNT(*) AS n\nFROM words a JOIN words b\n  ON a.article = b.article AND a.word < b.word\nGROUP BY a.word, b.word';
  if (state.query === 'top') return `${inner}\nORDER BY n DESC, w1, w2\nLIMIT 10`;
  const having = state.query === 'having' ? `\nHAVING COUNT(*) >= ${tau()}` : '';
  const indented = (inner + having).split('\n').map(l => `  ${l}`).join('\n');
  return `WITH p AS (\n${indented})\nSELECT w1, w2, n, COUNT(*) OVER () AS pairs, SUM(n) OVER () AS total\nFROM p\nORDER BY n DESC, w1, w2\nLIMIT 10`;
}

function tessOpts() {
  if (state.query === 'top') return { mode: 1, topk: 10, top: 10 };
  if (state.query === 'having') return { mode: 1, minValue: tau(), top: 10 };
  return { mode: 1, top: 10 };
}

function renderControls() {
  const seg = $('sizes');
  seg.replaceChildren(...SIZES.map(n => {
    const b = document.createElement('button');
    b.type = 'button';
    b.textContent = fmt.format(n);
    b.setAttribute('role', 'radio');
    b.setAttribute('aria-checked', String(n === state.articles));
    b.disabled = state.busy;
    b.onclick = () => { state.articles = n; renderControls(); };
    return b;
  }));
  const s = stats[state.articles];
  $('sizeinfo').textContent = s ? `${fmt.format(s.rows)} rows, ${fmt.format(s.joined)} joined rows` : '';
  $('joinedrows').textContent = s ? fmt.format(s.joined) : 'millions of';
  $('sql').textContent = sql();
  $('run').disabled = state.busy;
  for (const el of document.querySelectorAll('input[name=q], #pct')) el.disabled = state.busy;
}

function lane(which, time, text, bad) {
  $(`${which}time`).textContent = time;
  const st = $(`${which}state`);
  st.textContent = text;
  st.classList.toggle('bad', !!bad);
}

function ticker(which) {
  const t0 = performance.now();
  let on = true;
  const step = () => { if (!on) return; $(`${which}time`).textContent = seconds(performance.now() - t0); requestAnimationFrame(step); };
  requestAnimationFrame(step);
  return () => { on = false; };
}

function runTess(articles, opts) {
  return new Promise((resolve) => {
    const id = nextId++;
    const on = (e) => {
      if (e.data.id !== id) return;
      tess.removeEventListener('message', on);
      resolve(e.data);
    };
    tess.addEventListener('message', on);
    tess.postMessage({ type: 'run', id, articles, opts });
  });
}

function sameTop(duckRows, tessTop) {
  if (duckRows.length !== tessTop.length) return false;
  return duckRows.every((r, i) => r.w1 === tessTop[i][0] && r.w2 === tessTop[i][1] && Number(r.n) === tessTop[i][2]);
}

function showRows(top, summary) {
  $('summary').textContent = summary;
  $('rows').replaceChildren(...top.map(([a, b, n], i) => {
    const tr = document.createElement('tr');
    tr.innerHTML = `<td>${i + 1}</td><td class="pair"></td><td class="num">${fmt.format(n)}</td>`;
    const cell = tr.children[1];
    cell.append(wordText(a));
    const plus = document.createElement('span');
    plus.className = 'plus';
    plus.textContent = '+';
    cell.append(plus, wordText(b));
    return tr;
  }));
  $('result').hidden = false;
}

async function run() {
  state.busy = true;
  renderControls();
  $('verdict').textContent = '';
  $('result').hidden = true;
  lane('duck', '–', 'Waiting');
  lane('tess', '–', 'Running');
  const articles = state.articles, query = state.query, text = sql();
  if (duckArticles !== articles) {
    $('status').textContent = 'Preparing the table in DuckDB…';
    await useArticles(duck, articles);
    duckArticles = articles;
  }
  $('status').textContent = 'Running Tessera, then DuckDB, one after the other.';
  let stop = ticker('tess');
  const t = await runTess(articles, tessOpts());
  stop();
  if (t.type === 'error') lane('tess', '–', t.message, true);
  else lane('tess', seconds(t.ms), 'Done');
  lane('duck', '0.000 s', 'Running');
  stop = ticker('duck');
  const d = await runDuck(duck, text);
  stop();
  const oom = d.error && /out of memory/i.test(d.error);
  if (d.error) lane('duck', seconds(d.ms), oom ? 'Out of memory: a browser page has about 3 GB' : d.error, true);
  else lane('duck', seconds(d.ms), 'Done');

  const v = $('verdict');
  if (t.type !== 'error' && !d.error) {
    const same = sameTop(d.rows, t.top) && (query === 'top' || (Number(d.rows[0]?.pairs ?? 0) === t.cells && Number(d.rows[0]?.total ?? 0) === t.sum));
    const gain = d.ms / t.ms;
    v.innerHTML = same
      ? `<span class="same">Same answer.</span> Tessera was <span class="x">${gain >= 10 ? Math.round(gain) : gain.toFixed(1)}&times;</span> faster.`
      : 'The answers differ.';
    const summary = query === 'top' ? `The 10 most frequent pairs among ${fmt.format(articles)} articles.`
      : `${fmt.format(t.cells)} pairs${query === 'having' ? ` in at least ${fmt.format(tau())} articles` : ''}, ${fmt.format(t.sum)} co-occurrences in all. The 10 most frequent:`;
    showRows(t.top, summary);
  } else if (t.type !== 'error' && oom) {
    v.innerHTML = `DuckDB ran out of memory after ${seconds(d.ms)}. Tessera answered in <span class="x">${seconds(t.ms)}</span>.`;
    showRows(t.top, query === 'top' ? `Tessera's 10 most frequent pairs among ${fmt.format(articles)} articles.`
      : `Tessera found ${fmt.format(t.cells)} pairs. The 10 most frequent:`);
  }
  $('status').textContent = 'Ready. Change the articles or the query and run again.';
  state.busy = false;
  renderControls();
}

async function gunzip(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
  const buf = new Uint8Array(await res.arrayBuffer());
  if (buf[0] !== 0x1f || buf[1] !== 0x8b) return buf;
  return new Uint8Array(await new Response(new Blob([buf]).stream().pipeThrough(new DecompressionStream('gzip'))).arrayBuffer());
}

function setTheme(t) {
  document.documentElement.dataset.theme = t;
  try { localStorage.setItem('theme', t); } catch (e) {}
  $('theme').textContent = t === 'dark' ? 'Light' : 'Dark';
}

async function main() {
  const dark = document.documentElement.dataset.theme === 'dark' ||
    (!document.documentElement.dataset.theme && matchMedia('(prefers-color-scheme: dark)').matches);
  $('theme').textContent = dark ? 'Light' : 'Dark';
  $('theme').onclick = () => setTheme($('theme').textContent === 'Dark' ? 'dark' : 'light');
  for (const r of document.querySelectorAll('input[name=q]')) r.onchange = () => { state.query = r.value; renderControls(); };
  $('pct').onchange = (e) => { state.pct = Number(e.target.value); renderControls(); };
  $('run').onclick = run;
  renderControls();
  try {
    const bytes = await gunzip('data/nytimes.bin.gz');
    words = new TextDecoder().decode(await gunzip('data/words.txt.gz')).split('\n');
    const all = decode(bytes);
    const len = new Int32Array(all.articles);
    for (let i = 0; i < all.rows; i++) len[all.k[i]]++;
    for (const n of SIZES) {
      let rows = 0, joined = 0;
      for (let a = 0; a < n; a++) { rows += len[a]; joined += len[a] * (len[a] - 1) / 2; }
      stats[n] = { rows, joined };
    }
    renderControls();
    tess = new Worker('tessera_worker.js', { type: 'module' });
    const ready = new Promise(res => tess.addEventListener('message', e => { if (e.data.type === 'ready') res(e.data); }, { once: true }));
    tess.postMessage({ type: 'init', bytes: bytes.slice() });
    $('status').textContent = 'Loading DuckDB-WASM (about 5 MB)…';
    duck = await openDuck();
    $('duckver').textContent = duck.version.replace(/^v/, '') + ' (WASM)';
    $('status').textContent = 'Loading the articles into DuckDB…';
    await loadBase(duck, all);
    const r = await ready;
    $('tessver').textContent = `${r.version} (WASM)`;
    $('tessfoot').textContent = r.version;
    $('status').textContent = 'Ready.';
    state.busy = false;
    renderControls();
  } catch (err) {
    $('status').textContent = `Could not start: ${err && err.message || err}`;
  }
}

main();
