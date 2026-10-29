import { decode } from './dataset.js?v=afa3a41209';
import { openDuck, loadBase, useArticles, runDuck } from './duck_run.js?v=5cd050b38b';
import { dict } from './i18n.js?v=f3a48692d3';

const SIZES = [1000, 2000, 5000, 10000];
const fmt = new Intl.NumberFormat('en-US');
const $ = (id) => document.getElementById(id);
const state = { articles: 2000, query: 'top', pct: 2, busy: true };
let words = [], stats = {}, duck = null, duckArticles = 0, tess = null, nextId = 1;
let lang = document.documentElement.lang === 'ko' ? 'ko' : 'en', dark = false, tessVersion = null;
const live = new Map();

function t(key, vars = {}) {
  return (dict[lang][key] ?? dict.en[key]).replace(/\{(\w+)\}/g, (_, k) => vars[k]);
}

function say(id, key, vars = {}, html = false) {
  if (key) live.set(id, { key, vars, html }); else live.delete(id);
  const s = key ? t(key, vars) : '';
  if (html) $(id).innerHTML = s; else $(id).textContent = s;
}

function seconds(ms) {
  if (ms < 1000) return `${(ms / 1000).toFixed(3)}\u00a0s`;
  if (ms < 60000) return `${(ms / 1000).toFixed(2)}\u00a0s`;
  return `${(ms / 60000).toFixed(1)}\u00a0min`;
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
  $('sizeinfo').textContent = s ? t('sizeinfo', { rows: fmt.format(s.rows), joined: fmt.format(s.joined) }) : '';
  $('joinedrows').textContent = s ? fmt.format(s.joined) : t('joined.many');
  $('sql').textContent = sql();
  $('run').disabled = state.busy;
  for (const el of document.querySelectorAll('input[name=q], #pct')) el.disabled = state.busy;
}

