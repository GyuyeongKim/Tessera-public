export function runTessera(M, data, { mode = 1, minValue = 0, topk = -1, measure = 0, threshold = 0, word = -1, top = 10, verbose = 0 },
  onPhase = () => {}) {
  const n = data.rows;
  const kp = M._malloc(4 * n), xp = M._malloc(4 * n);
  M.HEAP32.set(data.k, kp >> 2);
  M.HEAP32.set(data.x, xp >> 2);
  const t0 = performance.now();
  const q = M._td_new();
  M._td_add(q, kp, xp, n);
  const add = performance.now() - t0;
  onPhase('added', add);
  const t1 = performance.now();
  if (measure) M._td_similarity(q, measure, threshold);
  const rc = M._td_run(q, mode, minValue, topk, verbose);
  if (rc !== 0) { const e = M.UTF8ToString(M._td_error()); M._td_free(q); M._free(kp); M._free(xp); throw new Error(e || `rc ${rc}`); }
  const run = performance.now() - t1;
  onPhase('ran', run);
  const ap = M._malloc(4 * top), bp = M._malloc(4 * top), vp = M._malloc(8 * top);
  const t2 = performance.now();
  const got = M._td_drain(q, word, top, ap, bp, vp);
  const drain = performance.now() - t2;
  const st = M._td_stats() >> 3;
  const res = { ms: add + run + drain, phases: { add, run, drain }, cells: M.HEAPF64[st], sum: M.HEAPF64[st + 1], top: [] };
  for (let i = 0; i < got; i++) res.top.push([M.HEAP32[(ap >> 2) + i], M.HEAP32[(bp >> 2) + i], M.HEAPF64[(vp >> 3) + i]]);
  M._td_free(q);
  for (const p of [kp, xp, ap, bp, vp]) M._free(p);
  return res;
}

// Reads the lines the core prints with verbose on: the words kept by the bound, the build and count times, and the
// number of pair additions.
export function parseVerbose(lines) {
  const s = lines.join('\n'), out = {};
  let m = s.match(/pushdown having >= (\d+): items (\d+) of (\d+)/);
  if (m) Object.assign(out, { bound: 'having', tau: +m[1], kept: +m[2] });
  m = s.match(/pushdown top-(\d+): pass 1 items (\d+) tau1 (\d+)(?:, pass 2 items (?:\d+ to )?(\d+)(?: rows \d+ of \d+)? tau (\d+))?/);
  if (m) Object.assign(out, { bound: 'top', topk: +m[1], first: +m[2], tau1: +m[3], kept: +(m[4] ?? m[2]), tau: +(m[5] ?? m[3]), passes: m[4] ? 2 : 1 });
  m = s.match(/tessera: n (\d+) rows (\d+) items (\d+) H \d+ \| build ([\d.]+) s, order ([\d.]+), panels ([\d.]+), dense ([\d.]+), sparse ([\d.]+), total ([\d.]+) \| tdpbuud \d+ panels \d+ block_pairs \d+ sparse_products (\d+)/);
  if (m) Object.assign(out, { rows: +m[1], keys: +m[2], words: +m[3], build: +m[4] * 1000, total: +m[9] * 1000, adds: +m[10] });
  return out;
}
