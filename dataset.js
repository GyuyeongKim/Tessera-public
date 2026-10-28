export function decode(bytes, articles) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const magic = String.fromCharCode(...bytes.subarray(0, 4));
  if (magic !== 'TSRD') throw new Error('not a Tessera demo dataset');
  const total = dv.getUint32(8, true), words = dv.getUint32(12, true), rows = dv.getUint32(16, true);
  const n = Math.min(articles ?? total, total);
  let p = 20;
  const varint = () => {
    let v = 0, s = 0, b;
    do { b = bytes[p++]; v += (b & 0x7f) * 2 ** s; s += 7; } while (b & 0x80);
    return v;
  };
  const k = new Int32Array(rows), x = new Int32Array(rows);
  let r = 0;
  for (let a = 0; a < n; a++) {
    const len = varint();
    let prev = -1;
    for (let i = 0; i < len; i++) { prev += varint() + 1; k[r] = a; x[r] = prev; r++; }
  }
  return { articles: n, words, rows: r, k: k.subarray(0, r), x: x.subarray(0, r) };
}
