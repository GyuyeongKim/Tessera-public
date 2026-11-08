// Draws what each engine does while its time runs: DuckDB's plan as EXPLAIN returns it, with the rows that leave each
// operator, and Tessera's steps with the times and counts its core reports.

const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

function width(n) {
  return n > 0 ? Math.max(2, Math.min(18, 1.5 * 1.32 ** Math.log10(n))) : 2;
}

function track(n, on) {
  const w = width(n), p = n > 1e6 ? 7 : n > 1e4 ? 10 : 15;
  return `<span class="track${on ? ' on' : ''}" style="--w:${w.toFixed(1)}px;--p:${p}px"></span>`;
}

function opName(node) { return String(node.name).trim(); }

function detail(node) {
  const e = node.extra_info || {}, list = (v) => (Array.isArray(v) ? v.join(', ') : v);
  switch (opName(node)) {
    case 'SEQ_SCAN': return e.Table;
    case 'HASH_JOIN': return [].concat(e.Conditions || []).join(' AND ');
    case 'HASH_GROUP_BY': return [e.Aggregates, e.Groups && `GROUP BY ${list(e.Groups)}`].filter(Boolean).join(' · ');
    case 'FILTER': return e.Expression;
    case 'TOP_N': return [`Top ${e.Top}`, list(e['Order By'])].filter(Boolean).join(' · ');
    default: return list(e.Projections ?? '');
  }
}

function levels(node) {
  const kids = node.children || [];
  if (!kids.length) return [[node]];
  if (kids.length === 2 && kids.every((k) => !(k.children || []).length)) return [kids, [node]];
  return [...kids.flatMap(levels), [node]];
}

