export function runTessera(M, data, { mode = 1, minValue = 0, topk = -1, measure = 0, threshold = 0, word = -1, top = 10 }) {
  const n = data.rows;
  const kp = M._malloc(4 * n), xp = M._malloc(4 * n);
  M.HEAP32.set(data.k, kp >> 2);
  M.HEAP32.set(data.x, xp >> 2);
  const t0 = performance.now();
  const q = M._td_new();
  M._td_add(q, kp, xp, n);
  if (measure) M._td_similarity(q, measure, threshold);
  const rc = M._td_run(q, mode, minValue, topk);
  if (rc !== 0) { const e = M.UTF8ToString(M._td_error()); M._td_free(q); M._free(kp); M._free(xp); throw new Error(e || `rc ${rc}`); }
  const ap = M._malloc(4 * top), bp = M._malloc(4 * top), vp = M._malloc(8 * top);
  const got = M._td_drain(q, word, top, ap, bp, vp);
  const ms = performance.now() - t0;
  const st = M._td_stats() >> 3;
  const res = { ms, cells: M.HEAPF64[st], sum: M.HEAPF64[st + 1], top: [] };
  for (let i = 0; i < got; i++) res.top.push([M.HEAP32[(ap >> 2) + i], M.HEAP32[(bp >> 2) + i], M.HEAPF64[(vp >> 3) + i]]);
  M._td_free(q);
  for (const p of [kp, xp, ap, bp, vp]) M._free(p);
  return res;
}
