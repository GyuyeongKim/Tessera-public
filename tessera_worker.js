import TesseraModule from './tessera.js?v=d022353133';
import { decode } from './dataset.js?v=afa3a41209';
import { runTessera } from './tessera_run.js?v=03e342dc8c';

let M, bytes;

onmessage = async (e) => {
  const m = e.data;
  if (m.type === 'init') {
    bytes = m.bytes;
    M = await TesseraModule();
    postMessage({ type: 'ready', version: M.UTF8ToString(M._td_version()) });
    return;
  }
  if (m.type === 'run') {
    try {
      const r = runTessera(M, decode(bytes, m.articles), m.opts);
      postMessage({ type: 'result', id: m.id, ...r });
    } catch (err) {
      postMessage({ type: 'error', id: m.id, message: String(err && err.message || err) });
    }
  }
};