export function createFlows({ duckEl, tessEl, duckBar, tessBar, t, fmt }) {
  const m = { plan: null, ctx: {}, duck: 'idle', tess: {}, tessMs: null, duckMs: null, tessRun: null };

  const ms = (x) => (x < 1 ? `${x.toFixed(1)} ms` : x < 1000 ? `${Math.round(x)} ms` : `${(x / 1000).toFixed(2)} s`);

  function rows(node) {
    const kids = (node.children || []).map(rows), c = m.ctx;
    switch (opName(node)) {
      case 'SEQ_SCAN': return c.rows;
      case 'HASH_JOIN': return c.joined;
      case 'HASH_GROUP_BY': return c.groups;
      case 'FILTER': return c.filtered ?? null;
      case 'TOP_N': return kids[0] == null ? null : Math.min(Number(node.extra_info?.Top ?? Infinity), kids[0]);
      case 'PROJECTION': case 'WINDOW': return kids[0] ?? null;
      default: return null;
    }
  }

  function edge(ns, on, label) {
    return `<li class="edge"><span class="tracks">${ns.map((n) => track(n ?? 0, on)).join('')}</span>` +
      `<span class="cnt">${label ?? ''}</span></li>`;
  }

  function drawDuck() {
    if (!m.plan) { duckEl.innerHTML = ''; return; }
    const run = m.duck === 'run';
    let h = `<p class="flow-h">${esc(t('flow.duck'))}</p><ol class="ops">`;
    for (const lvl of levels(m.plan)) {
      h += `<li class="lvl${lvl.length > 1 ? ' two' : ''}">` + lvl.map((n) =>
        `<div class="op"><code>${esc(opName(n))}</code><span class="x mono">${esc(detail(n) ?? '')}</span></div>`).join('') + '</li>';
      const out = lvl.map(rows);
      const known = out.every((n) => n != null);
      const label = !known ? '' : lvl.length > 1 && out[0] === out[1] ? t('flow.rows2', { n: fmt.format(out[0]) })
        : t('flow.rows', { n: fmt.format(out[0]) });
      h += edge(out, run, `<span class="${known && out[0] > 1e7 ? 'big' : ''}">${esc(label)}</span>`);
    }
    duckEl.innerHTML = h + `</ol><p class="flow-note">${esc(t('flow.duck.note'))}</p>`;
    duckEl.dataset.state = m.duck;
  }

  function bound() {
    const i = m.tess.info || {};
    if (m.ctx.query === 'all') return i.words ? t('ts.count.all', { words: fmt.format(i.words) }) : '';
    if (i.bound === 'top' && i.passes === 1) return t('ts.count.top', { h: fmt.format(i.first), tau: fmt.format(i.tau1) });
    if (i.bound === 'having') return t('ts.count.having', { h: fmt.format(i.kept), tau: fmt.format(i.tau), words: fmt.format(i.words) });
    return '';
  }

  function drawTess() {
    const s = m.tess, i = s.info || {}, st = s.steps || {}, c = m.ctx;
    const idle = !Object.keys(st).length;
    const step = (key, name, time, lines) => `<li class="lvl"><div class="op" data-s="${st[key] || (idle ? 'idle' : 'wait')}">` +
      `<span class="nm">${esc(t(name))}</span><span class="tm">${time == null ? '' : ms(time)}</span>` +
      lines.filter(Boolean).map((l) => `<span class="x">${l}</span>`).join('') + '</div></li>';
    const lit = (a, b) => st[a] === 'run' || st[b] === 'run';
    let h = `<p class="flow-h">${esc(t('flow.tess'))}</p><ol class="ops">`;
    h += step('in', 'ts.in', s.add, [c.rows != null ? esc(t('ts.in.d', { rows: fmt.format(c.rows) })) : '']);
    h += edge([c.rows], lit('in', 'lists'), c.rows != null ? esc(t('flow.rows', { n: fmt.format(c.rows) })) : '');
    h += step('lists', 'ts.lists', s.build, [esc(i.words ? t('ts.lists.d', { n: fmt.format(c.articles), words: fmt.format(i.words) }) : t('ts.lists.i'))]);
    h += `<li class="edge"><span class="tracks"><span class="track thin${lit('lists', 'count') ? ' on' : ''}" style="--w:2px;--p:15px"></span></span>` +
      `<span class="cnt">${esc(t('ts.norows'))}</span></li>`;
    h += step('count', 'ts.count', s.count, [esc(bound() || t(c.query === 'all' ? 'ts.count.ia' : 'ts.count.i')),
      i.adds != null ? `<b>${esc(t('ts.adds', { n: fmt.format(i.adds) }))}</b>` : '']);
    h += edge([s.cells], lit('count', 'out'), s.cells != null ? esc(t('flow.pairs', { n: fmt.format(s.cells) })) : '');
    h += step('out', 'ts.out', s.drain, [esc(t('ts.out.d'))]);
    h += edge([s.shown], st.out === 'run', s.shown != null ? esc(t('flow.rows', { n: fmt.format(s.shown) })) : '');
    tessEl.innerHTML = h + `</ol><p class="flow-note">${esc(t('flow.tess.note'))}</p>`;
  }

  function drawBars() {
    const scale = Math.max(m.tessMs ?? m.tessRun ?? 0, m.duckMs ?? 0);
    const w = (x) => (scale > 0 ? `max(3px, ${((100 * x) / scale).toFixed(3)}%)` : '0px');
    const s = m.tess;
    const fill = (x, segs) => `<span class="fill" style="width:${w(x)}">${segs}</span>`;
    if (m.tessMs != null && s.add != null) {
      const parts = [['in', s.add], ['lists', s.build ?? 0], ['count', s.count ?? 0], ['out', s.drain ?? 0]];
      tessBar.innerHTML = fill(m.tessMs, parts.map(([k, x]) => `<span class="part ${k}" style="flex-grow:${x}"></span>`).join(''));
    } else if (m.tessRun != null) {
      tessBar.innerHTML = fill(m.tessRun, '<span class="part run"></span>');
    } else tessBar.innerHTML = '';
    duckBar.innerHTML = m.duckMs != null ? fill(m.duckMs, `<span class="part ${m.duck}"></span>`) : '';
  }

  function widths() {
    const scale = Math.max(m.tessMs ?? m.tessRun ?? 0, m.duckMs ?? 0);
    const set = (bar, x) => { const f = bar.firstElementChild; if (f && x != null) f.style.width = `max(3px, ${((100 * x) / scale).toFixed(3)}%)`; };
    if (!(scale > 0)) return;
    set(tessBar, m.tessMs ?? m.tessRun);
    set(duckBar, m.duckMs);
  }

  function draw() { drawDuck(); drawTess(); drawBars(); }

  return {
    draw,
    setPlan(plan) { m.plan = plan; drawDuck(); },
    setContext(ctx) { m.ctx = ctx; drawDuck(); drawTess(); },
    reset() {
      m.duck = 'idle'; m.tess = { steps: {} }; m.tessMs = null; m.duckMs = null; m.tessRun = null;
      draw();
    },
    tessStart() { m.tess = { steps: { in: 'run' } }; m.tessRun = 0; draw(); },
    tessTick(x) { m.tessRun = x; widths(); },
    tessPhase(name, x, info) {
      const s = m.tess;
      if (name === 'added') { s.add = x; s.steps = { in: 'done', lists: 'run', count: 'run' }; }
      if (name === 'ran') {
        s.info = info || {};
        s.build = s.info.build != null ? Math.min(s.info.build, x) : null;
        s.count = s.build != null ? x - s.build : x;
        s.steps = { in: 'done', lists: 'done', count: 'done', out: 'run' };
      }
      drawTess();
    },
    tessDone(r) {
      const s = m.tess;
      if (r.type === 'error') { m.tess = { steps: {} }; m.tessRun = null; draw(); return; }
      s.info = r.info || s.info || {};
      s.add = r.phases.add; s.drain = r.phases.drain;
      s.build = s.info.build != null ? Math.min(s.info.build, r.phases.run) : null;
      s.count = s.build != null ? r.phases.run - s.build : r.phases.run;
      s.cells = r.cells; s.shown = r.top.length;
      s.steps = { in: 'done', lists: 'done', count: 'done', out: 'done' };
      m.tessMs = r.ms; m.tessRun = null;
      draw();
    },
    setFiltered(n) { m.ctx = { ...m.ctx, filtered: n }; drawDuck(); },
    duckStart() { m.duck = 'run'; m.duckMs = 0; drawDuck(); drawBars(); },
    duckTick(x) { m.duckMs = x; widths(); },
    duckDone(x, ok) { m.duck = ok ? 'done' : 'bad'; m.duckMs = x; drawDuck(); drawBars(); },
  };
}
