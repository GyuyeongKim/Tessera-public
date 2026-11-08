import TesseraModule from './tessera.js?v=de411f0484';
import { decode } from './dataset.js?v=6516010bf0';
import { runTessera, parseVerbose } from './tessera_run.js?v=55f60a89f6';

let M, bytes, lines = [];

onmessage = async (e) => {
  const m = e.data;
  if (m.type === 'init') {
    bytes = m.bytes;
    M = await TesseraModule({ printErr: (s) => lines.push(s) });
    postMessage({ type: 'ready', version: M.UTF8ToString(M._td_version()) });
    return;
  }
  if (m.type === 'run') {
    try {
      lines = [];
      const phase = (name, ms) => postMessage({ type: 'phase', id: m.id, name, ms, info: name === 'ran' ? parseVerbose(lines) : null });
      const r = runTessera(M, decode(bytes, m.articles), { ...m.opts, verbose: 1 }, phase);
      postMessage({ type: 'result', id: m.id, ...r, info: parseVerbose(lines) });
    } catch (err) {
      postMessage({ type: 'error', id: m.id, message: String(err && err.message || err) });
    }
  }
};
