import * as duckdb from 'https://cdn.jsdelivr.net/npm/@duckdb/duckdb-wasm@1.32.0/+esm';

export async function openDuck() {
  const bundle = await duckdb.selectBundle(duckdb.getJsDelivrBundles());
  const url = URL.createObjectURL(new Blob([`importScripts("${bundle.mainWorker}");`], { type: 'text/javascript' }));
  const db = new duckdb.AsyncDuckDB(new duckdb.VoidLogger(), new Worker(url));
  await db.instantiate(bundle.mainModule, bundle.pthreadWorker);
  URL.revokeObjectURL(url);
  const conn = await db.connect();
  const version = (await conn.query('SELECT version() AS v')).toArray()[0].v;
  return { db, conn, version };
}

export async function loadBase(duck, data) {
  const parts = ['article,word'];
  for (let i = 0; i < data.rows; i++) parts.push(`${data.k[i]},${data.x[i]}`);
  await duck.db.registerFileText('base.csv', parts.join('\n'));
  await duck.conn.query("CREATE OR REPLACE TABLE base AS SELECT * FROM read_csv('base.csv', header = true, columns = {'article': 'INTEGER', 'word': 'INTEGER'})");
  await duck.db.dropFile('base.csv');
}

export async function useArticles(duck, n) {
  await duck.conn.query(`CREATE OR REPLACE TABLE words AS SELECT * FROM base WHERE article < ${n}`);
}

export async function runDuck(duck, sql) {
  const t0 = performance.now();
  try {
    const rows = (await duck.conn.query(sql)).toArray().map(r => r.toJSON());
    return { ms: performance.now() - t0, rows };
  } catch (err) {
    return { ms: performance.now() - t0, error: String(err && err.message || err) };
  }
}