function lane(which, time, key, vars, bad) {
  $(`${which}time`).textContent = time;
  say(`${which}state`, key, vars);
  $(`${which}state`).classList.toggle('bad', !!bad);
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

function showRows(top, key, vars) {
  say('summary', key, vars);
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
  say('verdict', null);
  $('result').hidden = true;
  lane('duck', '–', 'lane.waiting');
  lane('tess', '–', 'lane.running');
  const articles = state.articles, query = state.query, text = sql();
  if (duckArticles !== articles) {
    say('status', 'status.table');
    await useArticles(duck, articles);
    duckArticles = articles;
  }
  say('status', 'status.running');
  let stop = ticker('tess');
  const r = await runTess(articles, tessOpts());
  stop();
  if (r.type === 'error') lane('tess', '–', 'raw', { s: r.message }, true);
  else lane('tess', seconds(r.ms), 'lane.done');
  lane('duck', '0.000\u00a0s', 'lane.running');
  stop = ticker('duck');
  const d = await runDuck(duck, text);
  stop();
  const oom = d.error && /out of memory/i.test(d.error);
  if (d.error) lane('duck', seconds(d.ms), oom ? 'lane.oom' : 'raw', { s: d.error }, true);
  else lane('duck', seconds(d.ms), 'lane.done');

  const n = fmt.format(articles), cells = fmt.format(r.cells), sum = fmt.format(r.sum);
  if (r.type !== 'error' && !d.error) {
    const same = sameTop(d.rows, r.top) && (query === 'top' || (Number(d.rows[0]?.pairs ?? 0) === r.cells && Number(d.rows[0]?.total ?? 0) === r.sum));
    const gain = d.ms / r.ms;
    if (same) say('verdict', 'verdict.same', { gain: gain >= 10 ? Math.round(gain) : gain.toFixed(1) }, true);
    else say('verdict', 'verdict.differ', {}, true);
    if (query === 'top') showRows(r.top, 'sum.top', { n });
    else if (query === 'having') showRows(r.top, 'sum.having', { cells, tau: fmt.format(tau()), sum });
    else showRows(r.top, 'sum.all', { cells, sum });
  } else if (r.type !== 'error' && oom) {
    say('verdict', 'verdict.oom', { duck: seconds(d.ms), tess: seconds(r.ms) }, true);
    showRows(r.top, query === 'top' ? 'sum.oomtop' : 'sum.oom', { n, cells });
  }
  say('status', 'status.again');
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

function setTheme(theme) {
  document.documentElement.dataset.theme = theme;
  try { localStorage.setItem('theme', theme); } catch (e) {}
  dark = theme === 'dark';
  $('theme').textContent = t(dark ? 'theme.light' : 'theme.dark');
}

function applyLang() {
  document.documentElement.lang = lang;
  document.title = t('title');
  document.querySelector('meta[name=description]').content = t('description');
  for (const el of document.querySelectorAll('[data-i18n]')) el.textContent = t(el.dataset.i18n);
  for (const el of document.querySelectorAll('[data-i18n-html]')) el.innerHTML = t(el.dataset.i18nHtml);
  for (const el of document.querySelectorAll('[data-i18n-aria]')) el.setAttribute('aria-label', t(el.dataset.i18nAria));
  $('lang').textContent = t('other');
  $('lang').lang = t('otherLang');
  $('lang').setAttribute('aria-label', t('aria.lang'));
  $('theme').textContent = t(dark ? 'theme.light' : 'theme.dark');
  for (const [id, { key, vars, html }] of live) say(id, key, vars, html);
  if (tessVersion) $('tessfoot').textContent = tessVersion;
  renderControls();
}

function setLang(next) {
  lang = next;
  try { localStorage.setItem('lang', lang); } catch (e) {}
  const url = new URL(location.href);
  if (url.searchParams.has('lang')) { url.searchParams.set('lang', lang); history.replaceState(null, '', url); }
  applyLang();
}

async function main() {
  dark = document.documentElement.dataset.theme === 'dark' ||
    (!document.documentElement.dataset.theme && matchMedia('(prefers-color-scheme: dark)').matches);
  $('theme').onclick = () => setTheme(dark ? 'light' : 'dark');
  $('lang').onclick = () => setLang(lang === 'ko' ? 'en' : 'ko');
  say('status', 'status.loading');
  lane('duck', '–', 'lane.waiting');
  lane('tess', '–', 'lane.waiting');
  applyLang();
  for (const r of document.querySelectorAll('input[name=q]')) r.onchange = () => { state.query = r.value; renderControls(); };
  $('pct').onchange = (e) => { state.pct = Number(e.target.value); renderControls(); };
  $('run').onclick = run;
  try {
    const bytes = await gunzip('data/nytimes.bin.gz?v=6a6ecf908e');
    words = new TextDecoder().decode(await gunzip('data/words.txt.gz?v=5e6ff6f0d1')).split('\n');
    const all = decode(bytes);
    const len = new Int32Array(all.articles);
    for (let i = 0; i < all.rows; i++) len[all.k[i]]++;
    for (const n of SIZES) {
      let rows = 0, joined = 0;
      for (let a = 0; a < n; a++) { rows += len[a]; joined += len[a] * (len[a] - 1) / 2; }
      stats[n] = { rows, joined };
    }
    renderControls();
    tess = new Worker('tessera_worker.js?v=632db4898a', { type: 'module' });
    const ready = new Promise(res => tess.addEventListener('message', e => { if (e.data.type === 'ready') res(e.data); }, { once: true }));
    tess.postMessage({ type: 'init', bytes: bytes.slice() });
    say('status', 'status.duck');
    duck = await openDuck();
    $('duckver').textContent = duck.version.replace(/^v/, '') + ' (WASM)';
    say('status', 'status.base');
    await loadBase(duck, all);
    const r = await ready;
    tessVersion = r.version;
    $('tessver').textContent = `${r.version} (WASM)`;
    $('tessfoot').textContent = r.version;
    say('status', 'status.ready');
    state.busy = false;
    renderControls();
  } catch (err) {
    say('status', 'status.failed', { e: err && err.message || err });
  }
}

main();
